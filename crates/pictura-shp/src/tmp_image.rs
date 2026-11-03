// SPDX-License-Identifier: GPL-3.0-or-later

//! TMP templates made from an image (File > New TMP from Image, and Save As
//! TMP of an image that is not a template).
//!
//! The image is laid over the template grid: its top-left pixel is the
//! top-left of the grid's bounding box, so the tile of cell `(col, row)`
//! sits at template pixel `((col - row) * cx / 2, (col + row) * cy / 2)`.
//! That is where every one of the 9,744 tiles of the owner's 2,745 real
//! templates sits horizontally; vertically the real files subtract
//! `(height - lowest height) * cy / 2`, which is zero when every tile has
//! the same height, as a new template's tiles do.
//!
//! Each chosen cell becomes a tile; the image's pixels inside its diamond
//! go on the Tiles layer. Pixels outside every diamond become extra blocks
//! only when asked: each goes to the tile whose diamond centre is nearest
//! (in the isometric metric, ties to the lower slot), and a tile's extra
//! block is the bounding box of its pixels. Otherwise they stay on the
//! Tiles layer outside the diamonds, where the save gate's Clip finds them.
//! Pixels that land off the canvas are counted and dropped.

use crate::palette::{fallback_palette, Palette};
use crate::terrain::{render, Placement, Plane};
use crate::tmp::{
    diamond_rows, Template, Tile, TileFields, FLAG_EXTRA, FLAG_Z, MAX_BLOCKS, MAX_EXTRA_SIDE,
    Z_MAX, Z_NONE,
};

/// The z-data a new tile gets.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ZData {
    None,
    /// The depths of flat ground: row `y` of the diamond holds
    /// `cy - 1 - y` (29 down to 1 for a 60x30 tile), what 8,156 of the 8,800
    /// flat (ramp 0) tiles of the owner's real templates hold.
    Flat,
}

/// How an image becomes a template.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ImageSpec {
    /// The tile size: 48x24 (Tiberian Sun) or 60x30 (Red Alert 2).
    pub cx: usize,
    pub cy: usize,
    pub blocks_x: usize,
    pub blocks_y: usize,
    /// Per slot (row by row), whether it holds a tile.
    pub cells: Vec<bool>,
    /// Height, land and ramp of every tile (the radar colors are set from
    /// the pixels afterwards, see [`average_radar`]).
    pub fields: TileFields,
    pub z: ZData,
    /// Keep pixels outside the diamonds as extra blocks.
    pub extras: bool,
}

/// A template made from an image, before its colours become indices.
#[derive(Debug, Clone)]
pub struct Made {
    /// The tiles, z-data and extra blocks; colour indices are all 0 (read
    /// the layers back for them).
    pub template: Template,
    pub placement: Placement,
    /// [`Placement::layers`] as canvas RGBA: the image's colours on the
    /// colour layers, the z-data on the z layers.
    pub layers: Vec<Vec<u8>>,
    /// Non-transparent image pixels outside every diamond and extra block
    /// (on the Tiles layer, for the save gate's Clip).
    pub outside: usize,
    /// Non-transparent image pixels that fell off the canvas.
    pub dropped: usize,
}

/// Where the tile of cell `(col, row)` sits in template pixels.
pub fn cell_position(cx: usize, cy: usize, col: usize, row: usize) -> (i32, i32) {
    let (col, row) = (col as i32, row as i32);
    ((col - row) * cx as i32 / 2, (col + row) * cy as i32 / 2)
}

/// The template x of the image's left edge (its top edge is y 0).
fn image_left(cx: usize, blocks_y: usize) -> i32 {
    -((blocks_y as i32 - 1) * cx as i32 / 2)
}

/// The size of a whole grid's bounding box, which the image is laid over.
pub fn grid_size(cx: usize, cy: usize, blocks_x: usize, blocks_y: usize) -> (usize, usize) {
    (
        (blocks_x + blocks_y) * cx / 2,
        (blocks_x + blocks_y) * cy / 2,
    )
}

/// Is `(cx, cy)` a tile size a game uses?
pub fn valid_tile_size(cx: usize, cy: usize) -> bool {
    matches!((cx, cy), (48, 24) | (60, 30))
}

/// The flat-ground z-data of a `cx` x `cy` tile (a rectangle, [`Z_NONE`]
/// outside the diamond).
pub fn flat_z(cx: usize, cy: usize) -> Vec<u8> {
    let mut rect = vec![Z_NONE; cx * cy];
    for (y, x, w) in diamond_rows(cx, cy) {
        let v = (cy - 1 - y).min(usize::from(Z_MAX)) as u8;
        rect[y * cx + x..y * cx + x + w].fill(v);
    }
    rect
}

/// For each slot of the grid, whether the image has a pixel that is not
/// fully transparent inside that cell's diamond.
pub fn default_cells(
    rgba: &[u8],
    iw: usize,
    ih: usize,
    cx: usize,
    cy: usize,
    blocks_x: usize,
    blocks_y: usize,
) -> Vec<bool> {
    let left = image_left(cx, blocks_y);
    let rows = diamond_rows(cx, cy);
    (0..blocks_x * blocks_y)
        .map(|slot| {
            let (x, y) = cell_position(cx, cy, slot % blocks_x, slot / blocks_x);
            rows.iter().any(|&(ry, rx, w)| {
                let py = y as i64 + ry as i64;
                (0..w).any(|k| {
                    let px = (x - left) as i64 + (rx + k) as i64;
                    (0..iw as i64).contains(&px)
                        && (0..ih as i64).contains(&py)
                        && rgba[(py as usize * iw + px as usize) * 4 + 3] != 0
                })
            })
        })
        .collect()
}

/// Why `spec` cannot make a template, if it cannot.
fn check(spec: &ImageSpec) -> Result<(), String> {
    if !valid_tile_size(spec.cx, spec.cy) {
        return Err(format!(
            "A TMP tile is 48x24 (Tiberian Sun) or 60x30 (Red Alert 2), not {}x{}.",
            spec.cx, spec.cy
        ));
    }
    let side = 1..=MAX_BLOCKS;
    if !side.contains(&spec.blocks_x) || !side.contains(&spec.blocks_y) {
        return Err(format!("A template is 1 to {MAX_BLOCKS} cells a side."));
    }
    if spec.cells.len() != spec.blocks_x * spec.blocks_y {
        return Err("The cell list does not match the grid.".to_string());
    }
    if !spec.cells.contains(&true) {
        return Err("Choose at least one cell to hold a tile.".to_string());
    }
    Ok(())
}

/// The template `spec` makes of the RGBA image `rgba` (`iw` x `ih`).
pub fn from_image(rgba: &[u8], iw: usize, ih: usize, spec: &ImageSpec) -> Result<Made, String> {
    check(spec)?;
    if rgba.len() != iw * ih * 4 {
        return Err("The image is not RGBA of its size.".to_string());
    }
    let (cx, cy) = (spec.cx, spec.cy);
    let mut tiles: Vec<Option<Tile>> = vec![None; spec.cells.len()];
    for (slot, _) in spec.cells.iter().enumerate().filter(|(_, &on)| on) {
        let (x, y) = cell_position(cx, cy, slot % spec.blocks_x, slot / spec.blocks_x);
        let mut tile = Tile::new(x, y, cx, cy);
        tile.fields = spec.fields;
        if spec.z == ZData::Flat {
            tile.z = Some(flat_z(cx, cy));
            tile.flags = FLAG_Z;
        }
        tiles[slot] = Some(tile);
    }
    let left = image_left(cx, spec.blocks_y);
    let live: Vec<usize> = (0..tiles.len()).filter(|&s| tiles[s].is_some()).collect();
    // Which tile's diamond covers each image pixel (diamonds never overlap).
    let mut cover = vec![u32::MAX; iw * ih];
    for &slot in &live {
        let tile = tiles[slot].as_ref().expect("live");
        for (ry, rx, w) in diamond_rows(cx, cy) {
            let py = i64::from(tile.y) + ry as i64;
            for k in 0..w {
                let px = i64::from(tile.x - left) + (rx + k) as i64;
                if (0..iw as i64).contains(&px) && (0..ih as i64).contains(&py) {
                    cover[py as usize * iw + px as usize] = slot as u32;
                }
            }
        }
    }
    // Pixels outside every diamond: the tile whose diamond centre is
    // nearest, when they become extra blocks.
    let mut owner = vec![u32::MAX; iw * ih];
    if spec.extras {
        let mut boxes: Vec<Option<(i64, i64, i64, i64)>> = vec![None; tiles.len()];
        let centres: Vec<(usize, i64, i64)> = live
            .iter()
            .map(|&s| {
                let t = tiles[s].as_ref().expect("live");
                (
                    s,
                    i64::from(t.x) + cx as i64 / 2,
                    i64::from(t.y) + cy as i64 / 2,
                )
            })
            .collect();
        let aspect = (cx / cy) as i64;
        for py in 0..ih {
            for px in 0..iw {
                let i = py * iw + px;
                if cover[i] != u32::MAX || rgba[i * 4 + 3] == 0 {
                    continue;
                }
                let (tx, ty) = (px as i64 + i64::from(left), py as i64);
                let &(slot, _, _) = centres
                    .iter()
                    .min_by_key(|&&(s, mx, my)| {
                        let (dx, dy) = (tx - mx, (ty - my) * aspect);
                        (dx * dx + dy * dy, s)
                    })
                    .expect("at least one tile");
                owner[i] = slot as u32;
                let b = boxes[slot].get_or_insert((tx, ty, tx, ty));
                *b = (b.0.min(tx), b.1.min(ty), b.2.max(tx), b.3.max(ty));
            }
        }
        for (slot, b) in boxes.iter().enumerate() {
            let (Some((x0, y0, x1, y1)), Some(tile)) = (b, tiles[slot].as_mut()) else {
                continue;
            };
            let (w, h) = ((x1 - x0 + 1) as usize, (y1 - y0 + 1) as usize);
            if w > MAX_EXTRA_SIDE || h > MAX_EXTRA_SIDE {
                return Err(format!(
                    "Tile {slot}'s extra block would be {w}x{h} px; an extra block is at most \
                     {MAX_EXTRA_SIDE} px a side."
                ));
            }
            (tile.extra_x, tile.extra_y) = (*x0 as i32, *y0 as i32);
            (tile.extra_w, tile.extra_h) = (w, h);
            tile.extra = Some(vec![0; w * h]);
            tile.flags |= FLAG_EXTRA;
        }
    }
    let template = Template {
        blocks_x: spec.blocks_x,
        blocks_y: spec.blocks_y,
        cx,
        cy,
        tiles,
        unreadable: Vec::new(),
    };
    let placement = Placement::new(&template)
        .ok_or("The template's canvas would be too large for one document.")?;
    let mut layers = render(&template, &placement, &fallback_palette());
    let specs = placement.layers();
    let layer_of = |plane: Plane, group: usize| {
        specs
            .iter()
            .position(|s| s.plane == plane && s.group == group)
            .expect("placed")
    };
    let (mut outside, mut dropped) = (0, 0);
    for py in 0..ih {
        for px in 0..iw {
            let i = py * iw + px;
            if rgba[i * 4 + 3] == 0 {
                continue;
            }
            let x = px as i64 + i64::from(left) + placement.origin_x;
            let y = py as i64 + placement.origin_y;
            if !(0..placement.width as i64).contains(&x)
                || !(0..placement.height as i64).contains(&y)
            {
                dropped += 1;
                continue;
            }
            let layer = if cover[i] != u32::MAX {
                let slot = cover[i] as usize;
                layer_of(Plane::Tiles, placement.tile_group[slot].unwrap_or(0))
            } else if owner[i] != u32::MAX {
                let slot = owner[i] as usize;
                layer_of(Plane::Extras, placement.extra_group[slot].unwrap_or(0))
            } else {
                outside += 1;
                layer_of(Plane::Tiles, 0)
            };
            let at = (y as usize * placement.width + x as usize) * 4;
            layers[layer][at..at + 4].copy_from_slice(&rgba[i * 4..i * 4 + 4]);
        }
    }
    Ok(Made {
        template,
        placement,
        layers,
        outside,
        dropped,
    })
}

/// A tile's radar colors from its pixels: the mean of its diamond's
/// non-transparent colours in the palette as read, as both the low and the
/// high color (black for an empty diamond).
pub fn average_radar(tile: &Tile, palette: &Palette) -> ([u8; 3], [u8; 3]) {
    let mean = crate::unit::average_color(&tile.image, palette).unwrap_or([0; 3]);
    (mean, mean)
}
