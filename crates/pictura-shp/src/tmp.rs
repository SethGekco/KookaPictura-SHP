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
//! layout still fits (every block keeps its size), so everything this module
//! does not model (the uninitialised bytes old tools left in unused header
//! fields, gaps, a tail) stays byte for byte. Otherwise it lays the records
//! out afresh like Ochre's `build_tmp`, keeping each tile's original 52
//! header bytes and changing only the fields it manages.

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

/// Encode `template`. With the file it was read from, every block that kept
/// its size is written over the original bytes in place (so a tile that did
/// not change keeps every byte, and so does everything between and after the
/// records); when a block appeared or disappeared, the file is laid out
/// afresh (see the module docs).
pub fn build(template: &Template, original: Option<&[u8]>) -> Result<Vec<u8>, Error> {
    if let Some(original) = original {
        if let Some(out) = patch(template, original) {
            return Ok(out);
        }
        if let Some(&slot) = template.unreadable.first() {
            return Err(Error::Unreadable(slot));
        }
    }
    Ok(rebuild(template))
}

/// The in-place save, or `None` when some block's presence or size changed
/// (or `template` is not the one `original` holds).
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
    let mut out = original.to_vec();
    for (old, new) in was.tiles.iter().zip(&template.tiles) {
        let (old, new) = match (old, new) {
            (None, None) => continue,
            (Some(old), Some(new)) => (old, new),
            _ => return None,
        };
        let fits = new.offset == old.offset
            && (new.x, new.y) == (old.x, old.y)
            && new.z.is_some() == old.z.is_some()
            && (new.extra_x, new.extra_y, new.extra_w, new.extra_h)
                == (old.extra_x, old.extra_y, old.extra_w, old.extra_h)
            && new.extra.is_some() == old.extra.is_some()
            && new.extra_z.is_some() == old.extra_z.is_some();
        if !fits {
            return None;
        }
        let at = old.offset;
        let (extra_off, z_off, extra_z_off) = old.data_offsets();
        new.fields.write(&mut out[at..at + TILE_HEADER]);
        let mut put = |start: usize, bytes: &[u8]| {
            out[start..start + bytes.len()].copy_from_slice(bytes);
        };
        put(at + TILE_HEADER, &encode_diamond(&new.image, cx, cy));
        if let Some(z) = &new.z {
            put(at + z_off as usize, &encode_diamond(z, cx, cy));
        }
        if let Some(extra) = &new.extra {
            put(at + extra_off as usize, extra);
        }
        if let Some(extra_z) = &new.extra_z {
            put(at + extra_z_off as usize, extra_z);
        }
    }
    Some(out)
}

/// The layout Ochre's `build_tmp` writes: per tile its header, diamond, then
/// whichever of z, extra and extra z it has, in that order.
fn rebuild(template: &Template) -> Vec<u8> {
    let (cx, cy) = (template.cx, template.cy);
    let cb = diamond_bytes(cx, cy);
    let count = template.blocks_x * template.blocks_y;
    let base = FILE_HEADER + 4 * count;
    let mut offsets = vec![0i32; count];
    let mut body = Vec::new();
    for (slot, tile) in template.tiles.iter().enumerate().take(count) {
        let Some(tile) = tile else {
            continue;
        };
        offsets[slot] = (base + body.len()) as i32;
        let has_z = tile.z.is_some();
        let has_extra = tile.extra.is_some();
        let has_extra_z = has_extra && has_z && tile.extra_z.is_some();
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
        head[0..4].copy_from_slice(&tile.x.to_le_bytes());
        head[4..8].copy_from_slice(&tile.y.to_le_bytes());
        head[36..40].copy_from_slice(&flags.to_le_bytes());
        tile.fields.write(&mut head);
        // Only fields the flags point at are written: a stale value in a field
        // the game never reads is the original file's, not ours.
        let new = tile.offset == 0;
        if has_z || new {
            head[12..16].copy_from_slice(&(z_off as i32).to_le_bytes());
        }
        if has_extra || new {
            head[8..12].copy_from_slice(&(extra_off as i32).to_le_bytes());
            for (k, v) in [tile.extra_x, tile.extra_y, ew as i32, eh as i32]
                .into_iter()
                .enumerate()
            {
                head[20 + 4 * k..24 + 4 * k].copy_from_slice(&v.to_le_bytes());
            }
            if (has_extra && has_z) || new {
                head[16..20].copy_from_slice(&(extra_z_off as i32).to_le_bytes());
            }
        }
        body.extend_from_slice(&head);
        body.extend_from_slice(&encode_diamond(&tile.image, cx, cy));
        if let Some(z) = &tile.z {
            body.extend_from_slice(&encode_diamond(z, cx, cy));
        }
        if let Some(extra) = &tile.extra {
            body.extend_from_slice(extra);
            if has_extra_z {
                body.extend_from_slice(tile.extra_z.as_deref().unwrap_or_default());
            }
        }
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
