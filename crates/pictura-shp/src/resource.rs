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
//! | format version (1) | 2 |
//! | canvas width, height | 2 + 2 |
//! | frame count, columns, gutter | 2 + 2 + 2 |
//! | palette: 256 x `0xAARRGGBB` as read from the `.pal` | 1024 |
//! | palette name, palette path: u16 length + UTF-8 each | 2 + n, 2 + n |
//! | original file: u32 length + bytes (0 for a sheet never read from a file) | 4 + n |
//!
//! The fixed fields come first, so [`Header::read`] answers layout questions
//! without copying the original file.

use crate::palette::Palette;
use crate::sheet::Layout;

/// The image-resource id of the block.
pub const RESOURCE_ID: u16 = 4848;
/// The block's Pascal name.
pub const RESOURCE_NAME: &str = "KookaSHP";
const MAGIC: &[u8; 4] = b"KSHP";
const VERSION: u16 = 1;
const FIXED: usize = 4 + 2 + 2 * 5 + 1024;

/// The fixed part of the block.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Header {
    pub layout: Layout,
    /// The palette as read, before [`crate::palette::unique_colours`].
    pub palette: Palette,
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

impl Header {
    /// The fixed fields and palette name of a block's data; `None` when it is
    /// not a `KookaSHP` block this version understands.
    pub fn read(data: &[u8]) -> Option<Header> {
        if data.len() < FIXED || &data[..4] != MAGIC || u16_at(data, 4)? != VERSION {
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
        let mut argb = [0u32; 256];
        for (i, c) in argb.iter_mut().enumerate() {
            *c = u32_at(data, 16 + 4 * i)?;
        }
        let (name, _) = string_at(data, FIXED)?;
        Some(Header {
            layout,
            palette: Palette { argb, name },
        })
    }
}

/// The whole block.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ShpResource {
    pub header: Header,
    /// Where the palette was loaded from (empty for the built-in fallback).
    pub palette_path: String,
    /// The bytes the document was opened from; empty for a new sheet.
    pub original: Vec<u8>,
}

impl ShpResource {
    pub fn encode(&self) -> Vec<u8> {
        let l = &self.header.layout;
        let name = self.header.palette.name.as_bytes();
        let path = self.palette_path.as_bytes();
        let name = &name[..name.len().min(usize::from(u16::MAX))];
        let path = &path[..path.len().min(usize::from(u16::MAX))];
        let mut out = Vec::with_capacity(FIXED + 8 + name.len() + path.len() + self.original.len());
        out.extend_from_slice(MAGIC);
        out.extend_from_slice(&VERSION.to_be_bytes());
        for v in [l.width, l.height, l.frames, l.cols, l.gutter] {
            out.extend_from_slice(&(v.min(usize::from(u16::MAX)) as u16).to_be_bytes());
        }
        for c in self.header.palette.argb {
            out.extend_from_slice(&c.to_be_bytes());
        }
        for s in [name, path] {
            out.extend_from_slice(&(s.len() as u16).to_be_bytes());
            out.extend_from_slice(s);
        }
        out.extend_from_slice(&(self.original.len() as u32).to_be_bytes());
        out.extend_from_slice(&self.original);
        out
    }

    pub fn decode(data: &[u8]) -> Option<ShpResource> {
        let (header, palette_path, original) = parts(data)?;
        Some(ShpResource {
            header,
            palette_path,
            original: original.to_vec(),
        })
    }
}

/// A block's header, palette path and original file bytes, the last without
/// copying them.
pub fn parts(data: &[u8]) -> Option<(Header, String, &[u8])> {
    let header = Header::read(data)?;
    let (_, at) = string_at(data, FIXED)?;
    let (palette_path, at) = string_at(data, at)?;
    let len = u32_at(data, at)? as usize;
    let original = data.get(at + 4..at + 4 + len)?;
    Some((header, palette_path, original))
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
