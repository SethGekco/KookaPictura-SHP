// Derived from Ochre's C&C addon (`addons/cnc/cnctmp.py`), Copyright (C)
// 2026 Ochre contributors, GPL-3.0-or-later, by the owner of this fork.
// SPDX-License-Identifier: GPL-3.0-or-later

//! TMP documents: where every tile lands on one canvas, its pixels as RGBA
//! layers, the exact way back, and the private `KookaTMP` image resource.
//!
//! The canvas is the template in its own pixel space: each diamond at the
//! position its record names, each extra block at its own position, shifted
//! so nothing is at a negative coordinate. Pixels go on layers by what they
//! are, so nothing ever has to share a pixel:
//!
//! - **Tiles**: the diamonds (they never overlap in a real template; a
//!   template whose diamonds do gets "Tiles 2" and so on).
//! - **Extras**: the extra blocks. They overlap the diamonds by design and,
//!   in some templates, each other, so a block whose rectangle meets another
//!   block's goes on "Extras 2", "Extras 3"...
//! - **Z Tiles**, **Z Extras**...: the z-data of the layer of the same name,
//!   as grey ([`z_rgba`]): depth `v` (0-31) is grey `8 v`, "no data" (255)
//!   is transparent, and an invalid value (32-254) keeps its number as the
//!   blue of an opaque `(255, 0, v)`, so every value round-trips.
//!
//! Colour layers use the SHP sheet's rule: index `i` is the unique colour
//! `U[i]` at alpha 255, index 0 is transparent, anything else is reported as
//! off-palette rather than guessed.

use crate::palette::{nearest, Palette};
use crate::resource::{find_block, set_block};
use crate::sheet::MAX_SHEET_SIDE;
use crate::tmp::{diamond_rows, Template, Tile, TileFields, Z_MAX, Z_NONE};
use std::collections::HashMap;

/// What a layer of a TMP document holds.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Plane {
    Tiles,
    Extras,
    ZTiles,
    ZExtras,
}

impl Plane {
    /// Does the layer hold z-data rather than colour?
    pub fn is_z(self) -> bool {
        matches!(self, Plane::ZTiles | Plane::ZExtras)
    }

    /// Does the layer hold extra blocks (or their z-data)?
    pub fn is_extra(self) -> bool {
        matches!(self, Plane::Extras | Plane::ZExtras)
    }
}

/// One layer of a TMP document.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct LayerSpec {
    pub plane: Plane,
    /// Which of the plane's layers (0 for "Tiles", 1 for "Tiles 2").
    pub group: usize,
    pub name: String,
}

/// Where a template's tiles and extras land on the canvas.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Placement {
    pub width: usize,
    pub height: usize,
    /// Canvas position = file position + origin.
    pub origin_x: i64,
    pub origin_y: i64,
    pub cx: usize,
    pub cy: usize,
    /// Per slot, the Tiles layer of its diamond.
    pub tile_group: Vec<Option<usize>>,
    /// Per slot, the Extras layer of its extra block.
    pub extra_group: Vec<Option<usize>>,
    pub tile_groups: usize,
    pub extra_groups: usize,
}

/// Canvas rows `(y, x, width)` covered by one tile or extra block.
pub type Rows = Vec<(usize, usize, usize)>;

/// Put each region (a list of canvas rows) on the first group where it
/// meets no pixel of another region; the group of each.
fn assign(regions: &[Option<Rows>], width: usize, height: usize) -> (Vec<Option<usize>>, usize) {
    let mut taken: Vec<Vec<bool>> = Vec::new();
    let mut out = Vec::with_capacity(regions.len());
    for region in regions {
        let Some(rows) = region else {
            out.push(None);
            continue;
        };
        let free = |mask: &Vec<bool>| {
            rows.iter()
                .all(|&(y, x, w)| !mask[y * width + x..y * width + x + w].contains(&true))
        };
        let group = match taken.iter().position(free) {
            Some(g) => g,
            None => {
                taken.push(vec![false; width * height]);
                taken.len() - 1
            }
        };
        for &(y, x, w) in rows {
            taken[group][y * width + x..y * width + x + w].fill(true);
        }
        out.push(Some(group));
    }
    (out, taken.len())
}

impl Placement {
    /// The placement of `t`, or `None` when its canvas would be wider or
    /// taller than [`MAX_SHEET_SIDE`] (a damaged or hostile file).
    pub fn new(t: &Template) -> Option<Placement> {
        let (cx, cy) = (t.cx as i64, t.cy as i64);
        let (mut x0, mut y0, mut x1, mut y1) = (i64::MAX, i64::MAX, i64::MIN, i64::MIN);
        let mut grow = |x: i64, y: i64, w: i64, h: i64| {
            x0 = x0.min(x);
            y0 = y0.min(y);
            x1 = x1.max(x + w);
            y1 = y1.max(y + h);
        };
        for (_, tile) in t.live() {
            grow(tile.x.into(), tile.y.into(), cx, cy);
            if tile.extra.is_some() {
                let (w, h) = (tile.extra_w as i64, tile.extra_h as i64);
                grow(tile.extra_x.into(), tile.extra_y.into(), w, h);
            }
        }
        let (width, height) = (x1 - x0, y1 - y0);
        let max = MAX_SHEET_SIDE as i64;
        if !(1..=max).contains(&width) || !(1..=max).contains(&height) {
            return None;
        }
        let mut p = Placement {
            width: width as usize,
            height: height as usize,
            origin_x: -x0,
            origin_y: -y0,
            cx: t.cx,
            cy: t.cy,
            tile_group: Vec::new(),
            extra_group: Vec::new(),
            tile_groups: 0,
            extra_groups: 0,
        };
        let diamonds: Vec<_> = t
            .tiles
            .iter()
            .map(|tile| tile.as_ref().map(|tile| p.diamond(tile)))
            .collect();
        let extras: Vec<_> = t
            .tiles
            .iter()
            .map(|tile| tile.as_ref().and_then(|tile| p.extra_rows(tile)))
            .collect();
        (p.tile_group, p.tile_groups) = assign(&diamonds, p.width, p.height);
        (p.extra_group, p.extra_groups) = assign(&extras, p.width, p.height);
        Some(p)
    }

    /// The canvas position of a tile's diamond rectangle.
    pub fn tile_origin(&self, tile: &Tile) -> (usize, usize) {
        (
            (i64::from(tile.x) + self.origin_x) as usize,
            (i64::from(tile.y) + self.origin_y) as usize,
        )
    }

    /// The canvas position of a tile's extra block.
    pub fn extra_origin(&self, tile: &Tile) -> (usize, usize) {
        (
            (i64::from(tile.extra_x) + self.origin_x) as usize,
            (i64::from(tile.extra_y) + self.origin_y) as usize,
        )
    }

    /// A tile's diamond as canvas rows `(y, x, width)`.
    pub fn diamond(&self, tile: &Tile) -> Vec<(usize, usize, usize)> {
        let (ox, oy) = self.tile_origin(tile);
        diamond_rows(self.cx, self.cy)
            .into_iter()
            .map(|(y, x, w)| (oy + y, ox + x, w))
            .collect()
    }

    /// A tile's extra block as canvas rows, when it has one.
    pub fn extra_rows(&self, tile: &Tile) -> Option<Rows> {
        tile.extra.as_ref()?;
        let (ox, oy) = self.extra_origin(tile);
        Some(
            (0..tile.extra_h)
                .map(|y| (oy + y, ox, tile.extra_w))
                .collect(),
        )
    }

    /// The document's layers, bottom first: Tiles, Extras, then the z
    /// layers in the same order.
    pub fn layers(&self) -> Vec<LayerSpec> {
        let named = |plane: Plane, base: &'static str, count: usize| {
            (0..count).map(move |group| LayerSpec {
                plane,
                group,
                name: if group == 0 {
                    base.to_string()
                } else {
                    format!("{base} {}", group + 1)
                },
            })
        };
        named(Plane::Tiles, "Tiles", self.tile_groups)
            .chain(named(Plane::Extras, "Extras", self.extra_groups))
            .chain(named(Plane::ZTiles, "Z Tiles", self.tile_groups))
            .chain(named(Plane::ZExtras, "Z Extras", self.extra_groups))
            .collect()
    }

    /// The canvas rows of slot `slot` on `spec`'s layer, if it has any there.
    pub fn rows_on(&self, t: &Template, spec: &LayerSpec, slot: usize) -> Option<Rows> {
        let tile = t.tiles.get(slot)?.as_ref()?;
        if spec.plane.is_extra() {
            (self.extra_group[slot] == Some(spec.group)).then(|| self.extra_rows(tile))?
        } else {
            (self.tile_group[slot] == Some(spec.group)).then(|| self.diamond(tile))
        }
    }

    /// For each canvas pixel of `spec`'s layer, the slot whose diamond or
    /// extra covers it (`u32::MAX`: none).
    pub fn owners(&self, t: &Template, spec: &LayerSpec) -> Vec<u32> {
        let mut out = vec![u32::MAX; self.width * self.height];
        for slot in 0..t.tiles.len() {
            for (y, x, w) in self.rows_on(t, spec, slot).unwrap_or_default() {
                out[y * self.width + x..y * self.width + x + w].fill(slot as u32);
            }
        }
        out
    }
}

/// A z value as a z-layer pixel.
pub fn z_rgba(v: u8) -> [u8; 4] {
    match v {
        Z_NONE => [0, 0, 0, 0],
        0..=Z_MAX => [v * 8, v * 8, v * 8, 255],
        _ => [255, 0, v, 255],
    }
}

/// A z-layer pixel as a z value, or `None` when it is none of
/// [`z_rgba`]'s colours.
pub fn rgba_z(px: [u8; 4]) -> Option<u8> {
    match px {
        [_, _, _, 0] => Some(Z_NONE),
        [g, g2, g3, 255] if g == g2 && g == g3 && g % 8 == 0 && g / 8 <= Z_MAX => Some(g / 8),
        [255, 0, v, 255] if v > Z_MAX && v != Z_NONE => Some(v),
        _ => None,
    }
}

/// The z value nearest a pixel: transparent under alpha 128, else the
/// nearest depth to its grey.
pub fn snap_z(px: [u8; 4]) -> u8 {
    if let Some(v) = rgba_z(px) {
        return v;
    }
    if px[3] < 128 {
        return Z_NONE;
    }
    let grey = (u32::from(px[0]) * 299 + u32::from(px[1]) * 587 + u32::from(px[2]) * 114) / 1000;
    ((grey + 4) / 8).min(u32::from(Z_MAX)) as u8
}

fn put(rgba: &mut [u8], at: usize, px: [u8; 4]) {
    rgba[at * 4..at * 4 + 4].copy_from_slice(&px);
}

fn get(rgba: &[u8], at: usize) -> [u8; 4] {
    [
        rgba[at * 4],
        rgba[at * 4 + 1],
        rgba[at * 4 + 2],
        rgba[at * 4 + 3],
    ]
}

fn colour(index: u8, unique: &Palette) -> [u8; 4] {
    if index == 0 {
        return [0; 4];
    }
    let c = unique.argb[usize::from(index)];
    [(c >> 16) as u8, (c >> 8) as u8, c as u8, 255]
}

/// The data a layer shows for one tile, as rows matching `rows_on`.
fn tile_data<'a>(tile: &'a Tile, plane: Plane, cx: usize, cy: usize) -> Option<Vec<&'a [u8]>> {
    let rect = |data: &'a [u8], w: usize, h: usize| -> Vec<&'a [u8]> {
        (0..h).map(|y| &data[y * w..y * w + w]).collect()
    };
    let diamond = |data: &'a [u8]| -> Vec<&'a [u8]> {
        diamond_rows(cx, cy)
            .into_iter()
            .map(|(y, x, w)| &data[y * cx + x..y * cx + x + w])
            .collect()
    };
    match plane {
        Plane::Tiles => Some(diamond(&tile.image)),
        Plane::ZTiles => tile.z.as_deref().map(diamond),
        Plane::Extras => tile
            .extra
            .as_deref()
            .map(|e| rect(e, tile.extra_w, tile.extra_h)),
        Plane::ZExtras => tile
            .extra_z
            .as_deref()
            .map(|e| rect(e, tile.extra_w, tile.extra_h)),
    }
}

/// Every layer of [`Placement::layers`] as straight RGBA, canvas-sized.
pub fn render(t: &Template, p: &Placement, unique: &Palette) -> Vec<Vec<u8>> {
    p.layers()
        .iter()
        .map(|spec| {
            let mut rgba = vec![0u8; p.width * p.height * 4];
            for (slot, tile) in t.live() {
                let (Some(rows), Some(data)) = (
                    p.rows_on(t, spec, slot),
                    tile_data(tile, spec.plane, p.cx, p.cy),
                ) else {
                    continue;
                };
                for (&(y, x, _), row) in rows.iter().zip(data) {
                    for (k, &v) in row.iter().enumerate() {
                        let px = if spec.plane.is_z() {
                            z_rgba(v)
                        } else {
                            colour(v, unique)
                        };
                        put(&mut rgba, y * p.width + x + k, px);
                    }
                }
            }
            rgba
        })
        .collect()
}

/// What reading the layers back found.
#[derive(Debug, Clone, Default)]
pub struct Scan {
    /// The template with the layers' pixels (fields untouched).
    pub template: Option<Template>,
    /// Colour pixels that are neither transparent nor an opaque `U[i]`.
    pub off_palette: usize,
    /// Z pixels that are none of [`z_rgba`]'s colours.
    pub off_scale: usize,
    /// Slots holding either of those.
    pub off_slots: Vec<usize>,
    /// Pixels drawn where no tile or extra of that layer is.
    pub outside: usize,
    /// The first off-palette / off-scale pixel and the first outside pixel.
    pub first_off: Option<(usize, usize)>,
    pub first_outside: Option<(usize, usize)>,
}

/// Read the layers (in [`Placement::layers`] order) back into `t`'s tiles.
/// A tile without z-data or extra z-data gains it when its z layer holds a
/// depth inside the tile's region.
pub fn read_back(
    t: &Template,
    p: &Placement,
    layers: &[&[u8]],
    inverse: &HashMap<u32, u8>,
) -> Scan {
    let mut scan = Scan::default();
    let mut out = t.clone();
    let (cx, cy) = (p.cx, p.cy);
    let mut off = vec![false; t.tiles.len()];
    for (spec, rgba) in p.layers().iter().zip(layers) {
        let owners = p.owners(t, spec);
        for (i, &owner) in owners.iter().enumerate() {
            if owner == u32::MAX && rgba[i * 4 + 3] != 0 {
                scan.outside += 1;
                scan.first_outside.get_or_insert((i % p.width, i / p.width));
            }
        }
        for (slot, tile) in out.tiles.iter_mut().enumerate() {
            let (Some(tile), Some(rows)) = (tile.as_mut(), p.rows_on(t, spec, slot)) else {
                continue;
            };
            let mut values = Vec::with_capacity(rows.iter().map(|r| r.2).sum());
            for &(y, x, w) in &rows {
                for k in 0..w {
                    let at = y * p.width + x + k;
                    let px = get(rgba, at);
                    let v = if spec.plane.is_z() {
                        rgba_z(px)
                    } else if px[3] == 0 {
                        Some(0)
                    } else if px[3] == 255 {
                        inverse
                            .get(&u32::from_be_bytes([0, px[0], px[1], px[2]]))
                            .copied()
                    } else {
                        None
                    };
                    let v = v.unwrap_or_else(|| {
                        if spec.plane.is_z() {
                            scan.off_scale += 1;
                        } else {
                            scan.off_palette += 1;
                        }
                        off[slot] = true;
                        scan.first_off.get_or_insert((at % p.width, at / p.width));
                        if spec.plane.is_z() {
                            Z_NONE
                        } else {
                            0
                        }
                    });
                    values.push(v);
                }
            }
            store(tile, spec.plane, &values, cx, cy);
        }
    }
    scan.off_slots = (0..off.len()).filter(|&s| off[s]).collect();
    scan.template = Some(out);
    scan
}

/// Put a region's values (row by row, as `rows_on` lists them) into a tile.
fn store(tile: &mut Tile, plane: Plane, values: &[u8], cx: usize, cy: usize) {
    let into_diamond = |rect: &mut Vec<u8>| {
        let mut i = 0;
        for (y, x, w) in diamond_rows(cx, cy) {
            rect[y * cx + x..y * cx + x + w].copy_from_slice(&values[i..i + w]);
            i += w;
        }
    };
    let has_depth = values.iter().any(|&v| v != Z_NONE);
    match plane {
        Plane::Tiles => into_diamond(&mut tile.image),
        Plane::ZTiles => {
            if tile.z.is_none() && has_depth {
                tile.z = Some(vec![Z_NONE; cx * cy]);
            }
            if let Some(z) = tile.z.as_mut() {
                into_diamond(z);
            }
        }
        Plane::Extras => tile.extra = Some(values.to_vec()),
        Plane::ZExtras => {
            if tile.extra_z.is_some() || has_depth {
                tile.extra_z = Some(values.to_vec());
            }
        }
    }
}

/// Make every pixel outside the regions of its layer transparent; the
/// pixels changed.
pub fn clip_outside(t: &Template, p: &Placement, layers: &mut [Vec<u8>]) -> usize {
    let mut changed = 0;
    for (spec, rgba) in p.layers().iter().zip(layers.iter_mut()) {
        for (i, &owner) in p.owners(t, spec).iter().enumerate() {
            if owner == u32::MAX && rgba[i * 4 + 3] != 0 {
                put(rgba, i, [0; 4]);
                changed += 1;
            }
        }
    }
    changed
}

/// Snap every off-palette colour pixel to the nearest palette index (alpha
/// under 128 is index 0; the match is against the palette as read, see
/// `sheet::snap`) and every off-scale z pixel to [`snap_z`], inside the
/// regions; the pixels changed.
pub fn snap(
    t: &Template,
    p: &Placement,
    layers: &mut [Vec<u8>],
    palette: &Palette,
    unique: &Palette,
) -> usize {
    let inverse = crate::sheet::inverse(unique);
    let mut changed = 0;
    for (spec, rgba) in p.layers().iter().zip(layers.iter_mut()) {
        for (i, &owner) in p.owners(t, spec).iter().enumerate() {
            if owner == u32::MAX {
                continue;
            }
            let px = get(rgba, i);
            let fixed = if spec.plane.is_z() {
                if rgba_z(px).is_some() {
                    continue;
                }
                z_rgba(snap_z(px))
            } else {
                let exact = px[3] == 0
                    || (px[3] == 255
                        && inverse.contains_key(&u32::from_be_bytes([0, px[0], px[1], px[2]])));
                if exact {
                    continue;
                }
                let index = if px[3] < 128 {
                    0
                } else {
                    nearest(palette, u32::from_be_bytes([255, px[0], px[1], px[2]]))
                };
                colour(index, unique)
            };
            if fixed != px {
                put(rgba, i, fixed);
                changed += 1;
            }
        }
    }
    changed
}

/// Which slot's diamond (else extra block) covers canvas pixel `(x, y)`.
pub fn tile_at(t: &Template, p: &Placement, x: usize, y: usize) -> Option<usize> {
    let hit = |rows: Vec<(usize, usize, usize)>| {
        rows.iter()
            .any(|&(ry, rx, w)| ry == y && (rx..rx + w).contains(&x))
    };
    let live: Vec<(usize, &Tile)> = t.live().collect();
    live.iter()
        .find(|(_, tile)| hit(p.diamond(tile)))
        .or_else(|| {
            live.iter()
                .find(|(_, tile)| p.extra_rows(tile).is_some_and(hit))
        })
        .map(|(slot, _)| *slot)
}

/// The image-resource id of the `KookaTMP` block.
pub const RESOURCE_ID: u16 = 4849;
/// The block's Pascal name.
pub const RESOURCE_NAME: &str = "KookaTMP";
const MAGIC: &[u8; 4] = b"KTMP";

/// The `KookaTMP` block: everything a TMP document needs beyond its pixels.
/// Its data, integers big-endian: magic `KTMP`, version 1 (u16), the palette
/// as 256 `0xAARRGGBB`, the palette name and path (u16 length + UTF-8 each),
/// the original file (u32 length + bytes), then the current fields of every
/// tile: u16 count, per tile u16 slot, height, land, ramp, radar low (3),
/// radar high (3).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct TmpResource {
    pub palette: Palette,
    pub palette_path: String,
    pub original: Vec<u8>,
    pub fields: Vec<(usize, TileFields)>,
}

impl TmpResource {
    pub fn encode(&self) -> Vec<u8> {
        let mut out = Vec::with_capacity(1100 + self.original.len() + 11 * self.fields.len());
        out.extend_from_slice(MAGIC);
        out.extend_from_slice(&1u16.to_be_bytes());
        for c in self.palette.argb {
            out.extend_from_slice(&c.to_be_bytes());
        }
        for s in [&self.palette.name, &self.palette_path] {
            let s = &s.as_bytes()[..s.len().min(usize::from(u16::MAX))];
            out.extend_from_slice(&(s.len() as u16).to_be_bytes());
            out.extend_from_slice(s);
        }
        out.extend_from_slice(&(self.original.len() as u32).to_be_bytes());
        out.extend_from_slice(&self.original);
        out.extend_from_slice(&(self.fields.len().min(usize::from(u16::MAX)) as u16).to_be_bytes());
        for (slot, f) in &self.fields {
            out.extend_from_slice(&(*slot as u16).to_be_bytes());
            out.extend_from_slice(&[f.height, f.land, f.ramp]);
            out.extend_from_slice(&f.radar_low);
            out.extend_from_slice(&f.radar_high);
        }
        out
    }

    pub fn decode(data: &[u8]) -> Option<TmpResource> {
        if data.get(..4)? != MAGIC || data.get(4..6)? != [0, 1] {
            return None;
        }
        let mut palette = Palette {
            argb: [0; 256],
            name: String::new(),
        };
        let mut at = 6;
        for c in palette.argb.iter_mut() {
            *c = u32::from_be_bytes(data.get(at..at + 4)?.try_into().ok()?);
            at += 4;
        }
        let string = |at: &mut usize| -> Option<String> {
            let len = usize::from(u16::from_be_bytes(data.get(*at..*at + 2)?.try_into().ok()?));
            let s = String::from_utf8_lossy(data.get(*at + 2..*at + 2 + len)?).into_owned();
            *at += 2 + len;
            Some(s)
        };
        palette.name = string(&mut at)?;
        let palette_path = string(&mut at)?;
        let len = u32::from_be_bytes(data.get(at..at + 4)?.try_into().ok()?) as usize;
        let original = data.get(at + 4..at + 4 + len)?.to_vec();
        at += 4 + len;
        let count = usize::from(u16::from_be_bytes(data.get(at..at + 2)?.try_into().ok()?));
        at += 2;
        let mut fields = Vec::with_capacity(count);
        for _ in 0..count {
            let e = data.get(at..at + 11)?;
            let slot = usize::from(u16::from_be_bytes([e[0], e[1]]));
            fields.push((
                slot,
                TileFields {
                    height: e[2],
                    land: e[3],
                    ramp: e[4],
                    radar_low: [e[5], e[6], e[7]],
                    radar_high: [e[8], e[9], e[10]],
                },
            ));
            at += 11;
        }
        Some(TmpResource {
            palette,
            palette_path,
            original,
            fields,
        })
    }

    /// The block in an image-resource section, when there is one.
    pub fn find(section: &[u8]) -> Option<TmpResource> {
        find_block(section, RESOURCE_ID).and_then(TmpResource::decode)
    }

    /// `section` with this block set (every other block kept).
    pub fn store(&self, section: &[u8]) -> Vec<u8> {
        set_block(section, RESOURCE_ID, RESOURCE_NAME, &self.encode())
    }

    /// The template the original file holds, with this block's fields.
    pub fn template(&self) -> Option<Template> {
        let mut t = crate::tmp::parse(&self.original).ok()?;
        for (slot, fields) in &self.fields {
            if let Some(Some(tile)) = t.tiles.get_mut(*slot) {
                tile.fields = *fields;
            }
        }
        Some(t)
    }
}

/// The fields of every tile of `t`, for a new block.
pub fn fields_of(t: &Template) -> Vec<(usize, TileFields)> {
    t.live().map(|(slot, tile)| (slot, tile.fields)).collect()
}
