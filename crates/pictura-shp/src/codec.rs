// Derived from qshp's `src/shpcodec.cpp` (sniff, decode, RLE-Zero encode) and
// Ochre's `addons/cnc/cncshp.py` (the span helpers). Both Copyright (C) 2026
// Ochre contributors, GPL-3.0-or-later, by the owner of this fork.
// SPDX-License-Identifier: GPL-3.0-or-later

//! The SHP (TS/RA2) file layout: sniffing, decoding and RLE-Zero.
//!
//! All little-endian:
//!
//! ```text
//! header   u16 zero, u16 width, u16 height, u16 frames
//! frame    u16 x, y, w, h; u32 flags; u8[4] radar; u32 reserved; u32 offset
//! ```
//!
//! The low byte of `flags` is the compression: 0 and 1 raw, 2 raw scanlines
//! each behind a u16 length that counts itself, 3 RLE-Zero (per scanline a
//! u16 length that counts itself, then a non-zero byte is a literal index and
//! `0 n` is a run of `n` transparent pixels, `n <= 255`). Some writers put a
//! size in the upper bits of `flags`, so only the low byte is read.

use std::fmt;

/// Bytes in the file header.
pub const HEADER: usize = 8;
/// Bytes in one frame header.
pub const FRAME_HEADER: usize = 24;
/// The largest canvas side and frame count accepted, as the reference tools.
pub const MAX_SIDE: u16 = 4096;
/// See [`MAX_SIDE`].
pub const MAX_FRAMES: u16 = 4096;

/// Why bytes could not be read or written as an SHP.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Error {
    /// The bytes fail [`sniff`].
    NotShp,
    /// The canvas or frame count is outside `1..=4096`.
    BadDimensions {
        width: usize,
        height: usize,
        frames: usize,
    },
    /// Plane `frame` does not hold `width * height` indices.
    BadPlane {
        frame: usize,
        len: usize,
        expected: usize,
    },
    /// The frame data would not fit the format's 32-bit offsets.
    TooLarge,
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Error::NotShp => f.write_str("not a Tiberian Sun / Red Alert 2 SHP"),
            Error::BadDimensions { width, height, frames } => write!(
                f,
                "an SHP holds 1 to 4096 frames of at most 4096x4096; got {frames} frames of {width}x{height}"
            ),
            Error::BadPlane { frame, len, expected } => write!(
                f,
                "frame {frame} has {len} indices; the canvas needs {expected}"
            ),
            Error::TooLarge => f.write_str("the frame data exceeds the 4 GiB an SHP can address"),
        }
    }
}

impl std::error::Error for Error {}

pub(crate) fn u16_at(data: &[u8], pos: usize) -> u16 {
    u16::from_le_bytes([data[pos], data[pos + 1]])
}

pub(crate) fn u32_at(data: &[u8], pos: usize) -> u32 {
    u32::from_le_bytes([data[pos], data[pos + 1], data[pos + 2], data[pos + 3]])
}

/// One frame header exactly as stored.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct FrameHeader {
    pub x: u16,
    pub y: u16,
    pub w: u16,
    pub h: u16,
    pub flags: u32,
    /// The radar colour: cosmetic, kept verbatim.
    pub radar: [u8; 4],
    /// Nominally reserved; real files often hold a leaked stack address.
    pub reserved: u32,
    /// Absolute file offset of the data; 0 means an empty frame.
    pub offset: u32,
}

impl FrameHeader {
    /// Frame header `i` of `data`, which must hold the whole table.
    pub fn read(data: &[u8], i: usize) -> Self {
        let p = HEADER + FRAME_HEADER * i;
        FrameHeader {
            x: u16_at(data, p),
            y: u16_at(data, p + 2),
            w: u16_at(data, p + 4),
            h: u16_at(data, p + 6),
            flags: u32_at(data, p + 8),
            radar: [data[p + 12], data[p + 13], data[p + 14], data[p + 15]],
            reserved: u32_at(data, p + 16),
            offset: u32_at(data, p + 20),
        }
    }

    /// The 24 bytes this header is stored as.
    pub fn to_bytes(&self) -> [u8; FRAME_HEADER] {
        let mut out = [0u8; FRAME_HEADER];
        out[0..2].copy_from_slice(&self.x.to_le_bytes());
        out[2..4].copy_from_slice(&self.y.to_le_bytes());
        out[4..6].copy_from_slice(&self.w.to_le_bytes());
        out[6..8].copy_from_slice(&self.h.to_le_bytes());
        out[8..12].copy_from_slice(&self.flags.to_le_bytes());
        out[12..16].copy_from_slice(&self.radar);
        out[16..20].copy_from_slice(&self.reserved.to_le_bytes());
        out[20..24].copy_from_slice(&self.offset.to_le_bytes());
        out
    }

    /// The compression, from the low byte of the flags.
    pub fn compression(&self) -> u32 {
        self.flags & 0xFF
    }

    /// Whether the file stores pixels for this frame: a non-empty rectangle
    /// and a non-zero offset.
    pub fn has_data(&self) -> bool {
        self.w != 0 && self.h != 0 && self.offset != 0
    }
}

/// Bytes needed to hold the header and the frame table, from the first 8
/// bytes of a file.
pub fn table_bytes(header: &[u8]) -> usize {
    HEADER + FRAME_HEADER * usize::from(u16_at(header, 6))
}

/// Is this a TS/RA2 SHP? The only magic is a leading zero word, so the whole
/// frame table is validated: every rectangle inside the canvas, every data
/// offset past the table and inside the file, a known compression.
///
/// `size` is the full file size, or `None` when it is unknown (a sequential
/// stream); `data` must then hold at least the header and frame table.
pub fn sniff(data: &[u8], size: Option<usize>) -> bool {
    if data.len() < HEADER {
        return false;
    }
    let width = u16_at(data, 2);
    let height = u16_at(data, 4);
    let count = u16_at(data, 6);
    if u16_at(data, 0) != 0
        || width < 1
        || height < 1
        || count < 1
        || width > MAX_SIDE
        || height > MAX_SIDE
        || count > MAX_FRAMES
    {
        return false;
    }
    let table = table_bytes(data);
    if data.len() < table || size.is_some_and(|size| size < table) {
        return false;
    }
    let mut drawn = 0usize;
    let mut no_data = true;
    for i in 0..usize::from(count) {
        let f = FrameHeader::read(data, i);
        no_data = no_data && f.offset == 0;
        if f.w == 0 || f.h == 0 {
            continue; // an empty frame; nothing else is read
        }
        let offset = f.offset as usize;
        if u32::from(f.x) + u32::from(f.w) > u32::from(width)
            || u32::from(f.y) + u32::from(f.h) > u32::from(height)
            || f.compression() > 3
            || offset < table
        {
            return false;
        }
        if let Some(size) = size {
            let raw = usize::from(f.w) * usize::from(f.h);
            if offset >= size || (f.compression() <= 1 && offset + raw > size) {
                return false;
            }
        }
        drawn += 1;
    }
    // Only files with two leading zero bytes get this far (ICO, TGA, WBMP and
    // the like), so an all-empty table -- which a stray header can fake -- is
    // claimed only in the exact shape real blank sprites have: nothing after
    // the table and every data offset 0.
    drawn > 0 || (size == Some(table) && no_data)
}

/// One RLE-Zero scanline into `row` (already zeroed). Returns the offset of
/// the next line. Lenient: truncated lines and over-long runs are clamped.
pub fn decode_rle_line(data: &[u8], pos: usize, row: &mut [u8]) -> usize {
    let size = data.len();
    if pos + 2 > size {
        return size;
    }
    let length = usize::from(u16_at(data, pos));
    if length < 2 {
        return pos + 2;
    }
    let end = (pos + length).min(size);
    let width = row.len();
    let mut i = pos + 2;
    let mut x = 0usize;
    while i < end && x < width {
        let v = data[i];
        if v != 0 {
            row[x] = v;
            x += 1;
            i += 1;
        } else {
            if i + 1 >= end {
                break;
            }
            x = width.min(x + usize::from(data[i + 1]));
            i += 2;
        }
    }
    pos + length
}

/// One length-prefixed raw scanline (compression 2) into `row`.
pub fn decode_prefixed_line(data: &[u8], pos: usize, row: &mut [u8]) -> usize {
    let size = data.len();
    if pos + 2 > size {
        return size;
    }
    let length = usize::from(u16_at(data, pos)).max(2);
    let avail = (pos + length).min(size) - (pos + 2);
    let take = row.len().min(avail);
    row[..take].copy_from_slice(&data[pos + 2..pos + 2 + take]);
    pos + length
}

/// One scanline to RLE-Zero, appended to `out`, with the length prefix
/// counting itself and transparent runs clamped at 255.
pub fn encode_rle_line(row: &[u8], out: &mut Vec<u8>) {
    let start = out.len();
    out.extend_from_slice(&[0, 0]);
    let mut x = 0;
    while x < row.len() {
        if row[x] == 0 {
            let mut run = 0u8;
            while x < row.len() && row[x] == 0 && run < 255 {
                run += 1;
                x += 1;
            }
            out.extend_from_slice(&[0, run]);
        } else {
            out.push(row[x]);
            x += 1;
        }
    }
    let length = (out.len() - start) as u16;
    out[start..start + 2].copy_from_slice(&length.to_le_bytes());
}

/// A `w`-wide cropped plane as RLE-Zero (compression 3).
pub fn encode_rle(plane: &[u8], w: usize) -> Vec<u8> {
    let mut out = Vec::with_capacity(plane.len() / 2 + 2 * (plane.len() / w.max(1)));
    for row in plane.chunks_exact(w.max(1)) {
        encode_rle_line(row, &mut out);
    }
    out
}

/// A `w`-wide cropped plane as length-prefixed raw scanlines (compression 2).
pub fn encode_prefixed(plane: &[u8], w: usize) -> Vec<u8> {
    let mut out = Vec::with_capacity(plane.len() + 2 * (plane.len() / w.max(1)));
    for row in plane.chunks_exact(w.max(1)) {
        out.extend_from_slice(&((w + 2) as u16).to_le_bytes());
        out.extend_from_slice(row);
    }
    out
}

/// How many bytes `h` length-prefixed lines (compression 2 or 3) occupy from
/// `offset`. The format never records it; it is needed to copy an untouched
/// frame's original bytes.
pub fn line_span(data: &[u8], offset: usize, h: usize) -> usize {
    let mut pos = offset;
    for _ in 0..h {
        if pos + 2 > data.len() {
            break;
        }
        pos += usize::from(u16_at(data, pos)).max(2);
    }
    pos.min(data.len()) - offset
}

/// Decode frame `f` of `data` to its `w * h` cropped indices, or `None` when
/// the file stores no pixels for it. Lenient, like the reference tools.
pub fn decode_frame(data: &[u8], f: &FrameHeader) -> Option<Vec<u8>> {
    let offset = f.offset as usize;
    if !f.has_data() || offset >= data.len() {
        return None;
    }
    let (w, h) = (usize::from(f.w), usize::from(f.h));
    let mut pixels = vec![0u8; w * h];
    let mut pos = offset;
    match f.compression() {
        3 => {
            for row in pixels.chunks_exact_mut(w) {
                pos = decode_rle_line(data, pos, row);
            }
        }
        2 => {
            for row in pixels.chunks_exact_mut(w) {
                pos = decode_prefixed_line(data, pos, row);
            }
        }
        _ => {
            // Raw: whatever is there; sniff has already checked it all is.
            let take = (w * h).min(data.len() - offset);
            pixels[..take].copy_from_slice(&data[offset..offset + take]);
        }
    }
    Some(pixels)
}

/// The stored size of frame `f`'s data: what an untouched frame copies.
pub fn frame_span(data: &[u8], f: &FrameHeader) -> usize {
    let offset = f.offset as usize;
    if !f.has_data() || offset >= data.len() {
        return 0;
    }
    let (w, h) = (usize::from(f.w), usize::from(f.h));
    match f.compression() {
        2 | 3 => line_span(data, offset, h),
        _ => (w * h).min(data.len() - offset),
    }
}

/// One decoded frame: its header and its cropped pixels.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Frame {
    pub header: FrameHeader,
    /// `w * h` indices, row-major, or `None` for an empty frame.
    pub pixels: Option<Vec<u8>>,
}

/// A decoded SHP.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Sprite {
    pub width: u16,
    pub height: u16,
    pub frames: Vec<Frame>,
}

impl Sprite {
    /// Frame `i` placed on the full canvas (`width * height` indices, 0
    /// outside the frame's rectangle). Out of range gives a blank canvas.
    pub fn canvas(&self, i: usize) -> Vec<u8> {
        let (cw, ch) = (usize::from(self.width), usize::from(self.height));
        let mut out = vec![0u8; cw * ch];
        if let Some(frame) = self.frames.get(i) {
            blit(&mut out, cw, ch, frame);
        }
        out
    }
}

/// Frame `i` of `data` (which must pass [`sniff`]) on the full canvas,
/// decoding that frame only. Out of range gives a blank canvas.
pub fn frame_canvas(data: &[u8], i: usize) -> Vec<u8> {
    let (cw, ch) = (usize::from(u16_at(data, 2)), usize::from(u16_at(data, 4)));
    let mut out = vec![0u8; cw * ch];
    if i < usize::from(u16_at(data, 6)) {
        let header = FrameHeader::read(data, i);
        let pixels = decode_frame(data, &header);
        blit(&mut out, cw, ch, &Frame { header, pixels });
    }
    out
}

/// Copy `frame`'s pixels into a `cw x ch` canvas, clipped to it.
pub(crate) fn blit(canvas: &mut [u8], cw: usize, ch: usize, frame: &Frame) {
    let Some(pixels) = frame.pixels.as_ref() else {
        return;
    };
    let f = &frame.header;
    let (x, y, w, h) = (
        usize::from(f.x),
        usize::from(f.y),
        usize::from(f.w),
        usize::from(f.h),
    );
    if x >= cw || y >= ch {
        return;
    }
    let cols = w.min(cw - x);
    for row in 0..h.min(ch - y) {
        let src = &pixels[row * w..row * w + cols];
        canvas[(y + row) * cw + x..(y + row) * cw + x + cols].copy_from_slice(src);
    }
}

/// Decode a whole file. Lenient about truncated scanlines, as real files
/// need; an error only when it is not an SHP at all.
pub fn parse(data: &[u8]) -> Result<Sprite, Error> {
    if !sniff(data, Some(data.len())) {
        return Err(Error::NotShp);
    }
    let count = usize::from(u16_at(data, 6));
    let frames = (0..count)
        .map(|i| {
            let header = FrameHeader::read(data, i);
            let pixels = decode_frame(data, &header);
            Frame { header, pixels }
        })
        .collect();
    Ok(Sprite {
        width: u16_at(data, 2),
        height: u16_at(data, 4),
        frames,
    })
}
