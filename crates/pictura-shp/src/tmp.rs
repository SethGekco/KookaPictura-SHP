// Derived from Ochre's C&C addon (`addons/cnc/cnctmp.py`), Copyright (C)
// 2026 Ochre contributors, GPL-3.0-or-later, by the owner of this fork.
// SPDX-License-Identifier: GPL-3.0-or-later

//! TMP isometric terrain templates (Tiberian Sun / Red Alert 2).
//!
//! The file header is four little-endian `i32`s: the template size in tiles
//! (`blocks_x`, `blocks_y`) and the tile size in pixels (`cx`, `cy`: 48x24
//! in Tiberian Sun, 60x30 in Red Alert 2). An offset table of
//! `blocks_x * blocks_y` `i32` file offsets follows; **0 is an empty slot**,
//! and most slots of a diamond-shaped template are empty.
//!
//! A tile record is a 52-byte header ([`TILE_HEADER`]) followed by data:
//!
//! | Bytes | Field |
//! |---|---|
//! | 0-7 | `x`, `y`: the tile's position in template pixels |
//! | 8-19 | extra, z and extra-z data offsets, **relative to the record** |
//! | 20-35 | the extra block's `x`, `y` (template pixels), width, height |
//! | 36-39 | flags: bit 0 has extra, bit 1 has z, bit 2 randomised |
//! | 40-42 | height, land type, ramp type |
//! | 43-48 | radar color low, radar color high (RGB each) |
//! | 49-51 | padding |
//!
//! The tile itself is a **diamond**: `cx * cy / 2` bytes holding only the
//! rhombus, row `y` of the top half `4 * (y + 1)` pixels wide (see
//! [`diamond_rows`]). Its z-data is a second diamond: 0-31 a depth, 255 no
//! data, 32-254 invalid. The extra block (art overhanging the tile) and its
//! z-data are plain rectangles.
//!
//! The writer ([`build`]) patches the original file in place whenever the
//! grid is the same, so everything this module does not model (the
//! uninitialised bytes old tools left in unused header fields, gaps, a tail)
//! stays byte for byte: a block that kept its size is written over itself, a
//! block that is new or changed size (z-data, an extra block) and a new
//! tile's whole record are appended at the end of the file, a moved tile
//! gets its header position and offset-table slot rewritten, and a removed
//! tile's slot becomes 0 (its record stays as unreferenced bytes). Only when
//! the grid itself differs, or what was written would not read back as the
//! template, does it lay the records out afresh like Ochre's `build_tmp`,
//! keeping each tile's original 52 header bytes and changing only the fields
//! it manages. [`build_with_layout`] says which of these it did.

use std::fmt;

/// The file header: blocks x/y, tile width/height.
pub const FILE_HEADER: usize = 16;
/// A tile record's header.
pub const TILE_HEADER: usize = 52;
pub const FLAG_EXTRA: u32 = 0x01;
pub const FLAG_Z: u32 = 0x02;
pub const FLAG_RANDOM: u32 = 0x04;
/// The deepest valid z value.
pub const Z_MAX: u8 = 31;
/// "No z-data" for a pixel.
pub const Z_NONE: u8 = 255;
/// The largest template side, in tiles, the sniffer accepts.
pub const MAX_BLOCKS: usize = 256;
/// The largest extra block side, in pixels.
pub const MAX_EXTRA_SIDE: usize = 4096;

/// Why a file is not a TMP this module can read.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Error {
    /// Too short for the file header or its offset table.
    Truncated,
    /// The header declares a size this module does not accept.
    BadHeader,
    /// No slot holds a whole tile.
    NoTiles,
    /// A rebuild was needed, but a slot points at data this module could not
    /// read, so it cannot be carried over.
    Unreadable(usize),
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Error::Truncated => write!(f, "the file ends inside its header or offset table"),
            Error::BadHeader => write!(f, "the header does not describe a TMP terrain template"),
            Error::NoTiles => write!(f, "no slot holds a whole tile"),
            Error::Unreadable(slot) => {
                write!(f, "slot {slot} points at data that cannot be read back")
            }
        }
    }
}

impl std::error::Error for Error {}

fn i32_at(data: &[u8], at: usize) -> Option<i32> {
    Some(i32::from_le_bytes(data.get(at..at + 4)?.try_into().ok()?))
}

fn u32_at(data: &[u8], at: usize) -> Option<u32> {
    Some(u32::from_le_bytes(data.get(at..at + 4)?.try_into().ok()?))
}

/// The rows of the rhombus as `(row, first x, width)`, rows of width 0
/// left out. The widths sum to exactly `cx * cy / 2`.
pub fn diamond_rows(cx: usize, cy: usize) -> Vec<(usize, usize, usize)> {
    let mut out = Vec::with_capacity(cy);
    let mut x = (cx / 2) as isize;
    let mut width = 0isize;
    for y in 0..cy {
        if y < cy / 2 {
            width += 4;
            x -= 2;
        } else {
            width -= 4;
            x += 2;
        }
        if width > 0 && x >= 0 && (x + width) as usize <= cx {
            out.push((y, x as usize, width as usize));
        }
    }
    out
}

/// The bytes of one diamond.
pub fn diamond_bytes(cx: usize, cy: usize) -> usize {
    diamond_rows(cx, cy).iter().map(|r| r.2).sum()
}

/// Is `(x, y)` of a `cx` x `cy` tile inside its diamond?
pub fn in_diamond(cx: usize, cy: usize, x: usize, y: usize) -> bool {
    diamond_rows(cx, cy)
        .iter()
        .any(|&(row, start, width)| row == y && (start..start + width).contains(&x))
}

/// Diamond-packed bytes to a full `cx` x `cy` rectangle, `fill` outside.
pub fn decode_diamond(data: &[u8], cx: usize, cy: usize, fill: u8) -> Vec<u8> {
    let mut rect = vec![fill; cx * cy];
    let mut i = 0;
    for (y, x, width) in diamond_rows(cx, cy) {
        let Some(chunk) = data.get(i..i + width) else {
            break;
        };
        rect[y * cx + x..y * cx + x + width].copy_from_slice(chunk);
        i += width;
    }
    rect
}

/// The exact inverse of [`decode_diamond`].
pub fn encode_diamond(rect: &[u8], cx: usize, cy: usize) -> Vec<u8> {
    let mut out = Vec::with_capacity(cx * cy / 2);
    for (y, x, width) in diamond_rows(cx, cy) {
        out.extend_from_slice(&rect[y * cx + x..y * cx + x + width]);
    }
    out
}

/// The per-tile values a user may change: they are written back exactly as
/// given (several land values share one label, see [`land_label`]).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct TileFields {
    pub height: u8,
    pub land: u8,
    pub ramp: u8,
    pub radar_low: [u8; 3],
    pub radar_high: [u8; 3],
}

impl TileFields {
    fn read(raw: &[u8; TILE_HEADER]) -> TileFields {
        TileFields {
            height: raw[40],
            land: raw[41],
            ramp: raw[42],
            radar_low: [raw[43], raw[44], raw[45]],
            radar_high: [raw[46], raw[47], raw[48]],
        }
    }

    fn write(&self, raw: &mut [u8]) {
        raw[40] = self.height;
        raw[41] = self.land;
        raw[42] = self.ramp;
        raw[43..46].copy_from_slice(&self.radar_low);
        raw[46..49].copy_from_slice(&self.radar_high);
    }
}

/// One tile of a template.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Tile {
    /// Where its record starts in the file it was read from (0: a new tile).
    pub offset: usize,
    pub x: i32,
    pub y: i32,
    pub flags: u32,
    pub fields: TileFields,
    pub extra_x: i32,
    pub extra_y: i32,
    /// The extra block's size (0 x 0 when there is none).
    pub extra_w: usize,
    pub extra_h: usize,
    /// The diamond as a `cx` x `cy` rectangle, 0 outside it.
    pub image: Vec<u8>,
    /// The z-data diamond as a rectangle, [`Z_NONE`] outside it.
    pub z: Option<Vec<u8>>,
    /// The extra block, `extra_w * extra_h`.
    pub extra: Option<Vec<u8>>,
    /// The extra block's z-data, same size.
    pub extra_z: Option<Vec<u8>>,
    /// The record's header as read (all zero for a new tile).
    pub raw: [u8; TILE_HEADER],
}

impl Tile {
    /// A new tile at `(x, y)`: a blank diamond, no z, no extra.
    pub fn new(x: i32, y: i32, cx: usize, cy: usize) -> Tile {
        Tile {
            offset: 0,
            x,
            y,
            flags: 0,
            fields: TileFields::default(),
            extra_x: 0,
            extra_y: 0,
            extra_w: 0,
            extra_h: 0,
            image: vec![0; cx * cy],
            z: None,
            extra: None,
            extra_z: None,
            raw: [0; TILE_HEADER],
        }
    }

    /// The extra, z and extra-z offsets the record's header holds.
    fn data_offsets(&self) -> (i32, i32, i32) {
        let at = |k: usize| i32_at(&self.raw, k).unwrap_or(0);
        (at(8), at(12), at(16))
    }
}

/// A whole template.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Template {
    pub blocks_x: usize,
    pub blocks_y: usize,
    pub cx: usize,
    pub cy: usize,
    /// One entry per slot, row by row; `None` for an empty slot.
    pub tiles: Vec<Option<Tile>>,
    /// Slots whose offset is not zero but point at no whole tile (a damaged
    /// file); kept by an in-place save, refused by a rebuild.
    pub unreadable: Vec<usize>,
}

impl Template {
    /// The slots holding a tile.
    pub fn live(&self) -> impl Iterator<Item = (usize, &Tile)> {
        self.tiles
            .iter()
            .enumerate()
            .filter_map(|(slot, t)| t.as_ref().map(|t| (slot, t)))
    }
}

fn header(data: &[u8]) -> Option<(usize, usize, usize, usize)> {
    let field = |k: usize| i32_at(data, 4 * k);
    let (bx, by, cx, cy) = (field(0)?, field(1)?, field(2)?, field(3)?);
    let tile_size = matches!((cx, cy), (48, 24) | (60, 30));
    let blocks = (1..=MAX_BLOCKS as i32).contains(&bx) && (1..=MAX_BLOCKS as i32).contains(&by);
    (tile_size && blocks).then_some((bx as usize, by as usize, cx as usize, cy as usize))
}

/// Can a whole tile record start at `offset`?
fn record_fits(data: &[u8], offset: i32, cb: usize) -> bool {
    offset > 0 && (offset as usize).saturating_add(TILE_HEADER + cb) <= data.len()
}

/// Is `data` a TMP? The format has no magic, so this checks the header (a
/// known tile size, 1-256 tiles a side), the offset table, and that at least
/// one slot holds a whole tile and none points inside the table. An SHP
/// never passes: its first two bytes are zero, so `blocks_x` would be a
/// multiple of 65 536.
pub fn sniff(data: &[u8]) -> bool {
    let Some((bx, by, cx, cy)) = header(data) else {
        return false;
    };
    let table_end = FILE_HEADER + 4 * bx * by;
    if data.len() < table_end {
        return false;
    }
    let cb = diamond_bytes(cx, cy);
    let mut whole = 0;
    for slot in 0..bx * by {
        let offset = i32_at(data, FILE_HEADER + 4 * slot).unwrap_or(0);
        if offset == 0 {
            continue;
        }
        if offset > 0 && (offset as usize) < table_end {
            return false;
        }
        if record_fits(data, offset, cb) {
            whole += 1;
        }
    }
    whole > 0
}

fn block_at(data: &[u8], start: usize, len: usize) -> Option<&[u8]> {
    data.get(start..start.checked_add(len)?)
}

fn read_tile(data: &[u8], offset: usize, cx: usize, cy: usize) -> Tile {
    let cb = diamond_bytes(cx, cy);
    let mut raw = [0u8; TILE_HEADER];
    raw.copy_from_slice(&data[offset..offset + TILE_HEADER]);
    let at = |k: usize| i32_at(&raw, k).unwrap_or(0);
    let flags = u32_at(&raw, 36).unwrap_or(0);
    let (extra_off, z_off, extra_z_off) = (at(8), at(12), at(16));
    let (ew, eh) = (at(28), at(32));
    let image = decode_diamond(&data[offset + TILE_HEADER..], cx, cy, 0);
    // Every data offset is relative to the record, not the file.
    let relative = |off: i32, len: usize| {
        (off > 0)
            .then(|| block_at(data, offset + off as usize, len))
            .flatten()
    };
    let z = if flags & FLAG_Z != 0 {
        relative(z_off, cb).map(|b| decode_diamond(b, cx, cy, Z_NONE))
    } else {
        None
    };
    let sized =
        (1..=MAX_EXTRA_SIDE as i32).contains(&ew) && (1..=MAX_EXTRA_SIDE as i32).contains(&eh);
    let extra_len = if sized { (ew * eh) as usize } else { 0 };
    let extra = if flags & FLAG_EXTRA != 0 && sized {
        relative(extra_off, extra_len).map(<[u8]>::to_vec)
    } else {
        None
    };
    let extra_z = if extra.is_some() && flags & FLAG_Z != 0 {
        relative(extra_z_off, extra_len).map(<[u8]>::to_vec)
    } else {
        None
    };
    let (extra_w, extra_h) = if extra.is_some() {
        (ew as usize, eh as usize)
    } else {
        (0, 0)
    };
    Tile {
        offset,
        x: at(0),
        y: at(4),
        flags,
        fields: TileFields::read(&raw),
        extra_x: at(20),
        extra_y: at(24),
        extra_w,
        extra_h,
        image,
        z,
        extra,
        extra_z,
        raw,
    }
}

/// Decode a template. Lenient: a block whose offset or size does not fit the
/// file is treated as absent (and kept as it is by an in-place save).
pub fn parse(data: &[u8]) -> Result<Template, Error> {
    if data.len() < FILE_HEADER {
        return Err(Error::Truncated);
    }
    let (bx, by, cx, cy) = header(data).ok_or(Error::BadHeader)?;
    if data.len() < FILE_HEADER + 4 * bx * by {
        return Err(Error::Truncated);
    }
    let cb = diamond_bytes(cx, cy);
    let mut tiles = Vec::with_capacity(bx * by);
    let mut unreadable = Vec::new();
    for slot in 0..bx * by {
        let offset = i32_at(data, FILE_HEADER + 4 * slot).unwrap_or(0);
        if record_fits(data, offset, cb) {
            tiles.push(Some(read_tile(data, offset as usize, cx, cy)));
        } else {
            if offset != 0 {
                unreadable.push(slot);
            }
            tiles.push(None);
        }
    }
    if tiles.iter().all(Option::is_none) {
        return Err(Error::NoTiles);
    }
    Ok(Template {
        blocks_x: bx,
        blocks_y: by,
        cx,
        cy,
        tiles,
        unreadable,
    })
}

/// How [`build_with_layout`] wrote a template.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Layout {
    /// Over the original file, the same length.
    InPlace,
    /// Over the original file, with blocks or records added at its end.
    Appended,
    /// Laid out afresh (no original, a different grid, or an original this
    /// module could not patch faithfully).
    Fresh,
}

/// Encode `template`. With the file it was read from, it is patched in
/// place (see the module docs), so a tile that did not change keeps every
/// byte, and so does everything between and after the records; otherwise
/// the file is laid out afresh.
pub fn build(template: &Template, original: Option<&[u8]>) -> Result<Vec<u8>, Error> {
    build_with_layout(template, original).map(|(bytes, _)| bytes)
}

/// [`build`], saying which layout it used.
pub fn build_with_layout(
    template: &Template,
    original: Option<&[u8]>,
) -> Result<(Vec<u8>, Layout), Error> {
    if let Some(original) = original {
        if let Some(out) = patch(template, original) {
            let layout = if out.len() == original.len() {
                Layout::InPlace
            } else {
                Layout::Appended
            };
            return Ok((out, layout));
        }
        if let Some(&slot) = template.unreadable.first() {
            return Err(Error::Unreadable(slot));
        }
    }
    Ok((rebuild(template), Layout::Fresh))
}

/// The z-data a tile is written with: its own, or a diamond of "no data"
/// when only its extra block has z-data (the flag covers both).
fn written_z(tile: &Tile, cx: usize, cy: usize) -> Option<Vec<u8>> {
    match (&tile.z, &tile.extra, &tile.extra_z) {
        (Some(z), _, _) => Some(z.clone()),
        (None, Some(_), Some(_)) => Some(vec![Z_NONE; cx * cy]),
        _ => None,
    }
}

/// Does `got` (read back) hold what `want` says, as written?
fn reads_as(got: &Tile, want: &Tile, cx: usize, cy: usize) -> bool {
    let diamond = |d: &Option<Vec<u8>>| d.as_ref().map(|d| encode_diamond(d, cx, cy));
    fn extra_z(t: &Tile) -> Option<&Vec<u8>> {
        t.extra.as_ref().and(t.extra_z.as_ref())
    }
    (got.x, got.y, got.fields) == (want.x, want.y, want.fields)
        && encode_diamond(&got.image, cx, cy) == encode_diamond(&want.image, cx, cy)
        && diamond(&got.z) == diamond(&written_z(want, cx, cy))
        && got.extra == want.extra
        && extra_z(got) == extra_z(want)
        && (want.extra.is_none()
            || (got.extra_x, got.extra_y, got.extra_w, got.extra_h)
                == (want.extra_x, want.extra_y, want.extra_w, want.extra_h))
}

fn put_i32(out: &mut [u8], at: usize, v: i32) {
    out[at..at + 4].copy_from_slice(&v.to_le_bytes());
}

/// Append `bytes` to `out`; their offset relative to the record at `record`.
fn append(out: &mut Vec<u8>, record: usize, bytes: &[u8]) -> Option<i32> {
    let off = i32::try_from(out.len() - record).ok()?;
    out.extend_from_slice(bytes);
    Some(off)
}

/// Write `new` over the record `old` was read from (same offset), appending
/// the blocks that are new or changed size.
fn patch_record(out: &mut Vec<u8>, old: &Tile, new: &Tile, cx: usize, cy: usize) -> Option<()> {
    let at = old.offset;
    let (extra_off, z_off, extra_z_off) = old.data_offsets();
    let mut head: [u8; TILE_HEADER] = out[at..at + TILE_HEADER].try_into().ok()?;
    let mut flags = u32_at(&head, 36)?;
    put_i32(&mut head, 0, new.x);
    put_i32(&mut head, 4, new.y);
    new.fields.write(&mut head);
    let diamond = encode_diamond(&new.image, cx, cy);
    out[at + TILE_HEADER..at + TILE_HEADER + diamond.len()].copy_from_slice(&diamond);
    let z = written_z(new, cx, cy);
    match (&old.z, &z) {
        (Some(_), Some(z)) => {
            let z = encode_diamond(z, cx, cy);
            let start = at + z_off as usize;
            out[start..start + z.len()].copy_from_slice(&z);
        }
        (None, Some(z)) => {
            let off = append(out, at, &encode_diamond(z, cx, cy))?;
            put_i32(&mut head, 12, off);
            flags |= FLAG_Z;
        }
        (Some(_), None) => flags &= !FLAG_Z,
        (None, None) => {}
    }
    let kept = old.extra.is_some()
        && new.extra.is_some()
        && (old.extra_w, old.extra_h) == (new.extra_w, new.extra_h);
    match &new.extra {
        Some(extra) if kept => {
            let start = at + extra_off as usize;
            out[start..start + extra.len()].copy_from_slice(extra);
        }
        Some(extra) => {
            let off = append(out, at, extra)?;
            put_i32(&mut head, 8, off);
            put_i32(&mut head, 28, i32::try_from(new.extra_w).ok()?);
            put_i32(&mut head, 32, i32::try_from(new.extra_h).ok()?);
            flags |= FLAG_EXTRA;
        }
        None if old.extra.is_some() => flags &= !FLAG_EXTRA,
        None => {}
    }
    if new.extra.is_some() {
        put_i32(&mut head, 20, new.extra_x);
        put_i32(&mut head, 24, new.extra_y);
        match new.extra_z.as_ref() {
            Some(extra_z) if kept && old.extra_z.is_some() => {
                let start = at + extra_z_off as usize;
                out[start..start + extra_z.len()].copy_from_slice(extra_z);
            }
            Some(extra_z) => {
                let off = append(out, at, extra_z)?;
                put_i32(&mut head, 16, off);
            }
            // No extra z-data: the field must not point at any.
            None if z.is_some() && !(kept && old.flags & FLAG_Z != 0) => {
                put_i32(&mut head, 16, 0);
            }
            None => {}
        }
    }
    head[36..40].copy_from_slice(&flags.to_le_bytes());
    out[at..at + TILE_HEADER].copy_from_slice(&head);
    Some(())
}

/// The in-place save, or `None` when the grid changed, `template` is not
/// one `original` holds, or what was written would not read back as it.
fn patch(template: &Template, original: &[u8]) -> Option<Vec<u8>> {
    let was = parse(original).ok()?;
    let same_shape = (was.blocks_x, was.blocks_y, was.cx, was.cy)
        == (
            template.blocks_x,
            template.blocks_y,
            template.cx,
            template.cy,
        )
        && was.tiles.len() == template.tiles.len();
    if !same_shape {
        return None;
    }
    let (cx, cy) = (template.cx, template.cy);
    let records: std::collections::HashMap<usize, &Tile> =
        was.live().map(|(_, t)| (t.offset, t)).collect();
    let mut out = original.to_vec();
    for (slot, new) in template.tiles.iter().enumerate() {
        let entry = FILE_HEADER + 4 * slot;
        let Some(new) = new else {
            if was.tiles[slot].is_some() {
                put_i32(&mut out, entry, 0);
            }
            continue;
        };
        let at = if new.offset == 0 {
            let at = out.len();
            let mut bytes = record(new, cx, cy);
            // A new tile has no history: unused fields are zero.
            out.append(&mut bytes);
            at
        } else {
            patch_record(&mut out, records.get(&new.offset)?, new, cx, cy)?;
            new.offset
        };
        put_i32(&mut out, entry, i32::try_from(at).ok()?);
    }
    let back = parse(&out).ok()?;
    let faithful = back
        .tiles
        .iter()
        .zip(&template.tiles)
        .all(|(got, want)| match (got, want) {
            (None, None) => true,
            (Some(got), Some(want)) => reads_as(got, want, cx, cy),
            _ => false,
        });
    faithful.then_some(out)
}

/// One tile's record as Ochre's `build_tmp` lays it out: its header,
/// diamond, then whichever of z, extra and extra z it has, in that order
/// (data offsets relative to the record).
fn record(tile: &Tile, cx: usize, cy: usize) -> Vec<u8> {
    let cb = diamond_bytes(cx, cy);
    let z = written_z(tile, cx, cy);
    let has_z = z.is_some();
    let has_extra = tile.extra.is_some();
    let has_extra_z = has_extra && tile.extra_z.is_some();
    let (ew, eh) = if has_extra {
        (tile.extra_w, tile.extra_h)
    } else {
        (0, 0)
    };
    let mut cursor = TILE_HEADER + cb;
    let (mut z_off, mut extra_off, mut extra_z_off) = (0usize, 0usize, 0usize);
    if has_z {
        z_off = cursor;
        cursor += cb;
    }
    if has_extra {
        extra_off = cursor;
        cursor += ew * eh;
        if has_extra_z {
            extra_z_off = cursor;
        }
    }
    // Keep what the original tool left in the fields this tile does not
    // use; change only the managed bits of the flag word.
    let mut head = tile.raw;
    let managed = FLAG_EXTRA | FLAG_Z;
    let mut flags = (tile.flags & !managed) | if has_extra { FLAG_EXTRA } else { 0 };
    if has_z {
        flags |= FLAG_Z;
    }
    put_i32(&mut head, 0, tile.x);
    put_i32(&mut head, 4, tile.y);
    head[36..40].copy_from_slice(&flags.to_le_bytes());
    tile.fields.write(&mut head);
    // Only fields the flags point at are written: a stale value in a field
    // the game never reads is the original file's, not ours.
    let new = tile.offset == 0;
    if has_z || new {
        put_i32(&mut head, 12, z_off as i32);
    }
    if has_extra || new {
        put_i32(&mut head, 8, extra_off as i32);
        for (k, v) in [tile.extra_x, tile.extra_y, ew as i32, eh as i32]
            .into_iter()
            .enumerate()
        {
            put_i32(&mut head, 20 + 4 * k, v);
        }
        if (has_extra && has_z) || new {
            put_i32(&mut head, 16, extra_z_off as i32);
        }
    }
    let mut out = Vec::with_capacity(cursor + if has_extra_z { ew * eh } else { 0 });
    out.extend_from_slice(&head);
    out.extend_from_slice(&encode_diamond(&tile.image, cx, cy));
    if let Some(z) = &z {
        out.extend_from_slice(&encode_diamond(z, cx, cy));
    }
    if let Some(extra) = &tile.extra {
        out.extend_from_slice(extra);
        if has_extra_z {
            out.extend_from_slice(tile.extra_z.as_deref().unwrap_or_default());
        }
    }
    out
}

/// The layout Ochre's `build_tmp` writes: the header, the offset table,
/// then every tile's [`record`] in slot order.
fn rebuild(template: &Template) -> Vec<u8> {
    let (cx, cy) = (template.cx, template.cy);
    let count = template.blocks_x * template.blocks_y;
    let base = FILE_HEADER + 4 * count;
    let mut offsets = vec![0i32; count];
    let mut body = Vec::new();
    for (slot, tile) in template.tiles.iter().enumerate().take(count) {
        let Some(tile) = tile else {
            continue;
        };
        offsets[slot] = (base + body.len()) as i32;
        body.extend_from_slice(&record(tile, cx, cy));
    }
    let mut out = Vec::with_capacity(base + body.len());
    for v in [template.blocks_x, template.blocks_y, cx, cy] {
        out.extend_from_slice(&(v as i32).to_le_bytes());
    }
    for o in offsets {
        out.extend_from_slice(&o.to_le_bytes());
    }
    out.extend_from_slice(&body);
    out
}

/// What the game calls a land type. Several values share a label (0, 1 and
/// 13 are all "Clear"); the number is what is saved.
pub fn land_label(land: u8) -> &'static str {
    match land {
        0 | 1 | 13 => "Clear",
        2..=4 => "Ice",
        5 => "Tunnel",
        6 => "Railroad",
        7 | 8 => "Rock",
        9 => "Water",
        10 => "Beach",
        11 | 12 => "Road",
        14 => "Rough",
        15 => "Cliff",
        _ => "Unknown",
    }
}

/// The slope shape a ramp type names.
pub fn ramp_label(ramp: u8) -> &'static str {
    const NAMES: [&str; 21] = [
        "None",
        "West",
        "North",
        "East",
        "South",
        "North-west",
        "North-east",
        "South-east",
        "South-west",
        "Inner north-west",
        "Inner north-east",
        "Inner south-east",
        "Inner south-west",
        "Steep south-east",
        "Steep south-west",
        "Steep north-west",
        "Steep north-east",
        "Double up SW-NE",
        "Double down SW-NE",
        "Double up NW-SE",
        "Double down NW-SE",
    ];
    NAMES.get(usize::from(ramp)).copied().unwrap_or("Unknown")
}
