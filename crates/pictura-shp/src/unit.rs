// Derived from Ochre's C&C addon (`addons/cnc/__init__.py`
// `SplitShadowsCommand`). Copyright (C) 2026 Ochre contributors, GPL-3.0-or-later, by the owner of
// this fork.
// SPDX-License-Identifier: GPL-3.0-or-later

//! Unit-sprite conventions: the shadow half.
//!
//! A TS/RA2 unit SHP conventionally holds 2N frames: N of art followed by N
//! shadow frames that use only index 0 (transparent) and index 1 (shadow).
//! Nothing in the file says so; it is read from the pixels ([`split`]).

use crate::codec::{decode_frame, FrameHeader};

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
