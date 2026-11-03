// SPDX-License-Identifier: GPL-3.0-or-later
//! Save As SHP of a TMP template. Every template and palette here is
//! generated.

use super::super::tests::{choice, doc};
use super::super::tmp_tests::{bytes, opened_tmp, CX, CY};
use super::tmp_shp::*;
use super::*;
use crate::cxxqt_object::open_jobs::apply_opened;
use pictura_shp::tmp::{diamond_rows, in_diamond};

fn colour(i: u8) -> [u8; 4] {
    let c = unique_colours(&choice().palette).argb[usize::from(i)];
    [(c >> 16) as u8, (c >> 8) as u8, c as u8, 255]
}

fn px(frame: &[u8], w: usize, x: usize, y: usize) -> [u8; 4] {
    let i = (y * w + x) * 4;
    [frame[i], frame[i + 1], frame[i + 2], frame[i + 3]]
}

fn limits() -> Limits {
    Limits {
        limit: 1 << 30,
        available: None,
    }
}

#[test]
fn one_frame_per_tile_holds_its_diamond() {
    let rust = opened_tmp(bytes());
    let (w, h, frames) = template_frames(doc(&rust), true, false).expect("frames");
    assert_eq!((w, h, frames.len()), (CX, CY, 2));
    for (frame, index) in frames.iter().zip([44u8, 48]) {
        for y in 0..CY {
            for x in 0..CX {
                let want = if in_diamond(CX, CY, x, y) {
                    colour(index)
                } else {
                    [0; 4]
                };
                assert_eq!(px(frame, w, x, y), want, "({x}, {y})");
            }
        }
    }
}

#[test]
fn extra_blocks_grow_every_frame_and_draw_over_the_diamond() {
    let rust = opened_tmp(bytes());
    // Tile 3's extra block sits 20 px above its tile (at 20, 10; the tile at
    // 0, 30), 12 x 14.
    let (w, h, frames) = template_frames(doc(&rust), true, true).expect("frames");
    assert_eq!((w, h), (CX, CY + 20));
    let (first, second) = (&frames[0], &frames[1]);
    // Tile 0 has no extra: its diamond, 20 px down.
    assert_eq!(px(first, w, 30, 20), colour(44));
    assert_eq!(px(first, w, 30, 5), [0; 4]);
    // Tile 3: its extra over the diamond (index 60, every fifth pixel 0).
    assert_eq!(px(second, w, 21, 0), colour(60));
    assert_eq!(px(second, w, 20, 0), [0; 4], "index 0 in the extra");
    let (y, x, _) = diamond_rows(CX, CY)[14];
    assert_eq!(px(second, w, x + 1, y + 20), colour(48));
}

#[test]
fn one_frame_of_the_whole_template_is_its_canvas() {
    let rust = opened_tmp(bytes());
    let d = doc(&rust);
    let (w, h, frames) = template_frames(d, false, true).expect("frames");
    assert_eq!(
        (w, h, frames.len()),
        (d.width as usize, d.height as usize, 1)
    );
    assert_eq!(px(&frames[0], w, 30, 1), colour(44));
    assert_eq!(px(&frames[0], w, 30, 45), colour(48));
    assert_eq!(px(&frames[0], w, 21, 10), colour(60), "the extra");
    let (_, _, without) = template_frames(d, false, false).expect("frames");
    assert_ne!(px(&without[0], w, 21, 10), colour(60));
}

#[test]
fn the_shp_document_keeps_indices_and_the_template_is_unchanged() {
    let rust = opened_tmp(bytes());
    let before = doc(&rust).clone();
    let (opened, layout) =
        template_to_sheet(doc(&rust), "clear01", true, false, false, &limits(), 0).expect("made");
    assert_eq!((layout.frames, layout.width, layout.height), (2, CX, CY));
    let mut sheet = PictureViewRust::default();
    apply_opened(&mut sheet, opened);
    assert_eq!(sheet.source_format, "shp");
    let shp = encode(doc(&sheet)).expect("an SHP saves");
    let sprite = pictura_shp::parse(&shp).expect("parses");
    assert_eq!(sprite.frames.len(), 2);
    let plane = frame_canvas(&shp, 1);
    let (y, x, _) = diamond_rows(CX, CY)[10];
    assert_eq!(plane[y * CX + x], 48);
    assert_eq!(plane[0], 0);
    assert_eq!(doc(&rust).image_resources, before.image_resources);
    assert_eq!(encode_template(doc(&rust)).expect("saves"), bytes());
    // Not a template: refused.
    assert!(template_to_sheet(doc(&sheet), "x", true, false, false, &limits(), 0).is_err());
}
