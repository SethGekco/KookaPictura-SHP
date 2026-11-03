//! TMP documents: placement, layers and the way back. Every template here
//! is generated in code.

use pictura_shp::palette::{fallback_palette, unique_colours, Palette};
use pictura_shp::sheet::inverse;
use pictura_shp::terrain::{
    clip_outside, fields_of, read_back, render, rgba_z, snap, snap_z, tile_at, z_rgba, Placement,
    Plane, TmpResource,
};
use pictura_shp::tmp::{build, diamond_rows, Template, Tile, TileFields, Z_NONE};

const CX: usize = 60;
const CY: usize = 30;

fn diamond_fill(v: u8, fill: u8) -> Vec<u8> {
    let mut rect = vec![fill; CX * CY];
    for (y, x, w) in diamond_rows(CX, CY) {
        rect[y * CX + x..y * CX + x + w].fill(v);
    }
    rect
}

/// Three tiles in a row of a 3 x 1 template; tile 0 has z-data, tiles 1 and
/// 2 have extra blocks that overlap each other above the template (so they
/// need two Extras layers); tile 2 also has extra z-data.
fn template() -> Template {
    let mut t0 = Tile::new(0, 0, CX, CY);
    t0.image = diamond_fill(5, 0);
    t0.z = Some(diamond_fill(3, Z_NONE));
    let mut t1 = Tile::new(30, 15, CX, CY);
    t1.image = diamond_fill(6, 0);
    t1.extra_x = 40;
    t1.extra_y = -20;
    (t1.extra_w, t1.extra_h) = (10, 25);
    t1.extra = Some((0..250).map(|i| (i % 3) as u8 * 7).collect());
    let mut t2 = Tile::new(60, 30, CX, CY);
    t2.image = diamond_fill(7, 0);
    t2.z = Some(diamond_fill(31, Z_NONE));
    t2.extra_x = 45;
    t2.extra_y = -10;
    (t2.extra_w, t2.extra_h) = (8, 8);
    t2.extra = Some(vec![9; 64]);
    t2.extra_z = Some(
        (0..64)
            .map(|i| if i == 0 { 200 } else { (i % 32) as u8 })
            .collect(),
    );
    t2.fields = TileFields {
        height: 2,
        land: 15,
        ramp: 3,
        radar_low: [1, 2, 3],
        radar_high: [4, 5, 6],
    };
    Template {
        blocks_x: 3,
        blocks_y: 1,
        cx: CX,
        cy: CY,
        tiles: vec![Some(t0), Some(t1), Some(t2)],
        unreadable: Vec::new(),
    }
}

fn palette() -> (Palette, Palette) {
    let p = fallback_palette();
    let u = unique_colours(&p);
    (p, u)
}

#[test]
fn the_canvas_holds_every_tile_and_extra() {
    let p = Placement::new(&template()).unwrap();
    assert_eq!(
        (p.origin_x, p.origin_y),
        (0, 20),
        "the extra 20 px above sets the origin"
    );
    assert_eq!((p.width, p.height), (120, 80));
    assert_eq!(p.tile_groups, 1, "diamonds never overlap");
    assert_eq!(
        p.extra_groups, 2,
        "the two extras overlap, so they need two layers"
    );
    let names: Vec<String> = p.layers().into_iter().map(|l| l.name).collect();
    assert_eq!(
        names,
        [
            "Tiles",
            "Extras",
            "Extras 2",
            "Z Tiles",
            "Z Extras",
            "Z Extras 2"
        ]
    );
    assert!(p.layers()[3].plane.is_z());
}

#[test]
fn z_values_round_trip_through_their_colours() {
    for v in 0..=255u8 {
        assert_eq!(rgba_z(z_rgba(v)), Some(v));
    }
    assert_eq!(z_rgba(31), [248, 248, 248, 255]);
    assert_eq!(z_rgba(Z_NONE)[3], 0);
    assert_eq!(rgba_z([100, 100, 100, 255]), None, "not on the scale");
    assert_eq!(snap_z([100, 100, 100, 255]), 13);
    assert_eq!(snap_z([255, 255, 255, 255]), 31);
    assert_eq!(snap_z([9, 9, 9, 60]), Z_NONE);
}

#[test]
fn layers_read_back_to_the_same_template() {
    let t = template();
    let p = Placement::new(&t).unwrap();
    let (_, u) = palette();
    let layers = render(&t, &p, &u);
    let refs: Vec<&[u8]> = layers.iter().map(Vec::as_slice).collect();
    let scan = read_back(&t, &p, &refs, &inverse(&u));
    assert_eq!((scan.off_palette, scan.off_scale, scan.outside), (0, 0, 0));
    assert_eq!(scan.template.unwrap(), t);
    // Index 0 inside the extra is transparent on its layer.
    let (ex, ey) = p.extra_origin(t.tiles[1].as_ref().unwrap());
    assert_eq!(layers[1][(ey * p.width + ex) * 4 + 3], 0);
}

#[test]
fn a_painted_pixel_lands_in_its_tile_and_the_rest_are_reported() {
    let t = template();
    let p = Placement::new(&t).unwrap();
    let (palette, u) = palette();
    let mut layers = render(&t, &p, &u);
    let (ox, oy) = p.tile_origin(t.tiles[0].as_ref().unwrap());
    let at = ((oy + 15) * p.width + ox + 30) * 4;
    let c = u.argb[42];
    layers[0][at..at + 4].copy_from_slice(&[(c >> 16) as u8, (c >> 8) as u8, c as u8, 255]);
    // A soft pixel inside tile 2's diamond, a pixel outside every diamond,
    // and an off-scale z pixel in tile 0.
    let (tx, ty) = p.tile_origin(t.tiles[2].as_ref().unwrap());
    let soft = ((ty + 15) * p.width + tx + 20) * 4;
    layers[0][soft..soft + 4].copy_from_slice(&[10, 20, 30, 128]);
    layers[0][0..4].copy_from_slice(&[1, 2, 3, 255]);
    layers[3][at..at + 4].copy_from_slice(&[100, 100, 100, 255]);
    let refs: Vec<&[u8]> = layers.iter().map(Vec::as_slice).collect();
    let scan = read_back(&t, &p, &refs, &inverse(&u));
    assert_eq!(scan.off_palette, 1);
    assert_eq!(scan.off_scale, 1);
    assert_eq!(scan.outside, 1);
    assert_eq!(scan.off_slots, vec![0, 2]);
    assert_eq!(scan.first_outside, Some((0, 0)));
    assert_eq!(
        scan.template.unwrap().tiles[0].as_ref().unwrap().image[15 * CX + 30],
        42
    );

    assert_eq!(clip_outside(&t, &p, &mut layers), 1);
    assert_eq!(snap(&t, &p, &mut layers, &palette, &u), 2);
    let refs: Vec<&[u8]> = layers.iter().map(Vec::as_slice).collect();
    let fixed = read_back(&t, &p, &refs, &inverse(&u));
    assert_eq!(
        (fixed.off_palette, fixed.off_scale, fixed.outside),
        (0, 0, 0)
    );
    let tile0 = fixed.template.as_ref().unwrap().tiles[0].clone().unwrap();
    assert_eq!(tile0.z.unwrap()[15 * CX + 30], 13);
}

#[test]
fn painting_z_into_a_tile_without_any_gives_it_z_data() {
    let t = template();
    let p = Placement::new(&t).unwrap();
    let (_, u) = palette();
    let mut layers = render(&t, &p, &u);
    let (ox, oy) = p.tile_origin(t.tiles[1].as_ref().unwrap());
    let at = ((oy + 10) * p.width + ox + 30) * 4;
    layers[3][at..at + 4].copy_from_slice(&z_rgba(9));
    let refs: Vec<&[u8]> = layers.iter().map(Vec::as_slice).collect();
    let back = read_back(&t, &p, &refs, &inverse(&u)).template.unwrap();
    let z = back.tiles[1].as_ref().unwrap().z.clone().unwrap();
    assert_eq!(z[10 * CX + 30], 9);
    assert_eq!(z.iter().filter(|&&v| v != Z_NONE).count(), 1);
    let bytes = build(&back, None).unwrap();
    let saved = pictura_shp::tmp::parse(&bytes).unwrap();
    let tile = saved.tiles[1].as_ref().unwrap();
    assert_eq!(
        tile.flags & pictura_shp::tmp::FLAG_Z,
        pictura_shp::tmp::FLAG_Z
    );
    assert_eq!(tile.z.as_ref(), Some(&z));
}

#[test]
fn tile_at_finds_diamonds_before_extras() {
    let t = template();
    let p = Placement::new(&t).unwrap();
    let (ox, oy) = p.tile_origin(t.tiles[2].as_ref().unwrap());
    assert_eq!(tile_at(&t, &p, ox + 30, oy + 15), Some(2));
    assert_eq!(tile_at(&t, &p, 0, 0), None);
    let (ex, ey) = p.extra_origin(t.tiles[1].as_ref().unwrap());
    assert_eq!(tile_at(&t, &p, ex, ey), Some(1));
}

#[test]
fn the_resource_keeps_palette_original_and_fields() {
    let t = template();
    let original = build(&t, None).unwrap();
    let parsed = pictura_shp::tmp::parse(&original).unwrap();
    let mut resource = TmpResource {
        palette: fallback_palette(),
        palette_path: "/x/isotem.pal".into(),
        original,
        fields: fields_of(&parsed),
    };
    resource.palette.name = "isotem.pal".into();
    let section = resource.store(b"8BIM\x03\xed\x00\x00\x00\x00\x00\x02ab");
    assert!(
        section.starts_with(b"8BIM\x03\xed"),
        "other blocks are kept"
    );
    let back = TmpResource::find(&section).unwrap();
    assert_eq!(back, resource);
    assert_eq!(back.template().unwrap(), parsed);
    resource.fields[2].1.height = 4;
    assert_eq!(
        resource.template().unwrap().tiles[2]
            .as_ref()
            .unwrap()
            .fields
            .height,
        4
    );
    assert!(Placement::new(&parsed).unwrap().layers()[0].plane == Plane::Tiles);
}
