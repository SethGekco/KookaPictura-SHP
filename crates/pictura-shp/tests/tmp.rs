//! TMP codec: every template here is generated in code.

mod common;

use common::Rng;
use pictura_shp::tmp::{
    build, decode_diamond, diamond_bytes, diamond_rows, encode_diamond, in_diamond, land_label,
    parse, ramp_label, sniff, Error, Template, Tile, TileFields, FLAG_EXTRA, FLAG_RANDOM, FLAG_Z,
    TILE_HEADER, Z_NONE,
};

/// A 2 x 2 template of `cx` x `cy` tiles with slot 1 empty; tile 0 has
/// z-data, tile 3 has an extra block overhanging above it (with z when
/// `extra_z`), all pixels random but deterministic.
fn template(cx: usize, cy: usize, extra_z: bool, seed: u64) -> Template {
    let mut rng = Rng::new(seed);
    let mut tile = |x: i32, y: i32| {
        let mut t = Tile::new(x, y, cx, cy);
        for (row, start, width) in diamond_rows(cx, cy) {
            for v in &mut t.image[row * cx + start..row * cx + start + width] {
                *v = rng.below(256) as u8;
            }
        }
        t.fields = TileFields {
            height: rng.below(5) as u8,
            land: 13,
            ramp: rng.below(21) as u8,
            radar_low: [1, 2, 3],
            radar_high: [4, 5, 6],
        };
        t
    };
    let mut t0 = tile(0, 0);
    let mut z = vec![Z_NONE; cx * cy];
    for (row, start, width) in diamond_rows(cx, cy) {
        for (k, v) in z[row * cx + start..row * cx + start + width]
            .iter_mut()
            .enumerate()
        {
            *v = ((row + k) % 32) as u8;
        }
    }
    t0.z = Some(z);
    let t2 = tile(-(cx as i32) / 2, cy as i32 / 2);
    let mut t3 = tile(0, cy as i32);
    let (ew, eh) = (cx / 2, cy + 7);
    t3.extra_x = 5;
    t3.extra_y = cy as i32 - 9;
    t3.extra_w = ew;
    t3.extra_h = eh;
    t3.extra = Some((0..ew * eh).map(|i| (i * 7 % 251) as u8).collect());
    if extra_z {
        t3.z = Some(decode_diamond(
            &vec![4; diamond_bytes(cx, cy)],
            cx,
            cy,
            Z_NONE,
        ));
        t3.extra_z = Some((0..ew * eh).map(|i| (i % 32) as u8).collect());
    }
    t3.flags = FLAG_RANDOM;
    Template {
        blocks_x: 2,
        blocks_y: 2,
        cx,
        cy,
        tiles: vec![Some(t0), None, Some(t2), Some(t3)],
        unreadable: Vec::new(),
    }
}

/// The parts of a tile a writer decides (not its offset or raw header).
type Content = (i32, i32, TileFields, Vec<u8>, [Option<Vec<u8>>; 3]);

fn content(t: &Template) -> Vec<Content> {
    t.live()
        .map(|(_, t)| {
            let blocks = [t.z.clone(), t.extra.clone(), t.extra_z.clone()];
            (t.x, t.y, t.fields, t.image.clone(), blocks)
        })
        .collect()
}

#[test]
fn diamond_rows_cover_half_the_tile_in_both_sizes() {
    for (cx, cy) in [(48, 24), (60, 30)] {
        assert_eq!(diamond_bytes(cx, cy), cx * cy / 2);
        let rows = diamond_rows(cx, cy);
        assert_eq!(rows[0], (0, cx / 2 - 2, 4));
        assert_eq!(rows[cy / 2 - 1], (cy / 2 - 1, 0, cx));
        assert_eq!(
            rows.last().unwrap().2,
            4,
            "the last row of width 0 is left out"
        );
        assert!(in_diamond(cx, cy, cx / 2, 0));
        assert!(!in_diamond(cx, cy, 0, 0));
        assert!(
            !in_diamond(cx, cy, cx / 2, cy - 1),
            "the bottom row is empty"
        );
    }
}

#[test]
fn diamond_packing_is_exact_and_fills_outside() {
    let (cx, cy) = (60, 30);
    let packed: Vec<u8> = (0..diamond_bytes(cx, cy))
        .map(|i| (i % 255) as u8 + 1)
        .collect();
    let rect = decode_diamond(&packed, cx, cy, Z_NONE);
    assert_eq!(rect[0], Z_NONE);
    assert_eq!(encode_diamond(&rect, cx, cy), packed);
}

#[test]
fn a_built_template_parses_back_in_both_tile_sizes() {
    for (cx, cy) in [(48, 24), (60, 30)] {
        for extra_z in [false, true] {
            let t = template(cx, cy, extra_z, 7);
            let bytes = build(&t, None).unwrap();
            assert!(sniff(&bytes));
            let back = parse(&bytes).unwrap();
            assert_eq!(
                (back.blocks_x, back.blocks_y, back.cx, back.cy),
                (2, 2, cx, cy)
            );
            assert!(back.tiles[1].is_none(), "an empty slot stays empty");
            assert_eq!(content(&back), content(&t));
            let t3 = back.tiles[3].as_ref().unwrap();
            assert_eq!(t3.flags & FLAG_EXTRA, FLAG_EXTRA);
            assert_eq!(t3.flags & FLAG_Z != 0, extra_z);
            assert_eq!(
                t3.flags & FLAG_RANDOM,
                FLAG_RANDOM,
                "the random bit is kept"
            );
            assert_eq!(
                back.tiles[2].as_ref().unwrap().z,
                None,
                "no z-data stays none"
            );
            // An unchanged re-encode, in place or laid out afresh, is identical.
            assert_eq!(build(&back, Some(&bytes)).unwrap(), bytes);
            assert_eq!(build(&back, None).unwrap(), bytes);
        }
    }
}

#[test]
fn data_offsets_are_relative_to_the_record() {
    let t = template(60, 30, true, 3);
    let bytes = build(&t, None).unwrap();
    let back = parse(&bytes).unwrap();
    let t3 = back.tiles[3].as_ref().unwrap();
    let rel = |k: usize| i32::from_le_bytes(t3.raw[k..k + 4].try_into().unwrap()) as usize;
    let cb = diamond_bytes(60, 30);
    assert_eq!(rel(12), TILE_HEADER + cb, "z right after the diamond");
    assert_eq!(rel(8), TILE_HEADER + 2 * cb, "then the extra");
    assert_eq!(rel(16), TILE_HEADER + 2 * cb + t3.extra_w * t3.extra_h);
    assert!(
        t3.offset > rel(8),
        "an absolute reading would point inside the file header"
    );
}

/// Uninitialised header bytes in unused fields, garbage between records and
/// a tail: everything an in-place save must keep.
fn messy_file() -> Vec<u8> {
    let t = template(60, 30, false, 11);
    let clean = build(&t, None).unwrap();
    let parsed = parse(&clean).unwrap();
    let mut out = clean[..16 + 16].to_vec();
    let mut offsets = Vec::new();
    for tile in parsed.tiles.iter().flatten() {
        out.extend_from_slice(&[0xCD; 6]); // garbage before the record
        offsets.push(out.len());
        let start = tile.offset;
        let end = parsed
            .tiles
            .iter()
            .flatten()
            .map(|t| t.offset)
            .filter(|&o| o > start)
            .min()
            .unwrap_or(clean.len());
        let mut record = clean[start..end].to_vec();
        record[49..52].copy_from_slice(&[0xCD; 3]); // padding
        if tile.extra.is_none() {
            record[8..12].copy_from_slice(&[0xCD; 4]); // unused extra offset
            record[20..36].copy_from_slice(&[0xCD; 16]);
        }
        out.extend_from_slice(&record);
    }
    out.extend_from_slice(b"tail");
    for (slot, at) in [(0usize, offsets[0]), (2, offsets[1]), (3, offsets[2])] {
        out[16 + 4 * slot..20 + 4 * slot].copy_from_slice(&(at as i32).to_le_bytes());
    }
    out
}

#[test]
fn an_in_place_save_keeps_garbage_gaps_and_the_tail() {
    let bytes = messy_file();
    let t = parse(&bytes).unwrap();
    assert_eq!(build(&t, Some(&bytes)).unwrap(), bytes);
}

#[test]
fn a_one_pixel_edit_changes_one_byte() {
    let bytes = messy_file();
    let mut t = parse(&bytes).unwrap();
    let (row, start, _) = diamond_rows(60, 30)[10];
    let tile = t.tiles[2].as_mut().unwrap();
    let at = row * 60 + start + 3;
    tile.image[at] = tile.image[at].wrapping_add(1);
    let want = tile.image[at];
    let out = build(&t, Some(&bytes)).unwrap();
    assert_eq!(out.len(), bytes.len());
    let diff: Vec<usize> = (0..out.len()).filter(|&i| out[i] != bytes[i]).collect();
    assert_eq!(diff.len(), 1);
    assert_eq!(
        parse(&out).unwrap().tiles[2].as_ref().unwrap().image[at],
        want
    );
}

#[test]
fn edited_fields_change_only_their_bytes() {
    let bytes = messy_file();
    let mut t = parse(&bytes).unwrap();
    let tile = t.tiles[3].as_mut().unwrap();
    let at = tile.offset;
    tile.fields.height = 9;
    tile.fields.ramp = 4;
    tile.fields.radar_high = [200, 100, 50];
    let out = build(&t, Some(&bytes)).unwrap();
    let diff: Vec<usize> = (0..out.len()).filter(|&i| out[i] != bytes[i]).collect();
    assert!(
        diff.iter().all(|&i| (at + 40..at + 49).contains(&i)),
        "{diff:?}"
    );
    let back = parse(&out).unwrap();
    let fields = back.tiles[3].as_ref().unwrap().fields;
    assert_eq!(
        (fields.height, fields.ramp, fields.radar_high),
        (9, 4, [200, 100, 50])
    );
}

#[test]
fn edited_z_and_extra_are_written_in_place() {
    let t0 = template(60, 30, true, 5);
    let bytes = build(&t0, None).unwrap();
    let mut t = parse(&bytes).unwrap();
    let tile = t.tiles[3].as_mut().unwrap();
    tile.extra.as_mut().unwrap()[0] = 99;
    tile.extra_z.as_mut().unwrap()[1] = 31;
    tile.z.as_mut().unwrap()[7 * 60 + 30] = 12;
    let out = build(&t, Some(&bytes)).unwrap();
    assert_eq!(out.len(), bytes.len());
    let back = parse(&out).unwrap();
    assert_eq!(content(&back), content(&t));
}

#[test]
fn a_tile_that_gains_z_data_is_laid_out_afresh() {
    let bytes = messy_file();
    let mut t = parse(&bytes).unwrap();
    let tile = t.tiles[2].as_mut().unwrap();
    assert!(tile.z.is_none());
    let mut z = vec![Z_NONE; 60 * 30];
    z[15 * 60 + 30] = 7;
    tile.z = Some(z);
    let out = build(&t, Some(&bytes)).unwrap();
    let back = parse(&out).unwrap();
    let tile = back.tiles[2].as_ref().unwrap();
    assert_eq!(tile.flags & FLAG_Z, FLAG_Z);
    assert_eq!(tile.z.as_ref().unwrap()[15 * 60 + 30], 7);
    assert_eq!(content(&back), content(&t));
    // The raw header's unused fields survive the rebuild.
    assert_eq!(&tile.raw[49..52], &[0xCD; 3]);
    assert_eq!(&tile.raw[20..36], &[0xCD; 16]);
}

#[test]
fn a_damaged_slot_is_kept_in_place_and_refused_by_a_rebuild() {
    let mut bytes = messy_file();
    // Slot 1 points past the end of the file.
    let len = bytes.len() as i32;
    bytes[20..24].copy_from_slice(&(len + 100).to_le_bytes());
    let mut t = parse(&bytes).unwrap();
    assert_eq!(t.unreadable, vec![1]);
    assert_eq!(build(&t, Some(&bytes)).unwrap(), bytes);
    t.tiles[2].as_mut().unwrap().z = Some(vec![Z_NONE; 60 * 30]);
    assert_eq!(build(&t, Some(&bytes)), Err(Error::Unreadable(1)));
}

#[test]
fn sniff_refuses_what_is_not_a_template() {
    let bytes = build(&template(48, 24, false, 1), None).unwrap();
    assert!(sniff(&bytes));
    assert!(!sniff(&bytes[..20]), "the offset table is cut");
    let mut wrong = bytes.clone();
    wrong[8..12].copy_from_slice(&50i32.to_le_bytes());
    assert!(!sniff(&wrong), "an unknown tile size");
    let mut wrong = bytes.clone();
    wrong[16..20].copy_from_slice(&20i32.to_le_bytes());
    assert!(!sniff(&wrong), "an offset inside the table");
    let mut empty = bytes[..32].to_vec();
    empty[16..32].fill(0);
    assert!(!sniff(&empty), "no tile at all");
    // An SHP's header (a zero word, then width, height, frames).
    let mut shp = vec![0u8, 0, 60, 0, 30, 0, 1, 0];
    shp.resize(200, 0);
    assert!(!sniff(&shp));
    assert_eq!(parse(&shp), Err(Error::BadHeader));
}

#[test]
fn labels_name_land_and_ramp_types() {
    assert_eq!(land_label(0), "Clear");
    assert_eq!(land_label(13), "Clear");
    assert_eq!(land_label(15), "Cliff");
    assert_eq!(land_label(200), "Unknown");
    assert_eq!(ramp_label(0), "None");
    assert_eq!(ramp_label(20), "Double down NW-SE");
    assert_eq!(ramp_label(21), "Unknown");
}
