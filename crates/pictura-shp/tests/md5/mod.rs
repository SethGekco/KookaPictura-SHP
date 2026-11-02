//! MD5 (RFC 1321), only to compare decodes with the reference manifest's
//! hashes; written here so the crate keeps no third-party dependency.

const S: [u32; 64] = [
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9,
    14, 20, 5, 9, 14, 20, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 6, 10, 15,
    21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
];

pub struct Md5 {
    state: [u32; 4],
    buffer: Vec<u8>,
    length: u64,
}

impl Md5 {
    pub fn new() -> Self {
        Md5 {
            state: [0x6745_2301, 0xEFCD_AB89, 0x98BA_DCFE, 0x1032_5476],
            buffer: Vec::with_capacity(64),
            length: 0,
        }
    }

    pub fn update(&mut self, mut data: &[u8]) {
        self.length = self.length.wrapping_add(data.len() as u64);
        if !self.buffer.is_empty() {
            let take = (64 - self.buffer.len()).min(data.len());
            self.buffer.extend_from_slice(&data[..take]);
            data = &data[take..];
            if self.buffer.len() == 64 {
                let block: [u8; 64] = self.buffer[..].try_into().unwrap();
                self.block(&block);
                self.buffer.clear();
            }
        }
        let (blocks, rest) = data.as_chunks::<64>();
        for block in blocks {
            self.block(block);
        }
        self.buffer.extend_from_slice(rest);
    }

    pub fn hex(mut self) -> String {
        let bits = self.length.wrapping_mul(8);
        let mut pad = vec![0x80u8];
        while (self.buffer.len() + pad.len()) % 64 != 56 {
            pad.push(0);
        }
        pad.extend_from_slice(&bits.to_le_bytes());
        let length = self.length;
        self.update(&pad);
        self.length = length;
        self.state
            .iter()
            .flat_map(|w| w.to_le_bytes())
            .map(|b| format!("{b:02x}"))
            .collect()
    }

    fn block(&mut self, block: &[u8; 64]) {
        let m: Vec<u32> = block
            .as_chunks::<4>()
            .0
            .iter()
            .map(|c| u32::from_le_bytes(*c))
            .collect();
        let [mut a, mut b, mut c, mut d] = self.state;
        for (i, &shift) in S.iter().enumerate() {
            let (f, g) = match i / 16 {
                0 => ((b & c) | (!b & d), i),
                1 => ((d & b) | (!d & c), (5 * i + 1) % 16),
                2 => (b ^ c ^ d, (3 * i + 5) % 16),
                _ => (c ^ (b | !d), (7 * i) % 16),
            };
            let k = ((i as f64 + 1.0).sin().abs() * 4_294_967_296.0) as u32;
            let rotated = a
                .wrapping_add(f)
                .wrapping_add(k)
                .wrapping_add(m[g])
                .rotate_left(shift);
            a = d;
            d = c;
            c = b;
            b = b.wrapping_add(rotated);
        }
        for (s, v) in self.state.iter_mut().zip([a, b, c, d]) {
            *s = s.wrapping_add(v);
        }
    }
}

#[test]
fn md5_matches_the_rfc_vectors() {
    let hash = |s: &[u8]| {
        let mut m = Md5::new();
        m.update(s);
        m.hex()
    };
    assert_eq!(hash(b""), "d41d8cd98f00b204e9800998ecf8427e");
    assert_eq!(hash(b"abc"), "900150983cd24fb0d6963f7d28e17f72");
    assert_eq!(
        hash(b"12345678901234567890123456789012345678901234567890123456789012345678901234567890"),
        "57edf4a22be3c955ac49da2e2107b67a"
    );
    let mut split = Md5::new();
    split.update(b"1234567890123456789012345678901234567890");
    split.update(b"1234567890123456789012345678901234567890");
    assert_eq!(split.hex(), "57edf4a22be3c955ac49da2e2107b67a");
}
