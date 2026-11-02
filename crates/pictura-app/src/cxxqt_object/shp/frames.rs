// SPDX-License-Identifier: GPL-3.0-or-later
//! Frame operations on SHP documents: insert blank frames, duplicate,
//! delete, move, reverse.
//!
//! A **whole** sheet is reflowed cell by cell (pixels copied exactly, so
//! even off-palette work travels with its frame) and the resource's frame
//! origins are rewritten in the same recorded edit, so undo and redo restore
//! the sheet, its layout and the origins together.
//!
//! A **paged** sheet (a window of a big sprite) is rebuilt like a page
//! change: the window must be on the palette with clean gutters (its frames
//! may leave the sheet for the frame store), the operation runs over the
//! whole frame list without decoding untouched frames, and History starts
//! again at the new window. Recording it would keep a copy of the resource,
//! which holds the whole original file, in every undo state.

use super::*;

use pictura_shp::encode_rle;
use pictura_shp::frames::FrameOp;
use pictura_shp::resource::{encode_parts, origins, stored, stored_canvas, Origins};
use pictura_shp::sheet::{gutter_pixels, reflow, window_start};
use std::collections::BTreeMap;

/// The operation `name` (`insert`, `duplicate`, `delete`, `move`, `reverse`)
/// from the shell's integers.
pub(in crate::cxxqt_object) fn parse_op(
    name: &str,
    first: i32,
    count: i32,
    to: i32,
) -> Option<FrameOp> {
    let n = |v: i32| usize::try_from(v).ok();
    FrameOp::parse(name, n(first)?, n(count)?, n(to)?)
}

/// The sheet after `op`: the first frame and layout of the new window (the
/// whole frame list for a whole sheet; for a paged one, the window of the
/// same size that holds the frame the operation focuses), and the frame
/// order.
fn next_sheet(header: &Header, op: FrameOp) -> Result<(usize, Layout, Vec<Option<usize>>), String> {
    let order = op
        .order(header.total)
        .ok_or("That frame operation does not apply here.")?;
    let total = order.len();
    let l = &header.layout;
    let (first, count) = if header.paged() {
        let span = header.span.clamp(1, total);
        let first = window_start(op.focus(total), span);
        (first, span.min(total - first))
    } else {
        (0, total)
    };
    let layout = Layout::new(l.width, l.height, count, l.gutter)
        .ok_or("That many frames do not fit one sheet.")?;
    Ok((first, layout, order))
}

/// What the sheet after `op` needs, measured against the size limit.
pub(in crate::cxxqt_object) fn frame_op_plan(
    doc: &Document,
    op: FrameOp,
    limits: &Limits,
) -> Option<ShpPlan> {
    let header = header(doc)?;
    let (first, layout, order) = next_sheet(&header, op).ok()?;
    let need = limits.assess(layout.sheet_bytes());
    Some(ShpPlan {
        valid: true,
        width: layout.width as i32,
        height: layout.height as i32,
        total: order.len() as i32,
        first: first as i32,
        count: layout.frames as i32,
        needed: need.needed,
        limit: limits.limit,
        available: limits.available.unwrap_or(0),
        estimate: need.estimate,
        stage: i32::from(need.stage),
        floor_ok: need.floor_ok,
    })
}

/// Replace the sheet layer's pixels with `rgba` of `width` x `height`.
fn set_layer_rgba(layer: &mut Layer, rgba: &[u8], width: u32, height: u32) {
    for (k, id) in [0i16, 1, 2, -1].into_iter().enumerate() {
        if let Some(channel) = layer.channels.iter_mut().find(|c| c.id == id) {
            let values: Vec<u8> = rgba.iter().skip(k).step_by(4).copied().collect();
            channel.data = values.into();
        }
    }
    layer.rect = pictura_core::PsdRect {
        top: 0,
        left: 0,
        bottom: height as i32,
        right: width as i32,
    };
    layer.source_channels = None;
}

/// The mutation half of a frame operation on a whole sheet; the frame to
/// show afterwards. Pixels in the gutters would be dropped by the reflow,
/// so they refuse it.
pub(in crate::cxxqt_object) fn whole_op_rust(
    rust: &mut PictureViewRust,
    op: FrameOp,
    limits: &Limits,
    allowance: u64,
) -> Result<usize, String> {
    let doc = rust.session.doc.as_mut().ok_or("no document")?;
    let header = checked_header(doc)?;
    let (_, layout, order) = next_sheet(&header, op)?;
    if layout.sheet_bytes() > header.layout.sheet_bytes() {
        limits.admit(layout.sheet_bytes(), allowance)?;
    }
    let block = block(doc).ok_or("missing SHP data")?;
    let (_, path, original) = resource::parts(block).ok_or("missing SHP data")?;
    let origins = origins(block).ok_or("damaged SHP data")?;
    let original = original.to_vec();
    let plane = doc.width as usize * doc.height as usize;
    let layer = sheet_layer(doc)?;
    let rgba = layer_rgba(layer, plane);
    let gutter = gutter_pixels(&header.layout, &rgba);
    if gutter > 0 {
        return fail(format!(
            "{gutter} pixels lie in the gaps between frames; clear the gutters before changing \
             frames."
        ));
    }
    let next_rgba = reflow(&header.layout, &rgba, &layout, &order);
    drop(rgba);
    let next_origins: Origins = order.iter().map(|o| o.and_then(|k| origins[k])).collect();
    let total = order.len();
    let next = Header {
        layout,
        palette: header.palette,
        first: 0,
        total,
        span: total,
    };
    let data = encode_parts(
        &next,
        &path,
        &original,
        std::iter::empty(),
        Some(&next_origins),
    );
    let (sw, sh) = (layout.sheet_width() as u32, layout.sheet_height() as u32);
    let resized = (sw, sh) != (doc.width, doc.height);
    if resized {
        pictura_render::resize_canvas_document(doc, sw, sh, pictura_render::Anchor::TopLeft)
            .map_err(|e| format!("cannot resize the sheet: {e:?}"))?;
    }
    set_layer_rgba(&mut doc.layers[0], &next_rgba, sw, sh);
    doc.image_resources = set_block(
        &doc.image_resources,
        resource::RESOURCE_ID,
        resource::RESOURCE_NAME,
        &data,
    )
    .into();
    if resized {
        rust.session.set_selection(None);
    }
    Ok(op.focus(total))
}

/// Where a frame's pixels are before a paged operation.
enum Content<'a> {
    Sheet(&'a [u8]),
    Stored(&'a [u8]),
    Original,
}

/// A frame operation on a paged sheet: the frame list is rebuilt without
/// decoding untouched frames, the window that holds the focused frame is
/// shown, and History starts again there. The frame to show afterwards.
pub(in crate::cxxqt_object) fn paged_op_rust(
    rust: &mut PictureViewRust,
    op: FrameOp,
    limits: &Limits,
    allowance: u64,
) -> Result<usize, String> {
    let doc = rust.session.doc.as_ref().ok_or("no document")?;
    let (header, scan) = clean_window(doc)?;
    let (first, layout, order) = next_sheet(&header, op)?;
    limits.admit(layout.sheet_bytes(), allowance)?;
    let block = block(doc).ok_or("missing SHP data")?;
    let (_, path, original) = resource::parts(block).ok_or("missing SHP data")?;
    if original.is_empty() {
        return fail("This SHP sheet has no source file to page through.");
    }
    let origins = origins(block).ok_or("damaged SHP data")?;
    let stored: BTreeMap<usize, &[u8]> = stored(block)
        .ok_or("damaged SHP data")?
        .into_iter()
        .collect();
    let (w, h) = (layout.width, layout.height);
    let window = header.window();
    let content = |k: usize| {
        if window.contains(&k) {
            Content::Sheet(&scan.planes[k - header.first])
        } else if let Some(data) = stored.get(&k) {
            Content::Stored(data)
        } else {
            Content::Original
        }
    };
    let next_origins: Origins = order.iter().map(|o| o.and_then(|k| origins[k])).collect();
    let shown = first..first + layout.frames;
    let mut planes = Vec::with_capacity(layout.frames);
    let mut store: Vec<(usize, Vec<u8>)> = Vec::new();
    for (i, from) in order.iter().enumerate() {
        let origin = next_origins[i];
        let pixels = from.map(content);
        if shown.contains(&i) {
            planes.push(match pixels {
                Some(Content::Sheet(p)) => p.to_vec(),
                Some(Content::Stored(data)) => stored_canvas(data, w, h),
                _ => origin_canvas(original, origin, w, h),
            });
        } else {
            match pixels {
                Some(Content::Sheet(p)) if *p != origin_canvas(original, origin, w, h)[..] => {
                    store.push((i, encode_rle(p, w)));
                }
                Some(Content::Stored(data)) => store.push((i, data.to_vec())),
                _ => {}
            }
        }
    }
    let total = order.len();
    let next = Header {
        layout,
        palette: header.palette.clone(),
        first,
        total,
        span: header.span.clamp(1, total),
    };
    let data = encode_parts(
        &next,
        &path,
        original,
        store.iter().map(|(f, d)| (*f, &d[..])),
        Some(&next_origins),
    );
    drop(store);
    drop(scan);
    let name = doc.layers[0].name.clone();
    install_window(
        rust,
        &name,
        &layout,
        planes,
        &next.palette,
        &data,
        op.label(),
        true,
    );
    Ok(op.focus(total))
}
