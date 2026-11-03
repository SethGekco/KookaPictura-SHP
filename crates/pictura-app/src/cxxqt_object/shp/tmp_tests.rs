// SPDX-License-Identifier: GPL-3.0-or-later
//! TMP terrain documents through the app glue. Every template and palette
//! here is generated; no game file is used.

use super::tests::{choice, doc};
use super::tmp::*;
use super::*;
use crate::cxxqt_object::open_jobs::apply_opened;
use pictura_shp::tmp::{build, diamond_rows, Template, Tile, TileFields, Z_NONE};

pub(super) const CX: usize = 60;
pub(super) const CY: usize = 30;

fn diamond(v: u8, fill: u8) -> Vec<u8> {
    let mut rect = vec![fill; CX * CY];
    for (y, x, w) in diamond_rows(CX, CY) {
        rect[y * CX + x..y * CX + x + w].fill(v);
    }
    rect
}

/// Two tiles of a 2 x 2 template (slots 0 and 3): tile 0 with z-data, tile
/// 3 with an extra block above it and its z-data, and recognisable fields.
pub(super) fn template() -> Template {
    let mut t0 = Tile::new(0, 0, CX, CY);
    t0.image = diamond(44, 0);
    t0.z = Some(diamond(5, Z_NONE));
    let mut t3 = Tile::new(0, 30, CX, CY);
    t3.image = diamond(48, 0);
    t3.z = Some(diamond(9, Z_NONE));
    t3.extra_x = 20;
    t3.extra_y = 10;
    (t3.extra_w, t3.extra_h) = (12, 14);
    t3.extra = Some((0..168).map(|i| if i % 5 == 0 { 0 } else { 60 }).collect());
    t3.extra_z = Some(vec![12; 168]);
    t3.fields = TileFields {
        height: 3,
        land: 15,
        ramp: 2,
        radar_low: [10, 20, 30],
        radar_high: [40, 50, 60],
    };
    Template {
        blocks_x: 2,
        blocks_y: 2,
        cx: CX,
        cy: CY,
        tiles: vec![Some(t0), None, None, Some(t3)],
        unreadable: Vec::new(),
    }
}

pub(super) fn bytes() -> Vec<u8> {
    let mut out = build(&template(), None).expect("build");
    out.extend_from_slice(b"tail");
    out
}

pub(super) fn opened_tmp(bytes: Vec<u8>) -> PictureViewRust {
    let opened = with_open_request(None, || {
        OPEN_PALETTE.with(|p| *p.borrow_mut() = Some(choice()));
        decode_shp("/tmp/fixture.tem", &bytes, false, &|_, _| {})
    })
    .expect("a TMP is claimed")
    .expect("opens");
    let mut rust = PictureViewRust::default();
    apply_opened(&mut rust, opened);
    rust
}

#[test]
fn a_tmp_opens_as_tile_extra_and_z_layers() {
    let rust = opened_tmp(bytes());
    let d = doc(&rust);
    assert_eq!(rust.source_format, "tmp");
    assert_eq!((d.width, d.height), (60, 60));
    let names: Vec<(&str, bool)> = d
        .layers
        .iter()
        .map(|l| (l.name.as_str(), l.visible))
        .collect();
    assert_eq!(
        names,
        [
            ("Tiles", true),
            ("Extras", true),
            ("Z Tiles", false),
            ("Z Extras", false)
        ]
    );
    assert!(d
        .layers
        .iter()
        .all(|l| !l.background && l.channels.iter().any(|c| c.id == -1)));
    let resource = tmp_resource(d).expect("resource");
    assert_eq!(resource.original, bytes());
    assert_eq!(resource.palette_path, "/pal/unittem.pal");
    assert!(header(d).is_none(), "a TMP is not an SHP sheet");

    // Tile 0's diamond centre is palette index 44, its z 5 is grey 40.
    let unique = unique_colours(&resource.palette);
    let plane = 60 * 60;
    let tiles = layer_rgba(&d.layers[0], plane);
    let at = (15 * 60 + 30) * 4;
    let c = unique.argb[44];
    assert_eq!(
        &tiles[at..at + 4],
        &[(c >> 16) as u8, (c >> 8) as u8, c as u8, 255]
    );
    assert_eq!(
        &layer_rgba(&d.layers[2], plane)[at..at + 4],
        &[40, 40, 40, 255]
    );
    assert_eq!(tiles[3], 0, "outside every diamond is transparent");
}

#[test]
fn the_tile_list_names_cells_fields_and_blocks() {
    let rust = opened_tmp(bytes());
    let tiles = tiles_of(doc(&rust));
    assert_eq!(tiles.len(), 2);
    let t3 = &tiles[1];
    assert_eq!((t3.slot, t3.col, t3.row), (3, 1, 1));
    assert_eq!((t3.x, t3.y), (0, 30));
    assert_eq!((t3.height, t3.land, t3.ramp), (3, 15, 2));
    assert_eq!((t3.radar_low, t3.radar_high), (0x0A141E, 0x28323C));
    assert!(t3.has_z && t3.has_extra && t3.has_extra_z);
    assert_eq!(
        (t3.extra_x, t3.extra_y, t3.extra_w, t3.extra_h),
        (20, 10, 12, 14)
    );
    assert!(!tiles[0].has_extra);
    let (_, t, p) = template_of(doc(&rust)).unwrap();
    assert_eq!(pictura_shp::terrain::tile_at(&t, &p, 30, 45), Some(3));
    assert_eq!(tmp_land_label(15).to_string(), "Cliff");
    assert_eq!(tmp_ramp_label(2).to_string(), "North");
}

#[test]
fn an_shp_with_a_theater_suffix_still_opens_as_an_shp() {
    let shp = super::tests::sprite();
    let opened = decode_shp("/tmp/tree.tem", &shp, false, &|_, _| {})
        .expect("claimed")
        .expect("opens");
    assert_eq!(opened.source_format, "shp");
    assert!(decode_shp("/tmp/x.tem", b"not a sprite at all", false, &|_, _| {}).is_none());
}

#[test]
fn change_palette_recolours_the_colour_layers_by_index() {
    let mut rust = opened_tmp(bytes());
    let mut other = choice();
    for c in other.palette.argb.iter_mut().skip(1) {
        *c ^= 0x0010_2030;
    }
    other.path = "/pal/isotem.pal".into();
    let z_before = doc(&rust).layers[2].clone();
    assert_eq!(tmp_rebind(&mut rust, other.clone()), Ok(true));
    let d = doc(&rust);
    let resource = tmp_resource(d).unwrap();
    assert_eq!(resource.palette_path, "/pal/isotem.pal");
    let unique = unique_colours(&other.palette);
    let tiles = layer_rgba(&d.layers[0], 60 * 60);
    let at = (15 * 60 + 30) * 4;
    let c = unique.argb[44];
    assert_eq!(
        &tiles[at..at + 3],
        &[(c >> 16) as u8, (c >> 8) as u8, c as u8]
    );
    assert_eq!(d.layers[2], z_before, "z layers are not colours");
}

/// `rebind_template` as the recorded edit runs it.
fn tmp_rebind(rust: &mut PictureViewRust, choice: PaletteChoice) -> Result<bool, String> {
    rebind_template(rust, choice)
}
