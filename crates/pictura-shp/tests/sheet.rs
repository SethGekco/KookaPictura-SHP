//! The RGBA sheet and the `KookaSHP` resource: layout, exact index recovery,
//! off-palette reporting, snap, gutter, rebind, and the resource round trip.

mod common;

use common::{place, sprite_plane, Rng};
use pictura_shp::palette::{fallback_palette, nearest, role, unique_colours, Palette, Role};
use pictura_shp::resource::{
    find_block, parts, set_block, stored, stored_canvas, Header, ShpResource, StoredFrame,
    RESOURCE_ID,
};
use pictura_shp::sheet::{
    auto_columns, clear_gutter, from_sheet, inverse, rebind, snap, to_sheet, window_frames,
    window_start, Layout, MAX_SHEET_SIDE,
};

/// A palette that repeats colours the way game palettes do, so
/// `unique_colours` has work to do.
fn repetitive_palette() -> Palette {
    let mut argb = [0u32; 256];
    for (i, c) in argb.iter_mut().enumerate() {
        let v = (i as u32 / 4) * 4; // four indices per colour
        *c = 0xFF00_0000 | (v << 16) | ((255 - v) << 8) | (v / 2);
    }
    argb[0] &= 0x00FF_FFFF;
    Palette {
        argb,
        name: "unittem.pal".into(),
    }
}

fn planes(rng: &mut Rng, w: usize, h: usize, n: usize) -> Vec<Vec<u8>> {
    (0..n)
        .map(|_| {
            let (fw, fh) = (
                1 + rng.below(w as u64) as usize,
                1 + rng.below(h as u64) as usize,
            );
            let (x, y) = (
                rng.below((w - fw + 1) as u64) as usize,
                rng.below((h - fh + 1) as u64) as usize,
            );
            place(w, h, x, y, fw, &sprite_plane(rng, fw, fh))
        })
        .collect()
}

#[test]
fn columns_follow_qshp_and_the_side_budget() {
    assert_eq!(auto_columns(1), 1);
    assert_eq!(auto_columns(8), 8);
    assert_eq!(auto_columns(64), 8);
    assert_eq!(auto_columns(10), 4);
    assert_eq!(auto_columns(5), 3);

    let l = Layout::new(20, 30, 64, 1).expect("fits");
    assert_eq!((l.cols, l.rows()), (8, 8));
    assert_eq!(
        (l.sheet_width(), l.sheet_height()),
        (8 * 20 + 7, 8 * 30 + 7)
    );
    assert_eq!(l.cell_origin(9), (21, 31));
    assert_eq!(l.frame_at(21, 31), Some(9));
    assert_eq!(l.frame_at(20, 0), None, "gutter column");
    assert_eq!(l.frame_at(0, 30), None, "gutter row");

    // 800 frames of 100 px rows would make a sheet 10,099 px tall at 8
    // columns; 4096-tall cells need more columns to stay under the budget.
    let tall = Layout::new(10, 400, 800, 1).expect("widened");
    assert!(tall.sheet_height() <= MAX_SHEET_SIDE);
    assert!(tall.cols > 8);
    assert!(Layout::new(4096, 4096, 4096, 1).is_none(), "cannot fit");
}

#[test]
fn a_sheet_maps_back_to_every_index_exactly() {
    let mut rng = Rng::new(7);
    let palette = repetitive_palette();
    let unique = unique_colours(&palette);
    let (w, h, n) = (13, 9, 11);
    let layout = Layout::new(w, h, n, 1).unwrap();
    let mut planes = planes(&mut rng, w, h, n);
    // Every index, including the repeated colours, appears somewhere.
    for i in 0..256usize {
        planes[i % n][i % (w * h)] = i as u8;
    }
    let rgba = to_sheet(&layout, &planes, &unique);
    assert_eq!(rgba.len(), layout.sheet_width() * layout.sheet_height() * 4);
    let scan = from_sheet(&layout, &rgba, &inverse(&unique));
    assert_eq!(scan.planes, planes);
    assert_eq!(scan.off_palette_total(), 0);
    assert_eq!(scan.gutter, 0);
}

#[test]
fn off_palette_pixels_are_reported_per_frame_never_guessed() {
    let unique = unique_colours(&repetitive_palette());
    let layout = Layout::new(4, 4, 3, 1).unwrap();
    let planes = vec![vec![5u8; 16]; 3];
    let mut rgba = to_sheet(&layout, &planes, &unique);
    let sw = layout.sheet_width();
    let at = |x: usize, y: usize| (y * sw + x) * 4;
    // Frame 1: a soft (half-alpha) pixel; frame 2: an opaque colour not in U.
    let (x1, y1) = layout.cell_origin(1);
    rgba[at(x1 + 1, y1 + 2) + 3] = 128;
    let (x2, y2) = layout.cell_origin(2);
    rgba[at(x2 + 3, y2)..at(x2 + 3, y2) + 3].copy_from_slice(&[1, 2, 3]);
    // An opaque pixel coloured like index 0 is off-palette too.
    rgba[at(x2, y2 + 1)..at(x2, y2 + 1) + 4].copy_from_slice(&{
        let c = unique.argb[0];
        [(c >> 16) as u8, (c >> 8) as u8, c as u8, 255]
    });
    // And a gutter pixel.
    rgba[at(4, 0) + 3] = 255;

    let scan = from_sheet(&layout, &rgba, &inverse(&unique));
    assert_eq!(scan.off_palette, vec![0, 1, 2]);
    assert_eq!(scan.off_frames(), vec![1, 2]);
    assert_eq!(scan.off_bounds[1], Some((x1 + 1, y1 + 2, 1, 1)));
    assert_eq!(scan.off_bounds[2], Some((x2, y2, 4, 2)));
    assert_eq!(scan.gutter, 1);

    assert_eq!(clear_gutter(&layout, &mut rgba), 1);
    assert_eq!(from_sheet(&layout, &rgba, &inverse(&unique)).gutter, 0);
}

#[test]
fn snap_maps_to_nearest_with_the_remap_ramp_last() {
    // Index 20 (remap) and index 40 share a colour; a pixel near it must
    // snap to 40.
    let mut palette = fallback_palette();
    palette.argb[20] = 0xFF80_4020;
    palette.argb[40] = 0xFF80_4020;
    let unique = unique_colours(&palette);
    let layout = Layout::new(3, 1, 1, 1).unwrap();
    let mut rgba = vec![
        0x81, 0x40, 0x20, 200, // near 40/20, mostly opaque
        9, 9, 9, 100, // mostly transparent: index 0
        0, 0, 0, 0, // already transparent
    ];
    assert_eq!(snap(&layout, &mut rgba, &palette, &unique), 2);
    let scan = from_sheet(&layout, &rgba, &inverse(&unique));
    assert_eq!(scan.off_palette_total(), 0);
    assert_eq!(
        scan.planes[0],
        vec![40, 0, 0],
        "the remap ramp loses the tie"
    );
    assert_eq!(nearest(&palette, 0xFF81_4020), 40);
}

#[test]
fn rebind_recolours_by_index() {
    let old = unique_colours(&fallback_palette());
    let new = unique_colours(&repetitive_palette());
    let layout = Layout::new(16, 16, 1, 0).unwrap();
    let plane: Vec<u8> = (0..=255u8).collect();
    let mut rgba = to_sheet(&layout, std::slice::from_ref(&plane), &old);
    rebind(&mut rgba, &old, &new);
    assert_eq!(from_sheet(&layout, &rgba, &inverse(&new)).planes[0], plane);
}

#[test]
fn the_resource_round_trips_and_keeps_its_neighbours() {
    let resource = ShpResource {
        header: Header::whole(Layout::new(30, 20, 16, 1).unwrap(), repetitive_palette()),
        palette_path: "/pal/unittem.pal".into(),
        original: (0..1001u32).map(|v| v as u8).collect(),
        stored: Vec::new(),
    };
    let data = resource.encode();
    assert_eq!(&data[4..6], &[0, 1], "a whole sheet stays version 1");
    assert_eq!(ShpResource::decode(&data), Some(resource.clone()));
    assert_eq!(Header::read(&data), Some(resource.header.clone()));

    // A section with an odd-named neighbour before and after.
    let neighbour = |id: u16, payload: &[u8]| set_block(&[], id, "n", payload);
    let mut section = neighbour(1005, b"abc");
    section.extend(neighbour(1036, b"xy"));
    let with = set_block(&section, RESOURCE_ID, "KookaSHP", &data);
    assert_eq!(find_block(&with, RESOURCE_ID), Some(&data[..]));
    assert_eq!(&with[..section.len()], &section[..], "neighbours untouched");
    assert_eq!(find_block(&with, 1036), Some(&b"xy"[..]));

    // Replacing keeps its place and the block count.
    let smaller = set_block(&with, RESOURCE_ID, "KookaSHP", b"KSHP");
    assert_eq!(find_block(&smaller, RESOURCE_ID), Some(&b"KSHP"[..]));
    assert_eq!(find_block(&smaller, 1005), Some(&b"abc"[..]));
    assert!(Header::read(b"KSHP").is_none());
}

#[test]
fn roles_follow_the_palette_kind() {
    assert_eq!(role("unittem.pal", 0), Role::Transparent);
    assert_eq!(role("unittem.pal", 1), Role::Shadow);
    assert_eq!(role("unittem.pal", 16), Role::Remap);
    assert_eq!(role("unittem.pal", 31), Role::Remap);
    assert_eq!(role("unittem.pal", 240), Role::Unlit);
    assert_eq!(role("unittem.pal", 255), Role::Plain);
    assert_eq!(role("isotem.pal", 1), Role::Plain);
    assert_eq!(role("ISOSNO.PAL", 20), Role::Plain);
    assert_eq!(role("isotem.pal", 0), Role::Transparent);
}

#[test]
fn a_window_and_its_stored_frames_round_trip_as_version_2() {
    let plane: Vec<u8> = (0..30 * 20).map(|i| (i % 7) as u8).collect();
    let resource = ShpResource {
        header: Header {
            layout: Layout::new(30, 20, 8, 1).unwrap(),
            palette: repetitive_palette(),
            first: 16,
            total: 40,
            span: 8,
        },
        palette_path: "/pal/unittem.pal".into(),
        original: (0..999u32).map(|v| v as u8).collect(),
        stored: vec![
            StoredFrame::new(3, &plane, 30),
            StoredFrame::new(39, &plane, 30),
        ],
    };
    let data = resource.encode();
    assert_eq!(&data[4..6], &[0, 2]);
    assert_eq!(ShpResource::decode(&data), Some(resource.clone()));
    let header = Header::read(&data).unwrap();
    assert!(header.paged());
    assert_eq!(header.window(), 16..24);
    let (_, path, original) = parts(&data).unwrap();
    assert_eq!(
        (path.as_str(), original),
        ("/pal/unittem.pal", &resource.original[..])
    );
    let stored = stored(&data).unwrap();
    assert_eq!(stored.iter().map(|s| s.0).collect::<Vec<_>>(), vec![3, 39]);
    assert_eq!(stored_canvas(stored[1].1, 30, 20), plane);

    // A window past the file's end is not a header.
    let mut bad = data.clone();
    bad[18..20].copy_from_slice(&20u16.to_be_bytes());
    assert!(Header::read(&bad).is_none());
}

#[test]
fn a_window_holds_what_fits_the_budget_in_whole_facing_sets() {
    // 136 frames of 2052x2816: 8 fit in 256 MiB (one facing set per row).
    let n = window_frames(2052, 2816, 136, 1, 256 << 20);
    assert_eq!(n, 8);
    let l = Layout::new(2052, 2816, n, 1).unwrap();
    assert_eq!(
        (l.cols, l.sheet_width(), l.sheet_height()),
        (8, 16423, 2816)
    );
    // The whole file when it fits; one frame when even that does not.
    assert_eq!(window_frames(6, 5, 10, 1, 1 << 20), 10);
    assert_eq!(window_frames(6, 5, 10, 1, 10), 1);
    // 10 frames of 6x5 in 600 bytes: 4 (2 x 2 cells, 13 x 11 x 4 = 572).
    assert_eq!(window_frames(6, 5, 10, 1, 600), 4);
    assert_eq!(window_start(9, 4), 8);
    assert_eq!(window_start(3, 4), 0);
}
