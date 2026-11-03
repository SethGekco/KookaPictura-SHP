// SPDX-License-Identifier: GPL-3.0-or-later
//! What an archive entry holds, told from its bytes (a MIX stores no names,
//! and the names it is matched with may be wrong).

/// An entry's kind. MIX is found by reading the nested index (see
/// [`super::probe`]); the rest from the entry's first bytes.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum Kind {
    Mix,
    Shp,
    Tmp,
    Pal,
    Ini,
    /// XCC's "local mix database": the names of the archive's entries.
    Lmd,
    Vxl,
    Wav,
    /// A string table (`.csf`).
    Csf,
    Empty,
    /// Data of another kind.
    Other,
    /// The index puts it (partly) outside the archive.
    Broken,
}

impl Kind {
    pub fn label(self) -> &'static str {
        match self {
            Kind::Mix => "MIX",
            Kind::Shp => "SHP",
            Kind::Tmp => "TMP",
            Kind::Pal => "PAL",
            Kind::Ini => "INI",
            Kind::Lmd => "LMD",
            Kind::Vxl => "VXL",
            Kind::Wav => "WAV",
            Kind::Csf => "CSF",
            Kind::Empty => "empty",
            Kind::Other => "other",
            Kind::Broken => "broken",
        }
    }

    /// A file suffix for an entry with no name (an exported `1A2B3C4D.shp`).
    pub fn suffix(self) -> &'static str {
        match self {
            Kind::Mix => "mix",
            Kind::Shp => "shp",
            Kind::Tmp => "tmp",
            Kind::Pal => "pal",
            Kind::Ini => "ini",
            Kind::Lmd => "dat",
            Kind::Vxl => "vxl",
            Kind::Wav => "wav",
            Kind::Csf => "csf",
            Kind::Empty | Kind::Other | Kind::Broken => "bin",
        }
    }
}

/// XCC's header id, which starts a local mix database.
pub(crate) const XCC_ID: &[u8] = b"XCC by Olaf van der Spek";

/// The kind of an entry of `len` bytes from its first bytes `head` (the
/// whole entry when it is small; an SHP's whole frame table). Never MIX.
pub fn sniff_kind(head: &[u8], len: u64) -> Kind {
    if len == 0 {
        return Kind::Empty;
    }
    let whole = head.len() as u64 == len;
    if head.starts_with(XCC_ID) {
        Kind::Lmd
    } else if head.starts_with(b"Voxel Animation\0") {
        Kind::Vxl
    } else if head.len() >= 12 && head.starts_with(b"RIFF") && &head[8..12] == b"WAVE" {
        Kind::Wav
    } else if head.starts_with(b" FSC") {
        Kind::Csf
    } else if crate::sniff(head, usize::try_from(len).ok()) {
        Kind::Shp
    } else if whole && crate::tmp::sniff(head) {
        Kind::Tmp
    } else if (len == 768 && whole && head.iter().all(|&v| v < 64)) || head.starts_with(b"JASC-PAL")
    {
        Kind::Pal
    } else if looks_like_ini(head) {
        Kind::Ini
    } else {
        Kind::Other
    }
}

/// Text (no control bytes but tab, CR, LF and the DOS end-of-file mark in
/// its first 4 KiB) with a `[section]` line (anywhere in `head`: some files
/// open with a long comment).
fn looks_like_ini(head: &[u8]) -> bool {
    if head[..head.len().min(4096)]
        .iter()
        .any(|&b| b < 0x20 && !matches!(b, b'\t' | b'\r' | b'\n' | 0x1A))
    {
        return false;
    }
    head.split(|&b| b == b'\n').any(|line| {
        let line = line.trim_ascii();
        line.starts_with(b"[") && line.contains(&b']')
    })
}
