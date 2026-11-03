// SPDX-License-Identifier: GPL-3.0-or-later
//! Names for MIX entries. An archive stores only IDs, so names are matched
//! by hashing candidates:
//!
//! - the list shipped with this crate (`data/mix-names.txt`): names read
//!   from the stock games' own INI files inside their archives, each one
//!   kept only when its ID is in a stock archive (see `tests/mix_names.rs`);
//! - a local list the user's own files add (kept outside the repository);
//! - an archive's "local mix database" (XCC's names file, stored in the
//!   archive itself);
//! - names read from INI files inside an archive ([`harvest_ini`]).

use super::ids::{td_id, ts_id, IdKind};
use super::kind::XCC_ID;
use super::Index;
use std::collections::{BTreeSet, HashMap};

/// The shipped list.
pub const SHIPPED: &str = include_str!("../../data/mix-names.txt");

/// Names by ID, for both ID kinds.
#[derive(Clone, Debug, Default)]
pub struct Names {
    crc: HashMap<u32, String>,
    classic: HashMap<u32, String>,
}

impl Names {
    pub fn new() -> Names {
        Names::default()
    }

    /// The shipped list.
    pub fn shipped() -> Names {
        let mut names = Names::new();
        names.add_list(SHIPPED);
        names
    }

    /// Add a name (the first name added for an ID wins).
    pub fn add(&mut self, name: &str) {
        let name = name.trim();
        if name.is_empty() || name.contains(['/', '\\']) {
            return;
        }
        self.crc
            .entry(ts_id(name))
            .or_insert_with(|| name.to_owned());
        self.classic
            .entry(td_id(name))
            .or_insert_with(|| name.to_owned());
    }

    /// Add the names of a list: one per line; `#` starts a comment line.
    pub fn add_list(&mut self, text: &str) {
        for line in text.lines() {
            if !line.trim_start().starts_with('#') {
                self.add(line);
            }
        }
    }

    pub fn len(&self) -> usize {
        self.crc.len()
    }

    pub fn is_empty(&self) -> bool {
        self.crc.is_empty()
    }

    pub fn get(&self, kind: IdKind, id: u32) -> Option<&str> {
        match kind {
            IdKind::Crc => self.crc.get(&id),
            IdKind::Classic => self.classic.get(&id),
        }
        .map(String::as_str)
    }

    /// The ID kind that names more of `index`'s entries; on a tie the
    /// layout decides (the old one is Tiberian Dawn's).
    pub fn kind_for(&self, index: &Index) -> IdKind {
        let count = |kind| {
            index
                .entries
                .iter()
                .filter(|e| self.get(kind, e.id).is_some())
                .count()
        };
        let (crc, classic) = (count(IdKind::Crc), count(IdKind::Classic));
        if crc > classic || (crc == classic && index.format != super::Format::Old) {
            IdKind::Crc
        } else {
            IdKind::Classic
        }
    }
}

/// The names in an XCC local mix database: the 32-byte XCC id, `size`,
/// `type`, `version`, `game` and `count` (`u32` each), then `count`
/// NUL-terminated names. `None` when the bytes are not one.
pub fn parse_lmd(bytes: &[u8]) -> Option<Vec<String>> {
    if !bytes.starts_with(XCC_ID) || bytes.len() < 52 {
        return None;
    }
    let count = u32::from_le_bytes(bytes[48..52].try_into().ok()?) as usize;
    let mut names = Vec::with_capacity(count.min(65_536));
    let mut rest = &bytes[52..];
    for _ in 0..count {
        let end = rest.iter().position(|&b| b == 0)?;
        names.push(String::from_utf8_lossy(&rest[..end]).into_owned());
        rest = &rest[end + 1..];
    }
    Some(names)
}

/// The kind of every name in `set` as a sorted list, one per line.
pub fn to_list(set: &BTreeSet<String>) -> String {
    let mut out = String::new();
    for name in set {
        out.push_str(name);
        out.push('\n');
    }
    out
}

/// The theaters: their INI name, file suffix and the letter `NewTheater`
/// art puts second in a name.
pub const THEATERS: [(&str, &str, char); 6] = [
    ("temperat", "tem", 'T'),
    ("snow", "sno", 'A'),
    ("urban", "urb", 'U'),
    ("urbann", "ubn", 'N'),
    ("desert", "des", 'D'),
    ("lunar", "lun", 'L'),
];

/// File suffixes a name in an INI value may carry.
const SUFFIXES: [&str; 22] = [
    "shp", "vxl", "hva", "pal", "tem", "sno", "urb", "ubn", "des", "lun", "mix", "wav", "ini",
    "pcx", "map", "mmx", "yrm", "mpr", "csf", "fnt", "bik", "aud",
];

fn is_token(value: &str) -> bool {
    !value.is_empty()
        && value.len() <= 40
        && value
            .bytes()
            .all(|b| b.is_ascii_alphanumeric() || matches!(b, b'_' | b'-' | b'.'))
}

/// Candidate file names from one INI file (`ini_name`, which says whether it
/// is a theater's: `temperatmd.ini` names `.tem` tiles). Every candidate is
/// a guess that only counts once its ID is found in an archive:
///
/// - theater INIs: each `[TileSetNNNN]`'s `FileName` with the tile numbers
///   `01..=TilesInSet`, plain and with the variation letters `a..=z`;
/// - every section name, and every comma-separated token of a value, as
///   `.shp`, `.vxl` and `.hva`, plus the theater forms art uses (each
///   theater's suffix; the second letter replaced by each theater's letter);
///   a token that already has a known suffix as it is;
/// - `Palette=` values as `<value><theater suffix>.pal` and `<value>.pal`.
pub fn harvest_ini(ini_name: &str, text: &str, out: &mut BTreeSet<String>) {
    let lower = ini_name.to_ascii_lowercase();
    let stem = lower.strip_suffix(".ini").unwrap_or(&lower);
    let theater = THEATERS
        .iter()
        .find(|(name, _, _)| stem == *name || stem.strip_suffix("md") == Some(*name))
        .map(|t| t.1);
    let mut file_name: Option<String> = None;
    let mut tiles: Option<usize> = None;
    let flush_tiles =
        |file: &mut Option<String>, count: &mut Option<usize>, out: &mut BTreeSet<String>| {
            if let (Some(suffix), Some(name), Some(n)) = (theater, file.take(), count.take()) {
                for i in 1..=n.min(99) {
                    out.insert(format!("{name}{i:02}.{suffix}"));
                    for v in b'a'..=b'z' {
                        out.insert(format!("{name}{i:02}{}.{suffix}", v as char));
                    }
                }
            }
        };
    for raw in text.lines() {
        let line = raw.split(';').next().unwrap_or("").trim();
        if let Some(section) = line.strip_prefix('[') {
            flush_tiles(&mut file_name, &mut tiles, out);
            if let Some(name) = section.split(']').next() {
                add_image(&name.trim().to_ascii_lowercase(), out);
            }
            continue;
        }
        let Some((key, value)) = line.split_once('=') else {
            continue;
        };
        let key = key.trim().to_ascii_lowercase();
        let value = value.trim().to_ascii_lowercase();
        match key.as_str() {
            "filename" => file_name = is_token(&value).then_some(value.clone()),
            "tilesinset" => tiles = value.parse().ok(),
            "palette" | "custompalette" if is_token(&value) => {
                let base = value.strip_suffix(".pal").unwrap_or(&value);
                out.insert(format!("{base}.pal"));
                for (_, suffix, _) in THEATERS {
                    out.insert(format!("{base}{suffix}.pal"));
                }
            }
            _ => {}
        }
        for token in value.split(',').map(str::trim).filter(|t| is_token(t)) {
            add_image(token, out);
        }
    }
    flush_tiles(&mut file_name, &mut tiles, out);
}

/// An image name's file names (see [`harvest_ini`]).
fn add_image(name: &str, out: &mut BTreeSet<String>) {
    if !is_token(name) {
        return;
    }
    if let Some((_, suffix)) = name.rsplit_once('.') {
        if SUFFIXES.contains(&suffix) {
            out.insert(name.to_owned());
        }
        return;
    }
    if name.bytes().all(|b| b.is_ascii_digit()) {
        return;
    }
    for suffix in ["shp", "vxl", "hva"] {
        out.insert(format!("{name}.{suffix}"));
    }
    for extra in ["tur", "barl"] {
        out.insert(format!("{name}{extra}.vxl"));
        out.insert(format!("{name}{extra}.hva"));
    }
    for (_, suffix, letter) in THEATERS {
        out.insert(format!("{name}.{suffix}"));
        if name.len() >= 2 && name.is_char_boundary(1) && name.is_char_boundary(2) {
            let swapped = format!(
                "{}{}{}",
                &name[..1],
                letter.to_ascii_lowercase(),
                &name[2..]
            );
            out.insert(format!("{swapped}.shp"));
            out.insert(format!("{swapped}.{suffix}"));
        }
    }
    let swapped_g = format!("{}g{}", &name[..1], name.get(2..).unwrap_or(""));
    if name.len() >= 2 {
        out.insert(format!("{swapped_g}.shp"));
    }
}

/// Whether a name's suffix fits the kind its entry was sniffed as (a guard
/// against the rare candidate whose ID matches another file's).
pub fn suffix_fits(name: &str, kind: super::Kind) -> bool {
    use super::Kind;
    let suffix = name
        .rsplit_once('.')
        .map(|(_, s)| s.to_ascii_lowercase())
        .unwrap_or_default();
    match suffix.as_str() {
        "shp" => kind == Kind::Shp,
        "tem" | "sno" | "urb" | "ubn" | "des" | "lun" => matches!(kind, Kind::Shp | Kind::Tmp),
        "tmp" => kind == Kind::Tmp,
        "vxl" => kind == Kind::Vxl,
        "pal" => kind == Kind::Pal,
        "mix" => kind == Kind::Mix,
        "ini" | "map" | "mmx" | "yrm" | "mpr" => matches!(kind, Kind::Ini | Kind::Other),
        "wav" => kind == Kind::Wav,
        "csf" => kind == Kind::Csf,
        "dat" => matches!(kind, Kind::Lmd | Kind::Other),
        _ => kind != Kind::Broken,
    }
}
