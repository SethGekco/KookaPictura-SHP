// SPDX-License-Identifier: GPL-3.0-or-later
//! New TMP templates from an image through the app glue. Every image and
//! palette here is generated.

use super::super::tests::{choice, doc};
use super::tmp_image::*;
use super::*;
use crate::cxxqt_object::open_jobs::apply_opened;
use pictura_shp::tmp::{diamond_rows, parse};
use pictura_shp::tmp_image::{cell_position, grid_size};

/// A 2 x 2 grid image: cells 0 and 3 painted with `colour`, plus one
/// opaque pixel of `stray` at template (10, 2), outside every diamond.
fn image(colour: [u8; 4], stray: [u8; 4]) -> Document {
    let (w, h) = grid_size(60, 30, 2, 2);
    let mut rgba = vec![0u8; w * h * 4];
    for slot in [0usize, 3] {
        let (x, y) = cell_position(60, 30, slot % 2, slot / 2);
        for (ry, rx, rw) in diamond_rows(60, 30) {
            for k in 0..rw {
                let i = (y as usize + ry) * w + (x + 30) as usize + rx + k;
                rgba[i * 4..i * 4 + 4].copy_from_slice(&colour);
            }
        }
    }
    let i = 2 * w + 40;
    rgba[i * 4..i * 4 + 4].copy_from_slice(&stray);
    Document::from_rgba("art", w as u32, h as u32, &rgba)
}

fn spec(extras: bool) -> pictura_shp::tmp_image::ImageSpec {
    let params = [60, 30, 2, 2, 1, 13, 0, 1, 1, i32::from(extras)];
    image_spec(&params, &[1, 0, 0, 1]).expect("in range").0
}

fn made(source: &Document, extras: bool, nearest: bool) -> PictureViewRust {
    let limits = Limits {
        limit: 1 << 30,
        available: None,
    };
    let (opened, _) = template_from_image(
        source,
        "art",
        &spec(extras),
        &choice(),
        nearest,
        false,
        &limits,
        0,
    )
    .expect("made");
    let mut rust = PictureViewRust::default();
    apply_opened(&mut rust, opened);
    rust
}

fn opaque(c: u32) -> [u8; 4] {
    [(c >> 16) as u8, (c >> 8) as u8, c as u8, 255]
}

#[test]
fn an_image_becomes_a_template_document_that_saves_as_built() {
    let unique = unique_colours(&choice().palette);
    let source = image(opaque(unique.argb[40]), [0; 4]);
    let rust = made(&source, false, true);
    let d = doc(&rust);
    assert_eq!(rust.source_format, "tmp");
    let names: Vec<&str> = d.layers.iter().map(|l| l.name.as_str()).collect();
    assert_eq!(names, ["Tiles", "Z Tiles"]);
    assert_eq!((d.width, d.height), (60, 60));
    let resource = tmp_resource(d).expect("a TMP document");
    assert_eq!(resource.palette_path, "/pal/unittem.pal");
    // Saving it unchanged writes exactly the bytes it was built as.
    assert_eq!(encode_template(d).expect("saves"), resource.original);
    let t = parse(&resource.original).unwrap();
    assert_eq!((t.blocks_x, t.blocks_y, t.cx, t.cy), (2, 2, 60, 30));
    let live: Vec<usize> = t.live().map(|(s, _)| s).collect();
    assert_eq!(live, [0, 3]);
    let t3 = t.tiles[3].as_ref().unwrap();
    assert_eq!((t3.x, t3.y), (0, 30));
    assert_eq!((t3.fields.height, t3.fields.land), (1, 13));
    let p = choice().palette.argb[40];
    assert_eq!(
        t3.fields.radar_low,
        [(p >> 16) as u8, (p >> 8) as u8, p as u8]
    );
    assert!(t3.z.is_some(), "flat z-data was asked for");
    let tiles = tiles_of(d);
    assert_eq!(tiles.len(), 2);
}

#[test]
fn exact_colours_leave_off_palette_pixels_for_the_gate() {
    let source = image([1, 2, 3, 200], [0; 4]);
    let exact = made(&source, false, false);
    let (_, _, scan) = scan_document(doc(&exact)).expect("scans");
    assert_eq!(scan.off_palette, 2 * 900);
    assert!(encode_template(doc(&exact)).is_err(), "Save refuses");
    let nearest = made(&source, false, true);
    let (_, _, scan) = scan_document(doc(&nearest)).expect("scans");
    assert_eq!(scan.off_palette, 0, "mapped to the nearest index");
}

#[test]
fn a_stray_pixel_is_clipped_at_save_unless_it_becomes_an_extra() {
    let unique = unique_colours(&choice().palette);
    let source = image(opaque(unique.argb[40]), opaque(unique.argb[50]));
    let kept = made(&source, false, true);
    let (_, _, scan) = scan_document(doc(&kept)).expect("scans");
    assert_eq!(scan.outside, 1, "the gate's Clip finds it");

    let extra = made(&source, true, true);
    let d = doc(&extra);
    let names: Vec<&str> = d.layers.iter().map(|l| l.name.as_str()).collect();
    assert_eq!(names, ["Tiles", "Extras", "Z Tiles", "Z Extras"]);
    let (_, _, scan) = scan_document(d).expect("scans");
    assert_eq!(scan.outside, 0);
    let t = parse(&encode_template(d).expect("saves")).unwrap();
    let with_extra: Vec<(usize, i32, i32, usize, usize, Option<u8>)> = t
        .live()
        .filter(|(_, tile)| tile.extra.is_some())
        .map(|(s, tile)| {
            (
                s,
                tile.extra_x,
                tile.extra_y,
                tile.extra_w,
                tile.extra_h,
                tile.extra.as_ref().map(|e| e[0]),
            )
        })
        .collect();
    assert_eq!(with_extra, [(0, 10, 2, 1, 1, Some(50))]);
    assert!(t.tiles[0].as_ref().unwrap().extra_z.is_none());
}

#[test]
fn the_shell_integers_are_checked() {
    assert!(
        image_spec(&[60, 30, 2, 2, 0, 0, 0, 1, 0], &[1; 4]).is_none(),
        "one short"
    );
    assert!(image_spec(&[60, 30, 2, 2, 300, 0, 0, 1, 0, 0], &[1; 4]).is_none());
    let (s, nearest) = image_spec(&[48, 24, 1, 3, 0, 0, 0, 0, 0, 1], &[0, 1, 0]).unwrap();
    assert_eq!((s.cx, s.cy, s.blocks_x, s.blocks_y), (48, 24, 1, 3));
    assert_eq!(s.cells, [false, true, false]);
    assert!(!nearest && s.extras);
}

/// FNV-1a 64 of `bytes` (the e2e script computes the same over Ochre's
/// arrays).
pub(super) fn fnv(bytes: &[u8]) -> u64 {
    bytes.iter().fold(0xcbf2_9ce4_8422_2325, |h, &b| {
        (h ^ u64::from(b)).wrapping_mul(0x100_0000_01b3)
    })
}

/// One line per tile of `t` as pictura-shp reads it, for the e2e script:
/// slot, x, y, flags, height, land, ramp, radar low and high, extra x, y,
/// w, h, and the FNV-1a of the image, z, extra and extra z (`-` when
/// absent).
pub(super) fn dump(t: &pictura_shp::tmp::Template) -> String {
    let hash =
        |b: Option<&Vec<u8>>| b.map_or_else(|| "-".to_string(), |b| format!("{:016x}", fnv(b)));
    let mut out = format!("{} {} {} {}\n", t.blocks_x, t.blocks_y, t.cx, t.cy);
    for (slot, tile) in t.live() {
        let f = &tile.fields;
        out += &format!(
            "{slot} {} {} {} {} {} {} {:02x}{:02x}{:02x} {:02x}{:02x}{:02x} {} {} {} {} {} {} {} {}\n",
            tile.x,
            tile.y,
            tile.flags,
            f.height,
            f.land,
            f.ramp,
            f.radar_low[0],
            f.radar_low[1],
            f.radar_low[2],
            f.radar_high[0],
            f.radar_high[1],
            f.radar_high[2],
            if tile.extra.is_some() { tile.extra_x } else { 0 },
            if tile.extra.is_some() { tile.extra_y } else { 0 },
            tile.extra_w,
            tile.extra_h,
            hash(Some(&tile.image)),
            hash(tile.z.as_ref()),
            hash(tile.extra.as_ref()),
            hash(tile.extra_z.as_ref()),
        );
    }
    out
}

/// A `w` x `h` image of colour stripes and a transparent band, for the e2e.
fn stripes(w: usize, h: usize, seed: usize) -> Document {
    let mut rgba = vec![0u8; w * h * 4];
    for y in 0..h {
        for x in 0..w {
            let i = (y * w + x) * 4;
            let v = ((x * 7 + y * 3 + seed * 11) % 251) as u8;
            let alpha = if (x + seed) % 23 < 2 { 0 } else { 255 };
            rgba[i..i + 4].copy_from_slice(&[v, v.wrapping_mul(3), 255 - v, alpha]);
        }
    }
    Document::from_rgba("stripes", w as u32, h as u32, &rgba)
}

/// The e2e half of New TMP from Image: writes templates made from images
/// (both games, extras on and off, flat and no z, both colour mappings,
/// clipped like Save would) with what pictura-shp reads in each, for
/// `~/Claude/shp-corpus/tmpedit/tmpedit-e2e.py` to compare with Ochre's
/// reader. `PICTURA_TMP_E2E_DIR=<dir> cargo test ... -- --ignored`.
#[test]
#[ignore]
fn e2e_writes_templates_made_from_images() {
    let Ok(dir) = std::env::var("PICTURA_TMP_E2E_DIR") else {
        return;
    };
    let limits = Limits {
        limit: 1 << 30,
        available: None,
    };
    let cases: [(&str, [i32; 10], usize, usize); 6] = [
        ("ra2-1x1-flat", [60, 30, 1, 1, 0, 0, 0, 1, 1, 0], 60, 30),
        ("ra2-2x2-extras", [60, 30, 2, 2, 0, 0, 0, 1, 1, 1], 120, 90),
        ("ra2-3x2-noz", [60, 30, 3, 2, 2, 13, 0, 1, 0, 1], 150, 75),
        ("ts-2x1-flat", [48, 24, 2, 1, 0, 0, 0, 1, 1, 0], 72, 36),
        ("ts-2x3-extras", [48, 24, 2, 3, 1, 15, 3, 1, 1, 1], 120, 80),
        ("ra2-4x4-exact", [60, 30, 4, 4, 0, 0, 0, 0, 1, 1], 240, 120),
    ];
    for (k, (name, params, w, h)) in cases.into_iter().enumerate() {
        let source = stripes(w, h, k);
        let rgba = super::tmp_image::image_cells_of(&source, &params);
        let (spec, nearest) = image_spec(&params, &rgba).expect("in range");
        let (opened, _) =
            template_from_image(&source, name, &spec, &choice(), nearest, false, &limits, 0)
                .expect("made");
        let mut rust = PictureViewRust::default();
        apply_opened(&mut rust, opened);
        // What the save gate's fixes would do, then Save.
        snap_template_rust(&mut rust).expect("snaps");
        clip_template_rust(&mut rust).expect("clips");
        let bytes = encode_template(doc(&rust)).expect("saves");
        let t = parse(&bytes).expect("parses");
        std::fs::write(format!("{dir}/{name}.tem"), &bytes).unwrap();
        std::fs::write(format!("{dir}/{name}.txt"), dump(&t)).unwrap();
    }
}
