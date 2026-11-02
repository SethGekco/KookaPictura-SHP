// SPDX-License-Identifier: GPL-3.0-or-later
//! Frame operations on whole and paged SHP documents: the reflow, the
//! resource under undo and redo, the size limit, and saved bytes.

use super::tests::{choice, doc, frame_bytes, opened, planes, roomy, sprite, FRAMES, H, W};
use super::window_tests::{head, paged, paint, small};
use super::*;
use crate::history::History;
use pictura_shp::frames::FrameOp;
use pictura_shp::parse;

/// The frames the document would save, decoded.
fn saved(rust: &PictureViewRust) -> Vec<Vec<u8>> {
    let bytes = encode(doc(rust)).expect("saves");
    let sprite = parse(&bytes).expect("parses");
    (0..sprite.frames.len()).map(|i| sprite.canvas(i)).collect()
}

/// `op` on a whole document, committed the way `shp_frame_op` does.
fn whole(rust: &mut PictureViewRust, op: FrameOp) -> usize {
    let focus = whole_op_rust(rust, op, &roomy(), 0).expect("applies");
    rust.apply_edit(op.label(), Edit::Full);
    focus
}

fn undo(rust: &mut PictureViewRust) {
    assert!(rust
        .session
        .restore(&mut rust.layer_target, History::undo_live, History::undo)
        .is_some());
}

fn redo(rust: &mut PictureViewRust) {
    assert!(rust
        .session
        .restore(&mut rust.layer_target, History::redo_live, History::redo)
        .is_some());
}

fn expect(order: &[Option<usize>]) -> Vec<Vec<u8>> {
    let p = planes();
    order
        .iter()
        .map(|o| o.map_or_else(|| vec![0u8; W * H], |k| p[k].clone()))
        .collect()
}

#[test]
fn each_operation_reflows_the_sheet_and_undo_redo_keep_resource_and_pixels_together() {
    let mut rust = opened(sprite(), choice());
    let ops = [
        FrameOp::Insert { at: 3, count: 2 },
        FrameOp::Duplicate { frame: 0 },
        FrameOp::Move { from: 1, to: 9 },
        FrameOp::Reverse { first: 2, count: 5 },
        FrameOp::Delete { first: 4, count: 3 },
    ];
    let mut order: Vec<Option<usize>> = (0..FRAMES).map(Some).collect();
    let mut states = vec![(doc(&rust).clone(), order.clone())];
    for op in ops {
        let step = op.order(order.len()).expect("applies");
        order = step.iter().map(|o| o.and_then(|k| order[k])).collect();
        let focus = whole(&mut rust, op);
        assert_eq!(focus, op.focus(order.len()));
        let h = head(&rust);
        assert_eq!(
            (h.total, h.layout.frames, h.first),
            (order.len(), order.len(), 0)
        );
        let d = doc(&rust);
        assert_eq!(
            (d.width as usize, d.height as usize),
            (h.layout.sheet_width(), h.layout.sheet_height()),
            "{op:?}: the sheet follows the layout"
        );
        let block = block(d).unwrap();
        let origins = resource::origins(block).unwrap();
        assert_eq!(origins, order, "{op:?}: origins");
        assert_eq!(saved(&rust), expect(&order), "{op:?}: saved frames");
        states.push((d.clone(), order.clone()));
    }
    // Undo back to the open, checking every state on the way, then redo.
    for k in (0..states.len() - 1).rev() {
        undo(&mut rust);
        let (want, order) = &states[k];
        assert_eq!(
            doc(&rust).image_resources,
            want.image_resources,
            "undo to {k}"
        );
        assert_eq!(doc(&rust).layers[0].channels, want.layers[0].channels);
        assert_eq!(saved(&rust), expect(order));
    }
    assert_eq!(
        encode(doc(&rust)).unwrap(),
        sprite(),
        "back to the original bytes"
    );
    for (want, order) in states.iter().skip(1) {
        redo(&mut rust);
        assert_eq!(doc(&rust).image_resources, want.image_resources);
        assert_eq!(saved(&rust), expect(order));
    }
}

#[test]
fn a_moved_frame_keeps_its_payload_and_the_others_theirs() {
    let file = sprite();
    let mut rust = opened(file.clone(), choice());
    whole(&mut rust, FrameOp::Move { from: 7, to: 2 });
    let out = encode(doc(&rust)).expect("saves");
    assert_ne!(out, file);
    let order = FrameOp::Move { from: 7, to: 2 }.order(FRAMES).unwrap();
    for (i, from) in order.iter().enumerate() {
        let j = from.unwrap();
        let (was_head, was_data) = frame_bytes(&file, j);
        let (now_head, now_data) = frame_bytes(&out, i);
        assert_eq!(now_data, was_data, "frame {i} keeps frame {j}'s payload");
        assert_eq!(
            &now_head[..20],
            &was_head[..20],
            "frame {i}: rect, flags, radar"
        );
    }
    assert!(out.ends_with(b"TAIL\x01\x02"));
    // Moving it back restores the file exactly.
    whole(&mut rust, FrameOp::Move { from: 2, to: 7 });
    assert_eq!(encode(doc(&rust)).unwrap(), file);
}

#[test]
fn an_off_palette_pixel_travels_with_its_frame() {
    let mut rust = opened(sprite(), choice());
    let l = head(&rust).layout;
    let (x, y) = l.cell_origin(1);
    let sw = doc(&rust).width as usize;
    {
        let d = rust.session.doc.as_mut().unwrap();
        let layer = &mut d.layers[0];
        let alpha = layer.channels.iter_mut().find(|c| c.id == -1).unwrap();
        let mut a = alpha.data.to_vec();
        a[(y + 2) * sw + x + 3] = 99;
        alpha.data = a.into();
    }
    rust.apply_edit("Soft", Edit::Full);
    whole(&mut rust, FrameOp::Insert { at: 0, count: 1 });
    let found = check(doc(&rust));
    assert_eq!(found.off_frames, vec![2], "frame 1 is now frame 2");
}

#[test]
fn growing_past_the_limit_needs_the_allowance_and_a_gutter_pixel_refuses() {
    let mut rust = opened(sprite(), choice());
    let bytes = head(&rust).layout.sheet_bytes();
    let tight = Limits {
        limit: bytes,
        available: None,
    };
    let op = FrameOp::Insert {
        at: FRAMES,
        count: 8,
    };
    let err = whole_op_rust(&mut rust, op, &tight, 0).unwrap_err();
    assert!(err.contains("size limit"), "{err}");
    let plan = frame_op_plan(doc(&rust), op, &tight).unwrap();
    assert!(plan.stage > 0 && plan.needed > bytes);
    assert!(whole_op_rust(&mut rust, op, &tight, plan.needed).is_ok());
    rust.apply_edit("Insert Frames", Edit::Full);
    assert_eq!(head(&rust).total, FRAMES + 8);

    // A pixel in a gutter would be dropped by the reflow: refused.
    let gx = head(&rust).layout.width;
    {
        let d = rust.session.doc.as_mut().unwrap();
        let alpha = d.layers[0]
            .channels
            .iter_mut()
            .find(|c| c.id == -1)
            .unwrap();
        let mut a = alpha.data.to_vec();
        a[gx] = 255;
        alpha.data = a.into();
    }
    let err = whole_op_rust(&mut rust, FrameOp::Duplicate { frame: 0 }, &roomy(), 0).unwrap_err();
    assert!(err.contains("gaps between frames"), "{err}");
    assert!(FrameOp::Delete {
        first: 0,
        count: FRAMES + 8
    }
    .order(FRAMES + 8)
    .is_none());
}

#[test]
fn a_paged_operation_rebuilds_the_window_and_keeps_every_frame() {
    let file = sprite();
    let mut rust = paged(&file);
    assert_eq!(head(&rust).window(), 0..4);
    // An edit on frame 1, then delete frame 0 and move frame 8 to 1.
    paint(&mut rust, 1, 2, 2, 77);
    let mut expected = planes();
    expected[1][2 * W + 2] = 77;
    let focus = paged_op_rust(
        &mut rust,
        FrameOp::Delete { first: 0, count: 1 },
        &small(),
        0,
    )
    .expect("deletes");
    assert_eq!(focus, 0);
    expected.remove(0);
    assert!(rust.session.dirty);
    let h = head(&rust);
    assert_eq!((h.total, h.first, h.layout.frames), (FRAMES - 1, 0, 4));
    assert_eq!(saved(&rust), expected);

    let focus =
        paged_op_rust(&mut rust, FrameOp::Move { from: 7, to: 0 }, &small(), 0).expect("moves");
    let moved = expected.remove(7);
    expected.insert(0, moved);
    assert_eq!(focus, 0);
    assert_eq!(saved(&rust), expected);
    // Frame 1 (the edited one) is still on the sheet; page away so it goes
    // to the frame store, then reverse everything from off the window.
    page_rust(&mut rust, 8, 0, &small(), 0).expect("pages");
    assert_eq!(head(&rust).window(), 8..9);
    let focus = paged_op_rust(
        &mut rust,
        FrameOp::Reverse { first: 0, count: 9 },
        &small(),
        0,
    )
    .expect("reverses");
    expected.reverse();
    assert_eq!(focus, 0);
    assert_eq!(head(&rust).window(), 0..4);
    assert_eq!(saved(&rust), expected);

    // Untouched frames kept their bytes: the copy of file frame 9.
    let out = encode(doc(&rust)).unwrap();
    let at = expected.iter().position(|p| *p == planes()[9]).unwrap();
    assert_eq!(frame_bytes(&out, at).1, frame_bytes(&file, 9).1);
}

#[test]
fn a_paged_operation_refuses_an_off_palette_window() {
    let mut rust = paged(&sprite());
    let l = head(&rust).layout;
    let (x, y) = l.cell_origin(0);
    let sw = doc(&rust).width as usize;
    {
        let d = rust.session.doc.as_mut().unwrap();
        let red = d.layers[0].channels.iter_mut().find(|c| c.id == 0).unwrap();
        let mut r = red.data.to_vec();
        r[(y + 3) * sw + x + 3] ^= 1;
        red.data = r.into();
    }
    let before = doc(&rust).image_resources.clone();
    let err = paged_op_rust(&mut rust, FrameOp::Duplicate { frame: 2 }, &small(), 0).unwrap_err();
    assert!(err.contains("not palette colours"), "{err}");
    assert_eq!(doc(&rust).image_resources, before);
}

#[test]
fn frame_size_moves_every_frame_by_the_anchor_and_undoes() {
    let mut rust = opened(sprite(), choice());
    let before = doc(&rust).clone();
    let (nw, nh) = (W + 2, H - 2);
    // Centred: one column of padding before, one row cropped off the top.
    assert_eq!(
        frame_size_rust(&mut rust, nw as i32, nh as i32, (1, 1), &roomy(), 0),
        Ok(1)
    );
    rust.apply_edit("Frame Size", Edit::Full);
    let header = header(doc(&rust)).expect("still an SHP");
    assert_eq!((header.layout.width, header.layout.height), (nw, nh));
    let frames = saved(&rust);
    assert_eq!(frames.len(), FRAMES);
    for (k, frame) in frames.iter().enumerate() {
        let old = &planes()[k];
        for y in 0..nh {
            assert_eq!(frame[y * nw], 0, "padding column");
            assert_eq!(frame[y * nw + nw - 1], 0, "padding column");
            assert_eq!(
                &frame[y * nw + 1..y * nw + 1 + W],
                &old[(y + 1) * W..(y + 2) * W]
            );
        }
    }
    // The same size again changes nothing; a paged sprite is refused.
    assert_eq!(
        frame_size_rust(&mut rust, nw as i32, nh as i32, (1, 1), &roomy(), 0),
        Ok(0)
    );
    undo(&mut rust);
    assert_eq!(doc(&rust).image_resources, before.image_resources);
    assert_eq!(saved(&rust), planes());
    let mut big = paged(&sprite());
    assert!(frame_size_rust(&mut big, 4, 4, (0, 0), &roomy(), 0).is_err());
    assert!(frame_size_rust(&mut rust, 0, 4, (0, 0), &roomy(), 0).is_err());
}
