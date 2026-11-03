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
//! TMP terrain templates share the glue (`shp/tmp.rs`): the content decides
//! which a file is, and the palette queries answer for both.
//!
//! The hooks into upstream files are listed in `crates/pictura-shp/HOOKS.md`.

mod imp;

use super::qobject::PictureView;
use imp::*;
pub(in crate::cxxqt_object) use imp::{
    decode_shp, save_westwood, tmp_suffix, with_document_palette, with_open_request, OnDisk,
    OpenRequest,
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

    /// Off-palette pixels for the overlay (see `shp_off_palette_points`).
    #[namespace = "pictura"]
    struct ShpOffPoints {
        total: i32,
        /// Sheet `x, y` pairs, at most the asked-for limit.
        xy: Vec<i32>,
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

    /// A frame's radar color (`0xRRGGBB`): the one in the file (none for
    /// an added frame) and the average of its pixels now (none for an
    /// empty frame).
    #[namespace = "pictura"]
    struct ShpRadar {
        has_file: bool,
        file: u32,
        has_average: bool,
        average: u32,
    }

    /// A palette kind: its name, its shadow index (-1 none) and its
    /// house-color remap ramp (`remap_first..=remap_last`, -1 none).
    #[namespace = "pictura"]
    struct ShpKind {
        label: QString,
        shadow: i32,
        remap_first: i32,
        remap_last: i32,
    }

    /// A TMP terrain document's template and palette. `valid` is false for
    /// any other document.
    #[namespace = "pictura"]
    struct TmpInfo {
        valid: bool,
        /// The template size in tiles.
        blocks_x: i32,
        blocks_y: i32,
        /// The tile size (48x24 or 60x30).
        cx: i32,
        cy: i32,
        /// Slots holding a tile.
        tiles: i32,
        /// The canvas.
        width: i32,
        height: i32,
        palette_name: QString,
        palette_path: QString,
    }

    /// What saving a TMP document would find (`tmp_check`).
    #[namespace = "pictura"]
    struct TmpCheck {
        /// Why the document cannot be read as a template at all (a missing
        /// or extra layer, a changed canvas); empty when it can.
        error: QString,
        /// Colour pixels that are not palette colours, inside tiles.
        off_palette: i32,
        /// Z-layer pixels that are not depths 0-31 (or "no z").
        off_scale: i32,
        /// The slots holding either, ascending.
        off_slots: Vec<i32>,
        /// The first such pixel, canvas `x, y`; empty when none.
        first_off: Vec<i32>,
        /// Pixels drawn outside every tile's diamond and extra block of
        /// their layer.
        outside: i32,
        first_outside: Vec<i32>,
    }

    /// One tile of a TMP document. Positions are canvas pixels; colors
    /// `0xRRGGBB`.
    #[namespace = "pictura"]
    struct TmpTile {
        slot: i32,
        /// The slot's cell in the template grid.
        col: i32,
        row: i32,
        /// The top-left of the tile's cx x cy rectangle (the diamond inside).
        x: i32,
        y: i32,
        height: i32,
        land: i32,
        ramp: i32,
        radar_low: u32,
        radar_high: u32,
        flags: u32,
        /// Whether its Z Tiles layer holds depths for it now.
        has_z: bool,
        has_extra: bool,
        has_extra_z: bool,
        extra_x: i32,
        extra_y: i32,
        extra_w: i32,
        extra_h: i32,
        /// Which Extras layer holds the extra (0: "Extras"), -1 for none.
        extra_layer: i32,
    }

    /// One entry of a listed MIX archive (a row of the MIX browser).
    #[namespace = "pictura"]
    struct MixRow {
        /// The row of the nested archive holding it; -1 for an entry of the
        /// archive itself.
        parent: i32,
        id: u32,
        /// Empty when no name list knows the ID.
        name: QString,
        /// "SHP", "TMP", "PAL", "MIX", "INI", ... (told from the content).
        kind: QString,
        /// A file suffix for the kind (`shp`; `bin` for unknown data).
        suffix: QString,
        size: u64,
        /// The IDs from the archive's own index down to this entry.
        chain: Vec<u32>,
    }

    /// A listed MIX archive.
    #[namespace = "pictura"]
    struct MixListing {
        /// Why it could not be read; empty when it was.
        error: QString,
        /// Its layout, encryption and checksum, for the browser.
        format: QString,
        rows: Vec<MixRow>,
    }

    #[namespace = "pictura"]
    extern "Rust" {
        /// True when `path` holds a TS/RA2 SHP (the whole frame table checks
        /// out); the shell resolves a palette only for these.
        fn shp_sniff_file(path: &QString) -> bool;

        /// The file holds a TMP terrain template (read whole; real ones are
        /// small).
        fn tmp_sniff_file(path: &QString) -> bool;

        fn tmp_info(view: &PictureView) -> TmpInfo;

        /// The tiles of a TMP document, in slot order.
        fn tmp_tiles(view: &PictureView) -> Vec<TmpTile>;

        /// The slot whose diamond (else extra block) covers canvas pixel
        /// `(x, y)`, or -1.
        fn tmp_tile_at(view: &PictureView, x: i32, y: i32) -> i32;

        /// The game's name for a land type (several values share one).
        fn tmp_land_label(land: i32) -> QString;

        /// The slope a ramp type names.
        fn tmp_ramp_label(ramp: i32) -> QString;

        /// Scan a TMP document the way Save will.
        fn tmp_check(view: &PictureView) -> TmpCheck;

        /// Snap every off-palette colour pixel and off-scale z pixel inside
        /// the tiles (recorded as "Snap to Palette"); the pixels changed, or
        /// -1 with the reason in `shp_last_error`.
        fn tmp_snap(view: Pin<&mut PictureView>) -> i32;

        /// Clear every pixel outside the tiles and extra blocks (recorded as
        /// "Clip to Tiles"); the pixels changed, or -1.
        fn tmp_clip(view: Pin<&mut PictureView>) -> i32;

        /// Set a tile's height, land and ramp types (0-255) and radar colors
        /// (`0xRRGGBB`), recorded as "Tile Fields" (nothing is recorded when
        /// they are unchanged); false with the reason in `shp_last_error`.
        fn tmp_set_tile_fields(
            view: Pin<&mut PictureView>,
            slot: i32,
            height: i32,
            land: i32,
            ramp: i32,
            radar_low: u32,
            radar_high: u32,
        ) -> bool;

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

        /// The palette kind the document's palette name suggests, as an
        /// index into `Kind::ALL` (0 unit, 1 terrain, 2 animation, 3
        /// cameo); -1 for a document that is not an SHP. The shell keeps a
        /// per-document override.
        fn shp_palette_kind(view: &PictureView) -> i32;

        /// What palette kind `kind` reserves (unit for an unknown kind).
        fn shp_kind(kind: i32) -> ShpKind;

        /// What the game does with `index` under a palette of kind `kind`,
        /// e.g. "house colour remap"; empty for an ordinary index.
        fn shp_kind_role(kind: i32, index: i32) -> QString;

        /// Scan the document the way Save will.
        fn shp_check(view: &PictureView) -> ShpCheck;

        /// Snap every off-palette pixel to its nearest index (recorded as
        /// "Snap to Palette"); the number of pixels changed, or -1 when it
        /// cannot (the reason is `shp_last_error`).
        fn shp_snap(view: Pin<&mut PictureView>) -> i32;

        /// Clear every gutter pixel (recorded as "Clear Frame Gutters"); the
        /// number changed, or -1 when it cannot.
        fn shp_clear_gutter(view: Pin<&mut PictureView>) -> i32;

        /// Snap the off-palette pixels of file frame `frame` only (recorded
        /// as "Snap Frame to Palette"); the number changed, or -1 when it
        /// cannot (the frame is not on the sheet, say).
        fn shp_snap_frame(view: Pin<&mut PictureView>, frame: i32) -> i32;

        /// The off-palette pixels inside the cells, for the overlay: how
        /// many, and the first `limit` as sheet `x, y` pairs, row by row.
        fn shp_off_palette_points(view: &PictureView, limit: i32) -> ShpOffPoints;

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
        /// for a frame off the sheet, the original frame it shows (65536
        /// for a blank added one) or a hash of its stored copy for an edited
        /// one; `u64::MAX` for a frame on the sheet.
        fn shp_frame_stamp(view: &PictureView, index: i32) -> u64;

        /// The sheet a frame operation would leave (see `shp_frame_op`),
        /// measured against the size limit; `valid` is false when the
        /// operation does not apply.
        fn shp_plan_frames(
            view: &PictureView,
            op: &QString,
            first: i32,
            count: i32,
            to: i32,
        ) -> ShpPlan;

        /// A frame operation: `insert` (`count` blank frames before frame
        /// `first`; `first` = the frame count appends), `duplicate` (frame
        /// `first`, the copy right after it), `delete` (`count` frames from
        /// `first`), `move` (frame `first` to position `to`), `reverse`
        /// (`count` frames from `first`). On a whole sheet it is one
        /// recorded edit; on a paged one the window is rebuilt and History
        /// starts again (the window must be on the palette first, as when
        /// paging). `allowance` as for `shp_show_frames`. The frame to show
        /// afterwards, or -1 with the reason in `shp_last_error`.
        fn shp_frame_op(
            view: Pin<&mut PictureView>,
            op: &QString,
            first: i32,
            count: i32,
            to: i32,
            allowance: u64,
        ) -> i32;

        /// What "Frame Size" to `width` x `height` needs against the size
        /// limit; `valid` is false for a paged sprite or a side outside
        /// 1..=4096.
        fn shp_plan_frame_size(view: &PictureView, width: i32, height: i32) -> ShpPlan;

        /// "Frame Size": every frame onto a `width` x `height` canvas, placed
        /// by `anchor` (`row * 3 + column` of a 3 x 3 grid; 4 centres it),
        /// cropped or padded with transparency; one recorded edit. Whole
        /// sheets only. 1 when it changed the sheet, 0 when nothing changed,
        /// -1 with the reason in `shp_last_error`.
        fn shp_frame_size(
            view: Pin<&mut PictureView>,
            width: i32,
            height: i32,
            anchor: i32,
            allowance: u64,
        ) -> i32;

        /// What a new sheet of `frames` frames of `width` x `height` needs,
        /// measured against the size limit; `valid` is false when the SHP
        /// format cannot hold it.
        fn shp_plan_sheet(width: i32, height: i32, frames: i32) -> ShpPlan;

        /// File > New SHP: make `view`'s document `frames` blank frames of
        /// `width` x `height` drawn in the palette at `palette` (empty: the
        /// grey fallback). False with the reason in `shp_last_error`.
        fn shp_new_document(
            view: Pin<&mut PictureView>,
            name: &QString,
            width: i32,
            height: i32,
            frames: i32,
            palette: &QString,
            allowance: u64,
        ) -> bool;

        /// Save As SHP of an image that is not an SHP: make `view`'s
        /// document the SHP sheet of `source`'s flattened image cut into
        /// frames. `cut` is frame width, height, columns, rows, gutter and
        /// frame count (cells read row by row). With `nearest`, colours that
        /// are not palette colours become the nearest index (alpha under
        /// 128: index 0); otherwise the save gate reports them.
        fn shp_convert(
            view: Pin<&mut PictureView>,
            source: &PictureView,
            name: &QString,
            cut: &[i32],
            palette: &QString,
            nearest: bool,
            allowance: u64,
        ) -> bool;

        /// The first shadow frame when the frames look like a unit sprite
        /// with a shadow half (an even count, the second half only indices 0
        /// and 1, art in the first half), else -1.
        fn shp_shadow_detect(view: &PictureView) -> i32;

        /// The shadow frames from `half` on that hold an index other than 0
        /// and 1 (the save gate warns about them).
        fn shp_shadow_violations(view: &PictureView, half: i32) -> Vec<i32>;

        /// The sheet after adding a shadow half (as many blank frames as
        /// there are), measured against the size limit.
        fn shp_plan_shadow_half(view: &PictureView) -> ShpPlan;

        /// "Generate Shadows": each art frame in `art` casts its silhouette
        /// as index 1 into its shadow frame (`half` + frame), moved by
        /// `dx`, `dy` and leaning `skew` pixels per row above the art's
        /// base. `half` 0 adds the shadow half first (size-checked; the
        /// allowance as for `shp_frame_op`). One recorded edit on a whole
        /// sheet; on a paged one the window is rebuilt and History starts
        /// again. The frames written, or -1 with the reason in
        /// `shp_last_error`.
        fn shp_generate_shadows(
            view: Pin<&mut PictureView>,
            half: i32,
            art: &[i32],
            dx: i32,
            dy: i32,
            skew: f64,
            allowance: u64,
        ) -> i32;

        /// "Clear Shadows": the shadow frames in `frames` (each at least
        /// `half`, the first shadow frame) become transparent, recorded like
        /// `shp_generate_shadows`. The frames cleared, or -1.
        fn shp_clear_shadows(view: Pin<&mut PictureView>, half: i32, frames: &[i32]) -> i32;

        /// The house-color preview's colors for the remap indices
        /// `first..=last`: `rgb` (`0xRRGGBB`) shaded like the palette's own
        /// entries there, as `0xFFRRGGBB`. Approximate: not the game's own
        /// formula. Empty for a document that is not an SHP.
        fn shp_house_ramp(view: &PictureView, rgb: u32, first: i32, last: i32) -> Vec<u32>;

        /// File frame `frame`'s radar color, in the file and as the average
        /// of its pixels now.
        fn shp_radar(view: &PictureView, frame: i32) -> ShpRadar;

        /// Recompute every drawn frame's radar color (the average of its
        /// pixels in the palette as read) on the next SHP save on this
        /// thread; that save takes the request. Off by default: Save keeps
        /// the radar colors the file has.
        fn shp_set_save_radar(on: bool);

        /// Test seam: the size limit and available memory in bytes (0 keeps
        /// the real value).
        fn shp_set_test_limits(limit: u64, available: u64);

        /// True when the file at `path` is a MIX archive.
        fn mix_sniff_file(path: &QString) -> bool;

        /// Every entry of the MIX at `path` and of the archives nested in
        /// it, depth first, with names and kinds. Reads the index and the
        /// first bytes of each entry, never a whole entry but INI files
        /// that name others.
        fn mix_list(path: &QString) -> MixListing;

        /// Copy the entry `chain` names out of the MIX at `path` into the
        /// file `out`; false with the reason in `shp_last_error`.
        fn mix_extract(path: &QString, chain: &[u32], out: &QString) -> bool;

        /// Write a copy of the MIX at `path` to `out` with the entry `chain`
        /// names replaced by the file `entry` (the archives on the chain
        /// unencrypted; everything else byte for byte), through a partial
        /// file renamed when complete. False with the reason.
        fn mix_replace(path: &QString, chain: &[u32], entry: &QString, out: &QString) -> bool;

        /// The member path (`ra2.mix/cache.mix/unittem.pal`) of the first
        /// entry called `name` in the MIX at `path` or a named archive
        /// nested in it, shallowest first; empty when there is none.
        fn mix_find(path: &QString, name: &QString) -> QString;

        /// The member path of the entry ending in `suffix` whose stem is the
        /// longest one starting `stem`; empty when there is none.
        fn mix_find_prefix(path: &QString, stem: &QString, suffix: &QString) -> QString;

        /// A file, or an entry inside a MIX, exists at `path`.
        fn mix_path_exists(path: &QString) -> bool;

        /// The measured real footprint per byte of sheet.
        fn shp_footprint_factor() -> u64;

        /// The SHP size limit in bytes of sheet.
        fn shp_size_limit() -> u64;
    }
}
