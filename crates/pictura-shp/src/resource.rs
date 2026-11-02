// SPDX-License-Identifier: GPL-3.0-or-later

//! The private `KookaSHP` image resource: everything an SHP document needs
//! beyond its pixels, kept inside the document so undo, redo and a PSD round
//! trip carry it with the pixels.
//!
//! The block is `8BIM`, id [`RESOURCE_ID`] (the public PSD specification sets
//! 4000-4999 aside for plug-ins), name [`RESOURCE_NAME`]. Its data, all
//! integers big-endian:
//!
//! | Field | Bytes |
//! |---|---|
//! | magic `KSHP` | 4 |
//! | format version (1 or 2) | 2 |
//! | canvas width, height | 2 + 2 |
//! | frames on the sheet, columns, gutter | 2 + 2 + 2 |
//! | version 2 only: first frame on the sheet, frames in the file, window size | 2 + 2 + 2 |
//! | palette: 256 x `0xAARRGGBB` as read from the `.pal` | 1024 |
//! | palette name, palette path: u16 length + UTF-8 each | 2 + n, 2 + n |
//! | original file: u32 length + bytes (0 for a sheet never read from a file) | 4 + n |
//! | version 2 and 3: edited frames off the sheet: u16 count, then per frame u16 index, u32 length, RLE-Zero lines of its full canvas | 2 + ... |
//! | version 3 only: frame origins: u16 count (the frames in the file), then per frame the original file's frame it came from, `0xFFFF` for an added frame | 2 + 2n |
//!
//! Version 1 is a sheet holding every frame. Version 2 is a **window**: the
//! sheet holds frames `first..first + frames` of `total` (windows are `span`
//! frames, aligned to multiples of it; the last may be shorter), and the frames
//! outside it that were edited keep their current indices here (the
//! **frame store**); every other frame is still the original file's. Version
//! 1 is written whenever the window is the whole file and nothing is stored,
//! so a document that never pages carries exactly the block it always did.
//!
//! Version 3 adds the **frame origins** once frames were inserted, deleted,
//! duplicated or moved: frame `i` of the document came from the original
//! file's frame `origins[i]` (or is new). Saving pairs each frame with its
//! origin, so a moved frame keeps its stored bytes. Without the field the
//! origins are the identity: frame `i` is the file's frame `i`.
//!
//! The fixed fields come first, so [`Header::read`] answers layout questions
//! without copying the original file.

use crate::codec::{decode_rle_line, encode_rle};
use crate::palette::Palette;
use crate::sheet::Layout;

/// The image-resource id of the block.
pub const RESOURCE_ID: u16 = 4848;
/// The block's Pascal name.
pub const RESOURCE_NAME: &str = "KookaSHP";
const MAGIC: &[u8; 4] = b"KSHP";
const PALETTE_AT: usize = 4 + 2 + 2 * 5;

/// The palette offset (and the size of the fixed fields before it) for a
/// format version.
fn palette_at(version: u16) -> usize {
    PALETTE_AT + if version >= 2 { 6 } else { 0 }
}

/// The fixed part of the block.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Header {
    /// The sheet: `layout.frames` frames from `first`.
    pub layout: Layout,
    /// The palette as read, before [`crate::palette::unique_colours`].
    pub palette: Palette,
    /// The file frame in the sheet's first cell.
    pub first: usize,
    /// The frames in the file.
    pub total: usize,
    /// The window size paging uses: `layout.frames`, except on a short last
    /// window.
    pub span: usize,
}

impl Header {
    /// A header whose sheet holds every frame.
    pub fn whole(layout: Layout, palette: Palette) -> Header {
        Header {
            first: 0,
            total: layout.frames,
            span: layout.frames,
            layout,
            palette,
        }
    }

    /// Does the sheet hold only some of the file's frames?
    pub fn paged(&self) -> bool {
        self.first != 0 || self.total != self.layout.frames
    }

    /// The file frames on the sheet.
    pub fn window(&self) -> std::ops::Range<usize> {
        self.first..self.first + self.layout.frames
    }
}

fn u16_at(data: &[u8], at: usize) -> Option<u16> {
    Some(u16::from_be_bytes(data.get(at..at + 2)?.try_into().ok()?))
}

fn u32_at(data: &[u8], at: usize) -> Option<u32> {
    Some(u32::from_be_bytes(data.get(at..at + 4)?.try_into().ok()?))
}

fn string_at(data: &[u8], at: usize) -> Option<(String, usize)> {
    let len = usize::from(u16_at(data, at)?);
    let bytes = data.get(at + 2..at + 2 + len)?;
    Some((String::from_utf8_lossy(bytes).into_owned(), at + 2 + len))
}

/// The version of a block's data, when it is one this code understands.
fn version(data: &[u8]) -> Option<u16> {
    if data.len() < 6 || &data[..4] != MAGIC {
        return None;
    }
    let v = u16_at(data, 4)?;
    (1..=3).contains(&v).then_some(v)
}

impl Header {
    /// The fixed fields and palette name of a block's data; `None` when it is
    /// not a `KookaSHP` block this version understands.
    pub fn read(data: &[u8]) -> Option<Header> {
        let version = version(data)?;
        let at = palette_at(version);
        if data.len() < at + 1024 {
            return None;
        }
        let field = |k: usize| u16_at(data, 6 + 2 * k).map(usize::from);
        let layout = Layout {
            width: field(0)?,
            height: field(1)?,
            frames: field(2)?,
            cols: field(3)?,
            gutter: field(4)?,
        };
        if !layout.fits() {
            return None;
        }
        let (first, total, span) = if version >= 2 {
            (field(5)?, field(6)?, field(7)?)
        } else {
            (0, layout.frames, layout.frames)
        };
        if first + layout.frames > total || span < layout.frames {
            return None;
        }
        let mut argb = [0u32; 256];
        for (i, c) in argb.iter_mut().enumerate() {
            *c = u32_at(data, at + 4 * i)?;
        }
        let (name, _) = string_at(data, at + 1024)?;
        Some(Header {
            layout,
            palette: Palette { argb, name },
            first,
            total,
            span,
        })
    }
}

/// One frame of the store: its current indices, as RLE-Zero lines of its
/// full canvas (compact: a sprite is mostly transparent).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct StoredFrame {
    pub frame: usize,
    pub data: Vec<u8>,
}

impl StoredFrame {
    /// Store a `width`-wide full canvas.
    pub fn new(frame: usize, plane: &[u8], width: usize) -> StoredFrame {
        StoredFrame {
            frame,
            data: encode_rle(plane, width),
        }
    }
}

/// A stored frame's full `width x height` canvas back.
pub fn stored_canvas(data: &[u8], width: usize, height: usize) -> Vec<u8> {
    let mut out = vec![0u8; width * height];
    let mut pos = 0;
    for row in out.chunks_exact_mut(width.max(1)) {
        pos = decode_rle_line(data, pos, row);
    }
    out
}

/// Where each document frame came from: `Some(j)` is the original file's
/// frame `j`, `None` a frame added in the editor.
pub type Origins = Vec<Option<usize>>;

const NO_ORIGIN: u16 = 0xFFFF;

/// The frames in an SHP file's header; 0 for no file.
pub fn file_frames(original: &[u8]) -> usize {
    original
        .get(6..8)
        .map_or(0, |b| usize::from(u16::from_le_bytes([b[0], b[1]])))
}

/// The origins of an untouched frame order: frame `i` is the file's frame
/// `i`, and frames past the file's count are new.
pub fn identity_origins(total: usize, file_frames: usize) -> Origins {
    (0..total).map(|i| (i < file_frames).then_some(i)).collect()
}

/// The whole block.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ShpResource {
    pub header: Header,
    /// Where the palette was loaded from (empty for the built-in fallback).
    pub palette_path: String,
    /// The bytes the document was opened from; empty for a new sheet.
    pub original: Vec<u8>,
    /// Edited frames outside the window, ascending by frame.
    pub stored: Vec<StoredFrame>,
    /// Where each of the `header.total` frames came from.
    pub origins: Origins,
}

impl ShpResource {
    pub fn encode(&self) -> Vec<u8> {
        encode_parts(
            &self.header,
            &self.palette_path,
            &self.original,
            self.stored.iter().map(|f| (f.frame, &f.data[..])),
            Some(&self.origins),
        )
    }

    pub fn decode(data: &[u8]) -> Option<ShpResource> {
        let (header, palette_path, original) = parts(data)?;
        let stored = stored(data)?
            .into_iter()
            .map(|(frame, data)| StoredFrame {
                frame,
                data: data.to_vec(),
            })
            .collect();
        Some(ShpResource {
            origins: origins(data)?,
            header,
            palette_path,
            original: original.to_vec(),
            stored,
        })
    }
}

/// A block's data from its parts, without first copying the original file
/// or the stored frames into a [`ShpResource`]. Version 1 unless the sheet
/// is a window or frames are stored (2), or `origins` (`None`: the
/// identity) is not the identity (3).
pub fn encode_parts<'a>(
    header: &Header,
    palette_path: &str,
    original: &[u8],
    stored: impl Iterator<Item = (usize, &'a [u8])> + Clone,
    origins: Option<&[Option<usize>]>,
) -> Vec<u8> {
    let l = &header.layout;
    let origins =
        origins.filter(|o| **o != identity_origins(header.total, file_frames(original))[..]);
    let paged = header.paged() || stored.clone().next().is_some() || origins.is_some();
    let version: u16 = match (origins.is_some(), paged) {
        (true, _) => 3,
        (false, true) => 2,
        (false, false) => 1,
    };
    let name = header.palette.name.as_bytes();
    let path = palette_path.as_bytes();
    let name = &name[..name.len().min(usize::from(u16::MAX))];
    let path = &path[..path.len().min(usize::from(u16::MAX))];
    let stored_len: usize = stored.clone().map(|(_, d)| 6 + d.len()).sum();
    let mut out = Vec::with_capacity(
        palette_at(version) + 1024 + 8 + name.len() + path.len() + original.len() + stored_len + 2,
    );
    out.extend_from_slice(MAGIC);
    out.extend_from_slice(&version.to_be_bytes());
    let mut fields = vec![l.width, l.height, l.frames, l.cols, l.gutter];
    if paged {
        fields.extend([header.first, header.total, header.span]);
    }
    for v in fields {
        out.extend_from_slice(&(v.min(usize::from(u16::MAX)) as u16).to_be_bytes());
    }
    for c in header.palette.argb {
        out.extend_from_slice(&c.to_be_bytes());
    }
    for s in [name, path] {
        out.extend_from_slice(&(s.len() as u16).to_be_bytes());
        out.extend_from_slice(s);
    }
    out.extend_from_slice(&(original.len() as u32).to_be_bytes());
    out.extend_from_slice(original);
    if paged {
        let count = stored.clone().count();
        out.extend_from_slice(&(count.min(usize::from(u16::MAX)) as u16).to_be_bytes());
        for (frame, data) in stored {
            out.extend_from_slice(&(frame.min(usize::from(u16::MAX)) as u16).to_be_bytes());
            out.extend_from_slice(&(data.len() as u32).to_be_bytes());
            out.extend_from_slice(data);
        }
    }
    if let Some(origins) = origins {
        out.extend_from_slice(&(origins.len().min(usize::from(u16::MAX)) as u16).to_be_bytes());
        for o in origins {
            let v = o.map_or(NO_ORIGIN, |j| j.min(usize::from(NO_ORIGIN - 1)) as u16);
            out.extend_from_slice(&v.to_be_bytes());
        }
    }
    out
}

/// Where the original file's length field starts, and the parsed header
/// and palette path.
fn after_strings(data: &[u8]) -> Option<(Header, String, usize)> {
    let header = Header::read(data)?;
    let version = version(data)?;
    let (_, at) = string_at(data, palette_at(version) + 1024)?;
    let (palette_path, at) = string_at(data, at)?;
    Some((header, palette_path, at))
}

/// A block's header, palette path and original file bytes, the last without
/// copying them.
pub fn parts(data: &[u8]) -> Option<(Header, String, &[u8])> {
    let (header, palette_path, at) = after_strings(data)?;
    let len = u32_at(data, at)? as usize;
    let original = data.get(at + 4..at + 4 + len)?;
    Some((header, palette_path, original))
}

/// A block's stored frames (index and RLE-Zero data), without copying them;
/// empty for version 1.
pub fn stored(data: &[u8]) -> Option<Vec<(usize, &[u8])>> {
    stored_and_end(data).map(|(stored, _)| stored)
}

/// Every frame's origin: the version 3 field, or the identity.
pub fn origins(data: &[u8]) -> Option<Origins> {
    let (header, _, original) = parts(data)?;
    if version(data)? < 3 {
        return Some(identity_origins(header.total, file_frames(original)));
    }
    let (_, at) = stored_and_end(data)?;
    let count = usize::from(u16_at(data, at)?);
    if count != header.total {
        return None;
    }
    (0..count)
        .map(|k| u16_at(data, at + 2 + 2 * k).map(|v| (v != NO_ORIGIN).then_some(usize::from(v))))
        .collect()
}

/// Stored frames as (frame, RLE-Zero data) borrowed from a block.
type StoredSlices<'a> = Vec<(usize, &'a [u8])>;

/// The stored frames and where the data after them starts.
fn stored_and_end(data: &[u8]) -> Option<(StoredSlices<'_>, usize)> {
    let (_, _, at) = after_strings(data)?;
    let len = u32_at(data, at)? as usize;
    let mut at = at + 4 + len;
    if version(data)? < 2 {
        return Some((Vec::new(), at));
    }
    let count = usize::from(u16_at(data, at)?);
    at += 2;
    let mut out = Vec::with_capacity(count);
    for _ in 0..count {
        let frame = usize::from(u16_at(data, at)?);
        let len = u32_at(data, at + 2)? as usize;
        out.push((frame, data.get(at + 6..at + 6 + len)?));
        at += 6 + len;
    }
    Some((out, at))
}

/// The data of the first `8BIM` block with `id` in an image-resource section
/// (a run of `signature id name size data` blocks), without copying it.
/// Parsing stops at anything that is not a well-formed block.
pub fn find_block(section: &[u8], id: u16) -> Option<&[u8]> {
    let mut at = 0;
    while let Some((block_id, data, next)) = block_at(section, at) {
        if block_id == Some(id) {
            return Some(data);
        }
        at = next;
    }
    None
}

/// One block at `at`: its id (`None` for a signature other than `8BIM`), its
/// data, and where the next block starts.
fn block_at(section: &[u8], at: usize) -> Option<(Option<u16>, &[u8], usize)> {
    let signature = section.get(at..at + 4)?;
    if !matches!(
        signature,
        b"8BIM" | b"8B64" | b"MeSa" | b"AgHg" | b"PHUT" | b"DCSR"
    ) {
        return None;
    }
    let id = u16::from_be_bytes(section.get(at + 4..at + 6)?.try_into().ok()?);
    let name_len = usize::from(*section.get(at + 6)?);
    let mut pos = at + 7 + name_len;
    if !(name_len + 1).is_multiple_of(2) {
        pos += 1;
    }
    let size = u32::from_be_bytes(section.get(pos..pos + 4)?.try_into().ok()?) as usize;
    pos += 4;
    let data = section.get(pos..pos + size)?;
    pos += size + size % 2;
    if pos > section.len() {
        return None;
    }
    let id = (signature == b"8BIM").then_some(id);
    Some((id, data, pos))
}

/// `section` with the `8BIM` block `id` replaced by one holding `data` (or
/// appended after the parsed blocks when there is none). Every other block,
/// and any unparsed remainder, is kept byte for byte.
pub fn set_block(section: &[u8], id: u16, name: &str, data: &[u8]) -> Vec<u8> {
    let mut block = Vec::with_capacity(16 + name.len() + data.len());
    let name = &name.as_bytes()[..name.len().min(255)];
    block.extend_from_slice(b"8BIM");
    block.extend_from_slice(&id.to_be_bytes());
    block.push(name.len() as u8);
    block.extend_from_slice(name);
    if !(name.len() + 1).is_multiple_of(2) {
        block.push(0);
    }
    block.extend_from_slice(&(data.len() as u32).to_be_bytes());
    block.extend_from_slice(data);
    if data.len() % 2 == 1 {
        block.push(0);
    }

    let mut out = Vec::with_capacity(section.len() + block.len());
    let mut at = 0;
    let mut placed = false;
    while let Some((block_id, _, next)) = block_at(section, at) {
        if block_id == Some(id) && !placed {
            out.extend_from_slice(&block);
            placed = true;
        } else if block_id != Some(id) {
            out.extend_from_slice(&section[at..next]);
        }
        at = next;
    }
    if !placed {
        out.extend_from_slice(&block);
    }
    out.extend_from_slice(&section[at..]);
    out
}
