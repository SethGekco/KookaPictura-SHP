// Derived from qshp's `src/shpcodec.cpp` (`readPalette`, `fallbackPalette`,
// `uniqueColours`, `nearest`) and Ochre's `addons/cnc/cncpal.py` (`read_pal`,
// `read_jasc`, `write_pal`). Both Copyright (C) 2026 Ochre contributors,
// GPL-3.0-or-later, by the owner of this fork.
// SPDX-License-Identifier: GPL-3.0-or-later

//! Westwood `.pal` palettes.
//!
//! A binary `.pal` is 768 bytes: 256 entries of three **6-bit** VGA values.
//! They expand as `(v << 2) | (v >> 4)`, so 63 becomes 255; the shortcut
//! `v << 2` gives 252 and a palette that can never reach white. Writing back
//! is `v >> 2`, which round-trips the expansion exactly.
//!
//! The same suffix also carries JASC-PAL text (Paint Shop Pro and several
//! modding tools). It is detected first: read as binary, its ASCII digits
//! would make a palette of confident nonsense.

/// 256 colours as `0xAARRGGBB`. Index 0 is transparent (alpha 0).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Palette {
    pub argb: [u32; 256],
    pub name: String,
}

const JASC_MAGIC: &[u8] = b"JASC-PAL";

/// Bytes in a binary VGA palette.
pub const VGA_BYTES: usize = 768;

/// 6-bit VGA to 8-bit: `(v << 2) | (v >> 4)`.
pub fn expand6(v: u8) -> u8 {
    let v = v & 0x3F;
    (v << 2) | (v >> 4)
}

/// 8-bit to 6-bit VGA: the exact inverse of [`expand6`] for its outputs.
pub fn compress8(v: u8) -> u8 {
    v >> 2
}

/// Is this a JASC-PAL text palette?
pub fn is_jasc(bytes: &[u8]) -> bool {
    bytes.starts_with(JASC_MAGIC)
}

/// A 768-byte 6-bit VGA palette or a JASC-PAL text file; `None` when it is
/// neither. JASC is checked first. Index 0 is made transparent.
pub fn read_palette(bytes: &[u8]) -> Option<Palette> {
    let mut argb = [0xFF00_0000u32; 256];
    if is_jasc(bytes) {
        // Magic, version, count, then "r g b" per line, 8-bit values. Short
        // files are padded, unparsable lines skipped, as the references do.
        let text = String::from_utf8_lossy(bytes);
        let mut i = 0;
        for line in text.lines().skip(3) {
            if i >= 256 {
                break;
            }
            let mut fields = line.split_whitespace().map(str::parse::<i64>);
            let (Some(Ok(r)), Some(Ok(g)), Some(Ok(b))) =
                (fields.next(), fields.next(), fields.next())
            else {
                continue;
            };
            argb[i] = 0xFF00_0000
                | ((r as u32 & 0xFF) << 16)
                | ((g as u32 & 0xFF) << 8)
                | (b as u32 & 0xFF);
            i += 1;
        }
    } else {
        if bytes.len() < VGA_BYTES {
            return None;
        }
        let (entries, _) = bytes[..VGA_BYTES].as_chunks::<3>();
        for (c, [r, g, b]) in argb.iter_mut().zip(entries) {
            *c = 0xFF00_0000
                | (u32::from(expand6(*r)) << 16)
                | (u32::from(expand6(*g)) << 8)
                | u32::from(expand6(*b));
        }
    }
    argb[0] &= 0x00FF_FFFF; // index 0 is transparent
    Some(Palette {
        argb,
        name: String::new(),
    })
}

/// The palette as 768 bytes of 6-bit VGA (`v >> 2`).
pub fn write_vga(palette: &Palette) -> Vec<u8> {
    let mut out = Vec::with_capacity(VGA_BYTES);
    for c in palette.argb {
        out.push(compress8((c >> 16) as u8));
        out.push(compress8((c >> 8) as u8));
        out.push(compress8(c as u8));
    }
    out
}

/// The palette as JASC-PAL text (8-bit values, CRLF lines as PSP writes).
pub fn write_jasc(palette: &Palette) -> String {
    let mut out = String::from("JASC-PAL\r\n0100\r\n256\r\n");
    for c in palette.argb {
        out.push_str(&format!(
            "{} {} {}\r\n",
            (c >> 16) & 0xFF,
            (c >> 8) & 0xFF,
            c & 0xFF
        ));
    }
    out
}

/// Used when no palette is configured: a grey ramp with the conventional
/// special indices made visible (0 transparent magenta, 1 shadow, 16-31
/// red). The indices are what matter; the colours only make it readable.
pub fn fallback_palette() -> Palette {
    let mut argb = [0u32; 256];
    for (i, c) in argb.iter_mut().enumerate() {
        let v = i as u32;
        *c = 0xFF00_0000 | (v << 16) | (v << 8) | v;
    }
    argb[0] = 0x00FF_00FF; // transparent
    argb[1] = 0xFF28_2828; // shadow
    for (i, c) in argb.iter_mut().enumerate().take(32).skip(16) {
        *c = 0xFF00_0000 | ((60 + (i as u32 - 16) * 12) << 16); // the remap ramp
    }
    Palette {
        argb,
        name: "grey (no palette set)".to_string(),
    }
}

/// The palette with every index given a colour of its own.
///
/// Game palettes repeat colours (one colour on dozens of indices, copies of
/// the 16-31 remap ramp), so a full-colour editor cannot tell those indices
/// apart. Each repeat is nudged by the smallest step that makes it unique (a
/// level or two, too little to see), so full colour maps back to every index
/// exactly. Index 0 keeps its colour; it is transparent anyway.
pub fn unique_colours(palette: &Palette) -> Palette {
    let mut out = palette.clone();
    let mut used: Vec<u32> = Vec::with_capacity(256);
    for i in 0..256 {
        let rgb = out.argb[i] & 0xFF_FFFF;
        let mut pick = rgb;
        if i > 0 && used.contains(&pick) {
            pick = nearest_free(rgb, &used);
        }
        used.push(pick);
        out.argb[i] = (out.argb[i] & 0xFF00_0000) | pick;
    }
    out
}

/// The nearest colour to `rgb` not in `used`: every offset with all three
/// channels within `r`, for `r = 1, 2, ...`, fewest total levels first, then
/// red, green, blue offsets ascending (qshp's search order).
fn nearest_free(rgb: u32, used: &[u32]) -> u32 {
    let (r0, g0, b0) = (
        ((rgb >> 16) & 0xFF) as i32,
        ((rgb >> 8) & 0xFF) as i32,
        (rgb & 0xFF) as i32,
    );
    for r in 1..256i32 {
        for total in 1..=3 * r {
            for dr in -r..=r {
                for dg in -r..=r {
                    for db in -r..=r {
                        if dr.abs() + dg.abs() + db.abs() != total
                            || dr.abs().max(dg.abs()).max(db.abs()) != r
                        {
                            continue;
                        }
                        let (red, green, blue) = (r0 + dr, g0 + dg, b0 + db);
                        if !(0..=255).contains(&red)
                            || !(0..=255).contains(&green)
                            || !(0..=255).contains(&blue)
                        {
                            continue;
                        }
                        let c = ((red as u32) << 16) | ((green as u32) << 8) | blue as u32;
                        if !used.contains(&c) {
                            return c;
                        }
                    }
                }
            }
        }
    }
    rgb
}

/// Nearest palette index for a colour, 1..=255 (index 0 is transparency,
/// which the caller decides by alpha). Exact matches win; on a tie an index
/// outside the 16-31 remap ramp wins, because the ramp is searched last, so
/// ordinary pixels never turn into house colour by accident.
pub fn nearest(palette: &Palette, argb: u32) -> u8 {
    let (r, g, b) = (
        ((argb >> 16) & 0xFF) as i64,
        ((argb >> 8) & 0xFF) as i64,
        (argb & 0xFF) as i64,
    );
    let mut best = 1usize;
    let mut best_d = i64::MAX;
    for k in 1..256usize {
        let i = match k {
            1..=15 => k,
            16..=239 => k + 16,
            _ => k - 224,
        };
        let c = palette.argb[i];
        let dr = ((c >> 16) & 0xFF) as i64 - r;
        let dg = ((c >> 8) & 0xFF) as i64 - g;
        let db = (c & 0xFF) as i64 - b;
        let d = dr * dr * 2 + dg * dg * 4 + db * db * 3;
        if d < best_d {
            best_d = d;
            best = i;
            if d == 0 {
                break;
            }
        }
    }
    best as u8
}

/// What the game does with a palette index, from Ochre's C&C palette
/// conventions (`addons/cnc/data/palettes.ini`).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Role {
    /// Index 0: transparency itself; it cannot be stored opaque.
    Transparent,
    /// Index 1 in unit palettes: the shadow.
    Shadow,
    /// 16-31 in unit palettes: the house-colour remap ramp.
    Remap,
    /// 240-254 in unit palettes: drawn unaffected by lighting.
    Unlit,
    /// No special meaning.
    Plain,
}

impl Role {
    pub fn label(self) -> &'static str {
        match self {
            Role::Transparent => "transparent",
            Role::Shadow => "shadow",
            Role::Remap => "house colour remap",
            Role::Unlit => "unaffected by lighting",
            Role::Plain => "",
        }
    }
}

/// True for a terrain palette (`iso*.pal`), which reserves only index 0.
pub fn is_terrain(name: &str) -> bool {
    name.to_ascii_lowercase().starts_with("iso")
}

/// The role of `index` in the palette called `name` (a file name such as
/// `unittem.pal`; any non-terrain palette uses the unit conventions).
pub fn role(name: &str, index: u8) -> Role {
    match index {
        0 => Role::Transparent,
        _ if is_terrain(name) => Role::Plain,
        1 => Role::Shadow,
        16..=31 => Role::Remap,
        240..=254 => Role::Unlit,
        _ => Role::Plain,
    }
}
