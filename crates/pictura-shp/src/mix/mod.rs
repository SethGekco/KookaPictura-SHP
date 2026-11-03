// SPDX-License-Identifier: GPL-3.0-or-later
//! Westwood MIX archives: read (every game's layout, nested archives,
//! encrypted and checksummed indexes) and write (a new, unencrypted archive
//! that copies untouched entries byte for byte).
//!
//! The layout, as documented by the ModdingWiki "MIX Format (TD)" and "MIX
//! Format (TS)" pages, XCC Utilities and OpenRA (`MixFile.cs`):
//!
//! - **Tiberian Dawn / early Red Alert** (no flags): `u16 count`,
//!   `u32 body_size`, then `count` entries of `u32 id, u32 offset, u32 size`;
//!   the body follows, offsets are relative to its start.
//! - **Red Alert, Tiberian Sun, Red Alert 2** (flags): the first `u16` is 0
//!   and the first `u32` holds the flags (`0x10000` a SHA-1 checksum of the
//!   body follows it, `0x20000` the index is encrypted). Unencrypted: the same
//!   count, size and entries follow the flags. Encrypted: an 80-byte key
//!   source follows (see [`PublicKey`]), then the count, size and entries
//!   encrypted with Blowfish, padded to whole 8-byte blocks; the body starts
//!   after the last block.
//! - IDs are hashes of the upper-case file name ([`ids`]); the archive holds
//!   no names. [`names`] keeps the names this crate can match.
//!
//! Everything here reads through a `Read + Seek` source at an absolute
//! offset, so a nested archive is just a range of its parent's file: no
//! archive is read into memory whole.

mod blowfish;
mod blowfish_tables;
pub mod ids;
mod key;
mod kind;
pub mod names;
mod sha1;
mod write;

pub use ids::IdKind;
pub use key::{key_source_for, PublicKey, BLOWFISH_KEY};
pub use kind::{sniff_kind, Kind};
pub use sha1::{sha1, Sha1};
pub use write::{Data, Plan};

use blowfish::Blowfish;
use std::fmt;
use std::fs::File;
use std::io::{self, Read, Seek, SeekFrom, Write};
use std::path::{Path, PathBuf};
use std::sync::OnceLock;

/// A SHA-1 checksum of the body follows it.
pub const FLAG_CHECKSUM: u32 = 0x0001_0000;
/// The index is encrypted.
pub const FLAG_ENCRYPTED: u32 = 0x0002_0000;
/// One index entry: id, offset, size.
pub const ENTRY_BYTES: u64 = 12;
/// The checksum's length.
pub const CHECKSUM_BYTES: u64 = 20;
/// The key source's length (Westwood's key).
pub const KEY_SOURCE_BYTES: u64 = 80;
/// Nested archives deeper than this are not opened.
pub const MAX_DEPTH: usize = 8;

/// Why an archive could not be read.
#[derive(Debug)]
pub struct MixError(pub String);

impl fmt::Display for MixError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.0)
    }
}

impl std::error::Error for MixError {}

impl From<io::Error> for MixError {
    fn from(e: io::Error) -> Self {
        MixError(e.to_string())
    }
}

fn bad(text: &str) -> MixError {
    MixError(text.to_owned())
}

/// The header layout of an archive.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Format {
    /// Tiberian Dawn / early Red Alert: no flags word.
    Old,
    /// A flags word first (Red Alert, Tiberian Sun, Red Alert 2).
    New { flags: u32 },
}

/// One index entry. `offset` is relative to the body.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Entry {
    pub id: u32,
    pub offset: u32,
    pub size: u32,
}

/// An archive's index. Positions are relative to the archive's start.
#[derive(Clone, Debug)]
pub struct Index {
    pub format: Format,
    /// In index order.
    pub entries: Vec<Entry>,
    /// The body size the header declares.
    pub body_size: u32,
    pub body_start: u64,
    /// The archive's length.
    pub len: u64,
}

impl Index {
    pub fn flags(&self) -> u32 {
        match self.format {
            Format::Old => 0,
            Format::New { flags } => flags,
        }
    }

    pub fn encrypted(&self) -> bool {
        self.flags() & FLAG_ENCRYPTED != 0
    }

    pub fn has_checksum(&self) -> bool {
        self.flags() & FLAG_CHECKSUM != 0
    }

    /// Where `e`'s data starts, from the archive's start.
    pub fn start(&self, e: &Entry) -> u64 {
        self.body_start + u64::from(e.offset)
    }

    pub fn in_bounds(&self, e: &Entry) -> bool {
        self.start(e) + u64::from(e.size) <= self.len
    }

    /// The position in `entries` of the first entry with `id`.
    pub fn find(&self, id: u32) -> Option<usize> {
        self.entries.iter().position(|e| e.id == id)
    }

    /// Bytes after the body (and its checksum) that the header does not
    /// account for; negative when the declared body runs past the end.
    pub fn trailing(&self) -> i64 {
        let checksum = if self.has_checksum() {
            CHECKSUM_BYTES
        } else {
            0
        };
        self.len as i64 - (self.body_start + u64::from(self.body_size) + checksum) as i64
    }

    /// True when the index describes an archive that fits its length: every
    /// entry and the declared body inside it.
    pub fn consistent(&self) -> bool {
        self.trailing() >= 0 && self.entries.iter().all(|e| self.in_bounds(e))
    }
}

fn westwood_key() -> &'static PublicKey {
    static KEY: OnceLock<PublicKey> = OnceLock::new();
    KEY.get_or_init(PublicKey::westwood)
}

fn read_at<R: Read + Seek>(r: &mut R, at: u64, len: usize) -> io::Result<Vec<u8>> {
    r.seek(SeekFrom::Start(at))?;
    let mut buf = vec![0u8; len];
    r.read_exact(&mut buf)?;
    Ok(buf)
}

fn u16_at(b: &[u8], at: usize) -> u16 {
    u16::from_le_bytes([b[at], b[at + 1]])
}

fn u32_at(b: &[u8], at: usize) -> u32 {
    u32::from_le_bytes([b[at], b[at + 1], b[at + 2], b[at + 3]])
}

/// Read the index of the archive at `base` (`len` bytes) in `r`.
pub fn read_index<R: Read + Seek>(r: &mut R, base: u64, len: u64) -> Result<Index, MixError> {
    read_index_with(r, base, len, westwood_key())
}

/// [`read_index`] with another RSA key (tests use a key pair of their own).
pub fn read_index_with<R: Read + Seek>(
    r: &mut R,
    base: u64,
    len: u64,
    key: &PublicKey,
) -> Result<Index, MixError> {
    if len < 6 {
        return Err(bad("too short for a MIX header"));
    }
    let head = read_at(r, base, len.min(10) as usize)?;
    let (format, body_size, body_start, entries_bytes) = if u16_at(&head, 0) != 0 {
        let count = u64::from(u16_at(&head, 0));
        let body_start = 6 + ENTRY_BYTES * count;
        if body_start > len {
            return Err(bad("the index runs past the end"));
        }
        let bytes = read_at(r, base + 6, (ENTRY_BYTES * count) as usize)?;
        (Format::Old, u32_at(&head, 2), body_start, bytes)
    } else {
        if head.len() < 10 {
            return Err(bad("too short for a MIX header"));
        }
        let flags = u32_at(&head, 0);
        if flags & !(FLAG_CHECKSUM | FLAG_ENCRYPTED) != 0 {
            return Err(bad("unknown header flags"));
        }
        if flags & FLAG_ENCRYPTED != 0 {
            let source_len = key.key_source_len() as u64;
            let at = 4 + source_len;
            if at + 8 > len {
                return Err(bad("too short for an encrypted MIX header"));
            }
            let source = read_at(r, base + 4, source_len as usize)?;
            let fish = Blowfish::new(&key.blowfish_key(&source));
            let mut first = read_at(r, base + at, 8)?;
            fish.decrypt(&mut first);
            let count = u64::from(u16_at(&first, 0));
            let blocks = (6 + ENTRY_BYTES * count).div_ceil(8);
            let body_start = at + 8 * blocks;
            if body_start > len {
                return Err(bad("the index runs past the end"));
            }
            let mut all = read_at(r, base + at, (8 * blocks) as usize)?;
            fish.decrypt(&mut all);
            let entries = all[6..6 + (ENTRY_BYTES * count) as usize].to_vec();
            (
                Format::New { flags },
                u32_at(&first, 2),
                body_start,
                entries,
            )
        } else {
            let count = u64::from(u16_at(&head, 4));
            let body_start = 10 + ENTRY_BYTES * count;
            if body_start > len {
                return Err(bad("the index runs past the end"));
            }
            let bytes = read_at(r, base + 10, (ENTRY_BYTES * count) as usize)?;
            (Format::New { flags }, u32_at(&head, 6), body_start, bytes)
        }
    };
    let entries = entries_bytes
        .as_chunks::<{ ENTRY_BYTES as usize }>()
        .0
        .iter()
        .map(|e| Entry {
            id: u32_at(e, 0),
            offset: u32_at(e, 4),
            size: u32_at(e, 8),
        })
        .collect();
    Ok(Index {
        format,
        entries,
        body_size,
        body_start,
        len,
    })
}

/// The index of the archive at `base` when it is a consistent MIX (every
/// entry and the declared body inside `len`), else `None`. Cheap checks on
/// the first bytes come first, so most data that is not a MIX costs no
/// further read.
pub fn probe<R: Read + Seek>(r: &mut R, base: u64, len: u64) -> Option<Index> {
    if len < 6 {
        return None;
    }
    let head = read_at(r, base, len.min(10) as usize).ok()?;
    if u16_at(&head, 0) == 0 {
        if head.len() < 10 || u32_at(&head, 0) & !(FLAG_CHECKSUM | FLAG_ENCRYPTED) != 0 {
            return None;
        }
    } else {
        // An old-format header must account for the whole archive.
        let end = 6 + ENTRY_BYTES * u64::from(u16_at(&head, 0)) + u64::from(u32_at(&head, 2));
        if end != len {
            return None;
        }
    }
    let index = read_index(r, base, len).ok()?;
    index.consistent().then_some(index)
}

/// Encrypt (or, with `decrypt`, decrypt) whole 8-byte blocks of `data` in
/// place with Blowfish under `key`, as a MIX index is. For tools and tests
/// that make an encrypted archive with a key pair of their own.
pub fn blowfish(key: &[u8], data: &mut [u8], decrypt: bool) {
    let fish = Blowfish::new(key);
    if decrypt {
        fish.decrypt(data);
    } else {
        fish.encrypt(data);
    }
}

/// The SHA-1 checksum stored after the body, and the one the body has now.
pub fn checksums<R: Read + Seek>(
    r: &mut R,
    base: u64,
    index: &Index,
) -> io::Result<Option<([u8; 20], [u8; 20])>> {
    if !index.has_checksum() || index.trailing() < 0 {
        return Ok(None);
    }
    r.seek(SeekFrom::Start(base + index.body_start))?;
    let mut hash = Sha1::new();
    let mut left = u64::from(index.body_size);
    let mut buf = vec![0u8; 1 << 20];
    while left > 0 {
        let n = left.min(buf.len() as u64) as usize;
        r.read_exact(&mut buf[..n])?;
        hash.update(&buf[..n]);
        left -= n as u64;
    }
    let mut stored = [0u8; 20];
    r.read_exact(&mut stored)?;
    Ok(Some((stored, hash.finish())))
}

/// Copy `len` bytes at `at` from `r` to `out`, a megabyte at a time.
pub fn copy_range<R: Read + Seek, W: Write + ?Sized>(
    r: &mut R,
    at: u64,
    len: u64,
    out: &mut W,
) -> io::Result<()> {
    r.seek(SeekFrom::Start(at))?;
    let mut buf = vec![0u8; (len.min(1 << 20)) as usize];
    let mut left = len;
    while left > 0 {
        let n = left.min(buf.len() as u64) as usize;
        r.read_exact(&mut buf[..n])?;
        out.write_all(&buf[..n])?;
        left -= n as u64;
    }
    Ok(())
}

/// A scanned archive: its index, the kind of every entry and the nested
/// archives, opened in turn.
#[derive(Clone, Debug)]
pub struct Node {
    /// Where the archive starts in the outermost file.
    pub start: u64,
    pub index: Index,
    /// Per entry (index order).
    pub kinds: Vec<Kind>,
    /// Nested archives: (entry position, its scan).
    pub children: Vec<(usize, Node)>,
}

impl Node {
    pub fn child(&self, entry: usize) -> Option<&Node> {
        self.children
            .iter()
            .find(|(i, _)| *i == entry)
            .map(|(_, n)| n)
    }

    /// Every node, depth first, this one first.
    pub fn walk(&self) -> Vec<&Node> {
        let mut out = vec![self];
        for (_, child) in &self.children {
            out.extend(child.walk());
        }
        out
    }
}

/// Reads entry heads in file order through one buffer, so a scan of an
/// archive of many small entries is close to one sequential read.
struct Heads<'a> {
    r: io::BufReader<&'a File>,
    pos: u64,
}

impl<'a> Heads<'a> {
    fn new(file: &'a File) -> Heads<'a> {
        Heads {
            r: io::BufReader::with_capacity(256 << 10, file),
            pos: u64::MAX,
        }
    }

    fn read_head(&mut self, at: u64, len: usize) -> io::Result<Vec<u8>> {
        self.seek(SeekFrom::Start(at))?;
        let mut buf = vec![0u8; len];
        self.read_exact(&mut buf)?;
        Ok(buf)
    }
}

impl Read for Heads<'_> {
    fn read(&mut self, buf: &mut [u8]) -> io::Result<usize> {
        let n = self.r.read(buf)?;
        self.pos = self.pos.wrapping_add(n as u64);
        Ok(n)
    }
}

impl Seek for Heads<'_> {
    fn seek(&mut self, to: SeekFrom) -> io::Result<u64> {
        // A short hop stays inside (or refills) the buffer instead of
        // dropping it: probing every entry seeks back and forth a little.
        if let SeekFrom::Start(p) = to {
            if self.pos != u64::MAX && p.abs_diff(self.pos) < (1 << 20) {
                self.r.seek_relative(p as i64 - self.pos as i64)?;
                self.pos = p;
                return Ok(p);
            }
        }
        let p = self.r.seek(to)?;
        self.pos = p;
        Ok(p)
    }
}

/// Entries up to this size are read whole to tell their kind (a TMP needs
/// its records); larger ones by their first 64 KiB.
const WHOLE_HEAD: u64 = 256 << 10;
const HEAD: u64 = 64 << 10;

/// The first bytes of an entry, enough for [`sniff_kind`]: the whole entry
/// when it is small, else 64 KiB, or an SHP's whole frame table.
fn entry_head(heads: &mut Heads<'_>, at: u64, len: u64) -> io::Result<Vec<u8>> {
    let first = if len <= WHOLE_HEAD { len } else { HEAD };
    let mut head = heads.read_head(at, first as usize)?;
    if head.len() >= crate::HEADER && head[0] == 0 && head[1] == 0 {
        let table = crate::table_bytes(&head) as u64;
        if table > head.len() as u64 && table <= len {
            head = heads.read_head(at, table as usize)?;
        }
    }
    Ok(head)
}

/// Scan the archive at `start` (`len` bytes) of `file`: its index, every
/// entry's kind, and nested archives (to [`MAX_DEPTH`]).
pub fn scan(file: &File, start: u64, len: u64) -> Result<Node, MixError> {
    let mut heads = Heads::new(file);
    let index = read_index(&mut heads, start, len)?;
    Ok(scan_node(&mut heads, start, index, 0))
}

fn scan_node(heads: &mut Heads<'_>, start: u64, index: Index, depth: usize) -> Node {
    let mut kinds = vec![Kind::Other; index.entries.len()];
    let mut children = Vec::new();
    let mut order: Vec<usize> = (0..index.entries.len()).collect();
    order.sort_by_key(|&i| index.entries[i].offset);
    for i in order {
        let e = index.entries[i];
        if !index.in_bounds(&e) {
            kinds[i] = Kind::Broken;
            continue;
        }
        let at = start + index.start(&e);
        let len = u64::from(e.size);
        // A valid SHP is an SHP even when its first bytes would also read as
        // an archive header (a sprite 1 to 3 pixels wide starts like the
        // flags word); anything else that probes as an archive is one.
        let sniffed = entry_head(heads, at, len).map_or(Kind::Broken, |h| sniff_kind(&h, len));
        kinds[i] = sniffed;
        if sniffed != Kind::Shp && sniffed != Kind::Broken && depth < MAX_DEPTH {
            if let Some(nested) = probe(heads, at, len) {
                kinds[i] = Kind::Mix;
                children.push((i, scan_node(heads, at, nested, depth + 1)));
            }
        }
    }
    children.sort_by_key(|(i, _)| *i);
    Node {
        start,
        index,
        kinds,
        children,
    }
}

/// Where the entry a chain of IDs names starts and how long it is, in the
/// outermost archive (`len` bytes at 0 of `r`). Every ID but the last names
/// a nested archive.
pub fn locate<R: Read + Seek>(r: &mut R, len: u64, chain: &[u32]) -> Result<(u64, u64), MixError> {
    let (mut start, mut size) = (0u64, len);
    for &id in chain {
        let index = read_index(r, start, size)?;
        let i = index
            .find(id)
            .ok_or_else(|| MixError(format!("no entry {id:08X} in the archive")))?;
        let e = index.entries[i];
        if !index.in_bounds(&e) {
            return Err(MixError(format!("entry {id:08X} runs past the end")));
        }
        start += index.start(&e);
        size = u64::from(e.size);
    }
    Ok((start, size))
}

/// The ID a path component names: `#1A2B3C4D` for an entry with no known
/// name, else the name's hash of `kind`.
pub fn component_id(component: &str, kind: IdKind) -> u32 {
    component
        .strip_prefix('#')
        .and_then(|hex| u32::from_str_radix(hex, 16).ok())
        .unwrap_or_else(|| kind.id(component))
}

/// The path component for an entry: its name, or `#` and its ID in hex.
pub fn component(name: Option<&str>, id: u32) -> String {
    name.map_or_else(|| format!("#{id:08X}"), str::to_owned)
}

/// Split a path into a MIX file and the entries inside it: for
/// `games/ra2.mix/cache.mix/unittem.pal`, the file `games/ra2.mix` and
/// `["cache.mix", "unittem.pal"]`. `None` when the path names a file itself
/// or no prefix of it is a file.
pub fn split_member_path(path: &str) -> Option<(PathBuf, Vec<String>)> {
    let path = Path::new(path);
    if path.is_file() {
        return None;
    }
    let mut members = Vec::new();
    let mut at = path;
    while let Some(parent) = at.parent() {
        members.push(at.file_name()?.to_string_lossy().into_owned());
        if parent.as_os_str().is_empty() {
            return None;
        }
        if parent.is_file() {
            members.reverse();
            return Some((parent.to_path_buf(), members));
        }
        at = parent;
    }
    None
}

/// The range of a member path's entry: (file, start, length). Each
/// component is matched by its Red Alert 2 ID, then by its Tiberian Dawn
/// one.
pub fn locate_member(path: &str) -> Result<(File, u64, u64), MixError> {
    let (file_path, members) =
        split_member_path(path).ok_or_else(|| MixError(format!("{path} is not inside a MIX")))?;
    let mut file = File::open(&file_path)?;
    let (mut start, mut size) = (0u64, file.metadata()?.len());
    for member in &members {
        let index = read_index(&mut file, start, size)?;
        let found = [IdKind::Crc, IdKind::Classic]
            .iter()
            .find_map(|&k| index.find(component_id(member, k)))
            .ok_or_else(|| MixError(format!("{member} is not in the archive")))?;
        let e = index.entries[found];
        if !index.in_bounds(&e) {
            return Err(MixError(format!(
                "{member} runs past the end of the archive"
            )));
        }
        start += index.start(&e);
        size = u64::from(e.size);
    }
    Ok((file, start, size))
}

/// The bytes at `path`: a plain file, or an entry inside a MIX
/// (`ra2.mix/cache.mix/unittem.pal`). Read whole, so for small entries
/// (palettes); documents are extracted with [`copy_range`].
pub fn read_path(path: &str) -> io::Result<Vec<u8>> {
    match std::fs::read(path) {
        Ok(bytes) => Ok(bytes),
        Err(plain) => match locate_member(path) {
            Ok((mut file, start, len)) => read_at(&mut file, start, len as usize),
            Err(_) => Err(plain),
        },
    }
}

/// True when `path` is a file or an entry inside a MIX.
pub fn path_exists(path: &str) -> bool {
    Path::new(path).is_file() || locate_member(path).is_ok()
}
