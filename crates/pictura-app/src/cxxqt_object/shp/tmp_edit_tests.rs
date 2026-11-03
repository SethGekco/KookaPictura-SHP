// SPDX-License-Identifier: GPL-3.0-or-later
//! Reshaping TMP templates from the Tiles panel. Every template and palette
//! here is generated; the ignored e2e test reads a copy of a real file only
//! when it is pointed at one.

use super::super::tests::{choice, doc};
use super::super::tmp_tests::{bytes, opened_tmp, CX, CY};
use super::tmp_edit::*;
use super::*;
use crate::history::History;
use pictura_shp::tmp::{parse, Tile, FLAG_EXTRA, FLAG_Z};

fn limits() -> Limits {
    Limits {
        limit: 1 << 30,
        available: None,
    }
}

fn color(i: u8) -> [u8; 4] {
    let c = unique_colours(&choice().palette).argb[usize::from(i)];
    [(c >> 16) as u8, (c >> 8) as u8, c as u8, 255]
}

/// Pixel (x, y) of the layer named `name`.
fn pixel(rust: &PictureViewRust, name: &str, x: usize, y: usize) -> [u8; 4] {
    let d = doc(rust);
    let (_, _, p) = template_of(d).expect("a template");
    let layers = read_layers(d, &p).expect("layers");
    let k = p
        .layers()
        .iter()
        .position(|s| s.name == name)
        .expect("that layer");
    let i = (y * p.width + x) * 4;
    layers[k][i..i + 4].try_into().unwrap()
}

/// Apply and record one edit; the dropped count.
fn apply(rust: &mut PictureViewRust, edit: TileEdit) -> usize {
    let dropped = edit_tiles_rust(rust, edit, &limits(), 0).expect("the edit applies");
    rust.apply_edit(edit.label(), Edit::Full);
    dropped
}

fn undo(rust: &mut PictureViewRust) {
    assert!(rust
        .session
        .restore(&mut rust.layer_target, History::undo_live, History::undo)
        .is_some());
}

fn tile(t: &pictura_shp::tmp::Template, slot: usize) -> &Tile {
    t.tiles[slot].as_ref().expect("a tile there")
}

#[test]
fn a_moved_tile_is_redrawn_in_its_cell_and_saves_in_place() {
    let mut rust = opened_tmp(bytes());
    let before = doc(&rust).clone();
    // Tile 0 (index 44, z 5) to cell (1, 0): half a tile right and down.
    let edit = TileEdit::Move {
        slot: 0,
        col: 1,
        row: 0,
    };
    assert_eq!(apply(&mut rust, edit), 0);
    let d = doc(&rust);
    let (_, t, p) = template_of(d).expect("a template");
    assert!(t.tiles[0].is_none());
    let moved = tile(&t, 1);
    assert_eq!((moved.x, moved.y), (30, 15));
    assert_eq!((d.width as usize, d.height as usize), (p.width, p.height));
    let (ox, oy) = p.tile_origin(moved);
    assert_eq!(pixel(&rust, "Tiles", ox + 30, oy + 15), color(44));
    assert_eq!(pixel(&rust, "Z Tiles", ox + 30, oy + 15), [40, 40, 40, 255]);
    let rows = tiles_of(doc(&rust));
    assert_eq!(rows.iter().map(|r| r.slot).collect::<Vec<_>>(), [1, 3]);

    let (saved, layout) = encode_template_with_layout(doc(&rust)).expect("saves");
    assert_eq!(layout, pictura_shp::tmp::Layout::InPlace);
    assert_eq!(save_layout(doc(&rust)), "in place");
    let original = bytes();
    assert_eq!(saved.len(), original.len());
    let record = parse(&original).unwrap().tiles[0].as_ref().unwrap().offset;
    let differ: Vec<usize> = (0..saved.len())
        .filter(|&i| saved[i] != original[i])
        .collect();
    assert!(
        differ
            .iter()
            .all(|&i| (16..24).contains(&i) || (record..record + 8).contains(&i)),
        "only the slots and the header position: {differ:?}"
    );
    let back = parse(&saved).unwrap();
    let was = parse(&original).unwrap();
    assert_eq!(tile(&back, 1).image, tile(&was, 0).image);
    assert_eq!(tile(&back, 1).z, tile(&was, 0).z);

    undo(&mut rust);
    assert_eq!(doc(&rust).image_resources, before.image_resources);
    assert_eq!(
        (doc(&rust).width, doc(&rust).height),
        (before.width, before.height)
    );
    assert_eq!(encode_template(doc(&rust)).expect("saves"), bytes());
}

#[test]
fn a_moved_tile_takes_its_extra_block_and_depths_along() {
    let mut rust = opened_tmp(bytes());
    // Tile 3 (extra at 20, 10) to cell (1, 0): half a tile right and up.
    apply(
        &mut rust,
        TileEdit::Move {
            slot: 3,
            col: 1,
            row: 0,
        },
    );
    let (_, t, p) = template_of(doc(&rust)).expect("a template");
    let moved = tile(&t, 1);
    assert_eq!(
        (moved.x, moved.y, moved.extra_x, moved.extra_y),
        (30, 15, 50, -5)
    );
    let (ex, ey) = p.extra_origin(moved);
    assert_eq!(pixel(&rust, "Extras", ex + 1, ey), color(60));
    assert_eq!(pixel(&rust, "Z Extras", ex + 1, ey), [96, 96, 96, 255]);
    let saved = encode_template(doc(&rust)).expect("saves");
    let back = parse(&saved).unwrap();
    let was = parse(&bytes()).unwrap();
    let (a, b) = (tile(&back, 1), tile(&was, 3));
    assert_eq!(
        (&a.extra, &a.extra_z, a.fields),
        (&b.extra, &b.extra_z, b.fields)
    );
    assert_eq!((a.extra_x, a.extra_y), (50, -5));
}

#[test]
fn tiles_are_added_blank_and_removed_and_both_undo() {
    let mut rust = opened_tmp(bytes());
    let before = doc(&rust).clone();
    // A new tile in cell (0, 1): half a tile left and down of tile 0.
    apply(&mut rust, TileEdit::Add { col: 0, row: 1 });
    let (_, t, p) = template_of(doc(&rust)).expect("a template");
    let new = tile(&t, 2);
    assert_eq!((new.x, new.y, new.offset), (-30, 15, 0));
    assert!(new.z.is_none() && new.extra.is_none());
    let (ox, oy) = p.tile_origin(new);
    assert_eq!(pixel(&rust, "Tiles", ox + 30, oy + 15), [0; 4], "blank");
    let (saved, layout) = encode_template_with_layout(doc(&rust)).expect("saves");
    assert_eq!(layout, pictura_shp::tmp::Layout::Appended);
    let original = bytes();
    assert_eq!(saved.len(), original.len() + 52 + CX * CY / 2);
    let back = parse(&saved).unwrap();
    assert_eq!(tile(&back, 2).flags & (FLAG_Z | FLAG_EXTRA), 0);
    assert_eq!(
        tile(&back, 0).image,
        tile(&parse(&original).unwrap(), 0).image
    );

    apply(&mut rust, TileEdit::Remove { slot: 0 });
    let (_, t, _) = template_of(doc(&rust)).expect("a template");
    assert_eq!(t.live().map(|(s, _)| s).collect::<Vec<_>>(), [2, 3]);
    let back = parse(&encode_template(doc(&rust)).expect("saves")).unwrap();
    assert!(back.tiles[0].is_none() && back.tiles[2].is_some());

    undo(&mut rust);
    undo(&mut rust);
    assert_eq!(doc(&rust).image_resources, before.image_resources);
    assert_eq!(encode_template(doc(&rust)).expect("saves"), bytes());
}

#[test]
fn extra_blocks_are_added_without_z_data_and_removed() {
    let mut rust = opened_tmp(bytes());
    let before = doc(&rust).clone();
    // Tile 0 has z-data; its new extra block is a tile-sized block above it.
    apply(
        &mut rust,
        TileEdit::AddExtra {
            slot: 0,
            dx: 0,
            dy: -(CY as i32),
            w: CX,
            h: CY,
        },
    );
    let rows = tiles_of(doc(&rust));
    assert!(rows[0].has_extra && !rows[0].has_extra_z);
    apply(&mut rust, TileEdit::RemoveExtra { slot: 3 });
    let (saved, layout) = encode_template_with_layout(doc(&rust)).expect("saves");
    assert_eq!(layout, pictura_shp::tmp::Layout::Appended);
    let back = parse(&saved).unwrap();
    let t0 = tile(&back, 0);
    assert_eq!(t0.flags & (FLAG_Z | FLAG_EXTRA), FLAG_Z | FLAG_EXTRA);
    assert_eq!(
        (t0.extra_x, t0.extra_y, t0.extra_w, t0.extra_h),
        (0, -30, CX, CY)
    );
    assert_eq!(t0.extra.as_deref(), Some(&vec![0u8; CX * CY][..]));
    assert!(t0.extra_z.is_none(), "added extras get no z-data");
    let t3 = tile(&back, 3);
    assert!(t3.extra.is_none() && t3.flags & FLAG_EXTRA == 0);
    assert!(t3.z.is_some(), "the tile keeps its own z-data");
    // Removing the extra dropped its pixels and layers.
    let names: Vec<String> = doc(&rust).layers.iter().map(|l| l.name.clone()).collect();
    assert_eq!(names, ["Tiles", "Extras", "Z Tiles", "Z Extras"]);

    undo(&mut rust);
    undo(&mut rust);
    assert_eq!(doc(&rust).image_resources, before.image_resources);
    assert_eq!(encode_template(doc(&rust)).expect("saves"), bytes());
}

#[test]
fn what_cannot_be_done_is_refused_and_changes_nothing() {
    let mut rust = opened_tmp(bytes());
    let before = doc(&rust).clone();
    let refused = |rust: &mut PictureViewRust, edit: TileEdit, why: &str| {
        let e = edit_tiles_rust(rust, edit, &limits(), 0).expect_err(why);
        assert!(e.contains(why), "{e}");
    };
    let mv = |slot, col, row| TileEdit::Move { slot, col, row };
    refused(&mut rust, mv(0, 0, 1 + 1), "outside the 2 x 2");
    refused(&mut rust, mv(0, 1, 1), "already holds tile 3");
    refused(&mut rust, mv(1, 0, 1), "no tile in slot 1");
    refused(&mut rust, TileEdit::Add { col: 1, row: 1 }, "already holds");
    let extra = |w| TileEdit::AddExtra {
        slot: 3,
        dx: 0,
        dy: 0,
        w,
        h: 4,
    };
    refused(&mut rust, extra(4), "already has an extra block");
    refused(
        &mut rust,
        TileEdit::RemoveExtra { slot: 0 },
        "no extra block",
    );
    refused(
        &mut rust,
        TileEdit::AddExtra {
            slot: 0,
            dx: 0,
            dy: 0,
            w: 0,
            h: 4,
        },
        "a side",
    );
    apply(&mut rust, TileEdit::Remove { slot: 3 });
    refused(&mut rust, TileEdit::Remove { slot: 0 }, "at least one tile");
    undo(&mut rust);
    assert_eq!(doc(&rust).image_resources, before.image_resources);
    // A canvas that grows past the size limit is refused.
    let tight = Limits {
        limit: 1,
        available: None,
    };
    let grow = TileEdit::Add { col: 1, row: 0 };
    assert!(edit_tiles_rust(&mut rust, grow, &tight, 0).is_err());
    assert_eq!(doc(&rust).image_resources, before.image_resources);
    assert_eq!(TileEdit::from_ints(9, 0, &[]), None);
    assert_eq!(
        TileEdit::from_ints(3, 2, &[1, -2, 3, 4]),
        Some(TileEdit::AddExtra {
            slot: 2,
            dx: 1,
            dy: -2,
            w: 3,
            h: 4
        })
    );
}

#[test]
fn pixels_outside_the_tiles_move_with_the_canvas() {
    let mut rust = opened_tmp(bytes());
    // A stray pixel at canvas (2, 58) on Tiles, outside every diamond.
    clip_template_rust(&mut rust).expect("clips");
    {
        let doc = rust.session.doc.as_mut().unwrap();
        let (_, _, p) = template_of(doc).unwrap();
        let mut layers = read_layers(doc, &p).unwrap();
        let i = (58 * p.width + 2) * 4;
        layers[0][i..i + 4].copy_from_slice(&color(70));
        let (w, h) = (doc.width, doc.height);
        store_canvas_rgba(&mut doc.layers[0], w, h, &layers[0]);
    }
    let (_, _, p) = template_of(doc(&rust)).unwrap();
    // Tile 0 to cell (0, 1) puts a diamond left of x 0: the origin moves.
    let dropped = apply(
        &mut rust,
        TileEdit::Move {
            slot: 0,
            col: 0,
            row: 1,
        },
    );
    assert_eq!(dropped, 0);
    let (_, _, np) = template_of(doc(&rust)).unwrap();
    let (sx, sy) = (np.origin_x - p.origin_x, np.origin_y - p.origin_y);
    assert_eq!((sx, sy), (30, -10));
    let (x, y) = ((2 + sx) as usize, (58 + sy) as usize);
    assert_eq!(pixel(&rust, "Tiles", x, y), color(70));
    let (_, _, scan) = scan_document(doc(&rust)).unwrap();
    assert_eq!(scan.outside, 1, "still for the gate's Clip");
}

/// The real-file half of the Tiles panel edits: opens a copy of a real
/// template, moves one tile to an empty cell, gives another tile an extra
/// block, saves, reopens the saved bytes and checks the document, then
/// writes `original.tem`, `edited.tem` and `edit.json` for
/// `~/Claude/shp-corpus/tmpedit/tmpedit-e2e.py edit` to compare in Ochre's
/// reader. `PICTURA_TMP_EDIT_SRC=<copy.tem> PICTURA_TMP_EDIT_DIR=<dir>
/// cargo test ... -- --ignored`.
#[test]
#[ignore]
fn e2e_edits_a_real_template() {
    let (Ok(src), Ok(dir)) = (
        std::env::var("PICTURA_TMP_EDIT_SRC"),
        std::env::var("PICTURA_TMP_EDIT_DIR"),
    ) else {
        return;
    };
    let original = std::fs::read(&src).expect("the copy reads");
    let mut rust = opened_tmp(original.clone());
    let (_, t, _) = template_of(doc(&rust)).expect("a template");
    let bx = t.blocks_x;
    let empty = (0..t.tiles.len())
        .find(|&s| t.tiles[s].is_none())
        .expect("an empty cell");
    let live: Vec<usize> = t.live().map(|(s, _)| s).collect();
    let from = *live.last().unwrap();
    let with_extra = *live
        .iter()
        .find(|&&s| s != from && t.tiles[s].as_ref().unwrap().extra.is_none())
        .expect("a tile without an extra block");
    let (cx, cy) = (t.cx as i32, t.cy as i32);
    let step = |a: usize, b: usize| {
        let (dc, dr) = (
            (b % bx) as i32 - (a % bx) as i32,
            (b / bx) as i32 - (a / bx) as i32,
        );
        ((dc - dr) * cx / 2, (dc + dr) * cy / 2)
    };
    let delta = step(from, empty);
    apply(
        &mut rust,
        TileEdit::Move {
            slot: from,
            col: empty % bx,
            row: empty / bx,
        },
    );
    apply(
        &mut rust,
        TileEdit::AddExtra {
            slot: with_extra,
            dx: 0,
            dy: -cy,
            w: t.cx,
            h: t.cy,
        },
    );
    let (saved, layout) = encode_template_with_layout(doc(&rust)).expect("saves");
    // Reopened, the document shows the tile in its new cell and the extra.
    let reopened = opened_tmp(saved.clone());
    let rows = tiles_of(doc(&reopened));
    assert!(rows.iter().any(|r| r.slot as usize == empty));
    assert!(!rows.iter().any(|r| r.slot as usize == from));
    assert!(rows
        .iter()
        .any(|r| r.slot as usize == with_extra && r.has_extra));
    assert_eq!(encode_template(doc(&reopened)).expect("saves"), saved);
    let w = t.tiles[with_extra].as_ref().unwrap();
    std::fs::write(format!("{dir}/original.tem"), &original).unwrap();
    std::fs::write(format!("{dir}/edited.tem"), &saved).unwrap();
    std::fs::write(
        format!("{dir}/edit.json"),
        format!(
            "{{\"moved\": {{\"{empty}\": {from}}}, \"delta\": [{}, {}], \"extra\": {{\"{with_extra}\": [{}, {}, {}, {}]}}, \"layout\": \"{layout:?}\"}}\n",
            delta.0,
            delta.1,
            w.x,
            w.y - cy,
            cx,
            cy
        ),
    )
    .unwrap();
}
