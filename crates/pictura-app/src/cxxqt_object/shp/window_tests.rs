// SPDX-License-Identifier: GPL-3.0-or-later
//! Paged SHP documents: the window, the frame store, paging, the size
//! limit and its allowance. Every sprite and palette is generated, and the
//! limits are small injected numbers, so nothing large is allocated.

use super::tests::{
    choice, dab, doc, frame_bytes, palette, pencil, planes, roomy, sprite, FRAMES, H, W,
};
use super::*;
use crate::history::History;
use pictura_shp::parse;

/// 10 frames of 12x10 in 2200 bytes: windows of 4 (2 x 2 cells, 25 x 21 x 4
/// = 2100 bytes), so frames 0-3, 4-7 and 8-9.
fn small() -> Limits {
    Limits {
        limit: 2200,
        available: None,
    }
}

fn paged(bytes: &[u8]) -> PictureViewRust {
    let opened = open_sheet(
        "/tmp/paged.shp",
        bytes,
        choice(),
        false,
        &|_, _| {},
        &small(),
        0,
    )
    .expect("opens");
    let mut rust = PictureViewRust::default();
    crate::cxxqt_object::open_jobs::apply_opened(&mut rust, opened);
    rust
}

fn head(rust: &PictureViewRust) -> Header {
    header(doc(rust)).expect("resource")
}

/// Paint index `index` at canvas pixel `(x, y)` of file frame `frame`, which
/// must be on the sheet.
fn paint(rust: &mut PictureViewRust, frame: usize, x: usize, y: usize, index: usize) {
    let h = head(rust);
    assert!(h.window().contains(&frame), "frame {frame} is on the sheet");
    let (x0, y0) = h.layout.cell_origin(frame - h.first);
    let colour = unique_colours(&palette()).argb[index];
    dab(
        rust,
        pencil(colour),
        (x0 + x) as f32 + 0.5,
        (y0 + y) as f32 + 0.5,
    );
}

fn page(rust: &mut PictureViewRust, frame: usize) {
    page_rust(rust, frame, 0, &small(), 0).expect("pages");
}

#[test]
fn a_sprite_over_the_limit_opens_as_a_window_and_saves_unchanged() {
    let rust = paged(&sprite());
    let h = head(&rust);
    assert_eq!((h.first, h.layout.frames, h.total), (0, 4, FRAMES));
    assert!(h.paged());
    let d = doc(&rust);
    assert_eq!((d.width, d.height), (25, 21));
    let (_, _, original) = resource::parts(block(d).unwrap()).unwrap();
    assert_eq!(original, &sprite()[..]);
    assert_eq!(encode(d).expect("encodes"), sprite());
    // The same file under the real budget opens whole, as before.
    let whole = super::tests::opened(sprite(), choice());
    assert!(!head(&whole).paged());
}

#[test]
fn an_edit_survives_paging_away_and_back_and_saves_only_that_frame() {
    let mut rust = paged(&sprite());
    page(&mut rust, 6);
    assert_eq!(head(&rust).first, 4);
    paint(&mut rust, 6, 3, 4, 22);
    assert!(rust.session.dirty);
    // Away: frame 6 goes into the frame store.
    page(&mut rust, 9);
    let h = head(&rust);
    assert_eq!((h.first, h.layout.frames, h.span), (8, 2, 4));
    let stored = resource::stored(block(doc(&rust)).unwrap()).unwrap();
    assert_eq!(stored.iter().map(|s| s.0).collect::<Vec<_>>(), vec![6]);
    assert!(rust.session.dirty, "paging keeps the document modified");
    assert!(
        !rust.session.history.can_undo(),
        "a page change is the edge of undo"
    );

    // Saved from there: exactly that pixel of frame 6 changed.
    let saved = encode(doc(&rust)).expect("saves with the edit off the sheet");
    let back = parse(&saved).unwrap();
    let mut expected = planes();
    expected[6][4 * W + 3] = 22;
    for (i, plane) in expected.iter().enumerate() {
        assert_eq!(&back.canvas(i), plane, "frame {i}");
    }
    let original = sprite();
    for i in (0..FRAMES).filter(|&i| i != 6) {
        assert_eq!(
            frame_bytes(&saved, i).1,
            frame_bytes(&original, i).1,
            "frame {i}"
        );
    }

    // And back: the pixel is on the sheet again and the store is empty.
    page(&mut rust, 5);
    assert_eq!(head(&rust).first, 4);
    assert!(resource::stored(block(doc(&rust)).unwrap())
        .unwrap()
        .is_empty());
    assert_eq!(encode(doc(&rust)).unwrap(), saved);
    let (x0, y0) = head(&rust).layout.cell_origin(2);
    let rgba = buffer_to_rgba_bytes(&current_buffer(doc(&rust), false));
    let at = ((y0 + 4) * 25 + x0 + 3) * 4;
    let c = unique_colours(&palette()).argb[22];
    assert_eq!(
        &rgba[at..at + 4],
        &[(c >> 16) as u8, (c >> 8) as u8, c as u8, 255]
    );
}

#[test]
fn a_paged_save_matches_the_same_edit_saved_whole() {
    let mut whole = super::tests::opened(sprite(), choice());
    let l = head(&whole).layout;
    let (x0, y0) = l.cell_origin(9);
    let colour = unique_colours(&palette()).argb[50];
    dab(
        &mut whole,
        pencil(colour),
        (x0 + 1) as f32 + 0.5,
        (y0 + 2) as f32 + 0.5,
    );
    let expected = encode(doc(&whole)).unwrap();

    let mut rust = paged(&sprite());
    page(&mut rust, 9);
    paint(&mut rust, 9, 1, 2, 50);
    assert_eq!(encode(doc(&rust)).unwrap(), expected, "on the sheet");
    page(&mut rust, 0);
    assert_eq!(encode(doc(&rust)).unwrap(), expected, "in the store");
}

#[test]
fn an_edit_undone_before_paging_leaves_nothing_stored() {
    let mut rust = paged(&sprite());
    paint(&mut rust, 1, 2, 2, 60);
    assert!(rust
        .session
        .restore(&mut rust.layer_target, History::undo_live, History::undo)
        .is_some());
    page(&mut rust, 4);
    assert!(resource::stored(block(doc(&rust)).unwrap())
        .unwrap()
        .is_empty());
    assert_eq!(encode(doc(&rust)).unwrap(), sprite());
}

#[test]
fn paging_refuses_off_palette_and_gutter_pixels_and_keeps_the_window() {
    let mut rust = paged(&sprite());
    let mut soft = pencil(unique_colours(&palette()).argb[21] ^ 0x0000_0300);
    soft.aliased = false;
    soft.diameter = 5;
    soft.hardness = 0;
    dab(&mut rust, soft, 17.0, 5.0);
    let found = check(doc(&rust));
    assert!(found.off_palette > 0);
    assert_eq!(found.off_frames, vec![1], "frames are numbered in the file");
    let err = page_rust(&mut rust, 5, 4, &small(), 0).unwrap_err();
    assert!(err.contains("palette"), "{err}");
    assert_eq!(head(&rust).first, 0, "still on the first window");
    assert!(rust.session.history.can_undo(), "nothing was reset");

    let mut rust = paged(&sprite());
    dab(
        &mut rust,
        pencil(unique_colours(&palette()).argb[50]),
        12.5,
        0.5,
    );
    assert_eq!(check(doc(&rust)).gutter, 1);
    let err = page_rust(&mut rust, 5, 4, &small(), 0).unwrap_err();
    assert!(err.contains("gaps"), "{err}");
}

#[test]
fn a_clean_document_stays_clean_across_a_page_change() {
    let mut rust = paged(&sprite());
    assert!(!rust.session.dirty);
    page(&mut rust, 8);
    assert!(!rust.session.dirty);
    assert_eq!(rust.path.as_deref(), Some("/tmp/paged.shp"));
    assert_eq!(rust.source_format, "shp");
}

#[test]
fn a_larger_window_needs_the_allowance_and_never_passes_the_floor() {
    let mut rust = paged(&sprite());
    // 6 frames: 3 x 2 cells, 38 x 21 x 4 = 3192 bytes, 45% over 2200.
    let err = page_rust(&mut rust, 0, 6, &small(), 0).unwrap_err();
    assert!(err.contains("size limit"), "{err}");
    page_rust(&mut rust, 0, 6, &small(), 3192).expect("allowed past the limit");
    assert_eq!(head(&rust).layout.frames, 6);

    let starved = Limits {
        limit: 2200,
        available: Some(3192 * FOOTPRINT_FACTOR - 1),
    };
    let err = page_rust(&mut rust, 0, 6, &starved, u64::MAX).unwrap_err();
    assert!(err.contains("available"), "{err}");
    assert_eq!(head(&rust).first, 0);
}

#[test]
fn the_limit_has_three_stages_and_a_floor() {
    let limits = Limits {
        limit: 1000,
        available: Some(2000 * FOOTPRINT_FACTOR),
    };
    assert_eq!(limits.assess(1000).stage, 0);
    assert_eq!(limits.assess(1100).stage, 1);
    assert_eq!(limits.assess(1101).stage, 2);
    assert!(limits.assess(2000).floor_ok);
    assert!(!limits.assess(2001).floor_ok);
    assert!(limits.admit(1000, 0).is_ok());
    assert!(limits.admit(1050, 0).is_err());
    assert!(limits.admit(1050, 1050).is_ok());
    assert!(limits.admit(2001, u64::MAX).is_err(), "the floor stays");
    assert_eq!(limits.window_budget(), 1000);
    let short = Limits {
        limit: 1000,
        available: Some(500 * FOOTPRINT_FACTOR),
    };
    assert_eq!(short.window_budget(), 500);
}

#[test]
fn an_open_over_the_limit_needs_an_allowance() {
    // One 12x10 frame is 480 bytes of sheet; a 450-byte limit is 6.7% under it.
    let tight = Limits {
        limit: 450,
        available: None,
    };
    let open = |allowance| {
        open_sheet(
            "/tmp/x.shp",
            &sprite(),
            choice(),
            false,
            &|_, _| {},
            &tight,
            allowance,
        )
    };
    assert!(open(0).is_none());
    assert!(shp_last_error().to_string().contains("size limit"));
    let opened = open(480).expect("opens with the allowance");
    let h = header(&opened.doc).unwrap();
    assert_eq!((h.first, h.layout.frames), (0, 1));
}

#[test]
fn an_allowance_for_the_whole_sheet_opens_it_whole() {
    // 10 frames: 4 x 3 cells, 51 x 32 x 4 = 6528 bytes, three times 2200.
    let whole = plan(W, H, FRAMES, 0, FRAMES, &small()).unwrap();
    assert_eq!((whole.needed, whole.stage), (6528, 2));
    let opened = open_sheet(
        "/tmp/x.shp",
        &sprite(),
        choice(),
        false,
        &|_, _| {},
        &small(),
        whole.needed,
    )
    .expect("opens whole with the allowance");
    let h = header(&opened.doc).unwrap();
    assert_eq!((h.first, h.layout.frames, h.total), (0, FRAMES, FRAMES));
    assert!(!h.paged());
    assert_eq!(encode(&opened.doc).unwrap(), sprite());
    // Without it, the same file opens as a window of 4.
    let windowed = open_plan(&sprite(), &small(), 0).unwrap();
    assert_eq!((windowed.count, windowed.stage), (4, 0));
}

#[test]
fn frames_off_the_sheet_have_thumbnails() {
    let mut rust = paged(&sprite());
    assert!(thumbnail(doc(&rust), 2, 56).argb.is_empty(), "on the sheet");
    let t = thumbnail(doc(&rust), 5, 56);
    assert_eq!((t.width, t.height), (W as i32, H as i32));
    let unique = unique_colours(&palette());
    let plane = &planes()[5];
    for (i, &v) in plane.iter().enumerate() {
        let want = if v == 0 {
            0
        } else {
            0xFF00_0000 | unique.argb[usize::from(v)]
        };
        assert_eq!(t.argb[i], want, "pixel {i}");
    }
    // A stored edit shows in its thumbnail; a small side scales down.
    paint(&mut rust, 1, 0, 0, 99);
    page(&mut rust, 4);
    let t = thumbnail(doc(&rust), 1, 56);
    assert_eq!(t.argb[0], 0xFF00_0000 | unique.argb[99]);
    let t = thumbnail(doc(&rust), 1, 6);
    assert_eq!((t.width, t.height), (6, 5));
}

#[test]
fn a_paged_document_survives_a_psd_round_trip_with_its_store() {
    let mut rust = paged(&sprite());
    paint(&mut rust, 2, 4, 4, 77);
    page(&mut rust, 8);
    let saved = encode(doc(&rust)).unwrap();
    let psd = pictura_codec::write_psd(doc(&rust)).expect("writes");
    let back = pictura_codec::read_psd(&psd).expect("reads");
    assert_eq!(header(&back), header(doc(&rust)));
    assert_eq!(encode(&back).unwrap(), saved);
}

#[test]
fn a_whole_document_keeps_the_version_1_block() {
    let rust = super::tests::opened(sprite(), choice());
    let data = block(doc(&rust)).unwrap();
    assert_eq!(&data[4..6], &[0, 1]);
    assert!(open_plan(&sprite(), &roomy(), 0).is_some_and(|p| p.count == FRAMES as i32));
}

/// A real big sprite (SHP_BIG, e.g. sppnuke.shp: 136 frames of 2052x2816)
/// under the real limits: it opens as a window, one pixel of a frame far from
/// frame 0 is recoloured, the window pages away and back and away again, and
/// the save goes to SHP_BIG_OUT. It prints `k x y old new` for the
/// independent check in phase2-e2e.py (`verify`) and the peak RSS.
#[test]
#[ignore = "needs SHP_BIG and SHP_BIG_OUT; run under a memory cap"]
fn a_real_big_sprite_pages_an_edit_far_from_frame_0() {
    let path = std::env::var("SHP_BIG").expect("SHP_BIG");
    let out = std::env::var("SHP_BIG_OUT").expect("SHP_BIG_OUT");
    let bytes = std::fs::read(&path).expect("reads");
    let limits = Limits::current();
    let plan = open_plan(&bytes, &limits, 0).expect("plans");
    eprintln!(
        "plan: {} frames of {}x{}, window {} ({} bytes of sheet, limit {})",
        plan.total, plan.width, plan.height, plan.count, plan.needed, plan.limit
    );
    let opened = open_sheet(&path, &bytes, choice(), false, &|_, _| {}, &limits, 0)
        .expect("opens as a window");
    let mut rust = PictureViewRust::default();
    crate::cxxqt_object::open_jobs::apply_opened(&mut rust, opened);
    let h = head(&rust);
    eprintln!(
        "window: frames {}-{} of {}, sheet {}x{}, VmHWM {} MB",
        h.first,
        h.first + h.layout.frames - 1,
        h.total,
        doc(&rust).width,
        doc(&rust).height,
        hwm() >> 20
    );
    assert!(h.paged());
    assert_eq!(encode(doc(&rust)).expect("encodes"), bytes, "unchanged");

    // A frame far from frame 0, an opaque pixel in it, and a new index.
    let k = h.total * 3 / 4 + 1;
    let (w, ht) = (h.layout.width, h.layout.height);
    let canvas = frame_canvas(&bytes, k);
    let at = canvas
        .iter()
        .enumerate()
        .filter(|(_, &v)| v != 0)
        .nth(canvas.iter().filter(|&&v| v != 0).count() / 2)
        .map(|(i, _)| i)
        .expect("an opaque pixel");
    let (x, y, old) = (at % w, at / w, canvas[at]);
    let new: u8 = if old == 200 { 201 } else { 200 };
    assert!(y < ht);
    drop(canvas);

    page_rust(&mut rust, k, 0, &limits, 0).expect("pages to k");
    paint(&mut rust, k, x, y, new.into());
    page_rust(&mut rust, 0, 0, &limits, 0).expect("pages away");
    page_rust(&mut rust, k, 0, &limits, 0).expect("pages back");
    page_rust(&mut rust, 0, 0, &limits, 0).expect("pages away again");
    let saved = encode(doc(&rust)).expect("saves");
    std::fs::write(&out, &saved).expect("writes");
    println!("EDIT {k} {x} {y} {old} {new}");
    eprintln!("peak RSS (VmHWM) {} MB", hwm() >> 20);
}

fn hwm() -> u64 {
    std::fs::read_to_string("/proc/self/status")
        .unwrap_or_default()
        .lines()
        .find_map(|l| l.strip_prefix("VmHWM:"))
        .and_then(|v| v.trim().trim_end_matches("kB").trim().parse::<u64>().ok())
        .unwrap_or(0)
        << 10
}
