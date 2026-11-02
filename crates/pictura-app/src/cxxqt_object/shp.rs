// SPDX-License-Identifier: GPL-3.0-or-later
//! Westwood SHP documents (KookaPictura-SHP fork).
//!
//! An SHP opens as one RGBA sheet (`pictura_shp::sheet`): every frame in a
//! cell, every palette index drawn as a colour of its own. The layout, the
//! palette as read and the original file bytes live in the document's
//! private `KookaSHP` image resource, so undo, redo and a PSD round trip
//! carry them with the pixels. Save maps the sheet back to indices exactly,
//! refuses off-palette pixels and gutter bleed (the shell offers a recorded
//! snap or clear first), and keeps every untouched frame's bytes.
//!
//! A file whose sheet would be over the size limit opens as a **window**
//! of frames; the frames outside it stay in the resource (the original
//! file, plus the edited ones in its frame store) until the shell pages
//! them in with `shp_show_frames`.
//!
//! The hooks into upstream files are listed in `crates/pictura-shp/HOOKS.md`.

mod imp;

use super::qobject::PictureView;
use imp::*;
pub(in crate::cxxqt_object) use imp::{
    decode_shp, save_shp, with_document_palette, with_open_request, OpenRequest,
};

#[cxx_qt::bridge]
pub mod ffi {
    unsafe extern "C++" {
        include!("cxx-qt-lib/qstring.h");
        type QString = cxx_qt_lib::QString;

        include!("pictura_app/src/cxxqt_object.cxxqt.h");
        #[namespace = "pictura"]
        type PictureView = super::PictureView;
    }

    /// The layout and palette of an SHP document. `valid` is false for any
    /// other document.
    #[namespace = "pictura"]
    struct ShpInfo {
        valid: bool,
        /// The SHP canvas, which is every cell's size.
        width: i32,
        height: i32,
        frames: i32,
        cols: i32,
        gutter: i32,
        palette_name: QString,
        palette_path: QString,
        /// True when the document was read from a file whose untouched
        /// frames Save keeps byte for byte.
        has_source: bool,
        /// The file frame in the sheet's first cell; `frames` from it are on
        /// the sheet.
        first: i32,
        /// The frames in the file (more than `frames` when it is paged).
        total: i32,
        /// The window size paging uses (`frames`, except on a short last
        /// window).
        span: i32,
    }

    /// A sheet measured against the SHP size limit. Sizes in bytes; a
    /// sheet's bytes are its RGBA.
    #[namespace = "pictura"]
    struct ShpPlan {
        valid: bool,
        /// The SHP canvas (or, for a size change, the new document size).
        width: i32,
        height: i32,
        total: i32,
        /// The window: `count` frames from `first`.
        first: i32,
        count: i32,
        /// The sheet's bytes.
        needed: u64,
        /// The size limit.
        limit: u64,
        /// Memory available now; 0 when it cannot be read.
        available: u64,
        /// The estimated real footprint (`needed` times the measured factor).
        estimate: u64,
        /// 0 within the limit, 1 at most 10% over it, 2 further.
        stage: i32,
        /// The estimate fits the memory available now. No setting holds a
        /// sheet when it does not.
        floor_ok: bool,
    }

    /// A small ARGB picture of a frame off the sheet.
    #[namespace = "pictura"]
    struct ShpThumb {
        width: i32,
        height: i32,
        /// `width * height` straight `0xAARRGGBB`, row-major.
        argb: Vec<u32>,
    }

    /// What Save would find. `error` is non-empty when the document cannot
    /// be saved as an SHP at all (not an SHP document, wrong size or mode).
    #[namespace = "pictura"]
    struct ShpCheck {
        error: QString,
        off_palette: i32,
        /// The frames holding off-palette pixels, ascending.
        off_frames: Vec<i32>,
        /// The off-palette bounds of the first such frame, sheet pixels
        /// `x, y, w, h`; empty when there are none.
        first_off: Vec<i32>,
        /// Non-transparent pixels in the gutters (bleed).
        gutter: i32,
    }

    #[namespace = "pictura"]
    extern "Rust" {
        /// True when `path` holds a TS/RA2 SHP (the whole frame table checks
        /// out); the shell resolves a palette only for these.
        fn shp_sniff_file(path: &QString) -> bool;

        /// The `.pal` the next `open_image_start` on this view uses for an
        /// SHP; empty for the built-in grey fallback.
        fn shp_set_open_palette(view: Pin<&mut PictureView>, path: &QString);

        fn shp_info(view: &PictureView) -> ShpInfo;

        /// The 256 colours the sheet draws index `i` with (`0xAARRGGBB`,
        /// each unique); empty for a document that is not an SHP.
        fn shp_palette_unique(view: &PictureView) -> Vec<u32>;

        /// The 256 colours of the palette as read from the `.pal`.
        fn shp_palette_source(view: &PictureView) -> Vec<u32>;

        /// The palette index an opaque `0xRRGGBB` stands for, or -1 when it
        /// is off-palette.
        fn shp_index_of(view: &PictureView, rgb: u32) -> i32;

        /// What the game does with `index` under this palette, e.g. "house
        /// colour remap"; empty for an ordinary index.
        fn shp_role(view: &PictureView, index: i32) -> QString;

        /// Scan the document the way Save will.
        fn shp_check(view: &PictureView) -> ShpCheck;

        /// Snap every off-palette pixel to its nearest index (recorded as
        /// "Snap to Palette"); the number of pixels changed, or -1 when it
        /// cannot (the reason is `shp_last_error`).
        fn shp_snap(view: Pin<&mut PictureView>) -> i32;

        /// Clear every gutter pixel (recorded as "Clear Frame Gutters"); the
        /// number changed, or -1 when it cannot.
        fn shp_clear_gutter(view: Pin<&mut PictureView>) -> i32;

        /// Recolour the sheet by index with the palette at `path` and keep
        /// it in the document, as one recorded "Change Palette" edit.
        fn shp_rebind_palette(view: Pin<&mut PictureView>, path: &QString) -> bool;

        /// Why the last SHP save, snap, clear, rebind or page change on this
        /// thread failed.
        fn shp_last_error() -> QString;

        /// The window `path` would open with (`count` 0), or its first
        /// `count` frames (the whole file when `count` is its frame count),
        /// from its header (nothing is decoded); `valid` is false for a file
        /// that is not an SHP.
        fn shp_plan_file(path: &QString, count: i32) -> ShpPlan;

        /// The window of `count` frames that holds `frame` (windows are
        /// aligned to multiples of `count`; 0 keeps the document's size).
        fn shp_plan_window(view: &PictureView, frame: i32, count: i32) -> ShpPlan;

        /// The SHP document resized to `width` x `height`.
        fn shp_plan_size(view: &PictureView, width: i32, height: i32) -> ShpPlan;

        /// Bytes of sheet past the size limit the user agreed the next open
        /// on this view may hold (0: none).
        fn shp_set_open_allowance(view: Pin<&mut PictureView>, bytes: u64);

        /// Page: show the window of `count` frames (0: the document's
        /// window size) that holds `frame`,
        /// keeping the outgoing frames' edits in the frame store. History
        /// starts again at the new window. `allowance` as above. False with
        /// the reason in `shp_last_error` when it cannot.
        fn shp_show_frames(
            view: Pin<&mut PictureView>,
            frame: i32,
            count: i32,
            allowance: u64,
        ) -> bool;

        /// Frame `index` off the sheet, at most `side` pixels a side; empty
        /// for a frame on the sheet.
        fn shp_frame_thumbnail(view: &PictureView, index: i32, side: i32) -> ShpThumb;

        /// What frame `index`'s thumbnail depends on besides the palette:
        /// 0 for an original frame off the sheet, a hash of its stored copy
        /// for an edited one, `u64::MAX` for a frame on the sheet.
        fn shp_frame_stamp(view: &PictureView, index: i32) -> u64;

        /// Test seam: the size limit and available memory in bytes (0 keeps
        /// the real value).
        fn shp_set_test_limits(limit: u64, available: u64);

        /// The measured real footprint per byte of sheet.
        fn shp_footprint_factor() -> u64;

        /// The SHP size limit in bytes of sheet.
        fn shp_size_limit() -> u64;
    }
}
