// SPDX-License-Identifier: GPL-3.0-or-later
//! The shadow tools on whole and paged SHP documents: detection, the check,
//! generating and clearing, the added shadow half, undo, and saved bytes.

use super::tests::{choice, doc, frame_bytes, opened, planes, roomy, sprite, FRAMES, H, W};
use super::unit::{clear_rust, frame_uses, generate_rust};
use super::window_tests::{head, paged, paint, small};
use super::*;
use crate::history::History;
use pictura_shp::unit::{silhouette, split, ShadowParams, Use};
use pictura_shp::{build, parse};

const ART: usize = 4;

/// Four art frames and their straight shadows.
fn unit_planes() -> Vec<Vec<u8>> {
    let art: Vec<Vec<u8>> = planes().into_iter().take(ART).collect();
    let shadows: Vec<Vec<u8>> = art
        .iter()
        .map(|a| silhouette(a, W, H, ShadowParams::STRAIGHT))
        .collect();
    art.into_iter().chain(shadows).collect()
}

fn unit_sprite() -> Vec<u8> {
    build(W as u16, H as u16, &unit_planes()).expect("build")
}

/// Frame `k` of `a` keeps `b`'s header fields (all but the data offset,
/// which moves when frames are added) and data bytes.
fn same_frame(a: &[u8], b: &[u8], k: usize) {
    let (ha, da) = frame_bytes(a, k);
    let (hb, db) = frame_bytes(b, k);
    assert_eq!(ha[..20], hb[..20], "frame {k} header");
    assert_eq!(da, db, "frame {k} data");
}

/// The frames the document would save, decoded.
fn saved(rust: &PictureViewRust) -> Vec<Vec<u8>> {
    let bytes = encode(doc(rust)).expect("saves");
    let sprite = parse(&bytes).expect("parses");
    (0..sprite.frames.len()).map(|i| sprite.canvas(i)).collect()
}

fn undo(rust: &mut PictureViewRust) {
    assert!(rust
        .session
        .restore(&mut rust.layer_target, History::undo_live, History::undo)
        .is_some());
}

fn generate(rust: &mut PictureViewRust, half: usize, art: &[usize], p: ShadowParams) {
    let paged = head(rust).paged();
    generate_rust(rust, half, art, p, &roomy(), 0).expect("generates");
    if !paged {
        rust.apply_edit("Generate Shadows", Edit::Full);
    }
}

fn clear(rust: &mut PictureViewRust, half: usize, frames: &[usize]) {
    let paged = head(rust).paged();
    clear_rust(rust, half, frames).expect("clears");
    if !paged {
        rust.apply_edit("Clear Shadows", Edit::Full);
    }
}

const LEAN: ShadowParams = ShadowParams {
    dx: 2,
    dy: -1,
    skew: 0.5,
};

#[test]
fn the_shadow_half_is_detected_and_coloured_shadow_pixels_are_reported() {
    let mut rust = opened(unit_sprite(), choice());
    let uses = frame_uses(doc(&rust)).expect("uses");
    assert_eq!(&uses[..ART], &[Use::Art; ART]);
    assert_eq!(&uses[ART..], &[Use::Shadow; ART]);
    assert_eq!(split(&uses), Some(ART));
    // A plain sprite has no shadow half.
    let plain = opened(sprite(), choice());
    assert_eq!(split(&frame_uses(doc(&plain)).unwrap()), None);
    // An art color painted into shadow frame 6 is reported, and the split
    // is gone (the check names the frame; the toggle in the shell keeps it).
    paint(&mut rust, 6, 3, 3, 45);
    let uses = frame_uses(doc(&rust)).unwrap();
    assert_eq!(pictura_shp::unit::violations(&uses, ART), vec![6]);
    assert_eq!(split(&uses), None);
}

#[test]
fn clear_then_generate_writes_only_shadow_frames_and_undo_restores_them() {
    let file = unit_sprite();
    let mut rust = opened(file.clone(), choice());
    clear(&mut rust, ART, &[4, 5, 6, 7]);
    let after = saved(&rust);
    assert_eq!(&after[..ART], &unit_planes()[..ART]);
    assert!(after[ART..].iter().all(|p| p.iter().all(|&v| v == 0)));
    assert_eq!(
        split(&frame_uses(doc(&rust)).unwrap()),
        Some(ART),
        "cleared shadows are still the shadow half"
    );

    generate(&mut rust, ART, &[0, 2], LEAN);
    let bytes = encode(doc(&rust)).unwrap();
    let after = saved(&rust);
    let art = &unit_planes()[..ART];
    for a in 0..ART {
        assert_eq!(after[a], art[a], "art frame {a} unchanged");
        assert_eq!(
            frame_bytes(&bytes, a),
            frame_bytes(&file, a),
            "art frame {a} bytes"
        );
    }
    assert_eq!(after[4], silhouette(&art[0], W, H, LEAN));
    assert_eq!(after[6], silhouette(&art[2], W, H, LEAN));
    assert!(after[5].iter().all(|&v| v == 0), "frame 5 not asked for");
    assert!(after[4].iter().all(|&v| v <= 1) && after[4].contains(&1));

    undo(&mut rust);
    assert!(saved(&rust)[ART..]
        .iter()
        .all(|p| p.iter().all(|&v| v == 0)));
    undo(&mut rust);
    assert_eq!(
        encode(doc(&rust)).unwrap(),
        file,
        "back to the original bytes"
    );
}

#[test]
fn generating_without_a_shadow_half_adds_one_through_the_size_check() {
    let file = sprite();
    let mut rust = opened(file.clone(), choice());
    let all: Vec<usize> = (0..FRAMES).collect();
    // Doubling the sheet goes over a limit the sheet fits now.
    let tight = Limits {
        limit: head(&rust).layout.sheet_bytes(),
        available: None,
    };
    let refused = generate_rust(&mut rust, 0, &all, ShadowParams::STRAIGHT, &tight, 0);
    assert!(refused.is_err_and(|e| e.contains("size limit")));
    assert_eq!(head(&rust).total, FRAMES);

    generate(&mut rust, 0, &all, ShadowParams::STRAIGHT);
    let h = head(&rust);
    assert_eq!((h.total, h.layout.frames), (FRAMES * 2, FRAMES * 2));
    let uses = frame_uses(doc(&rust)).unwrap();
    assert_eq!(split(&uses), Some(FRAMES));
    let bytes = encode(doc(&rust)).unwrap();
    let after = saved(&rust);
    for (k, art) in planes().iter().enumerate() {
        assert_eq!(&after[k], art);
        same_frame(&bytes, &file, k);
        assert_eq!(
            after[FRAMES + k],
            silhouette(art, W, H, ShadowParams::STRAIGHT)
        );
    }
    undo(&mut rust);
    assert_eq!(head(&rust).total, FRAMES);
    assert_eq!(encode(doc(&rust)).unwrap(), file);
}

#[test]
fn shadow_tools_refuse_frames_outside_their_half() {
    let mut rust = opened(unit_sprite(), choice());
    let p = ShadowParams::STRAIGHT;
    assert!(generate_rust(&mut rust, ART, &[ART], p, &roomy(), 0).is_err());
    assert!(generate_rust(&mut rust, 3, &[0], p, &roomy(), 0).is_err());
    assert!(clear_rust(&mut rust, ART, &[1]).is_err());
    assert!(clear_rust(&mut rust, ART, &[]).is_err());
    assert_eq!(encode(doc(&rust)).unwrap(), unit_sprite());
}

#[test]
fn a_paged_sprite_generates_into_the_frame_store_and_saves_the_same() {
    let file = unit_sprite();
    let mut rust = paged(&file);
    let h = head(&rust);
    assert!(h.paged());
    assert_eq!(h.window(), 0..ART, "the window holds the art only");
    assert_eq!(split(&frame_uses(doc(&rust)).unwrap()), Some(ART));

    clear(&mut rust, ART, &[5, 7]);
    generate(&mut rust, ART, &[1, 3], LEAN);
    let h = head(&rust);
    assert_eq!(h.window(), 0..ART, "the window stays");
    let mut whole = opened(file.clone(), choice());
    clear(&mut whole, ART, &[5, 7]);
    generate(&mut whole, ART, &[1, 3], LEAN);
    assert_eq!(
        encode(doc(&rust)).unwrap(),
        encode(doc(&whole)).unwrap(),
        "paged and whole agree"
    );
    let after = saved(&rust);
    let art = &unit_planes()[..ART];
    assert_eq!(after[5], silhouette(&art[1], W, H, LEAN));
    assert_eq!(after[7], silhouette(&art[3], W, H, LEAN));
    assert_eq!(after[4], unit_planes()[4]);
    // The shadow frames read back from the store.
    assert_eq!(split(&frame_uses(doc(&rust)).unwrap()), Some(ART));
}

#[test]
fn a_paged_sprite_without_shadows_gains_a_shadow_half() {
    let file = sprite();
    let mut rust = paged(&file);
    assert!(head(&rust).paged());
    let all: Vec<usize> = (0..FRAMES).collect();
    let limits = small();
    generate_rust(&mut rust, 0, &all, ShadowParams::STRAIGHT, &limits, 0).expect("generates");
    let h = head(&rust);
    assert_eq!(h.total, FRAMES * 2);
    let after = saved(&rust);
    for (k, art) in planes().iter().enumerate() {
        assert_eq!(&after[k], art);
        assert_eq!(
            after[FRAMES + k],
            silhouette(art, W, H, ShadowParams::STRAIGHT)
        );
    }
    let bytes = encode(doc(&rust)).unwrap();
    for k in 0..FRAMES {
        same_frame(&bytes, &file, k);
    }
}
