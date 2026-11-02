// Derived from Ochre's `addons/cnc/cncshp.py` (`read_frames`, `build_shp`,
// `_unchanged`, `trailing_bytes`) and qshp's `src/shpcodec.cpp` (`build`).
// Both Copyright (C) 2026 Ochre contributors, GPL-3.0-or-later, by the owner
// of this fork.
// SPDX-License-Identifier: GPL-3.0-or-later

//! The file an SHP document was opened from, and the writer that keeps it.
//!
//! A re-encode is only guaranteed to be *correct*, not identical: real files
//! leave frames uncompressed where RLE would win, use both raw flags, pack
//! frames with or without 8-byte alignment, fill the gaps between frames with
//! leftover buffer bytes, declare trailing runs one longer than the row, and
//! end with bytes nothing reads. None of that can be inferred, so a
//! [`Source`] remembers it and [`build_preserving`] gives every untouched
//! frame its original bytes back.

use std::borrow::Cow;
use std::collections::HashMap;
use std::ops::Range;

use crate::codec::{
    blit, decode_frame, encode_prefixed, encode_rle, frame_span, sniff, Error, Frame, FrameHeader,
    Sprite, FRAME_HEADER, HEADER, MAX_FRAMES, MAX_SIDE,
};

/// One frame as the file stored it. Its pixels are decoded only when asked
/// for ([`Source::pixels`]), so reading a file never decodes every frame.
#[derive(Debug, Clone)]
pub struct SourceFrame {
    header: FrameHeader,
    /// The file stores pixels for it (`decode_frame` gives `Some`).
    drawn: bool,
    /// The stored data, exactly: `frame_span` bytes from the offset.
    payload: Range<usize>,
    /// The gap before the payload in file order (not always zeros).
    pad: Range<usize>,
}

impl SourceFrame {
    pub fn header(&self) -> &FrameHeader {
        &self.header
    }

    /// Whether the file stores pixels for this frame.
    pub fn has_pixels(&self) -> bool {
        self.drawn
    }
}

/// A parsed SHP that keeps every byte a writer is free to choose.
#[derive(Debug, Clone)]
pub struct Source {
    bytes: Vec<u8>,
    width: u16,
    height: u16,
    frames: Vec<SourceFrame>,
    tail: Range<usize>,
}

impl Source {
    /// Parse `bytes`, which must pass [`sniff`]. Reads the frame table and
    /// measures each frame's stored data; decodes no pixels.
    pub fn read(bytes: Vec<u8>) -> Result<Self, Error> {
        if !sniff(&bytes, Some(bytes.len())) {
            return Err(Error::NotShp);
        }
        let data = &bytes;
        let width = crate::codec::u16_at(data, 2);
        let height = crate::codec::u16_at(data, 4);
        let count = usize::from(crate::codec::u16_at(data, 6));
        let mut frames: Vec<SourceFrame> = (0..count)
            .map(|i| {
                let header = FrameHeader::read(data, i);
                // Exactly when `decode_frame` gives pixels.
                let drawn = header.has_data() && (header.offset as usize) < data.len();
                let start = header.offset as usize;
                let span = if drawn { frame_span(data, &header) } else { 0 };
                SourceFrame {
                    header,
                    drawn,
                    payload: start..start + span,
                    pad: start..start,
                }
            })
            .collect();

        // The gaps BETWEEN data blocks are not always zero padding: real
        // files leave fragments of whatever the writer's buffer held there.
        // Each gap travels with the frame whose data follows it, in file
        // order (header order and file order need not agree). Only frames
        // with data take part: an empty frame's offset is never written to.
        let mut order: Vec<usize> = (0..count)
            .filter(|&i| !frames[i].payload.is_empty())
            .collect();
        order.sort_by_key(|&i| frames[i].payload.start);
        let mut cursor = HEADER + FRAME_HEADER * count;
        for &i in &order {
            let payload = frames[i].payload.clone();
            if payload.start > cursor {
                frames[i].pad = cursor..payload.start;
            }
            cursor = payload.end;
        }

        // Whatever follows the last frame's data. Some writers round the
        // file up to 8 and some do not, and the frame table cannot tell them
        // apart, so it is remembered rather than inferred.
        // The frame whose data starts last (the first of equals, as Ochre).
        let mut last: Option<&Range<usize>> = None;
        for &i in &order {
            let payload = &frames[i].payload;
            if last.is_none_or(|l| payload.start > l.start) {
                last = Some(payload);
            }
        }
        let tail = match last.map(|l| l.end) {
            Some(end) if end > 0 && end <= bytes.len() => end..bytes.len(),
            _ => bytes.len()..bytes.len(),
        };
        Ok(Source {
            width,
            height,
            frames,
            tail,
            bytes,
        })
    }

    /// The original file.
    pub fn bytes(&self) -> &[u8] {
        &self.bytes
    }

    pub fn width(&self) -> u16 {
        self.width
    }

    pub fn height(&self) -> u16 {
        self.height
    }

    pub fn frames(&self) -> &[SourceFrame] {
        &self.frames
    }

    /// Frame `i`'s original data bytes (empty for an empty frame).
    pub fn payload(&self, i: usize) -> &[u8] {
        &self.bytes[self.frames[i].payload.clone()]
    }

    /// The gap stored before frame `i`'s data.
    pub fn pad(&self, i: usize) -> &[u8] {
        &self.bytes[self.frames[i].pad.clone()]
    }

    /// The bytes after the last frame's data.
    pub fn tail(&self) -> &[u8] {
        &self.bytes[self.tail.clone()]
    }

    /// Frame `i` decoded: its `w * h` cropped indices, or `None` when the
    /// file stores no pixels for it (or there is no frame `i`).
    pub fn pixels(&self, i: usize) -> Option<Vec<u8>> {
        let frame = self.frames.get(i).filter(|f| f.drawn)?;
        decode_frame(&self.bytes, &frame.header)
    }

    /// Frame `i` decoded, with its header.
    pub fn frame(&self, i: usize) -> Option<Frame> {
        let frame = self.frames.get(i)?;
        Some(Frame {
            header: frame.header,
            pixels: self.pixels(i),
        })
    }

    /// The decoded sprite (every frame decoded).
    pub fn sprite(&self) -> Sprite {
        Sprite {
            width: self.width,
            height: self.height,
            frames: (0..self.frames.len())
                .filter_map(|i| self.frame(i))
                .collect(),
        }
    }

    /// Frame `i` on the full canvas (0 outside its rectangle).
    pub fn canvas(&self, i: usize) -> Vec<u8> {
        let (cw, ch) = (usize::from(self.width), usize::from(self.height));
        let mut out = vec![0u8; cw * ch];
        if let Some(frame) = self.frame(i) {
            blit(&mut out, cw, ch, &frame);
        }
        out
    }

    /// Every frame on the full canvas.
    pub fn canvases(&self) -> Vec<Vec<u8>> {
        (0..self.frames.len()).map(|i| self.canvas(i)).collect()
    }

    /// Do `planes` (full canvases) hold exactly what this file decodes to?
    pub fn unchanged(&self, width: u16, height: u16, planes: &[Vec<u8>]) -> bool {
        width == self.width
            && height == self.height
            && planes.len() == self.frames.len()
            && planes
                .iter()
                .enumerate()
                .all(|(i, plane)| *plane == self.canvas(i))
    }
}

/// A rectangle in canvas pixels.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
struct Rect {
    x: usize,
    y: usize,
    w: usize,
    h: usize,
}

impl Rect {
    fn contains(&self, other: &Rect) -> bool {
        other.x >= self.x
            && other.y >= self.y
            && other.x + other.w <= self.x + self.w
            && other.y + other.h <= self.y + self.h
    }
}

/// Tight bounds of the non-zero indices of a `cw`-wide canvas.
fn crop_bounds(plane: &[u8], cw: usize) -> Option<Rect> {
    let (mut x0, mut y0, mut x1, mut y1) = (usize::MAX, usize::MAX, 0, 0);
    let mut any = false;
    for (y, row) in plane.chunks_exact(cw).enumerate() {
        let Some(first) = row.iter().position(|&v| v != 0) else {
            continue;
        };
        let last = row.iter().rposition(|&v| v != 0).unwrap_or(first);
        any = true;
        x0 = x0.min(first);
        x1 = x1.max(last);
        y0 = y0.min(y);
        y1 = y;
    }
    any.then(|| Rect {
        x: x0,
        y: y0,
        w: x1 - x0 + 1,
        h: y1 - y0 + 1,
    })
}

fn crop(plane: &[u8], cw: usize, r: &Rect) -> Vec<u8> {
    let mut out = Vec::with_capacity(r.w * r.h);
    for y in r.y..r.y + r.h {
        out.extend_from_slice(&plane[y * cw + r.x..y * cw + r.x + r.w]);
    }
    out
}

fn check_dimensions(width: u16, height: u16, count: usize) -> Result<(), Error> {
    if width == 0
        || height == 0
        || width > MAX_SIDE
        || height > MAX_SIDE
        || count == 0
        || count > usize::from(MAX_FRAMES)
    {
        return Err(Error::BadDimensions {
            width: usize::from(width),
            height: usize::from(height),
            frames: count,
        });
    }
    Ok(())
}

fn check(width: u16, height: u16, planes: &[Vec<u8>]) -> Result<(), Error> {
    check_dimensions(width, height, planes.len())?;
    let expected = usize::from(width) * usize::from(height);
    match planes.iter().position(|p| p.len() != expected) {
        Some(frame) => Err(Error::BadPlane {
            frame,
            len: planes[frame].len(),
            expected,
        }),
        None => Ok(()),
    }
}

/// Serialise full-canvas index planes (`width * height` each, index 0
/// transparent) to SHP bytes, keeping whatever `source` stored for every
/// frame the edit left alone. Frame `i` pairs with source frame `i`; a
/// source whose canvas differs is not paired at all.
///
/// - Nothing changed (same canvas, same frame count, every plane equal to
///   its decode): the original file, verbatim.
/// - Otherwise [`build_frames`] writes it frame by frame.
pub fn build_preserving(
    width: u16,
    height: u16,
    planes: &[Vec<u8>],
    source: Option<&Source>,
) -> Result<Vec<u8>, Error> {
    check(width, height, planes)?;
    if let Some(source) = source {
        if source.unchanged(width, height, planes) {
            return Ok(source.bytes.clone());
        }
    }
    build_frames(width, height, planes, source)
}

/// One frame handed to [`build_with`].
#[derive(Debug, Clone)]
pub enum FramePlane<'a> {
    /// Exactly what source frame `i` decodes to: the caller knows it is
    /// untouched, so it is written as [`build_frames`] writes an unchanged
    /// frame without being decoded or compared.
    Unchanged,
    /// The frame's full canvas (`width * height` indices).
    Pixels(Cow<'a, [u8]>),
}

/// The per-frame writer behind [`build_preserving`], without its whole-file
/// shortcut. The rules, from Ochre's `build_shp`:
///
/// - An **unchanged** frame (same rectangle, same pixels) keeps its payload,
///   flags, radar colour and reserved word; its original gap bytes come back
///   when its offset still lines up. Frames that shared one data block in
///   the source keep sharing it.
/// - A **changed** frame that had data keeps its old rectangle while the
///   content still fits inside it, else it is re-cropped; it keeps its flag
///   word and its compression family (2 prefixed, RLE bit RLE-Zero, else
///   raw), radar and reserved.
/// - A **new** frame, or one that was empty in the source, is cropped and
///   stored RLE-Zero only when that is smaller than raw, as XCC does.
/// - An empty frame that stays empty keeps its 24 header bytes.
/// - Data is placed at the original offset when it is still ahead, else
///   aligned to 8; the source's trailing bytes are kept.
pub fn build_frames(
    width: u16,
    height: u16,
    planes: &[Vec<u8>],
    source: Option<&Source>,
) -> Result<Vec<u8>, Error> {
    check(width, height, planes)?;
    build_with(width, height, planes.len(), source, |i| {
        FramePlane::Pixels(Cow::Borrowed(&planes[i]))
    })
}

/// [`build_frames`] with the frames supplied one at a time by `plane(i)`, so
/// a caller holding most frames as "unchanged" never decodes or keeps them
/// all at once. [`FramePlane::Unchanged`] needs a paired source frame `i`.
pub fn build_with<'a>(
    width: u16,
    height: u16,
    count: usize,
    source: Option<&Source>,
    plane: impl FnMut(usize) -> FramePlane<'a>,
) -> Result<Vec<u8>, Error> {
    build_mapped(width, height, count, source, Some, plane)
}

/// A content-addressed index of a source's drawn frames: the hash of each
/// full canvas, built on first use.
struct ContentIndex<'s> {
    source: &'s Source,
    by_hash: Option<HashMap<u64, Vec<usize>>>,
}

fn canvas_hash(plane: &[u8]) -> u64 {
    use std::hash::{Hash, Hasher};
    let mut h = std::collections::hash_map::DefaultHasher::new();
    plane.hash(&mut h);
    h.finish()
}

impl ContentIndex<'_> {
    /// A drawn source frame whose full canvas is exactly `plane`.
    fn find(&mut self, plane: &[u8]) -> Option<usize> {
        if plane.iter().all(|&v| v == 0) {
            return None;
        }
        let source = self.source;
        let by_hash = self.by_hash.get_or_insert_with(|| {
            let mut map: HashMap<u64, Vec<usize>> = HashMap::new();
            for j in (0..source.frames.len()).filter(|&j| source.frames[j].drawn) {
                map.entry(canvas_hash(&source.canvas(j)))
                    .or_default()
                    .push(j);
            }
            map
        });
        by_hash
            .get(&canvas_hash(plane))?
            .iter()
            .copied()
            .find(|&j| source.canvas(j) == plane)
    }
}

/// [`build_with`] with output frame `i` paired with source frame `pair(i)`
/// (`None`: a frame the source never had) instead of source frame `i`, so
/// moved, duplicated and inserted frames keep what their own source frame
/// stored. A [`FramePlane::Pixels`] frame that is not exactly its paired
/// frame's decode, but is exactly another source frame's, is written as that
/// frame (content match): a frame moved by hand keeps its bytes too.
///
/// A frame keeps its original offset (and gap bytes) only while it sits at
/// its own position (`pair(i) == Some(i)`); a moved frame is aligned to 8,
/// so a reorder never leaves a hole. A source frame written twice (a
/// duplicate) gets a second copy of its bytes; only frames that shared one
/// data block in the source keep sharing it.
pub fn build_mapped<'a>(
    width: u16,
    height: u16,
    count: usize,
    source: Option<&Source>,
    pair: impl Fn(usize) -> Option<usize>,
    mut plane: impl FnMut(usize) -> FramePlane<'a>,
) -> Result<Vec<u8>, Error> {
    check_dimensions(width, height, count)?;
    let source = source.filter(|s| s.width == width && s.height == height);
    let cw = usize::from(width);
    let expected = cw * usize::from(height);
    let base = HEADER + FRAME_HEADER * count;
    let mut headers = Vec::with_capacity(FRAME_HEADER * count);
    let mut body: Vec<u8> = Vec::new();
    // Source offset -> (payload, output offset, source frame) of payloads
    // already copied verbatim.
    let mut shared: HashMap<u32, (Range<usize>, u32, usize)> = HashMap::new();
    let mut content = source.map(|source| ContentIndex {
        source,
        by_hash: None,
    });

    for i in 0..count {
        let mut given = plane(i);
        let mut j = source.and_then(|s| pair(i).filter(|&j| j < s.frames.len()));
        if let (Some(s), Some(index), FramePlane::Pixels(pixels)) =
            (source, content.as_mut(), &given)
        {
            if pixels.len() == expected && j.is_none_or(|j| s.canvas(j)[..] != pixels[..]) {
                if let Some(found) = index.find(pixels) {
                    j = Some(found);
                    given = FramePlane::Unchanged;
                }
            }
        }
        let was = j.and_then(|j| source.map(|s| (s, &s.frames[j])));
        let old = was.and_then(|(_, f)| {
            let h = &f.header;
            f.drawn.then_some(Rect {
                x: usize::from(h.x),
                y: usize::from(h.y),
                w: usize::from(h.w),
                h: usize::from(h.h),
            })
        });
        let drawn = was.filter(|(_, f)| f.drawn);
        let rect = match &given {
            FramePlane::Unchanged => {
                if was.is_none() {
                    return Err(Error::BadPlane {
                        frame: i,
                        len: 0,
                        expected,
                    });
                }
                old
            }
            FramePlane::Pixels(pixels) => {
                if pixels.len() != expected {
                    return Err(Error::BadPlane {
                        frame: i,
                        len: pixels.len(),
                        expected,
                    });
                }
                // Keep the stored rectangle while the content still fits
                // inside it; honouring it unconditionally would drop edits
                // made outside it.
                match (old, crop_bounds(pixels, cw)) {
                    (Some(old), None) => Some(old),
                    (Some(old), Some(new)) if old.contains(&new) => Some(old),
                    (_, content) => content,
                }
            }
        };
        let Some(rect) = rect else {
            // Empty and empty before: the header as it was, or all zeros.
            let header = was.map(|(_, f)| f.header).unwrap_or_default();
            headers.extend_from_slice(&header.to_bytes());
            continue;
        };

        let mut header = FrameHeader {
            x: rect.x as u16,
            y: rect.y as u16,
            w: rect.w as u16,
            h: rect.h as u16,
            ..FrameHeader::default()
        };
        if let Some((_, f)) = was {
            header.radar = f.header.radar;
            header.reserved = f.header.reserved;
        }
        let mut verbatim: Option<(&Source, &SourceFrame)> = None;
        let payload: Cow<[u8]> = match (&given, drawn) {
            // An untouched drawn frame: its stored bytes, never decoded.
            (FramePlane::Unchanged, Some((s, f))) => {
                header.flags = f.header.flags;
                verbatim = Some((s, f));
                Cow::Borrowed(&s.bytes[f.payload.clone()])
            }
            (FramePlane::Unchanged, None) => unreachable!("an undrawn frame has no rectangle"),
            (FramePlane::Pixels(pixels), Some((s, f))) => {
                header.flags = f.header.flags;
                let sub = crop(pixels, cw, &rect);
                if Some(rect) == old && j.and_then(|j| s.pixels(j)).as_deref() == Some(&sub[..]) {
                    verbatim = Some((s, f));
                    Cow::Borrowed(&s.bytes[f.payload.clone()])
                } else if f.header.compression() == 2 {
                    Cow::Owned(encode_prefixed(&sub, rect.w))
                } else if f.header.flags & 2 != 0 {
                    Cow::Owned(encode_rle(&sub, rect.w))
                } else {
                    Cow::Owned(sub)
                }
            }
            (FramePlane::Pixels(pixels), None) => {
                let sub = crop(pixels, cw, &rect);
                let rle = encode_rle(&sub, rect.w);
                if rle.len() < sub.len() {
                    header.flags = 3;
                    Cow::Owned(rle)
                } else {
                    header.flags = 0;
                    Cow::Owned(sub)
                }
            }
        };

        let j = j.unwrap_or(usize::MAX);
        if let Some((_, f)) = verbatim {
            let key = f.header.offset;
            if let Some((range, offset, first)) = shared.get(&key) {
                if *range == f.payload && *first != j {
                    header.offset = *offset;
                    headers.extend_from_slice(&header.to_bytes());
                    continue;
                }
            }
        }

        let here = base + body.len();
        let mut pad_len = (8 - here % 8) % 8;
        let mut pad: Option<&[u8]> = None;
        if let Some((s, f)) = drawn.filter(|_| j == i) {
            let want = f.header.offset as usize;
            if want >= here {
                // The original gap, its bytes rather than zeros, when the
                // data still lands on its original offset.
                pad_len = want - here;
                let original = &s.bytes[f.pad.clone()];
                if original.len() == pad_len {
                    pad = Some(original);
                }
            }
        }
        match pad {
            Some(bytes) => body.extend_from_slice(bytes),
            None => body.resize(body.len() + pad_len, 0),
        }
        let offset = u32::try_from(base + body.len()).map_err(|_| Error::TooLarge)?;
        header.offset = offset;
        if let Some((_, f)) = verbatim {
            shared
                .entry(f.header.offset)
                .or_insert((f.payload.clone(), offset, j));
        }
        body.extend_from_slice(&payload);
        headers.extend_from_slice(&header.to_bytes());
    }

    let tail = source.map(Source::tail).unwrap_or(&[]);
    let mut out = Vec::with_capacity(base + body.len() + tail.len());
    out.extend_from_slice(&0u16.to_le_bytes());
    out.extend_from_slice(&width.to_le_bytes());
    out.extend_from_slice(&height.to_le_bytes());
    out.extend_from_slice(&(count as u16).to_le_bytes());
    out.extend_from_slice(&headers);
    out.extend_from_slice(&body);
    out.extend_from_slice(tail);
    Ok(out)
}

/// Serialise planes with no source: each frame cropped to its content and
/// stored RLE-Zero when that is smaller, raw otherwise, aligned to 8.
pub fn build(width: u16, height: u16, planes: &[Vec<u8>]) -> Result<Vec<u8>, Error> {
    build_frames(width, height, planes, None)
}
