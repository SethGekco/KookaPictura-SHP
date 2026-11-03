//! New TMP templates from an image. Every image here is generated in code.

use pictura_shp::palette::{fallback_palette, unique_colours};
use pictura_shp::sheet::inverse;
use pictura_shp::terrain::{read_back, Plane};
use pictura_shp::tmp::{build, diamond_rows, parse, TileFields, Z_NONE};
use pictura_shp::tmp_image::{
    average_radar, cell_position, default_cells, flat_z, from_image, grid_size, ImageSpec, ZData,
};

/// An image of the whole `bx` x `by` grid of 60x30 tiles with `colour`
/// painted into the diamonds of `cells` (opaque), everything else clear.
fn grid_image(bx: usize, by: usize, cells: &[usize], colour: [u8; 4]) -> (Vec<u8>, usize, usize) {
    let (w, h) = grid_size(60, 30, bx, by);
    let mut rgba = vec![0u8; w * h * 4];
    let left = (by as i32 - 1) * 30;
    for &slot in cells {
        let (x, y) = cell_position(60, 30, slot % bx, slot / bx);
        for (ry, rx, rw) in diamond_rows(60, 30) {
            for k in 0..rw {
                let (px, py) = ((x + left) as usize + rx + k, y as usize + ry);
                rgba[(py * w + px) * 4..(py * w + px) * 4 + 4].copy_from_slice(&colour);
            }
        }
    }
    (rgba, w, h)
}

fn spec(bx: usize, by: usize, cells: Vec<bool>) -> ImageSpec {
    ImageSpec {
        cx: 60,
        cy: 30,
        blocks_x: bx,
        blocks_y: by,
        cells,
        fields: TileFields {
            height: 0,
            land: 0,
            ramp: 0,
            radar_low: [0; 3],
            radar_high: [0; 3],
        },
        z: ZData::None,
        extras: false,
    }
}

#[test]
fn cells_sit_where_the_real_templates_put_them() {
    assert_eq!(cell_position(60, 30, 0, 0), (0, 0));
    assert_eq!(cell_position(60, 30, 1, 0), (30, 15));
    assert_eq!(cell_position(60, 30, 0, 1), (-30, 15));
    assert_eq!(cell_position(48, 24, 2, 1), (24, 36));
    assert_eq!(grid_size(60, 30, 2, 2), (120, 60));
    assert_eq!(grid_size(48, 24, 1, 1), (48, 24));
}

#[test]
fn default_cells_are_the_diamonds_holding_pixels() {
    let (rgba, w, h) = grid_image(2, 2, &[0, 3], [9, 9, 9, 255]);
    assert_eq!(
        default_cells(&rgba, w, h, 60, 30, 2, 2),
        [true, false, false, true]
    );
    let blank = vec![0u8; w * h * 4];
    assert_eq!(
        default_cells(&blank, w, h, 60, 30, 2, 2),
        [false; 4],
        "transparent pixels do not count"
    );
}

#[test]
fn flat_z_counts_down_the_diamond_rows() {
    let z = flat_z(60, 30);
    let rows = diamond_rows(60, 30);
    assert_eq!(rows.len(), 29);
    for (y, x, w) in rows {
        assert!(z[y * 60 + x..y * 60 + x + w]
            .iter()
            .all(|&v| usize::from(v) == 29 - y));
    }
    assert_eq!(z[0], Z_NONE, "outside the diamond");
    assert!(flat_z(48, 24).iter().all(|&v| v == Z_NONE || v <= 23));
}

#[test]
fn an_image_becomes_tiles_that_build_and_read_back() {
    let palette = fallback_palette();
    let unique = unique_colours(&palette);
    let c = unique.argb[40];
    let colour = [(c >> 16) as u8, (c >> 8) as u8, c as u8, 255];
    let (rgba, w, h) = grid_image(2, 2, &[0, 3], colour);
    let mut s = spec(2, 2, vec![true, false, false, true]);
    s.fields.height = 2;
    s.fields.land = 13;
    s.z = ZData::Flat;
    let made = from_image(&rgba, w, h, &s).expect("made");
    assert_eq!((made.outside, made.dropped), (0, 0));
    let p = &made.placement;
    assert_eq!((p.width, p.height), (60, 60), "two tiles stacked");
    let names: Vec<String> = p.layers().into_iter().map(|l| l.name).collect();
    assert_eq!(names, ["Tiles", "Z Tiles"]);
    let refs: Vec<&[u8]> = made.layers.iter().map(Vec::as_slice).collect();
    let scan = read_back(&made.template, p, &refs, &inverse(&unique));
    assert_eq!((scan.off_palette, scan.off_scale, scan.outside), (0, 0, 0));
    let t = scan.template.unwrap();
    let t0 = t.tiles[0].as_ref().unwrap();
    let t3 = t.tiles[3].as_ref().unwrap();
    assert_eq!((t3.x, t3.y), (0, 30));
    assert_eq!(t0.fields.height, 2);
    assert_eq!(t3.fields.land, 13);
    assert_eq!(t0.z.as_deref(), Some(&flat_z(60, 30)[..]));
    for (y, x, rw) in diamond_rows(60, 30) {
        assert!(t0.image[y * 60 + x..y * 60 + x + rw]
            .iter()
            .all(|&v| v == 40));
    }
    assert_eq!(average_radar(t0, &palette).0, {
        let p = palette.argb[40];
        [(p >> 16) as u8, (p >> 8) as u8, p as u8]
    });
    let bytes = build(&t, None).unwrap();
    let mut got = parse(&bytes).unwrap().tiles;
    let mut t = t;
    for tile in t.tiles.iter_mut().flatten() {
        tile.flags = 0;
    }
    for tile in got.iter_mut().flatten() {
        (tile.offset, tile.raw) = (0, [0; 52]);
        tile.flags = 0;
    }
    assert_eq!(got, t.tiles, "the built file reads back as made");
}

#[test]
fn pixels_outside_the_diamonds_stay_for_the_gate_unless_they_become_extras() {
    let (mut rgba, w, h) = grid_image(2, 1, &[0, 1], [200, 10, 10, 255]);
    // A 3x2 block above tile 1's diamond, nearer its centre than tile 0's.
    for y in 9..11 {
        for x in 75..78 {
            rgba[(y * w + x) * 4..(y * w + x) * 4 + 4].copy_from_slice(&[1, 2, 3, 255]);
        }
    }
    let s = spec(2, 1, vec![true, true]);
    let kept = from_image(&rgba, w, h, &s).expect("made");
    assert_eq!(kept.outside, 6);
    assert_eq!(kept.placement.layers().len(), 2, "Tiles and Z Tiles only");
    assert!(kept
        .template
        .tiles
        .iter()
        .flatten()
        .all(|t| t.extra.is_none()));

    let mut s = s;
    s.extras = true;
    let made = from_image(&rgba, w, h, &s).expect("made");
    assert_eq!(made.outside, 0);
    let t1 = made.template.tiles[1].as_ref().unwrap();
    assert_eq!(
        (t1.extra_x, t1.extra_y, t1.extra_w, t1.extra_h),
        (75, 9, 3, 2),
        "the bounding box of the pixels nearest tile 1"
    );
    assert!(made.template.tiles[0].as_ref().unwrap().extra.is_none());
    assert!(t1.extra_z.is_none(), "no z-data for a new extra");
    let p = &made.placement;
    let extras = p
        .layers()
        .iter()
        .position(|l| l.plane == Plane::Extras)
        .unwrap();
    let at = |x: usize, y: usize| {
        let i = ((y as i64 + p.origin_y) as usize * p.width + (x as i64 + p.origin_x) as usize) * 4;
        made.layers[extras][i..i + 4].to_vec()
    };
    assert_eq!(at(75, 9), [1, 2, 3, 255]);
    assert_eq!(at(77, 10), [1, 2, 3, 255]);
    assert_eq!(at(74, 9), [0; 4]);
}

#[test]
fn what_cannot_be_a_template_is_refused() {
    let (rgba, w, h) = grid_image(1, 1, &[0], [1, 1, 1, 255]);
    let mut s = spec(1, 1, vec![true]);
    s.cx = 64;
    assert!(from_image(&rgba, w, h, &s).unwrap_err().contains("60x30"));
    let s = spec(1, 1, vec![false]);
    assert!(from_image(&rgba, w, h, &s)
        .unwrap_err()
        .contains("at least one"));
    let s = spec(2, 1, vec![true]);
    assert!(from_image(&rgba, w, h, &s).is_err(), "cells do not match");
    let s = spec(300, 1, vec![true; 300]);
    assert!(from_image(&rgba, w, h, &s).unwrap_err().contains("256"));
}

#[test]
fn an_image_bigger_than_the_canvas_drops_what_falls_off() {
    let mut rgba = vec![255u8; 100 * 40 * 4];
    rgba.iter_mut().skip(3).step_by(4).for_each(|a| *a = 255);
    let made = from_image(&rgba, 100, 40, &spec(1, 1, vec![true])).expect("made");
    assert_eq!((made.placement.width, made.placement.height), (60, 30));
    assert_eq!(
        made.outside,
        60 * 30 - 900,
        "inside the canvas, outside the diamond"
    );
    assert_eq!(made.dropped, 100 * 40 - 60 * 30);
}
