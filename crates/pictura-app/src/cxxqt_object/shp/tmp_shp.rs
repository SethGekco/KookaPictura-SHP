// SPDX-License-Identifier: GPL-3.0-or-later
//! Save As SHP of a TMP template: an explicit conversion into a new SHP
//! document, either one frame per tile (the tile's diamond, with its extra
//! block drawn over it when asked) or one frame of the whole template. Only
//! pixels travel: an SHP has no place for tile headers (position, height,
//! land, ramp, radar colors) or z-data. The template is not changed.

use super::*;

use pictura_shp::terrain::Plane;

/// Draw `rows` of `from` (canvas RGBA, `width` wide) into `frame` (`fw`
/// wide) shifted by `(dx, dy)`; only pixels that are not transparent.
fn over(
    frame: &mut [u8],
    fw: usize,
    from: &[u8],
    width: usize,
    rows: &[(usize, usize, usize)],
    (dx, dy): (i64, i64),
) {
    for &(y, x, w) in rows {
        for k in 0..w {
            let at = (y * width + x + k) * 4;
            if from[at + 3] == 0 {
                continue;
            }
            let (fx, fy) = ((x + k) as i64 + dx, y as i64 + dy);
            let to = (fy as usize * fw + fx as usize) * 4;
            frame[to..to + 4].copy_from_slice(&from[at..at + 4]);
        }
    }
}

/// The frames of `doc` (a template), each `width` x `height` RGBA: one per
/// tile in slot order, or one of the whole canvas; extra blocks drawn over
/// the diamonds when `extras`.
pub(in crate::cxxqt_object) fn template_frames(
    doc: &Document,
    per_tile: bool,
    extras: bool,
) -> Result<(usize, usize, Vec<Vec<u8>>), String> {
    let (_, t, p) = template_of(doc)
        .ok_or("This document is not a TMP template, or its TMP data is damaged.")?;
    let layers = read_layers(doc, &p)?;
    let specs = p.layers();
    let colour: Vec<(usize, &LayerSpec)> = specs
        .iter()
        .enumerate()
        .filter(|(_, s)| s.plane == Plane::Tiles || (extras && s.plane == Plane::Extras))
        .collect();
    if !per_tile {
        let mut frame = vec![0u8; p.width * p.height * 4];
        let all: Vec<(usize, usize, usize)> = (0..p.height).map(|y| (y, 0, p.width)).collect();
        for (i, _) in &colour {
            over(&mut frame, p.width, &layers[*i], p.width, &all, (0, 0));
        }
        return Ok((p.width, p.height, vec![frame]));
    }
    // Every frame holds its tile's rectangle at the same place, grown to
    // hold the furthest extra block.
    let (mut x0, mut y0, mut x1, mut y1) = (0i64, 0i64, p.cx as i64, p.cy as i64);
    for (_, tile) in t.live().filter(|_| extras) {
        if tile.extra.is_some() {
            let (ex, ey) = (
                i64::from(tile.extra_x - tile.x),
                i64::from(tile.extra_y - tile.y),
            );
            x0 = x0.min(ex);
            y0 = y0.min(ey);
            x1 = x1.max(ex + tile.extra_w as i64);
            y1 = y1.max(ey + tile.extra_h as i64);
        }
    }
    let (fw, fh) = ((x1 - x0) as usize, (y1 - y0) as usize);
    let frames = t
        .live()
        .map(|(slot, tile)| {
            let mut frame = vec![0u8; fw * fh * 4];
            let (tx, ty) = p.tile_origin(tile);
            let shift = (-(tx as i64) - x0, -(ty as i64) - y0);
            for (i, spec) in &colour {
                if let Some(rows) = p.rows_on(&t, spec, slot) {
                    over(&mut frame, fw, &layers[*i], p.width, &rows, shift);
                }
            }
            frame
        })
        .collect();
    Ok((fw, fh, frames))
}

/// The SHP document Save As SHP makes of a template.
pub(in crate::cxxqt_object) fn template_to_sheet(
    doc: &Document,
    name: &str,
    per_tile: bool,
    extras: bool,
    gpu: bool,
    limits: &Limits,
    allowance: u64,
) -> Result<(OpenedDocument, Layout), String> {
    let resource = tmp_resource(doc).ok_or("This document is not a TMP template.")?;
    let (w, h, frames) = template_frames(doc, per_tile, extras)?;
    let layout = super::super::authoring::new_layout(w, h, frames.len())?;
    limits.admit(layout.sheet_bytes(), allowance)?;
    let sw = layout.sheet_width();
    let mut rgba = vec![0u8; layout.sheet_bytes() as usize];
    for (k, frame) in frames.iter().enumerate() {
        let (dx, dy) = layout.cell_origin(k);
        for y in 0..h {
            let to = ((dy + y) * sw + dx) * 4;
            rgba[to..to + w * 4].copy_from_slice(&frame[y * w * 4..(y + 1) * w * 4]);
        }
    }
    drop(frames);
    let choice = PaletteChoice {
        palette: resource.palette,
        path: resource.palette_path,
    };
    let opened =
        super::super::authoring::new_document(name, &layout, rgba, &choice, gpu, "Convert to SHP");
    Ok((opened, layout))
}

/// Make the SHP in `view`; `[frames, width, height]`, or empty with the
/// reason in `shp_last_error`.
pub(in crate::cxxqt_object) fn tmp_to_shp(
    view: Pin<&mut PictureView>,
    source: &PictureView,
    name: &QString,
    per_tile: bool,
    extras: bool,
    allowance: u64,
) -> Vec<i32> {
    let gpu = view.rust().gpu_compute;
    let made = match source.rust().session.doc.as_ref() {
        Some(doc) => template_to_sheet(
            doc,
            &name.to_string(),
            per_tile,
            extras,
            gpu,
            &Limits::current(),
            allowance,
        ),
        None => fail("no document"),
    };
    let summary = made
        .as_ref()
        .ok()
        .map(|(_, l)| vec![l.frames as i32, l.width as i32, l.height as i32]);
    if super::super::authoring::install(view, made.map(|(opened, _)| opened), true) {
        summary.unwrap_or_default()
    } else {
        Vec::new()
    }
}
