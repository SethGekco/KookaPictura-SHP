// SPDX-License-Identifier: GPL-3.0-or-later
//! Reshaping a TMP template from the Tiles panel: move a tile to another
//! cell, add or remove a tile, add or remove a tile's extra block. Each is
//! one undoable step: the new shapes go into the `KookaTMP` resource
//! (version 2, see `pictura_shp::terrain::Shape`), the layers are laid out
//! again for the new placement (every tile's pixels carried to where it now
//! is, new tiles and extra blocks transparent, pixels outside every tile
//! shifted with the canvas), and the canvas grows or shrinks to fit. Save
//! then patches the original file: moved headers and slots in place, new
//! blocks and records appended (`pictura_shp::tmp::build_with_layout`).

use super::*;

use pictura_shp::terrain::{shapes_of, ExtraShape, Shape};
use pictura_shp::tmp::{Layout, MAX_EXTRA_SIDE};

const NOT_TMP: &str = "This document is not a TMP template, or its TMP data is damaged.";

/// One reshaping edit. Cells are column and row of the template grid; an
/// extra block's position is relative to its tile's.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(in crate::cxxqt_object) enum TileEdit {
    Move {
        slot: usize,
        col: usize,
        row: usize,
    },
    Add {
        col: usize,
        row: usize,
    },
    Remove {
        slot: usize,
    },
    AddExtra {
        slot: usize,
        dx: i32,
        dy: i32,
        w: usize,
        h: usize,
    },
    RemoveExtra {
        slot: usize,
    },
}

impl TileEdit {
    /// The bridge's integers: op (0 move, 1 add, 2 remove, 3 add extra, 4
    /// remove extra), slot, then the op's values (column and row; or x and
    /// y offset, width and height of the extra block).
    pub(in crate::cxxqt_object) fn from_ints(op: i32, slot: i32, args: &[i32]) -> Option<Self> {
        let n = |v: i32| usize::try_from(v).ok();
        let arg = |k: usize| args.get(k).copied();
        Some(match op {
            0 => TileEdit::Move {
                slot: n(slot)?,
                col: n(arg(0)?)?,
                row: n(arg(1)?)?,
            },
            1 => TileEdit::Add {
                col: n(arg(0)?)?,
                row: n(arg(1)?)?,
            },
            2 => TileEdit::Remove { slot: n(slot)? },
            3 => TileEdit::AddExtra {
                slot: n(slot)?,
                dx: arg(0)?,
                dy: arg(1)?,
                w: n(arg(2)?)?,
                h: n(arg(3)?)?,
            },
            4 => TileEdit::RemoveExtra { slot: n(slot)? },
            _ => return None,
        })
    }

    pub(in crate::cxxqt_object) fn label(self) -> &'static str {
        match self {
            TileEdit::Move { .. } => "Move Tile",
            TileEdit::Add { .. } => "Add Tile",
            TileEdit::Remove { .. } => "Remove Tile",
            TileEdit::AddExtra { .. } => "Add Extra Block",
            TileEdit::RemoveExtra { .. } => "Remove Extra Block",
        }
    }
}

/// The pixel step from one cell to another: a column is half a tile right
/// and down, a row half a tile left and down (what every real template's
/// diamonds follow; see SHP-PLAN.md).
fn cell_step(cx: usize, cy: usize, dc: i64, dr: i64) -> (i32, i32) {
    let (cx, cy) = (cx as i64, cy as i64);
    (((dc - dr) * cx / 2) as i32, ((dc + dr) * cy / 2) as i32)
}

/// What an edit makes of the resource: the new resource and, per old slot
/// that keeps its tile, its new slot.
fn reshape(
    resource: &TmpResource,
    t: &Template,
    edit: TileEdit,
) -> Result<(TmpResource, Vec<(usize, usize)>), String> {
    let (bx, by, cx, cy) = (t.blocks_x, t.blocks_y, t.cx, t.cy);
    let mut shapes = resource.shapes.clone().unwrap_or_else(|| shapes_of(t));
    let mut fields = resource.fields.clone();
    let mut carry: Vec<(usize, usize)> = shapes.iter().map(|(s, _)| (*s, *s)).collect();
    let at =
        |shapes: &Vec<(usize, Shape)>, slot: usize| shapes.iter().position(|(s, _)| *s == slot);
    let live = |shapes: &Vec<(usize, Shape)>, slot: usize| {
        at(shapes, slot).ok_or_else(|| format!("The template has no tile in slot {slot}."))
    };
    let cell = |col: usize, row: usize| -> Result<usize, String> {
        if col >= bx || row >= by {
            return fail(format!(
                "Cell ({col}, {row}) is outside the {bx} x {by} template grid."
            ));
        }
        Ok(row * bx + col)
    };
    let free = |shapes: &Vec<(usize, Shape)>, to: usize| -> Result<(), String> {
        match at(shapes, to) {
            Some(_) => fail(format!(
                "Cell ({}, {}) already holds tile {to}.",
                to % bx,
                to / bx
            )),
            None => Ok(()),
        }
    };
    match edit {
        TileEdit::Move { slot, col, row } => {
            let k = live(&shapes, slot)?;
            let to = cell(col, row)?;
            if to == slot {
                return fail(format!("Tile {slot} is already in that cell."));
            }
            free(&shapes, to)?;
            let (dx, dy) = cell_step(
                cx,
                cy,
                col as i64 - (slot % bx) as i64,
                row as i64 - (slot / bx) as i64,
            );
            let shape = &mut shapes[k];
            shape.0 = to;
            shape.1.x += dx;
            shape.1.y += dy;
            if let Some(e) = shape.1.extra.as_mut() {
                e.x += dx;
                e.y += dy;
            }
            for entry in fields.iter_mut().filter(|(s, _)| *s == slot) {
                entry.0 = to;
            }
            for entry in carry.iter_mut().filter(|(s, _)| *s == slot) {
                entry.1 = to;
            }
        }
        TileEdit::Add { col, row } => {
            let to = cell(col, row)?;
            free(&shapes, to)?;
            // Placed from the first tile by the grid step, with its height
            // (which shifts a tile up) and land and ramp types.
            let &(reference, ref_shape) = shapes.first().ok_or(NOT_TMP)?;
            let (dx, dy) = cell_step(
                cx,
                cy,
                col as i64 - (reference % bx) as i64,
                row as i64 - (reference / bx) as i64,
            );
            shapes.push((
                to,
                Shape {
                    source: None,
                    x: ref_shape.x + dx,
                    y: ref_shape.y + dy,
                    extra: None,
                },
            ));
            let base = fields
                .iter()
                .find(|(s, _)| *s == reference)
                .map(|(_, f)| *f)
                .unwrap_or_default();
            fields.push((
                to,
                TileFields {
                    radar_low: [0; 3],
                    radar_high: [0; 3],
                    ..base
                },
            ));
        }
        TileEdit::Remove { slot } => {
            let k = live(&shapes, slot)?;
            if shapes.len() == 1 {
                return fail("A template needs at least one tile.");
            }
            shapes.remove(k);
            fields.retain(|(s, _)| *s != slot);
            carry.retain(|(s, _)| *s != slot);
        }
        TileEdit::AddExtra { slot, dx, dy, w, h } => {
            let k = live(&shapes, slot)?;
            let side = 1..=MAX_EXTRA_SIDE;
            if !side.contains(&w) || !side.contains(&h) {
                return fail(format!(
                    "An extra block is 1 to {MAX_EXTRA_SIDE} pixels a side."
                ));
            }
            let shape = &mut shapes[k].1;
            if shape.extra.is_some() {
                return fail(format!(
                    "Tile {slot} already has an extra block; remove it first."
                ));
            }
            shape.extra = Some(ExtraShape {
                x: shape.x + dx,
                y: shape.y + dy,
                w,
                h,
                kept: false,
            });
        }
        TileEdit::RemoveExtra { slot } => {
            let k = live(&shapes, slot)?;
            if shapes[k].1.extra.take().is_none() {
                return fail(format!("Tile {slot} has no extra block."));
            }
        }
    }
    shapes.sort_by_key(|(s, _)| *s);
    fields.sort_by_key(|(s, _)| *s);
    let next = TmpResource {
        shapes: Some(shapes),
        fields,
        ..resource.clone()
    };
    Ok((next, carry))
}

/// The layers of `p` laid out again for `np`: each kept tile's region of
/// every plane copied to its new slot's region, and every pixel outside all
/// regions moved with the canvas origin onto the layer of the same name (or
/// the plane's first). Returns the layers and how many of those outside
/// pixels found no place (off the new canvas or under a tile).
fn relayout(
    (t, p, layers): (&Template, &Placement, &[Vec<u8>]),
    (nt, np): (&Template, &Placement),
    carry: &[(usize, usize)],
) -> (Vec<Vec<u8>>, usize) {
    let specs = np.layers();
    let (w, h) = (np.width, np.height);
    let mut out: Vec<Vec<u8>> = specs.iter().map(|_| vec![0u8; w * h * 4]).collect();
    let (sx, sy) = (np.origin_x - p.origin_x, np.origin_y - p.origin_y);
    let mut dropped = 0;
    let mut strays = vec![Vec::new(); specs.len()];
    for (spec, rgba) in p.layers().iter().zip(layers) {
        let target = specs
            .iter()
            .position(|s| s.name == spec.name)
            .or_else(|| specs.iter().position(|s| s.plane == spec.plane));
        for (i, &owner) in p.owners(t, spec).iter().enumerate() {
            if owner != u32::MAX || rgba[i * 4 + 3] == 0 {
                continue;
            }
            let (x, y) = ((i % p.width) as i64 + sx, (i / p.width) as i64 + sy);
            match target {
                Some(k) if (0..w as i64).contains(&x) && (0..h as i64).contains(&y) => {
                    let at = y as usize * w + x as usize;
                    out[k][at * 4..at * 4 + 4].copy_from_slice(&rgba[i * 4..i * 4 + 4]);
                    strays[k].push(at);
                }
                _ => dropped += 1,
            }
        }
        for &(from, to) in carry {
            let Some(rows) = p.rows_on(t, spec, from) else {
                continue;
            };
            let Some((k, new_rows)) = specs.iter().enumerate().find_map(|(k, s)| {
                (s.plane == spec.plane)
                    .then(|| np.rows_on(nt, s, to).map(|r| (k, r)))
                    .flatten()
            }) else {
                continue;
            };
            if new_rows.iter().map(|r| r.2).ne(rows.iter().map(|r| r.2)) {
                continue;
            }
            for (&(y, x, n), &(ny, nx, _)) in rows.iter().zip(&new_rows) {
                let src = (y * p.width + x) * 4;
                let dst = (ny * w + nx) * 4;
                out[k][dst..dst + n * 4].copy_from_slice(&rgba[src..src + n * 4]);
            }
        }
    }
    // An outside pixel that now lies on a region was overwritten by it.
    for (k, spec) in specs.iter().enumerate() {
        let owners = np.owners(nt, spec);
        dropped += strays[k]
            .iter()
            .filter(|&&at| owners[at] != u32::MAX)
            .count();
    }
    (out, dropped)
}

/// The mutation half of every reshaping edit; the outside pixels that had
/// to be dropped. Nothing changes on an error.
pub(in crate::cxxqt_object) fn edit_tiles_rust(
    rust: &mut PictureViewRust,
    edit: TileEdit,
    limits: &Limits,
    allowance: u64,
) -> Result<usize, String> {
    let doc = rust.session.doc.as_ref().ok_or("no document")?;
    let (resource, t, p) = template_of(doc).ok_or(NOT_TMP)?;
    let layers = read_layers(doc, &p)?;
    let (next, carry) = reshape(&resource, &t, edit)?;
    let nt = next.template().ok_or(NOT_TMP)?;
    let np = Placement::new(&nt).ok_or("The tiles would lie too far apart to fit one canvas.")?;
    let specs = np.layers();
    let bytes = |w: usize, h: usize, n: usize| (w * h * 4 * n) as u64;
    let needed = bytes(np.width, np.height, specs.len());
    if needed > bytes(p.width, p.height, p.layers().len()) {
        limits.admit(needed, allowance).map_err(|reason| {
            format!(
                "A {}x{} template canvas of {} layers: {reason}",
                np.width,
                np.height,
                specs.len()
            )
        })?;
    }
    let (out, dropped) = relayout((&t, &p, &layers), (&nt, &np), &carry);
    let doc = rust.session.doc.as_mut().ok_or("no document")?;
    let (w, h) = (np.width as u32, np.height as u32);
    let resized = (w, h) != (doc.width, doc.height);
    if resized {
        pictura_render::resize_canvas_document(doc, w, h, pictura_render::Anchor::TopLeft)
            .map_err(|e| format!("cannot resize the template canvas: {e:?}"))?;
    }
    let mut stack = Vec::with_capacity(specs.len());
    for (spec, rgba) in specs.iter().zip(out) {
        let mut layer = match doc.layers.iter().find(|l| l.name == spec.name) {
            Some(layer) => layer.clone(),
            None => {
                let mut layer = Document::from_rgba(&spec.name, w, h, &[]).layers[0].clone();
                layer.visible = !spec.plane.is_z();
                layer
            }
        };
        store_canvas_rgba(&mut layer, w, h, &rgba);
        stack.push(layer);
    }
    doc.layers = LayerStack::from_layers(stack);
    doc.image_resources = next.store(&doc.image_resources).into();
    rust.layer_target = LayerTarget::for_opened(doc, Some("0".to_string()));
    rust.session.set_selection(None);
    Ok(dropped)
}

/// One reshaping edit from the Tiles panel, recorded under its label. The
/// outside pixels that were dropped, or -1 with the reason kept.
pub(in crate::cxxqt_object) fn tmp_edit_tiles(
    view: Pin<&mut PictureView>,
    op: i32,
    slot: i32,
    args: &[i32],
    allowance: u64,
) -> i32 {
    let Some(edit) = TileEdit::from_ints(op, slot, args) else {
        set_error("That tile edit does not apply here.");
        return -1;
    };
    let limits = Limits::current();
    let mut outcome: Result<usize, String> = fail("no document");
    view.edit(edit.label(), |rust| {
        outcome = if rust.modal_transform_blocks_edit() {
            fail("Finish the transform first.")
        } else {
            edit_tiles_rust(rust, edit, &limits, allowance)
        };
        match outcome {
            Ok(_) => Edit::Full,
            Err(_) => Edit::None,
        }
    });
    match outcome {
        Ok(dropped) => {
            set_error("");
            dropped.min(i32::MAX as usize) as i32
        }
        Err(message) => {
            set_error(&message);
            -1
        }
    }
}

/// How Save would write the document now: "in place", "appended" (new or
/// grown blocks added at the end of the file), "fresh" (laid out afresh),
/// or empty when it cannot be saved as it is.
pub(in crate::cxxqt_object) fn save_layout(doc: &Document) -> &'static str {
    match encode_template_with_layout(doc) {
        Ok((_, Layout::InPlace)) => "in place",
        Ok((_, Layout::Appended)) => "appended",
        Ok((_, Layout::Fresh)) => "fresh",
        Err(_) => "",
    }
}

pub(in crate::cxxqt_object) fn tmp_save_layout(view: &PictureView) -> QString {
    QString::from(view.rust().session.doc.as_ref().map_or("", save_layout))
}
