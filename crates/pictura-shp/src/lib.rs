// Derived from qshp (`src/shpcodec.{h,cpp}`) and Ochre's C&C addon
// (`addons/cnc/cncshp.py`, `cncpal.py`). Both Copyright (C) 2026 Ochre
// contributors, GPL-3.0-or-later, by the owner of this fork.
// SPDX-License-Identifier: GPL-3.0-or-later

//! Westwood SHP sprites (Tiberian Sun / Red Alert 2) and `.pal` palettes.
//!
//! Standard library only: no Qt, no other Kooka Pictura crate. The pieces:
//!
//! - [`sniff`] validates the whole frame table, because the format's only
//!   magic is a leading zero word.
//! - [`parse`] decodes leniently (real files carry truncated lines and
//!   over-long runs); [`Sprite::canvas`] places a frame on the full canvas.
//! - [`Source`] keeps everything a writer is free to choose, frame by frame,
//!   and [`build_preserving`] gives untouched frames their original bytes.
//! - [`palette`] reads and writes `.pal` files and maps colours to indices.
//! - [`sheet`] lays the frames out on one RGBA sheet and maps it back to
//!   indices exactly, reporting (never guessing) off-palette pixels.
//! - [`frames`] inserts, duplicates, deletes, moves and reverses frames.
//! - [`unit`] finds, checks and casts the shadow half of a unit sprite.
//! - [`resource`] is the private image resource that keeps an SHP
//!   document's layout, palette and original bytes inside the document.
//!
//! See `README.md` for the format sources.

mod codec;
pub mod frames;
pub mod palette;
pub mod resource;
pub mod sheet;
mod source;
pub mod unit;

pub use codec::{
    decode_frame, decode_prefixed_line, decode_rle_line, encode_prefixed, encode_rle,
    encode_rle_line, frame_canvas, frame_span, line_span, parse, sniff, table_bytes, Error, Frame,
    FrameHeader, Sprite, FRAME_HEADER, HEADER, MAX_FRAMES, MAX_SIDE,
};
pub use source::{
    build, build_frames, build_mapped, build_preserving, build_with, FramePlane, Source,
    SourceFrame,
};
