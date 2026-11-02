//! Fixture helpers: every SHP and palette in these tests is generated here.
//! No game file is, or may ever be, checked in.

#![allow(dead_code)]

use pictura_shp::{FrameHeader, FRAME_HEADER, HEADER};

/// A small deterministic PRNG (SplitMix64), so round trips are reproducible.
pub struct Rng(u64);

impl Rng {
    pub fn new(seed: u64) -> Self {
        Rng(seed)
    }

    pub fn next(&mut self) -> u64 {
        self.0 = self.0.wrapping_add(0x9E37_79B9_7F4A_7C15);
        let mut z = self.0;
        z = (z ^ (z >> 30)).wrapping_mul(0xBF58_476D_1CE4_E5B9);
        z = (z ^ (z >> 27)).wrapping_mul(0x94D0_49BB_1331_11EB);
        z ^ (z >> 31)
    }

    pub fn below(&mut self, n: u64) -> u64 {
        self.next() % n.max(1)
    }
}

/// A header with the given rectangle, flags and offset; radar and reserved
/// get recognisable non-zero values so tests can see them survive.
pub fn header(x: u16, y: u16, w: u16, h: u16, flags: u32, offset: u32) -> FrameHeader {
    FrameHeader {
        x,
        y,
        w,
        h,
        flags,
        radar: [0x11, 0x22, 0x33, 0x44],
        reserved: 0x0012_F0A4,
        offset,
    }
}

/// Assemble a file: header and table, then each `(offset, bytes)` block at
/// its offset with every gap filled with `fill`, then `tail`.
pub fn assemble(
    width: u16,
    height: u16,
    headers: &[FrameHeader],
    blocks: &[(usize, Vec<u8>)],
    fill: u8,
    tail: &[u8],
) -> Vec<u8> {
    let mut out = Vec::new();
    out.extend_from_slice(&0u16.to_le_bytes());
    out.extend_from_slice(&width.to_le_bytes());
    out.extend_from_slice(&height.to_le_bytes());
    out.extend_from_slice(&(headers.len() as u16).to_le_bytes());
    for h in headers {
        out.extend_from_slice(&h.to_bytes());
    }
    assert_eq!(out.len(), HEADER + FRAME_HEADER * headers.len());
    for (offset, bytes) in blocks {
        if out.len() < offset + bytes.len() {
            out.resize(offset + bytes.len(), fill);
        }
        out[*offset..offset + bytes.len()].copy_from_slice(bytes);
    }
    out.extend_from_slice(tail);
    out
}

/// The table size for `n` frames.
pub fn table(n: usize) -> usize {
    HEADER + FRAME_HEADER * n
}

/// A `w x h` cropped plane with a mix of runs and literals.
pub fn sprite_plane(rng: &mut Rng, w: usize, h: usize) -> Vec<u8> {
    let mut out = vec![0u8; w * h];
    for y in 0..h {
        let mut x = 0;
        while x < w {
            let run = 1 + rng.below(w as u64 / 2 + 1) as usize;
            let solid = rng.below(3) != 0;
            for v in out[y * w + x..y * w + (x + run).min(w)].iter_mut() {
                *v = if solid { 1 + rng.below(255) as u8 } else { 0 };
            }
            x += run;
        }
    }
    out
}

/// Place a cropped plane on a `cw x ch` canvas.
pub fn place(cw: usize, ch: usize, x: usize, y: usize, w: usize, plane: &[u8]) -> Vec<u8> {
    let mut out = vec![0u8; cw * ch];
    for (row, src) in plane.chunks_exact(w).enumerate() {
        out[(y + row) * cw + x..(y + row) * cw + x + w].copy_from_slice(src);
    }
    out
}

/// The 24 header bytes of frame `i` in `file`.
pub fn header_bytes(file: &[u8], i: usize) -> &[u8] {
    &file[HEADER + FRAME_HEADER * i..HEADER + FRAME_HEADER * (i + 1)]
}
