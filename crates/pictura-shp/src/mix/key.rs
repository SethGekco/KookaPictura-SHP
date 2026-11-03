// SPDX-License-Identifier: GPL-3.0-or-later
//! The Blowfish key of an encrypted MIX index, recovered from the 80-byte
//! "key source" that follows the flags.
//!
//! The algorithm is the one every public reader implements: XCC Utilities
//! by Olaf van der Spek (`mix_decode.cpp`: `init_pubkey`, `process_predata`,
//! `get_blowfish_key`) and OpenRA (`OpenRA.Mods.Cnc/FileFormats/
//! BlowfishKeyProvider.cs`); ccmaps-net reads it the same way. This is a new
//! implementation from that description, not a copy of either:
//!
//! - Westwood's public RSA key is a base64 string. Decoded, it is a DER-style
//!   integer (tag 2, a length, then the big-endian modulus `n`); the public
//!   exponent is 65537.
//! - `bits` is the bit length of `n` minus one, and each block of the key
//!   source is `a + 1` bytes where `a = (bits - 1) / 8` (40 bytes for the
//!   320-bit Westwood key, so the 80 bytes are two blocks).
//! - Each block, read as a **little-endian** integer `m`, becomes
//!   `m^65537 mod n`; its low `a` bytes (little-endian) are appended to the
//!   output. The Blowfish key is the first 56 output bytes.

/// The public key in Westwood's games (as published in XCC and OpenRA).
const WESTWOOD_PUBLIC_KEY: &str = "AihRvNoIbTn85FZRYNZRcT+i6KpU+maCsEqr3Q5q+LDB5tH7Tz2qQ38V";

/// The Blowfish key length MIX archives use.
pub const BLOWFISH_KEY: usize = 56;

/// Little-endian 32-bit limbs, no leading zero limbs required.
#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) struct Big(Vec<u32>);

impl Big {
    pub(crate) fn from_le_bytes(bytes: &[u8]) -> Big {
        let mut limbs = vec![0u32; bytes.len().div_ceil(4).max(1)];
        for (i, b) in bytes.iter().enumerate() {
            limbs[i / 4] |= u32::from(*b) << (8 * (i % 4));
        }
        Big(limbs).trimmed()
    }

    pub(crate) fn from_be_bytes(bytes: &[u8]) -> Big {
        let mut le = bytes.to_vec();
        le.reverse();
        Big::from_le_bytes(&le)
    }

    /// The low `len` bytes, little-endian.
    pub(crate) fn to_le_bytes(&self, len: usize) -> Vec<u8> {
        (0..len)
            .map(|i| {
                self.0
                    .get(i / 4)
                    .map_or(0, |limb| (limb >> (8 * (i % 4))) as u8)
            })
            .collect()
    }

    fn trimmed(mut self) -> Big {
        while self.0.len() > 1 && self.0.last() == Some(&0) {
            self.0.pop();
        }
        self
    }

    pub(crate) fn bits(&self) -> usize {
        let top = self.0.iter().rposition(|&l| l != 0);
        top.map_or(0, |i| 32 * i + 32 - self.0[i].leading_zeros() as usize)
    }

    fn bit(&self, i: usize) -> bool {
        self.0.get(i / 32).is_some_and(|l| (l >> (i % 32)) & 1 == 1)
    }

    fn mul(&self, other: &Big) -> Big {
        let mut out = vec![0u32; self.0.len() + other.0.len()];
        for (i, &a) in self.0.iter().enumerate() {
            let mut carry = 0u64;
            for (j, &b) in other.0.iter().enumerate() {
                let t = u64::from(a) * u64::from(b) + u64::from(out[i + j]) + carry;
                out[i + j] = t as u32;
                carry = t >> 32;
            }
            out[i + other.0.len()] = carry as u32;
        }
        Big(out).trimmed()
    }

    /// `self mod m` by shift-and-subtract (the numbers here are a few
    /// hundred bits, so the simple way is fast enough).
    fn rem(&self, m: &Big) -> Big {
        let mut r = Big(vec![0u32; m.0.len() + 1]);
        for i in (0..self.bits()).rev() {
            r.shl1();
            if self.bit(i) {
                r.0[0] |= 1;
            }
            if r.ge(m) {
                r.sub(m);
            }
        }
        r.trimmed()
    }

    fn shl1(&mut self) {
        let mut carry = 0;
        for limb in self.0.iter_mut() {
            let next = *limb >> 31;
            *limb = (*limb << 1) | carry;
            carry = next;
        }
        if carry != 0 {
            self.0.push(carry);
        }
    }

    fn ge(&self, other: &Big) -> bool {
        let len = self.0.len().max(other.0.len());
        for i in (0..len).rev() {
            let a = self.0.get(i).copied().unwrap_or(0);
            let b = other.0.get(i).copied().unwrap_or(0);
            if a != b {
                return a > b;
            }
        }
        true
    }

    /// `self -= other`, which must not be larger.
    fn sub(&mut self, other: &Big) {
        let mut borrow = 0i64;
        for i in 0..self.0.len() {
            let t = i64::from(self.0[i]) - i64::from(other.0.get(i).copied().unwrap_or(0)) - borrow;
            self.0[i] = t as u32;
            borrow = i64::from(t < 0);
        }
    }

    /// `self^e mod m`.
    pub(crate) fn pow_mod(&self, e: &Big, m: &Big) -> Big {
        let mut result = Big(vec![1]);
        let base = self.rem(m);
        for i in (0..e.bits()).rev() {
            result = result.mul(&result).rem(m);
            if e.bit(i) {
                result = result.mul(&base).rem(m);
            }
        }
        result
    }
}

/// An RSA public key as the MIX key recovery uses it.
#[derive(Clone, Debug)]
pub struct PublicKey {
    modulus: Big,
    exponent: Big,
}

impl PublicKey {
    /// A key from its big-endian modulus and exponent (tests use a key pair
    /// of their own, since nobody can make a key source for Westwood's).
    pub fn new(modulus_be: &[u8], exponent_be: &[u8]) -> PublicKey {
        PublicKey {
            modulus: Big::from_be_bytes(modulus_be),
            exponent: Big::from_be_bytes(exponent_be),
        }
    }

    /// Westwood's public key.
    pub fn westwood() -> PublicKey {
        let der = base64(WESTWOOD_PUBLIC_KEY);
        // Tag 2 (an integer), then a short-form length, then the modulus.
        debug_assert_eq!(der.first(), Some(&2));
        let len = usize::from(der[1]);
        PublicKey {
            modulus: Big::from_be_bytes(&der[2..2 + len]),
            exponent: Big(vec![0x10001]),
        }
    }

    /// The output bytes each key-source block yields.
    fn block_out(&self) -> usize {
        (self.modulus.bits() - 2) / 8
    }

    /// The length of the key source: enough blocks for 56 key bytes.
    pub fn key_source_len(&self) -> usize {
        let a = self.block_out();
        (55 / a + 1) * (a + 1)
    }

    /// The Blowfish key a key source stands for.
    pub fn blowfish_key(&self, source: &[u8]) -> [u8; BLOWFISH_KEY] {
        let a = self.block_out();
        let mut out = Vec::with_capacity(self.key_source_len());
        for block in source.chunks_exact(a + 1) {
            let m = Big::from_le_bytes(block);
            out.extend(m.pow_mod(&self.exponent, &self.modulus).to_le_bytes(a));
        }
        out.resize(BLOWFISH_KEY.max(out.len()), 0);
        let mut key = [0u8; BLOWFISH_KEY];
        key.copy_from_slice(&out[..BLOWFISH_KEY]);
        key
    }
}

/// The key source that stands for `key` under a key pair whose private
/// exponent is `private_be` (the inverse of `PublicKey::blowfish_key`).
/// Only a test key pair can use it: Westwood's private key is not public.
pub fn key_source_for(public: &PublicKey, private_be: &[u8], key: &[u8]) -> Vec<u8> {
    let d = Big::from_be_bytes(private_be);
    let a = public.block_out();
    let blocks = public.key_source_len() / (a + 1);
    let mut padded = key.to_vec();
    padded.resize(blocks * a, 0);
    let mut out = Vec::new();
    for chunk in padded.chunks_exact(a) {
        let m = Big::from_le_bytes(chunk);
        out.extend(m.pow_mod(&d, &public.modulus).to_le_bytes(a + 1));
    }
    out
}

fn base64(text: &str) -> Vec<u8> {
    let value = |c: u8| -> u32 {
        match c {
            b'A'..=b'Z' => u32::from(c - b'A'),
            b'a'..=b'z' => u32::from(c - b'a') + 26,
            b'0'..=b'9' => u32::from(c - b'0') + 52,
            b'+' => 62,
            _ => 63,
        }
    };
    let mut out = Vec::new();
    for chunk in text.as_bytes().chunks(4) {
        let mut acc = 0u32;
        for &c in chunk {
            acc = (acc << 6) | value(c);
        }
        acc <<= 6 * (4 - chunk.len()) as u32;
        let bytes = acc.to_be_bytes();
        out.extend_from_slice(&bytes[1..chunk.len()]);
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_westwood_key_is_319_bits_and_reads_80_bytes() {
        let key = PublicKey::westwood();
        assert_eq!(key.modulus.bits(), 319);
        assert_eq!(key.block_out(), 39);
        assert_eq!(key.key_source_len(), 80);
    }

    #[test]
    fn pow_mod_matches_small_numbers() {
        let b = Big::from_le_bytes(&[7]);
        let e = Big::from_le_bytes(&[13]);
        let m = Big::from_le_bytes(&[0xA1, 0x0F]); // 4001
                                                   // 7^13 mod 4001 = 96889010407 mod 4001
        assert_eq!(
            b.pow_mod(&e, &m),
            Big::from_le_bytes(&(96_889_010_407u64 % 4001).to_le_bytes())
        );
    }
}
