// SPDX-License-Identifier: GPL-3.0-or-later
//! Writing an archive: a plan of entries (ranges of the source file, new
//! bytes, or a nested archive rebuilt the same way) streamed to a new file.
//!
//! The header written is never encrypted (the games read both kinds;
//! encrypting needs Westwood's private key). An archive read with a SHA-1
//! checksum of its body is written with a new one, computed while the body
//! streams out. The Tiberian Dawn layout is kept for an archive read in it. Untouched entries are
//! copied byte for byte, in their original body order; the index is sorted
//! by ID as a signed number, the order the games' binary search expects.

use super::{
    copy_range, read_index, Index, MixError, Sha1, CHECKSUM_BYTES, ENTRY_BYTES, FLAG_CHECKSUM,
};
use std::io::{self, Read, Seek, Write};

/// An entry's data.
#[derive(Clone, Debug)]
pub enum Data {
    /// `len` bytes at `start` of the source file.
    Range {
        start: u64,
        len: u64,
    },
    Bytes(Vec<u8>),
    /// A nested archive, written from its own plan.
    Mix(Box<Plan>),
}

impl Data {
    pub fn len(&self) -> u64 {
        match self {
            Data::Range { len, .. } => *len,
            Data::Bytes(b) => b.len() as u64,
            Data::Mix(plan) => plan.len(),
        }
    }

    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }
}

/// A new archive: its entries in body order.
#[derive(Clone, Debug, Default)]
pub struct Plan {
    /// The Tiberian Dawn layout (no flags word).
    pub old_format: bool,
    /// A SHA-1 checksum of the body follows it (never in the old layout).
    pub checksum: bool,
    pub entries: Vec<(u32, Data)>,
}

impl Plan {
    /// Every entry of `index` (an archive at `start` of the source) as it
    /// is, in the original body order. Refused when an entry lies outside
    /// the archive, since it could not be copied.
    pub fn from_index(index: &Index, start: u64) -> Result<Plan, MixError> {
        let mut order: Vec<usize> = (0..index.entries.len()).collect();
        order.sort_by_key(|&i| (index.entries[i].offset, i));
        let mut entries = Vec::with_capacity(order.len());
        for i in order {
            let e = index.entries[i];
            if !index.in_bounds(&e) {
                return Err(MixError(format!(
                    "entry {:08X} lies outside the archive",
                    e.id
                )));
            }
            entries.push((
                e.id,
                Data::Range {
                    start: start + index.start(&e),
                    len: u64::from(e.size),
                },
            ));
        }
        Ok(Plan {
            old_format: index.format == super::Format::Old,
            checksum: index.has_checksum(),
            entries,
        })
    }

    /// Replace the data of entry `id`, or add it at the end of the body.
    pub fn set(&mut self, id: u32, data: Data) {
        match self.entries.iter_mut().find(|(i, _)| *i == id) {
            Some(slot) => slot.1 = data,
            None => self.entries.push((id, data)),
        }
    }

    /// Remove entry `id`; false when there is none.
    pub fn remove(&mut self, id: u32) -> bool {
        let before = self.entries.len();
        self.entries.retain(|(i, _)| *i != id);
        self.entries.len() != before
    }

    fn header_len(&self) -> u64 {
        let fixed = if self.old_format { 6 } else { 10 };
        fixed + ENTRY_BYTES * self.entries.len() as u64
    }

    fn body_len(&self) -> u64 {
        self.entries.iter().map(|(_, d)| d.len()).sum()
    }

    fn has_checksum(&self) -> bool {
        self.checksum && !self.old_format
    }

    /// The archive's length.
    pub fn len(&self) -> u64 {
        let checksum = if self.has_checksum() {
            CHECKSUM_BYTES
        } else {
            0
        };
        self.header_len() + self.body_len() + checksum
    }

    pub fn is_empty(&self) -> bool {
        self.entries.is_empty()
    }

    /// The header: flags (unless old), count, body size, and the index
    /// sorted by signed ID; the body is packed in entry order.
    fn header(&self) -> Result<Vec<u8>, MixError> {
        let count = u16::try_from(self.entries.len())
            .map_err(|_| MixError("more than 65,535 entries".into()))?;
        let body = u32::try_from(self.body_len())
            .map_err(|_| MixError("the archive would be over 4 GiB".into()))?;
        let mut index = Vec::with_capacity(self.entries.len());
        let mut offset = 0u64;
        for (id, data) in &self.entries {
            index.push((*id, offset as u32, data.len() as u32));
            offset += data.len();
        }
        index.sort_by_key(|&(id, _, _)| id as i32);
        let mut out = Vec::with_capacity(self.header_len() as usize);
        if !self.old_format {
            let flags = if self.has_checksum() {
                FLAG_CHECKSUM
            } else {
                0
            };
            out.extend_from_slice(&flags.to_le_bytes());
        }
        out.extend_from_slice(&count.to_le_bytes());
        out.extend_from_slice(&body.to_le_bytes());
        for (id, offset, size) in index {
            out.extend_from_slice(&id.to_le_bytes());
            out.extend_from_slice(&offset.to_le_bytes());
            out.extend_from_slice(&size.to_le_bytes());
        }
        Ok(out)
    }

    /// Write the archive to `out`, copying ranges from `src`.
    pub fn write<R: Read + Seek>(&self, src: &mut R, out: &mut dyn Write) -> Result<(), MixError> {
        out.write_all(&self.header()?)?;
        if !self.has_checksum() {
            return self.write_body(src, out);
        }
        let mut hashing = Hashing {
            out,
            sha: Sha1::new(),
        };
        self.write_body(src, &mut hashing)?;
        let digest = hashing.sha.finish();
        hashing.out.write_all(&digest)?;
        Ok(())
    }

    fn write_body<R: Read + Seek>(&self, src: &mut R, out: &mut dyn Write) -> Result<(), MixError> {
        for (_, data) in &self.entries {
            match data {
                Data::Range { start, len } => copy_range(src, *start, *len, out)?,
                Data::Bytes(bytes) => out.write_all(bytes)?,
                Data::Mix(plan) => plan.write(src, out)?,
            }
        }
        Ok(())
    }

    /// The plan of the outermost archive (`len` bytes at 0 of `src`) with
    /// the entry a chain of IDs names replaced by `data`; every archive on
    /// the way is rebuilt (unencrypted), everything else is copied as it is.
    pub fn replacing<R: Read + Seek>(
        src: &mut R,
        len: u64,
        chain: &[u32],
        data: Data,
    ) -> Result<Plan, MixError> {
        Self::replacing_at(src, 0, len, chain, data)
    }

    fn replacing_at<R: Read + Seek>(
        src: &mut R,
        start: u64,
        len: u64,
        chain: &[u32],
        data: Data,
    ) -> Result<Plan, MixError> {
        let index = read_index(src, start, len)?;
        let mut plan = Plan::from_index(&index, start)?;
        let (&id, rest) = chain
            .split_first()
            .ok_or_else(|| MixError("no entry named".into()))?;
        let i = index
            .find(id)
            .ok_or_else(|| MixError(format!("no entry {id:08X} in the archive")))?;
        let replacement = if rest.is_empty() {
            data
        } else {
            let e = index.entries[i];
            Data::Mix(Box::new(Self::replacing_at(
                src,
                start + index.start(&e),
                u64::from(e.size),
                rest,
                data,
            )?))
        };
        plan.set(id, replacement);
        Ok(plan)
    }
}

/// Hashes what passes through it.
struct Hashing<'a> {
    out: &'a mut dyn Write,
    sha: Sha1,
}

impl Write for Hashing<'_> {
    fn write(&mut self, buf: &[u8]) -> io::Result<usize> {
        let n = self.out.write(buf)?;
        self.sha.update(&buf[..n]);
        Ok(n)
    }

    fn flush(&mut self) -> io::Result<()> {
        self.out.flush()
    }
}
