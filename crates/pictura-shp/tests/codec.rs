//! Sniffing and decoding: the format rules and the real-file quirks.

mod common;

use common::{assemble, header, table, Rng};
use pictura_shp::{
    build_frames, build_preserving, decode_rle_line, encode_prefixed, encode_rle, encode_rle_line,
    parse, sniff, Error, Source,
};

fn rle_row(row: &[u8]) -> Vec<u8> {
    let mut out = Vec::new();
    encode_rle_line(row, &mut out);
    out
}

#[test]
fn rle_transparent_runs_clamp_at_255() {
    assert_eq!(
        rle_row(&[0u8; 600]),
        vec![8, 0, 0, 255, 0, 255, 0, 90],
        "a 600-wide empty row is three runs, not one"
    );
    assert_eq!(rle_row(&[0u8; 255]), vec![4, 0, 0, 255]);
    assert_eq!(rle_row(&[0u8; 256]), vec![6, 0, 0, 255, 0, 1]);
}

#[test]
fn rle_line_length_counts_itself() {
    assert_eq!(rle_row(&[5, 0, 0, 7]), vec![6, 0, 5, 0, 2, 7]);
    assert_eq!(rle_row(&[]), vec![2, 0], "an empty row is just its length");
    let mut row = vec![0u8; 4];
    let next = decode_rle_line(&[6, 0, 5, 0, 2, 7], 0, &mut row);
    assert_eq!(next, 6);
    assert_eq!(row, vec![5, 0, 0, 7]);
}

#[test]
fn rle_never_stores_a_literal_zero() {
    let mut rng = Rng::new(7);
    for _ in 0..200 {
        let w = 1 + rng.below(700) as usize;
        let row: Vec<u8> = (0..w)
            .map(|_| {
                if rng.below(2) == 0 {
                    0
                } else {
                    rng.below(256) as u8
                }
            })
            .collect();
        let line = rle_row(&row);
        // Every 0 in the stream is a run marker followed by a non-zero count.
        let mut i = 2;
        while i < line.len() {
            if line[i] == 0 {
                assert!(line[i + 1] > 0, "a run of zero length");
                i += 2;
            } else {
                i += 1;
            }
        }
        let mut back = vec![0u8; w];
        decode_rle_line(&line, 0, &mut back);
        assert_eq!(back, row);
    }
}

/// One 5x3 plane stored four ways, the flags carrying upper bits.
#[test]
fn all_four_compressions_decode_alike() {
    let plane: Vec<u8> = vec![
        0, 9, 9, 0, 0, //
        3, 0, 0, 0, 4, //
        0, 0, 200, 0, 0,
    ];
    let t = table(4);
    let raw0 = plane.clone();
    let raw1 = plane.clone();
    let pre = encode_prefixed(&plane, 5);
    let rle = encode_rle(&plane, 5);
    let o0 = t;
    let o1 = o0 + raw0.len();
    let o2 = o1 + raw1.len() + 3; // unaligned, with a gap
    let o3 = o2 + pre.len();
    let headers = [
        header(1, 2, 5, 3, 0x0000_0000, o0 as u32),
        header(1, 2, 5, 3, 0x0000_0001, o1 as u32),
        header(1, 2, 5, 3, 0x02A4_0002, o2 as u32),
        header(1, 2, 5, 3, 0x02A4_0003, o3 as u32),
    ];
    let file = assemble(
        8,
        6,
        &headers,
        &[(o0, raw0), (o1, raw1), (o2, pre), (o3, rle)],
        0xAB,
        &[],
    );
    assert!(sniff(&file, Some(file.len())));
    let sprite = parse(&file).expect("parses");
    for (i, frame) in sprite.frames.iter().enumerate() {
        assert_eq!(frame.pixels.as_deref(), Some(&plane[..]), "frame {i}");
    }
    assert_eq!(sprite.canvas(3)[2 * 8 + 2], 9);
    assert_eq!(sprite.canvas(3)[0], 0);
}

#[test]
fn sniff_validates_the_whole_frame_table() {
    let t = table(1) as u32;
    let ok = |h: pictura_shp::FrameHeader, body: usize| {
        assemble(8, 8, &[h], &[(t as usize, vec![1u8; body])], 0, &[])
    };
    let good = ok(header(0, 0, 4, 4, 0, t), 16);
    assert!(sniff(&good, Some(good.len())));
    assert!(sniff(&good, None), "a stream needs only the table");
    assert!(
        !sniff(&good[..good.len() - 1], Some(good.len() - 1)),
        "raw data past the end"
    );
    assert!(
        !sniff(&ok(header(5, 0, 4, 4, 0, t), 16), Some(24 + 8 + 16)),
        "rect outside canvas"
    );
    assert!(
        !sniff(&ok(header(0, 0, 4, 4, 4, t), 16), Some(48)),
        "compression 4"
    );
    assert!(
        !sniff(&ok(header(0, 0, 4, 4, 3, t - 2), 16), Some(48)),
        "offset inside the table"
    );
    assert!(
        !sniff(&ok(header(0, 0, 4, 4, 3, 999), 16), Some(48)),
        "offset past the end"
    );
    let mut nonzero = good.clone();
    nonzero[0] = 1;
    assert!(
        !sniff(&nonzero, Some(nonzero.len())),
        "the leading word must be zero"
    );
    let mut big = good.clone();
    big[2..4].copy_from_slice(&4097u16.to_le_bytes());
    assert!(!sniff(&big, Some(big.len())), "canvas over 4096");
    assert!(!sniff(&[0, 0, 8, 0, 8, 0, 0, 0], Some(8)), "no frames");
    assert!(!sniff(&good[..20], None), "a truncated table");
}

#[test]
fn a_blank_sprite_is_claimed_only_in_its_real_shape() {
    let blank = assemble(8, 8, &[header(0, 0, 0, 0, 0, 0); 2], &[], 0, &[]);
    assert!(
        sniff(&blank, Some(blank.len())),
        "table only, every offset 0"
    );
    let mut trailing = blank.clone();
    trailing.push(0);
    assert!(
        !sniff(&trailing, Some(trailing.len())),
        "bytes after an all-empty table"
    );
    let pointing = assemble(8, 8, &[header(0, 0, 0, 0, 0, 56)], &[], 0, &[]);
    assert!(
        !sniff(&pointing, Some(pointing.len())),
        "an empty table with a data offset"
    );
    let sprite = parse(&blank).expect("blank parses");
    assert!(sprite.frames.iter().all(|f| f.pixels.is_none()));
}

#[test]
fn empty_frames_with_a_non_zero_offset_are_empty() {
    // Real files point empty frames at the end of the data and still give
    // them flags and a radar colour; nothing reads any of it.
    let plane = vec![7u8; 4];
    let t = table(3);
    let end = t + plane.len();
    let headers = [
        header(0, 0, 2, 2, 0, t as u32),
        header(3, 1, 0, 0, 0x0000_0003, end as u32),
        header(0, 0, 5, 0, 0x0000_0001, end as u32),
    ];
    let file = assemble(4, 4, &headers, &[(t, plane)], 0, &[]);
    assert!(sniff(&file, Some(file.len())));
    let source = Source::read(file.clone()).expect("reads");
    assert!(source.canvas(1).iter().all(|&v| v == 0));
    assert!(source.payload(1).is_empty());
    let out = build_frames(4, 4, &source.canvases(), Some(&source)).expect("builds");
    assert_eq!(
        out, file,
        "empty headers come back verbatim, odd w/h and all"
    );
}

#[test]
fn unaligned_offsets_garbage_gaps_and_tails_survive() {
    let mut rng = Rng::new(3);
    let a = common::sprite_plane(&mut rng, 6, 5);
    let b = common::sprite_plane(&mut rng, 3, 7);
    let ra = encode_rle(&a, 6);
    let t = table(2);
    let oa = t + 5; // a garbage gap after the table
    let ob = oa + ra.len() + 3; // and between the frames
    let headers = [
        header(1, 1, 6, 5, 0x0000_0003, oa as u32),
        header(0, 2, 3, 7, 0x0000_0001, ob as u32),
    ];
    let file = assemble(9, 9, &headers, &[(oa, ra), (ob, b)], 0x1E, b"\x0f\x0f\x00");
    let source = Source::read(file.clone()).expect("reads");
    assert_eq!(source.pad(0), &[0x1E; 5]);
    assert_eq!(source.pad(1), &[0x1E; 3]);
    assert_eq!(source.tail(), b"\x0f\x0f\x00");
    let planes = source.canvases();
    assert_eq!(
        build_preserving(9, 9, &planes, Some(&source)).unwrap(),
        file
    );
    assert_eq!(
        build_frames(9, 9, &planes, Some(&source)).unwrap(),
        file,
        "the frame-by-frame writer alone reproduces the file too"
    );
}

#[test]
fn shared_offsets_stay_shared() {
    let plane = vec![4u8, 0, 4, 4];
    let t = table(3);
    let rle = encode_rle(&plane, 2);
    let ob = t + rle.len();
    let headers = [
        header(0, 0, 2, 2, 3, t as u32),
        header(1, 1, 1, 1, 0, ob as u32),
        header(0, 0, 2, 2, 3, t as u32),
    ];
    let file = assemble(3, 3, &headers, &[(t, rle), (ob, vec![9])], 0, &[]);
    let source = Source::read(file.clone()).expect("reads");
    let mut planes = source.canvases();
    assert_eq!(build_frames(3, 3, &planes, Some(&source)).unwrap(), file);
    // An edit elsewhere still keeps the two frames on one block.
    planes[1][4] = 8;
    let out = build_frames(3, 3, &planes, Some(&source)).unwrap();
    assert_eq!(common::header_bytes(&out, 0), common::header_bytes(&out, 2));
    let back = parse(&out).unwrap();
    assert_eq!(back.canvas(1)[4], 8);
}

#[test]
fn truncated_lines_and_over_long_runs_decode_leniently() {
    // Row 0: a run declared longer than the row; row 1: a line length of 0
    // (empty); row 2: a literal then a run marker with no count, cut by EOF.
    let t = table(1);
    let lines: Vec<u8> = vec![
        6, 0, 5, 0, 200, 6, // 5, then 200 transparent (clamped), then 6 is past the row
        0, 0, // length < 2: an empty line
        5, 0, 7, 0, // declares 5 bytes, file ends after 4
    ];
    let file = assemble(
        4,
        3,
        &[header(0, 0, 4, 3, 3, t as u32)],
        &[(t, lines)],
        0,
        &[],
    );
    assert!(sniff(&file, Some(file.len())));
    let sprite = parse(&file).expect("lenient");
    assert_eq!(
        sprite.frames[0].pixels.as_deref(),
        Some(&[5, 0, 0, 0, 0, 0, 0, 0, 7, 0, 0, 0][..])
    );
    let source = Source::read(file.clone()).unwrap();
    assert_eq!(
        source.payload(0).len(),
        file.len() - t,
        "the span stops at EOF"
    );
    assert_eq!(
        build_frames(4, 3, &source.canvases(), Some(&source)).unwrap(),
        file
    );
}

#[test]
fn prefixed_lines_longer_than_the_row_keep_their_bytes() {
    let t = table(1);
    let lines: Vec<u8> = vec![5, 0, 1, 2, 3, 4, 0, 4, 5]; // 3 bytes for a 2-wide row
    let file = assemble(
        2,
        2,
        &[header(0, 0, 2, 2, 2, t as u32)],
        &[(t, lines)],
        0,
        &[],
    );
    let source = Source::read(file.clone()).unwrap();
    assert_eq!(source.canvas(0), vec![1, 2, 4, 5]);
    assert_eq!(
        build_preserving(2, 2, &source.canvases(), Some(&source)).unwrap(),
        file
    );
}

#[test]
fn not_an_shp_is_an_error() {
    assert_eq!(parse(b"\x89PNG\r\n\x1a\n").unwrap_err(), Error::NotShp);
    assert!(Source::read(vec![0; 4]).is_err());
}
