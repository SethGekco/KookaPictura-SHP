// SPDX-License-Identifier: GPL-3.0-or-later
//! The shadow tools on whole and paged SHP documents: detection, the check,
//! generating and clearing, the added shadow half, undo, and saved bytes.

use super::tests::{choice, doc, frame_bytes, opened, planes, roomy, sprite, FRAMES, H, W};
use super::unit::{clear_rust, encode_for_save, frame_uses, generate_rust, radar_info, OnDisk};
use super::window_tests::{head, paged, paint, small};
use super::*;
use crate::history::History;
use pictura_shp::unit::{average_color, silhouette, split, ShadowParams, Use};
use pictura_shp::{build, parse, FrameHeader};

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

fn pack(c: [u8; 3]) -> u32 {
    (u32::from(c[0]) << 16) | (u32::from(c[1]) << 8) | u32::from(c[2])
}

#[test]
fn save_keeps_the_radar_colors_unless_asked_to_recompute_them() {
    let file = sprite();
    let mut rust = opened(file.clone(), choice());
    assert_eq!(
        encode_for_save(doc(&rust), false, &OnDisk::default()).unwrap(),
        file
    );
    let recomputed = encode_for_save(doc(&rust), true, &OnDisk::default()).unwrap();
    assert_eq!(recomputed.len(), file.len());
    for (k, plane) in planes().iter().enumerate() {
        let want = average_color(plane, &choice().palette).expect("drawn");
        let got = FrameHeader::read(&recomputed, k).radar;
        assert_eq!(got, [want[0], want[1], want[2], 0], "frame {k}");
        // Everything but the radar field is as it was.
        let (h, d) = frame_bytes(&recomputed, k);
        let (fh, fd) = frame_bytes(&file, k);
        assert_eq!((&h[..12], &h[16..], d), (&fh[..12], &fh[16..], fd));
    }
    // The readout: the file's field and the average now, which follows an edit.
    let info = radar_info(doc(&rust), 3, &OnDisk::default());
    let field = FrameHeader::read(&file, 3).radar;
    assert!(info.has_file && info.has_average);
    assert_eq!(info.file, pack([field[0], field[1], field[2]]));
    assert_eq!(
        info.average,
        pack(average_color(&planes()[3], &choice().palette).unwrap())
    );
    paint(&mut rust, 3, 0, 0, 200);
    let mut edited = planes()[3].clone();
    edited[0] = 200;
    assert_eq!(
        radar_info(doc(&rust), 3, &OnDisk::default()).average,
        pack(average_color(&edited, &choice().palette).unwrap())
    );
}

#[test]
fn a_recompute_save_becomes_the_radar_in_the_file() {
    let file = sprite();
    let mut rust = opened(file.clone(), choice());
    let mut on_disk = OnDisk::default();
    let redone = encode_for_save(doc(&rust), true, &on_disk).unwrap();
    assert_ne!(redone, file);
    on_disk.remember(doc(&rust), &redone);
    // Off again: the recomputed fields stay; every other byte is the file's.
    assert_eq!(
        encode_for_save(doc(&rust), false, &on_disk).unwrap(),
        redone
    );
    let field = FrameHeader::read(&redone, 3).radar;
    assert_ne!(field, FrameHeader::read(&file, 3).radar);
    let info = radar_info(doc(&rust), 3, &on_disk);
    assert!(info.has_file);
    assert_eq!(info.file, pack([field[0], field[1], field[2]]));

    // An edit after the save keeps the field on disk; untouched frames are
    // byte-identical to the saved file.
    paint(&mut rust, 3, 0, 0, 200);
    let edited = encode_for_save(doc(&rust), false, &on_disk).unwrap();
    assert_eq!(FrameHeader::read(&edited, 3).radar, field);
    for k in (0..FRAMES).filter(|&k| k != 3) {
        same_frame(&edited, &redone, k);
    }
    // Saving is not an edit: undo leaves what is on disk alone.
    undo(&mut rust);
    assert_eq!(
        encode_for_save(doc(&rust), false, &on_disk).unwrap(),
        redone
    );

    // A document read from another file finds the table stale.
    let mut other = file.clone();
    other[8 + 24 * 3 + 12..8 + 24 * 3 + 16].copy_from_slice(&[1, 2, 3, 0]);
    let reread = opened(other.clone(), choice());
    assert_eq!(
        encode_for_save(doc(&reread), false, &on_disk).unwrap(),
        other
    );
    assert_eq!(radar_info(doc(&reread), 3, &on_disk).file, 0x010203);
}

#[test]
fn an_added_frame_keeps_the_radar_it_was_saved_with() {
    let mut rust = opened(sprite(), choice());
    whole_op_rust(
        &mut rust,
        pictura_shp::frames::FrameOp::Insert { at: 0, count: 1 },
        &roomy(),
        0,
    )
    .expect("inserts");
    paint(&mut rust, 0, 2, 2, 200);
    let mut on_disk = OnDisk::default();
    assert!(!radar_info(doc(&rust), 0, &on_disk).has_file);
    let redone = encode_for_save(doc(&rust), true, &on_disk).unwrap();
    let field = FrameHeader::read(&redone, 0).radar;
    assert_ne!(field, [0; 4]);
    on_disk.remember(doc(&rust), &redone);
    assert_eq!(
        encode_for_save(doc(&rust), false, &on_disk).unwrap(),
        redone
    );
    assert_eq!(
        radar_info(doc(&rust), 0, &on_disk).file,
        pack([field[0], field[1], field[2]])
    );
}

#[test]
fn the_radar_readout_reads_frames_off_a_paged_sheet_and_added_frames() {
    let file = sprite();
    let rust = paged(&file);
    let info = radar_info(doc(&rust), 8, &OnDisk::default());
    assert!(!head(&rust).window().contains(&8));
    let field = FrameHeader::read(&file, 8).radar;
    assert_eq!(info.file, pack([field[0], field[1], field[2]]));
    assert_eq!(
        info.average,
        pack(average_color(&planes()[8], &choice().palette).unwrap())
    );
    // A blank added frame has neither.
    let mut whole = opened(file, choice());
    whole_op_rust(
        &mut whole,
        pictura_shp::frames::FrameOp::Insert { at: 0, count: 1 },
        &roomy(),
        0,
    )
    .expect("inserts");
    let added = radar_info(doc(&whole), 0, &OnDisk::default());
    assert!(!added.has_file && !added.has_average);
}

#[test]
fn palette_kinds_reach_the_shell_with_their_roles() {
    use super::{shp_kind, shp_kind_role};
    let unit = shp_kind(0);
    assert_eq!(unit.label.to_string(), "unit");
    assert_eq!(
        (unit.shadow, unit.remap_first, unit.remap_last),
        (1, 16, 31)
    );
    for (kind, shadow) in [(1, -1), (2, 1), (3, -1)] {
        let k = shp_kind(kind);
        assert_eq!((k.shadow, k.remap_first, k.remap_last), (shadow, -1, -1));
    }
    assert_eq!(shp_kind(9).label.to_string(), "unit", "unknown is unit");
    assert_eq!(shp_kind_role(0, 20).to_string(), "house colour remap");
    assert_eq!(shp_kind_role(1, 1).to_string(), "");
    assert_eq!(shp_kind_role(2, 1).to_string(), "shadow");
    assert_eq!(shp_kind_role(3, 0).to_string(), "transparent");
    assert_eq!(shp_kind_role(-1, 1).to_string(), "");
}
