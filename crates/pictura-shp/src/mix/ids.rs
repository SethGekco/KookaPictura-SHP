// SPDX-License-Identifier: GPL-3.0-or-later
//! The IDs a MIX index stores instead of file names.
//!
//! Both hashes work on the name in upper case (ASCII). Described publicly by
//! XCC (`mix_file.cpp`, `Cmix_file::get_id`), OpenRA (`PackageEntry.HashFilename`)
//! and the ModdingWiki "MIX Format (TS)" page:
//!
//! - **Tiberian Dawn / Red Alert** ([`td_id`]): the name, padded with zero
//!   bytes to a multiple of four, read as little-endian words; the ID starts
//!   at 0 and each word is added after rotating it left by one bit.
//! - **Tiberian Sun / Red Alert 2** ([`ts_id`]): the standard CRC-32 (the
//!   IEEE polynomial, reflected, as in zip) of the name, padded when its
//!   length is not a multiple of four: one byte holding `len % 4`, then
//!   copies of the first byte of the last, partial group of four until the
//!   length is a multiple of four.

/// Which ID a MIX index uses.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum IdKind {
    /// Tiberian Dawn and Red Alert: rotate and add.
    Classic,
    /// Tiberian Sun, Red Alert 2 and Yuri's Revenge: CRC-32.
    Crc,
}

impl IdKind {
    pub fn id(self, name: &str) -> u32 {
        match self {
            IdKind::Classic => td_id(name),
            IdKind::Crc => ts_id(name),
        }
    }
}

fn upper(name: &str) -> Vec<u8> {
    name.bytes().map(|b| b.to_ascii_uppercase()).collect()
}

/// The Tiberian Dawn / Red Alert ID of `name`.
pub fn td_id(name: &str) -> u32 {
    let mut bytes = upper(name);
    bytes.resize(bytes.len().div_ceil(4) * 4, 0);
    bytes.as_chunks::<4>().0.iter().fold(0u32, |id, w| {
        id.rotate_left(1)
            .wrapping_add(u32::from_le_bytes([w[0], w[1], w[2], w[3]]))
    })
}

/// The Tiberian Sun / Red Alert 2 ID of `name`.
pub fn ts_id(name: &str) -> u32 {
    let mut bytes = upper(name);
    let len = bytes.len();
    if !len.is_multiple_of(4) {
        let last = bytes[len & !3];
        bytes.push((len % 4) as u8);
        while !bytes.len().is_multiple_of(4) {
            bytes.push(last);
        }
    }
    crc32(&bytes)
}

const fn crc_table() -> [u32; 256] {
    let mut table = [0u32; 256];
    let mut i = 0;
    while i < 256 {
        let mut c = i as u32;
        let mut k = 0;
        while k < 8 {
            c = if c & 1 != 0 {
                0xEDB8_8320 ^ (c >> 1)
            } else {
                c >> 1
            };
            k += 1;
        }
        table[i] = c;
        i += 1;
    }
    table
}

static CRC_TABLE: [u32; 256] = crc_table();

/// The standard CRC-32 (zip, PNG).
pub fn crc32(data: &[u8]) -> u32 {
    !data.iter().fold(!0u32, |c, &b| {
        CRC_TABLE[usize::from((c as u8) ^ b)] ^ (c >> 8)
    })
}
