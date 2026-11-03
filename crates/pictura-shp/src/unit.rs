// Derived from Ochre's C&C addon (`addons/cnc/__init__.py`
// `SplitShadowsCommand`, `addons/cnc/cncshp.py` `_average_colour`). Copyright (C) 2026 Ochre contributors, GPL-3.0-or-later, by the owner of
// this fork.
// SPDX-License-Identifier: GPL-3.0-or-later

//! Unit-sprite conventions: the shadow half, a house-color preview ramp and
//! the radar color.
//!
//! A TS/RA2 unit SHP conventionally holds 2N frames: N of art followed by N
//! shadow frames that use only index 0 (transparent) and index 1 (shadow).
//! Nothing in the file says so; it is read from the pixels ([`split`]).

use crate::codec::{decode_frame, FrameHeader, FRAME_HEADER, HEADER};
use crate::palette::Palette;
use std::ops::RangeInclusive;

/// The shadow index of unit palettes.
pub const SHADOW: u8 = 1;

/// What a frame holds, as far as the shadow split cares.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Use {
    /// Every pixel is index 0.
    Empty,
    /// Only indices 0 and 1, at least one 1: what a shadow frame looks like.
    Shadow,
    /// Some index other than 0 and 1.
    Art,
}

/// The use of a full-canvas (or cropped) index plane.
pub fn classify(plane: &[u8]) -> Use {
    let mut found = Use::Empty;
    for &v in plane {
        match v {
            0 => {}
            SHADOW => found = Use::Shadow,
            _ => return Use::Art,
        }
    }
    found
}

/// The use of frame `i` of an SHP file's bytes (which must pass
/// [`crate::sniff`]), decoded from its stored rectangle only.
pub fn file_frame_use(data: &[u8], i: usize) -> Use {
    let header = FrameHeader::read(data, i);
    if !header.has_data() {
        return Use::Empty;
    }
    decode_frame(data, &header).map_or(Use::Empty, |plane| classify(&plane))
}

/// The first shadow frame when the frames look like a unit sprite with a
/// shadow half: an even count, no frame of the second half holding any
/// index but 0 and 1, and some art in the first half (the rule of Ochre's
/// `SplitShadowsCommand`, plus the art, so a sprite of blank frames is not a
/// unit). Empty shadow frames count: a unit whose shadows were cleared still
/// has its shadow half.
pub fn split(uses: &[Use]) -> Option<usize> {
    let n = uses.len();
    if n < 2 || !n.is_multiple_of(2) {
        return None;
    }
    let half = n / 2;
    let shadows = uses[half..].iter().all(|u| *u != Use::Art);
    (shadows && uses[..half].contains(&Use::Art)).then_some(half)
}

/// The shadow frames (`half..`) that hold an index other than 0 and 1.
pub fn violations(uses: &[Use], half: usize) -> Vec<usize> {
    (half.min(uses.len())..uses.len())
        .filter(|&f| uses[f] == Use::Art)
        .collect()
}

/// How a shadow is cast from the art.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct ShadowParams {
    /// Offset in pixels.
    pub dx: i32,
    pub dy: i32,
    /// Horizontal lean in pixels per row above the art's lowest row, so the
    /// shadow stays attached at the base (positive leans right).
    pub skew: f64,
}

impl ShadowParams {
    /// The art's silhouette where it stands.
    pub const STRAIGHT: ShadowParams = ShadowParams {
        dx: 0,
        dy: 0,
        skew: 0.0,
    };
}

/// The shadow of an art frame (`width` x `height` indices): every pixel that
/// is not index 0 becomes [`SHADOW`], moved by the offset and sheared by the
/// skew. Rows move whole, so the shear leaves no holes; pixels pushed off
/// the canvas are dropped.
pub fn silhouette(art: &[u8], width: usize, height: usize, p: ShadowParams) -> Vec<u8> {
    let mut out = vec![0u8; width * height];
    let row = |y: usize| &art[y * width..(y + 1) * width];
    let Some(base) = (0..height).rev().find(|&y| row(y).iter().any(|&v| v != 0)) else {
        return out;
    };
    for y in 0..=base {
        let ty = y as i64 + i64::from(p.dy);
        if ty < 0 || ty >= height as i64 {
            continue;
        }
        let shift = i64::from(p.dx) + (p.skew * (base - y) as f64).round() as i64;
        for (x, &v) in row(y).iter().enumerate() {
            let tx = x as i64 + shift;
            if v != 0 && (0..width as i64).contains(&tx) {
                out[ty as usize * width + tx as usize] = SHADOW;
            }
        }
    }
    out
}

/// A house-color ramp for the remap indices `range` of `palette` (as read
/// from the `.pal`), as `0xFFRRGGBB` per index: `rgb` scaled by each
/// entry's luma relative to the brightest entry of the range, so the ramp
/// keeps the palette's own shading. An approximation for previews only: the
/// game builds its ramps its own way. A range that is black throughout
/// gets an even ramp from `rgb` down.
pub fn house_ramp(palette: &Palette, range: RangeInclusive<u8>, rgb: u32) -> Vec<u32> {
    let luma = |c: u32| {
        let (r, g, b) = ((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF);
        f64::from(299 * r + 587 * g + 114 * b) / 1000.0
    };
    let entries: Vec<u32> = range.map(|i| palette.argb[usize::from(i)]).collect();
    let n = entries.len();
    let brightest = entries.iter().map(|&c| luma(c)).fold(0.0, f64::max);
    entries
        .iter()
        .enumerate()
        .map(|(i, &c)| {
            let factor = if brightest > 0.0 {
                luma(c) / brightest
            } else {
                (n - i) as f64 / n as f64
            };
            let scale =
                |shift: u32| ((f64::from((rgb >> shift) & 0xFF) * factor).round() as u32).min(255);
            0xFF00_0000 | (scale(16) << 16) | (scale(8) << 8) | scale(0)
        })
        .collect()
}

/// The radar color of a frame: the mean of its non-transparent pixels'
/// colors in the palette as read, each channel rounded down (Ochre's
/// `_average_colour`). `None` for an empty frame.
pub fn average_color(plane: &[u8], palette: &Palette) -> Option<[u8; 3]> {
    let mut sum = [0u64; 3];
    let mut count = 0u64;
    for &v in plane.iter().filter(|&&v| v != 0) {
        let c = palette.argb[usize::from(v)];
        sum[0] += u64::from((c >> 16) as u8);
        sum[1] += u64::from((c >> 8) as u8);
        sum[2] += u64::from(c as u8);
        count += 1;
    }
    (count > 0).then(|| sum.map(|s| (s / count) as u8))
}

/// Recompute the radar field of every frame of SHP bytes (which must pass
/// [`crate::sniff`]) as [`average_color`] then a zero byte, as Ochre's
/// writer does when asked. A frame with no pixel keeps its field. Returns
/// how many fields changed.
pub fn recompute_radar(bytes: &mut [u8], palette: &Palette) -> usize {
    let count = usize::from(u16::from_le_bytes([bytes[6], bytes[7]]));
    let mut changed = 0;
    for i in 0..count {
        let header = FrameHeader::read(bytes, i);
        if !header.has_data() {
            continue;
        }
        let Some([r, g, b]) =
            decode_frame(bytes, &header).and_then(|plane| average_color(&plane, palette))
        else {
            continue;
        };
        let at = HEADER + FRAME_HEADER * i + 12;
        let next = [r, g, b, 0];
        if bytes[at..at + 4] != next {
            bytes[at..at + 4].copy_from_slice(&next);
            changed += 1;
        }
    }
    changed
}
