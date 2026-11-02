// Derived from qshp's `src/shpcodec.cpp` (`autoColumns`, the sheet layout of
// `qshpplugin.cpp`). Copyright (C) 2026 Ochre contributors,
// GPL-3.0-or-later, by the owner of this fork.
// SPDX-License-Identifier: GPL-3.0-or-later

//! An SHP as one RGBA sheet: every frame in a cell of a grid, every palette
//! index as a colour of its own.
//!
//! Frame `i` sits in cell `(i % cols, i / cols)`; cells are the canvas size
//! and are separated by a transparent gutter. Index `i` is drawn as the
//! *unique* palette colour `U[i]` at full opacity and index 0 as a fully
//! transparent pixel, so the sheet maps back to indices exactly. A pixel that
//! maps to no index (partial alpha, or a colour outside `U`) is
//! **off-palette**: it is reported, and only replaced when the user asks for
//! a snap.

use std::collections::HashMap;

use crate::palette::{nearest, Palette};

/// The longest sheet side an editor document may have.
pub const MAX_SHEET_SIDE: usize = 30_000;

/// The default transparent gutter between cells, in pixels.
pub const DEFAULT_GUTTER: usize = 1;

/// Where the frames of an SHP sit on its sheet.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Layout {
    /// The SHP canvas, which is every cell's size.
    pub width: usize,
    pub height: usize,
    pub frames: usize,
    pub cols: usize,
    pub gutter: usize,
}

/// qshp's column count: 8 (one facing set per row) when the frame count is
/// a multiple of 8, otherwise the smallest square that holds them all.
pub fn auto_columns(frames: usize) -> usize {
    if frames >= 8 && frames.is_multiple_of(8) {
        return 8;
    }
    let mut c = 1;
    while c * c < frames {
        c += 1;
    }
    c
}

impl Layout {
    /// The layout for `frames` frames of `width` x `height`: [`auto_columns`],
    /// widened while the sheet would be taller than [`MAX_SHEET_SIDE`].
    /// `None` when no column count fits both sides.
    pub fn new(width: usize, height: usize, frames: usize, gutter: usize) -> Option<Layout> {
        if width == 0 || height == 0 || frames == 0 {
            return None;
        }
        let mut layout = Layout {
            width,
            height,
            frames,
            cols: auto_columns(frames).min(frames),
            gutter,
        };
        while layout.sheet_height() > MAX_SHEET_SIDE && layout.cols < frames {
            layout.cols += 1;
        }
        layout.fits().then_some(layout)
    }

    /// Both sheet sides within [`MAX_SHEET_SIDE`] and a sane column count.
    pub fn fits(&self) -> bool {
        self.cols > 0
            && self.width > 0
            && self.height > 0
            && self.frames > 0
            && self.sheet_width() <= MAX_SHEET_SIDE
            && self.sheet_height() <= MAX_SHEET_SIDE
    }

    pub fn rows(&self) -> usize {
        self.frames.div_ceil(self.cols.max(1))
    }

    pub fn sheet_width(&self) -> usize {
        let cols = self.cols.min(self.frames);
        cols * self.width + cols.saturating_sub(1) * self.gutter
    }

    pub fn sheet_height(&self) -> usize {
        let rows = self.rows();
        rows * self.height + rows.saturating_sub(1) * self.gutter
    }

    /// The top-left sheet pixel of frame `i`'s cell.
    pub fn cell_origin(&self, i: usize) -> (usize, usize) {
        let cols = self.cols.max(1);
        (
            (i % cols) * (self.width + self.gutter),
            (i / cols) * (self.height + self.gutter),
        )
    }

    /// The frame whose cell holds sheet pixel `(x, y)`, or `None` in a gutter
    /// or past the last frame.
    pub fn frame_at(&self, x: usize, y: usize) -> Option<usize> {
        let (pw, ph) = (self.width + self.gutter, self.height + self.gutter);
        let (col, row) = (x / pw, y / ph);
        if x % pw >= self.width || y % ph >= self.height || col >= self.cols {
            return None;
        }
        let i = row * self.cols + col;
        (i < self.frames).then_some(i)
    }
}

/// `U^-1`: each unique colour's index, for 1..=255. Index 0 is not in it,
/// because the format cannot store index 0 as an opaque pixel.
pub fn inverse(unique: &Palette) -> HashMap<u32, u8> {
    (1..256usize)
        .map(|i| (unique.argb[i] & 0xFF_FFFF, i as u8))
        .collect()
}

/// The RGBA sheet for full-canvas index planes, drawn with `unique` (which
/// must come from [`crate::palette::unique_colours`]). Gutters and index 0
/// are transparent black.
pub fn to_sheet(layout: &Layout, planes: &[Vec<u8>], unique: &Palette) -> Vec<u8> {
    let (sw, sh) = (layout.sheet_width(), layout.sheet_height());
    let mut out = vec![0u8; sw * sh * 4];
    for (i, plane) in planes.iter().enumerate().take(layout.frames) {
        let (x0, y0) = layout.cell_origin(i);
        for y in 0..layout.height {
            let row = &plane[y * layout.width..(y + 1) * layout.width];
            let start = ((y0 + y) * sw + x0) * 4;
            let dst = &mut out[start..start + layout.width * 4];
            for (px, &index) in dst.as_chunks_mut::<4>().0.iter_mut().zip(row) {
                if index == 0 {
                    continue;
                }
                let c = unique.argb[usize::from(index)];
                px.copy_from_slice(&[(c >> 16) as u8, (c >> 8) as u8, c as u8, 255]);
            }
        }
    }
    out
}

/// What a sheet maps back to.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Scan {
    /// Full-canvas index planes; an off-palette pixel reads as index 0.
    pub planes: Vec<Vec<u8>>,
    /// Off-palette pixels per frame.
    pub off_palette: Vec<usize>,
    /// Off-palette pixel bounds per frame, `(x, y, w, h)` in sheet pixels.
    pub off_bounds: Vec<Option<(usize, usize, usize, usize)>>,
    /// Gutter (and unused-cell) pixels that are not fully transparent.
    pub gutter: usize,
}

impl Scan {
    pub fn off_palette_total(&self) -> usize {
        self.off_palette.iter().sum()
    }

    /// The frames holding off-palette pixels, ascending.
    pub fn off_frames(&self) -> Vec<usize> {
        (0..self.off_palette.len())
            .filter(|&i| self.off_palette[i] > 0)
            .collect()
    }
}

/// Map an RGBA sheet (`layout.sheet_width() x sheet_height()`) back to index
/// planes: alpha 0 is index 0, alpha 255 is looked up in `inverse`, anything
/// else is off-palette. Nothing is guessed.
pub fn from_sheet(layout: &Layout, rgba: &[u8], inverse: &HashMap<u32, u8>) -> Scan {
    let sw = layout.sheet_width();
    let mut scan = Scan {
        planes: vec![vec![0u8; layout.width * layout.height]; layout.frames],
        off_palette: vec![0; layout.frames],
        off_bounds: vec![None; layout.frames],
        gutter: 0,
    };
    for (y, row) in rgba.chunks_exact(sw * 4).enumerate() {
        for (x, px) in row.as_chunks::<4>().0.iter().enumerate() {
            let Some(frame) = layout.frame_at(x, y) else {
                if px[3] != 0 {
                    scan.gutter += 1;
                }
                continue;
            };
            if px[3] == 0 {
                continue;
            }
            let rgb = (u32::from(px[0]) << 16) | (u32::from(px[1]) << 8) | u32::from(px[2]);
            let index = if px[3] == 255 {
                inverse.get(&rgb).copied()
            } else {
                None
            };
            let (x0, y0) = layout.cell_origin(frame);
            match index {
                Some(i) => scan.planes[frame][(y - y0) * layout.width + (x - x0)] = i,
                None => {
                    scan.off_palette[frame] += 1;
                    scan.off_bounds[frame] = Some(grow(scan.off_bounds[frame], x, y));
                }
            }
        }
    }
    scan
}

fn grow(
    bounds: Option<(usize, usize, usize, usize)>,
    x: usize,
    y: usize,
) -> (usize, usize, usize, usize) {
    match bounds {
        None => (x, y, 1, 1),
        Some((bx, by, bw, bh)) => {
            let (x0, y0) = (bx.min(x), by.min(y));
            let (x1, y1) = ((bx + bw).max(x + 1), (by + bh).max(y + 1));
            (x0, y0, x1 - x0, y1 - y0)
        }
    }
}

/// Snap every off-palette pixel inside a cell, in place: alpha below 128
/// becomes index 0 (transparent), anything else the [`nearest`] index of the
/// palette as read (so the remap ramp loses real ties), drawn as its unique
/// colour. Pixels already on the palette and the gutters are left alone.
/// Returns how many pixels changed.
pub fn snap(layout: &Layout, rgba: &mut [u8], palette: &Palette, unique: &Palette) -> usize {
    let inverse = inverse(unique);
    let sw = layout.sheet_width();
    let mut changed = 0;
    for (y, row) in rgba.chunks_exact_mut(sw * 4).enumerate() {
        for (x, px) in row.as_chunks_mut::<4>().0.iter_mut().enumerate() {
            if layout.frame_at(x, y).is_none() || px[3] == 0 {
                continue;
            }
            let rgb = (u32::from(px[0]) << 16) | (u32::from(px[1]) << 8) | u32::from(px[2]);
            if px[3] == 255 && inverse.contains_key(&rgb) {
                continue;
            }
            changed += 1;
            if px[3] < 128 {
                px.copy_from_slice(&[0, 0, 0, 0]);
                continue;
            }
            let c = unique.argb[usize::from(nearest(palette, 0xFF00_0000 | rgb))];
            px.copy_from_slice(&[(c >> 16) as u8, (c >> 8) as u8, c as u8, 255]);
        }
    }
    changed
}

/// Make every gutter (and unused-cell) pixel transparent, in place. Returns
/// how many pixels changed.
pub fn clear_gutter(layout: &Layout, rgba: &mut [u8]) -> usize {
    let sw = layout.sheet_width();
    let mut changed = 0;
    for (y, row) in rgba.chunks_exact_mut(sw * 4).enumerate() {
        for (x, px) in row.as_chunks_mut::<4>().0.iter_mut().enumerate() {
            if layout.frame_at(x, y).is_none() && *px != [0, 0, 0, 0] {
                px.copy_from_slice(&[0, 0, 0, 0]);
                changed += 1;
            }
        }
    }
    changed
}

/// Recolour a sheet from one palette to another by index, in place: every
/// pixel that is `old_unique[i]` becomes `new_unique[i]`. Off-palette pixels
/// and transparency are left alone. Returns how many pixels changed.
pub fn rebind(rgba: &mut [u8], old_unique: &Palette, new_unique: &Palette) -> usize {
    let old = inverse(old_unique);
    let mut changed = 0;
    for px in rgba.as_chunks_mut::<4>().0.iter_mut() {
        if px[3] != 255 {
            continue;
        }
        let rgb = (u32::from(px[0]) << 16) | (u32::from(px[1]) << 8) | u32::from(px[2]);
        let Some(&i) = old.get(&rgb) else {
            continue;
        };
        let c = new_unique.argb[usize::from(i)];
        let next = [(c >> 16) as u8, (c >> 8) as u8, c as u8, 255];
        if *px != next {
            px.copy_from_slice(&next);
            changed += 1;
        }
    }
    changed
}
