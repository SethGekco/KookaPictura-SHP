// SPDX-License-Identifier: GPL-3.0-or-later
//! Blowfish (Bruce Schneier, 1993; public domain), electronic codebook mode,
//! as MIX archives use it for their index: standard Blowfish with each 8-byte
//! block read as two big-endian words, which is what XCC's and OpenRA's
//! readers amount to (OpenRA `Blowfish.cs` swaps the little-endian words it
//! reads before the rounds).

use super::blowfish_tables::{P, S};

pub(crate) struct Blowfish {
    p: [u32; 18],
    s: [[u32; 256]; 4],
}

impl Blowfish {
    /// The key schedule for a key of 1 to 72 bytes (MIX keys are 56).
    pub(crate) fn new(key: &[u8]) -> Blowfish {
        let mut bf = Blowfish { p: P, s: S };
        if !key.is_empty() {
            let mut j = 0;
            for p in bf.p.iter_mut() {
                let mut word = 0u32;
                for _ in 0..4 {
                    word = (word << 8) | u32::from(key[j % key.len()]);
                    j += 1;
                }
                *p ^= word;
            }
        }
        let (mut l, mut r) = (0u32, 0u32);
        for i in (0..18).step_by(2) {
            (l, r) = bf.encrypt_words(l, r);
            bf.p[i] = l;
            bf.p[i + 1] = r;
        }
        for b in 0..4 {
            for i in (0..256).step_by(2) {
                (l, r) = bf.encrypt_words(l, r);
                bf.s[b][i] = l;
                bf.s[b][i + 1] = r;
            }
        }
        bf
    }

    fn f(&self, x: u32) -> u32 {
        let [a, b, c, d] = x.to_be_bytes();
        (self.s[0][usize::from(a)].wrapping_add(self.s[1][usize::from(b)])
            ^ self.s[2][usize::from(c)])
        .wrapping_add(self.s[3][usize::from(d)])
    }

    fn encrypt_words(&self, mut l: u32, mut r: u32) -> (u32, u32) {
        for i in 0..16 {
            l ^= self.p[i];
            r ^= self.f(l);
            std::mem::swap(&mut l, &mut r);
        }
        std::mem::swap(&mut l, &mut r);
        r ^= self.p[16];
        l ^= self.p[17];
        (l, r)
    }

    fn decrypt_words(&self, mut l: u32, mut r: u32) -> (u32, u32) {
        for i in (2..18).rev() {
            l ^= self.p[i];
            r ^= self.f(l);
            std::mem::swap(&mut l, &mut r);
        }
        std::mem::swap(&mut l, &mut r);
        r ^= self.p[1];
        l ^= self.p[0];
        (l, r)
    }

    fn run(&self, data: &mut [u8], decrypt: bool) {
        for block in data.as_chunks_mut::<8>().0 {
            let l = u32::from_be_bytes([block[0], block[1], block[2], block[3]]);
            let r = u32::from_be_bytes([block[4], block[5], block[6], block[7]]);
            let (l, r) = if decrypt {
                self.decrypt_words(l, r)
            } else {
                self.encrypt_words(l, r)
            };
            block[..4].copy_from_slice(&l.to_be_bytes());
            block[4..].copy_from_slice(&r.to_be_bytes());
        }
    }

    /// Encrypt whole 8-byte blocks in place (a shorter tail is left alone).
    pub(crate) fn encrypt(&self, data: &mut [u8]) {
        self.run(data, false);
    }

    /// Decrypt whole 8-byte blocks in place (a shorter tail is left alone).
    pub(crate) fn decrypt(&self, data: &mut [u8]) {
        self.run(data, true);
    }
}
