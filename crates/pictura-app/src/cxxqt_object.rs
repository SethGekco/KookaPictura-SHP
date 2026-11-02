//! The cxx-qt bridge: a Rust `QObject` that owns the image shown by the shell.
#![allow(clippy::too_many_arguments)] // brush parameter lists mirror the C++ API
mod actions_engine;
mod adjustment_edit;
mod align;
mod annotations;
mod auto_blend;
mod channels;
mod clipboard;
mod clipping;
mod color_management;
mod color_range;
mod command_catalogue;
mod crop_group;
mod display_preview;
mod document_session;
mod edit;
mod engine_tables;
mod export;
mod eyedropper_sample;
mod filter_map;
mod filter_tools;
mod guides;
mod hdr_conversion;
mod healing;
mod helpers;
mod helpers_composite;
mod image_adjust;
mod image_hdr_toning;
mod image_match_color;
mod image_replace_color;
mod impl_core;
mod impl_filters;
mod impl_history;
mod impl_layers;
mod impl_layers_create;
mod impl_layers_merge;
mod impl_layers_rasterize;
mod impl_layers_select;
mod impl_layers_smart_object;
mod impl_paint;
mod impl_pictura_raw;
mod impl_selection;
mod impl_transform;
mod layer_arrange;
mod layer_path;
mod layer_style;
mod layer_target_bridge;
mod layers_smart_filters;
mod magnetic;
mod metrics;
mod open_jobs;
mod paint_tools;
mod path_list;
mod paths;
mod pdf_export;
mod picture;
mod save_format;
mod selection_move;
mod shapes;
mod shp;
mod state;
mod svg_export;
mod type_tools;
mod web_export;
pub use state::PictureViewRust;
#[cfg(test)]
mod canvas_view_test;
#[cfg(test)]
mod damage_policy_tests;
#[cfg(test)]
mod tests;
#[cfg(test)]
mod tests_auto_select;
#[cfg(test)]
mod tests_impl;
#[cfg(test)]
mod tests_profiles;
#[cfg(test)]
mod tool_recording_tests;
#[cxx_qt::bridge]
pub mod qobject {
    #[rustfmt::skip]
    unsafe extern "C++" {
        include!("cxx-qt-lib/qstring.h");
        type QString = cxx_qt_lib::QString;
        include!("cxx-qt-lib/qimage.h");
        type QImage = cxx_qt_lib::QImage;
        include!("cxx-qt-lib/qstringlist.h");
        type QStringList = cxx_qt_lib::QStringList;
        include!("decode_image.h");
        /// Decode `data` with Qt to tightly packed RGBA8888 and report the decoded size; empty with zero dimensions when Qt cannot read it.
        fn decode_image_rgba(data: &[u8], width: &mut i32, height: &mut i32) -> Vec<u8>;
    }
    #[namespace = "pictura"]
    struct LayerRow {
        path: QString,
        depth: i32,
        name: QString,
        kind: QString,
        visible: bool,
        blend: QString,
        opacity: i32,
        fill: i32,
        lock: i32,
        color: i32,
        clipping: bool,
        has_mask: bool,
        mask_linked: bool,
        mask_disabled: bool,
        has_vector_mask: bool,
        vector_mask_linked: bool,
        vector_mask_disabled: bool,
        has_adjustment: bool,
        adjustment_kind: QString,
        expandable: bool,
        child_count: i32,
        linked: bool,
        placed: bool,
        shape: bool,
        smart_object: bool,
        has_style: bool,
        /// Present effect names joined by `\n`; the adapter splits them back.
        style_effects: QString,
        /// The enabled subset of `style_effects`, joined by `\n`.
        style_effects_on: QString,
        /// The layer's master effect switch (`fx.visible`); true when absent.
        style_visible: bool,
        has_blend_if: bool,
        thumbnail: QImage,
        mask_thumbnail: QImage,
        vector_mask_thumbnail: QImage,
    }
    extern "RustQt" {
        #[qobject]
        #[namespace = "pictura"]
        type PictureView = super::PictureViewRust;
        /// Emitted whenever the layer stack changes and the image is refreshed.
        #[qsignal]
        fn changed(self: Pin<&mut Self>);
        /// Emitted after a region composite. The receiver blits `region` at `(x, y)`; the full image was not rebuilt.
        #[qsignal]
        #[cxx_name = "regionBlitted"]
        fn region_blitted(self: Pin<&mut Self>, region: QImage, x: i32, y: i32);
        /// Emitted after a document region composite. The receiver patches its present crop over `(x, y, w, h)` and repaints; no region pixels cross the bridge.
        #[qsignal]
        #[cxx_name = "regionDamaged"]
        fn region_damaged(self: Pin<&mut Self>, x: i32, y: i32, w: i32, h: i32);
        /// Emitted after a document replacement, or a history restore that cancelled a live session; handlers drop their local gesture state.
        #[qsignal]
        #[cxx_name = "sessionCancelled"]
        fn session_cancelled(self: Pin<&mut Self>);
        /// Emitted after a history restore that cancelled no live session. Handlers drop local gesture state; `changed` has already scheduled the panels.
        #[qsignal]
        #[cxx_name = "historyRestored"]
        fn history_restored(self: Pin<&mut Self>);
        /// Emitted on the GUI thread when a started document open ends. `status` is `ok`, a PDF probe status, or `error:<message>`; `warnings` are the renderer's recoverable problems (PDF/EPS imports).
        #[qsignal]
        #[cxx_name = "openFinished"]
        fn open_finished(self: Pin<&mut Self>, status: QString, warnings: QStringList);
        /// Emitted on the GUI thread when a PDF probe found editing data, before its PSD decode: the frame creates the loading tab here.
        #[qsignal]
        #[cxx_name = "openDetected"]
        fn open_detected(self: Pin<&mut Self>);
        /// Emitted on the GUI thread at an open phase boundary; `percent` is a phase weight, not measured work.
        #[qsignal]
        #[cxx_name = "openProgress"]
        fn open_progress(self: Pin<&mut Self>, percent: i32, phase: QString);
        /// Try to load a PSD through `pictura-codec`. Returns `false` and falls back to a generated test image when the file is missing or unsupported.
        #[qinvokable]
        fn open(self: Pin<&mut Self>, path: &QString) -> bool;
        /// `File > Open` for a common raster image: decode `path` into an RGB/8-bit document recording that path; `false` without mutating on any refusal.
        #[qinvokable]
        fn open_image(self: Pin<&mut Self>, path: &QString) -> bool;
        /// `File > Open As Smart Object…`: open `path` as a new untitled document holding that PSD/PSB source as an embedded smart object; one state.
        #[qinvokable]
        fn open_as_smart_object(self: Pin<&mut Self>, path: &QString) -> bool;
        /// Create a new `width`×`height` document. `mode` is `"rgb"`/`"grayscale"`, `depth` must be 8, `background` is `"white"`/`"transparent"`. Resets selection/history; false without mutating on any invalid parameter.
        #[qinvokable]
        fn new_document(
            self: Pin<&mut Self>,
            width: i32,
            height: i32,
            mode: &QString,
            depth: i32,
            background: &QString,
        ) -> bool;
        /// Serialize the document to `path` in the format named by the path's extension: PSD/PSB through the codec, WebP through the engine encoder, every other raster format through the Qt encode edge. Clears the dirty flag; false on any encode/IO error.
        #[qinvokable]
        fn save(self: Pin<&mut Self>, path: &QString) -> bool;
        /// Whether the document has unsaved changes (false when none).
        #[qinvokable]
        fn is_dirty(&self) -> bool;
        /// Path the document was last opened from or saved to; empty when untitled.
        #[qinvokable]
        fn file_path(&self) -> QString;
        /// Set the panel's active layer path; an empty path disables tool edits.
        #[qinvokable]
        fn set_active_layer(self: Pin<&mut Self>, path: &QString);
        /// The active layer's panel path, or empty when none is active.
        #[qinvokable]
        fn active_layer_path(&self) -> QString;
        /// Whether the single active layer a tool edit may target is visible.
        #[qinvokable]
        fn active_layer_visible(&self) -> bool;
        /// Record the Layers panel's isolated set: while the Selected filter is on, `paths` are the panel paths Move Auto-Select may pick. `active` false clears the scope.
        #[qinvokable]
        fn isolation_set(self: Pin<&mut Self>, paths: &QStringList, active: bool);
        /// Whether the Layers panel's Selected filter is isolating a set.
        #[qinvokable]
        fn isolation_active(&self) -> bool;
        /// Post-open notices (mode, depth, ICC), `"; "`-joined; empty when none.
        #[qinvokable]
        fn open_notice(&self) -> QString;

        /// Assign (retag) or convert (transform and retag) the active document to a built-in profile: 0 sRGB, 1 Adobe RGB, 2 Pro Photo RGB. False without a document or on a bad index.
        #[qinvokable]
        fn assign_profile(self: Pin<&mut Self>, profile_index: i32) -> bool;
        #[qinvokable]
        fn convert_profile(self: Pin<&mut Self>, profile_index: i32) -> bool;

        /// File Info rows: EXIF, parsed XMP properties, IPTC-IIM, and the raw XMP packet.
        #[qinvokable]
        fn exif_rows(&self) -> QStringList;
        #[qinvokable]
        fn iptc_rows(&self) -> QStringList;
        #[qinvokable]
        fn xmp_packet(&self) -> QString;
        #[qinvokable]
        fn xmp_rows(&self) -> QStringList;
        #[qinvokable]
        fn iptc_edit_fields(&self) -> QStringList;
        #[qinvokable]
        fn apply_metadata_edits(self: Pin<&mut Self>, edits: &QStringList) -> bool;

        /// Export the active document's managed metadata to `dest` as a standalone XMP template; records no history.
        #[qinvokable]
        fn export_metadata_template(&self, dest: &QString) -> bool;

        /// Apply the XMP template at `path` with `mode` (0 Append, 1 Replace, 2 KeepOriginalReplaceMatching) as one state.
        #[qinvokable]
        fn apply_metadata_template(self: Pin<&mut Self>, path: &QString, mode: i32) -> bool;

        /// Working mode: `"rgb"` or `"grayscale"`; empty without a document.
        #[qinvokable]
        fn document_mode(&self) -> QString;

        /// Bit depth per channel (8/16/32/1) of the loaded document, or 0 when none.
        #[qinvokable]
        fn document_depth_bits(&self) -> i32;

        /// The image to display: cached when clean, else rebuilt.
        #[qinvokable]
        fn image(self: Pin<&mut Self>) -> QImage;

        /// A premultiplied crop of view-pyramid `level` at `(x, y, w, h)`, or an empty image for an out-of-range level or rectangle.
        #[qinvokable]
        fn display_image(&self, level: i32, x: i32, y: i32, w: i32, h: i32) -> QImage;

        /// The **canvas display source** crop at `(x, y, w, h)`: the solo mask plane while a mask is soloed, else the composite. The canvas provider uses this; other panels keep `display_image`.
        #[qinvokable]
        fn canvas_display_image(&self, level: i32, x: i32, y: i32, w: i32, h: i32) -> QImage;

        /// A zero-copy view of stored pyramid `level` (premultiplied RGBA8888), or an empty image for level 0 or past the last level. The image keeps the level's storage alive.
        #[qinvokable]
        fn canvas_level_image(&self, level: i32) -> QImage;

        /// Extents of every view-pyramid level as flat `w,h` pairs; empty without a document.
        #[qinvokable]
        fn display_level_extents(&self) -> Vec<i32>;

        /// The outstanding canvas damage as `"x y w h"`, or empty when clean.
        #[qinvokable]
        fn take_canvas_damage(self: Pin<&mut Self>) -> QString;
        /// Cancel the active edit session and present its restoration.
        #[qinvokable]
        fn cancel_active_session(self: Pin<&mut Self>) -> bool;
        /// Non-consuming revision of the displayed canvas pixels; bumped when the image or pyramid changes.
        #[qinvokable]
        fn canvas_revision(&self) -> u64;
        #[qinvokable]
        fn set_presented_view(self: Pin<&mut Self>, level: i32, x: i32, y: i32, w: i32, h: i32);
        #[qinvokable]
        fn document_generation(&self) -> u64; // document load/replace epoch
        /// Whether a document is loaded; drives shell command enablement.
        #[qinvokable]
        fn has_document(&self) -> bool;
        /// Width of the loaded document in pixels (0 when none).
        #[qinvokable]
        fn document_width(&self) -> i32;
        /// Height of the loaded document in pixels (0 when none).
        #[qinvokable]
        fn document_height(&self) -> i32;
        /// Number of top-level layers in the loaded document (0 when none).
        #[qinvokable]
        fn layer_count(&self) -> i32;
        /// Index of the topmost pixel layer (neither a group nor an adjustment), or -1 when there is none.
        #[qinvokable]
        fn topmost_pixel_layer_index(&self) -> i32;
        /// Name of layer `i`, or empty when out of range.
        #[qinvokable]
        fn layer_name(&self, i: i32) -> QString;

        /// `"pixel"`/`"group"`/`"adjustment"`/`"background"` for layer `i`, or empty.
        #[qinvokable]
        fn layer_kind(&self, i: i32) -> QString;

        /// Visibility flag of layer `i` (false when out of range).
        #[qinvokable]
        fn layer_visible(&self, i: i32) -> bool;

        /// Blend mode of layer `i` as its 4-byte PSD key (e.g. `"mul "`), or empty when out of range.
        #[qinvokable]
        fn layer_blend(&self, i: i32) -> QString;

        /// Set layer `i`'s blend mode from a 4-byte PSD `key`; false on an unknown key.
        #[qinvokable]
        fn set_layer_blend(self: Pin<&mut Self>, i: i32, key: &QString) -> bool;

        /// Opacity of layer `i` in `0..=255`, or 0 when out of range.
        #[qinvokable]
        fn layer_opacity(&self, i: i32) -> i32;

        /// Set layer `i`'s opacity, clamped to `0..=255`; refused for the Background.
        #[qinvokable]
        fn set_layer_opacity(self: Pin<&mut Self>, i: i32, value: i32) -> bool;

        /// Fill opacity of layer `i` in `0..=255` (distinct from Opacity).
        #[qinvokable]
        fn layer_fill(&self, i: i32) -> i32;

        /// Set layer `i`'s fill, clamped to `0..=255`; refused for a group or Background.
        #[qinvokable]
        fn set_layer_fill(self: Pin<&mut Self>, i: i32, value: i32) -> bool;

        /// Lock flags of layer `i` as a bitmask (`0x01` transparency, `0x02` pixels, `0x04` position), or 0 when out of range.
        #[qinvokable]
        fn layer_lock(&self, i: i32) -> i32;

        /// Set one lock flag of layer `i` (`transparency`/`pixels`/`position`/`all`).
        #[qinvokable]
        fn set_layer_lock(self: Pin<&mut Self>, i: i32, flag: &QString, on: bool) -> bool;

        /// Color label of layer `i` as a byte (`0` none … `7` gray), or 0.
        #[qinvokable]
        fn layer_color(&self, i: i32) -> i32;

        /// Set layer `i`'s color label (`0..=7`); refused for the Background.
        #[qinvokable]
        fn set_layer_color(self: Pin<&mut Self>, i: i32, value: i32) -> bool;

        /// Rename layer `i`. Captures history, marks dirty, recomposites, and emits [`changed`]. Returns false when layer `i` is out of range.
        #[qinvokable]
        fn set_layer_name(self: Pin<&mut Self>, i: i32, name: &QString) -> bool;

        /// Swap layer `i` with the neighbour `delta` positions away; one undo state.
        #[qinvokable]
        fn move_layer(self: Pin<&mut Self>, i: i32, delta: i32) -> bool;

        /// Insert a new transparent raster layer above `above`; returns the new index.
        #[qinvokable]
        fn add_layer(self: Pin<&mut Self>, above: i32) -> i32;

        /// Insert an empty group above `above`; returns the new index, or -1.
        #[qinvokable]
        fn add_group(self: Pin<&mut Self>, above: i32) -> i32;

        /// Deep-clone layer `index` above itself as `"<name> copy"`; new index or -1.
        #[qinvokable]
        fn duplicate_layer(self: Pin<&mut Self>, index: i32) -> i32;

        /// Wrap layer `index` in a new group in place; returns the group's index.
        #[qinvokable]
        fn group_layer(self: Pin<&mut Self>, index: i32) -> i32;

        /// Splice group `index`'s children into its parent position; false when not a group.
        #[qinvokable]
        fn ungroup_layer(self: Pin<&mut Self>, index: i32) -> bool;

        /// The RGBA content of layer `i` scaled to fit `size`×`size` keeping the aspect ratio; null for groups/adjustments or an out-of-range argument.
        #[qinvokable]
        fn layer_thumbnail(&self, i: i32, size: i32) -> QImage;

        /// One crossing for every Layers-panel row; empty without a document.
        #[qinvokable]
        fn layers_snapshot(&self, thumb_size: i32, entire_document: bool) -> Vec<LayerRow>;

        // M39 tree rows, path/batch mutation; see docs/dev/m39-panel-anatomy.md §3.2.

        /// `Prefix N` for the next free name in the whole tree.
        #[qinvokable]
        fn next_layer_name(&self, prefix: &QString) -> QString;

        /// Rename the node at `path` (one undo state); false when unresolved.
        #[qinvokable]
        fn set_layer_name_path(self: Pin<&mut Self>, path: &QString, name: &QString) -> bool;

        /// Move the node at `path` `delta` places within its container; one undo state.
        #[qinvokable]
        fn move_layer_path(self: Pin<&mut Self>, path: &QString, delta: i32) -> bool;

        /// Move node `path` next to (or into) `target`: 0 above, 1 below, 2 into a
        /// group. Refuses the Background, locked sources, and a bad drop target.
        #[qinvokable]
        fn move_layer_to(self: Pin<&mut Self>, path: &QString, target: &QString, mode: i32)
            -> bool;

        /// Dry-run of [`move_layer_to`]: whether the move would be accepted; no history.
        #[qinvokable]
        fn can_move_layer_to(&self, path: &QString, target: &QString, mode: i32) -> bool;

        /// Set visibility on every path; returns the number changed (one state if non-zero).
        #[qinvokable]
        fn set_layers_visible(self: Pin<&mut Self>, paths: &QStringList, visible: bool) -> i32;

        /// Set the blend mode from a 4-byte PSD `key` on every path. Returns the number changed; records one undo state only when non-zero.
        #[qinvokable]
        fn set_layers_blend(self: Pin<&mut Self>, paths: &QStringList, key: &QString) -> i32;

        /// Set opacity (clamped `0..=255`) on every path. Returns the number changed; records one undo state only when non-zero.
        #[qinvokable]
        fn set_layers_opacity(self: Pin<&mut Self>, paths: &QStringList, value: i32) -> i32;

        /// Set fill opacity (clamped `0..=255`) on every path. Returns the number changed; records one undo state only when non-zero.
        #[qinvokable]
        fn set_layers_fill(self: Pin<&mut Self>, paths: &QStringList, value: i32) -> i32;

        /// Preview opacity (clamped `0..=255`) on every path without recording history. Returns the number changed.
        #[qinvokable]
        fn preview_layers_opacity(self: Pin<&mut Self>, paths: &QStringList, value: i32) -> i32;

        /// Commit opacity: apply and record one undo state when the value changed or a preview ran. Returns the number changed.
        #[qinvokable]
        fn commit_layers_opacity(self: Pin<&mut Self>, paths: &QStringList, value: i32) -> i32;

        /// Preview fill opacity (clamped `0..=255`) on every path without recording history. Returns the number changed.
        #[qinvokable]
        fn preview_layers_fill(self: Pin<&mut Self>, paths: &QStringList, value: i32) -> i32;

        /// Commit fill opacity: apply and record one undo state when the value
        /// changed or a preview ran. Returns the number changed.
        #[qinvokable]
        fn commit_layers_fill(self: Pin<&mut Self>, paths: &QStringList, value: i32) -> i32;

        /// Set one lock flag on every path. Returns the number changed; records one state.
        #[qinvokable]
        fn set_layers_lock(
            self: Pin<&mut Self>,
            paths: &QStringList,
            flag: &QString,
            on: bool,
        ) -> i32;

        /// Set the color label on every path. Returns the number changed; records one undo state only when non-zero.
        #[qinvokable]
        fn set_layers_color(self: Pin<&mut Self>, paths: &QStringList, value: i32) -> i32;

        /// Solo visibility: set exactly `paths` visible and every other node invisible; records one state under `label`.
        #[qinvokable]
        fn apply_visibility(self: Pin<&mut Self>, paths: &QStringList, label: &QString) -> i32;

        /// Delete every eligible path. Returns the number deleted; records one undo state only when non-zero.
        #[qinvokable]
        fn delete_layers(self: Pin<&mut Self>, paths: &QStringList) -> i32;

        /// Deep-copy every path above itself; records one state when a copy is created.
        #[qinvokable]
        fn duplicate_layers(self: Pin<&mut Self>, paths: &QStringList) -> QStringList;

        /// Wrap the selection in one new group at the topmost selected position, returning its path or empty on refusal.
        #[qinvokable]
        fn group_layers(self: Pin<&mut Self>, paths: &QStringList) -> QString;

        /// Splice each selected group's children in place. Returns the number changed.
        #[qinvokable]
        fn ungroup_layers(self: Pin<&mut Self>, paths: &QStringList) -> i32;

        /// Insert a new transparent raster layer inside `selection_path` when it
        /// is a group else above it, returning the new path or empty.
        #[qinvokable]
        fn add_layer_in(self: Pin<&mut Self>, selection_path: &QString) -> QString;

        /// Insert a new empty group by the [`add_layer_in`] rule, returning the new path or empty.
        #[qinvokable]
        fn add_group_in(self: Pin<&mut Self>, selection_path: &QString) -> QString;

        /// Create a raster layer from the New Layer dialog directly above
        /// `selection_path` (top when empty) with the chosen name/color/blend/
        /// opacity/fill, optional neutral fill, and clipping flag. `blend_key` is
        /// a 4-byte PSD key; records one "New Layer" state. Returns the new path.
        #[qinvokable]
        fn new_layer_dialog(
            self: Pin<&mut Self>,
            selection_path: &QString,
            name: &QString,
            color: i32,
            blend_key: &QString,
            opacity: i32,
            fill: i32,
            neutral_fill: bool,
            clipping: bool,
        ) -> QString;

        /// Create an empty group from the New Group dialog, carrying
        /// name/color/blend/opacity. Records one "New Group" state.
        #[qinvokable]
        fn new_group_dialog(
            self: Pin<&mut Self>,
            selection_path: &QString,
            name: &QString,
            color: i32,
            blend_key: &QString,
            opacity: i32,
        ) -> QString;

        /// Wrap `paths` in one group carrying the dialog's name/color/blend/
        /// opacity. Records one "Group from Layers" state.
        #[qinvokable]
        fn group_from_layers_dialog(
            self: Pin<&mut Self>,
            paths: &QStringList,
            name: &QString,
            color: i32,
            blend_key: &QString,
            opacity: i32,
        ) -> QString;

        /// Whether Merge Down / Merge Layers would apply to `paths`. Read-only.
        #[qinvokable]
        fn can_merge_layers(&self, paths: &QStringList) -> bool;

        /// Whether Merge Clipping Mask would apply to `path`. Read-only.
        #[qinvokable]
        fn can_merge_clipping_mask(&self, path: &QString) -> bool;

        /// Merge Down / Merge Layers for `paths`; returns the merged path, empty on refusal.
        #[qinvokable]
        fn merge_layers(self: Pin<&mut Self>, paths: &QStringList) -> QString;

        /// Merge every eye-visible layer anchored on `path`; returns the merged path, empty on refusal.
        #[qinvokable]
        fn merge_visible(self: Pin<&mut Self>, path: &QString) -> QString;

        /// Collapse the clipping group above `path`; records one
        /// "Merge Clipping Mask" state. Returns replaced, 0 on refusal.
        #[qinvokable]
        fn merge_clipping_mask(self: Pin<&mut Self>, path: &QString) -> i32;

        /// Flatten the tree into one opaque Background layer; records one
        /// "Flatten Image" state. Returns replaced, 0 on refusal.
        #[qinvokable]
        fn flatten_image(self: Pin<&mut Self>) -> i32;

        /// `Layer from Background…`: clear the flag and unlock `path`. Records
        /// one "Layer from Background" state; false when not the Background.
        #[qinvokable]
        fn layer_from_background(self: Pin<&mut Self>, path: &QString) -> bool;

        /// `Layer from Background…` with the dialog's name/color: clear the
        /// flag, unlock, rename, and set the color. Records one state.
        #[qinvokable]
        fn convert_background(
            self: Pin<&mut Self>,
            path: &QString,
            name: &QString,
            color: i32,
        ) -> bool;

        /// `Background From Layer`: flag `path`, fill transparency with the
        /// background color, and move it to the bottom. Records one state;
        /// false for a group, adjustment, or existing Background.
        #[qinvokable]
        fn background_from_layer(self: Pin<&mut Self>, path: &QString) -> bool;

        /// `Layer via Copy` (`Ctrl+J`): copy the selection's pixels from `path`
        /// into a new layer above it; records one "Layer via Copy" state.
        #[qinvokable]
        fn layer_via_copy(self: Pin<&mut Self>, path: &QString) -> QString;

        /// `Layer via Cut` (`Shift+Ctrl+J`): as [`layer_via_copy`], clearing the
        /// selected pixels from the source; records one "Layer via Cut" state.
        #[qinvokable]
        fn layer_via_cut(self: Pin<&mut Self>, path: &QString) -> QString;

        /// The layer and every layer matching its kind and attributes; sets the target.
        #[qinvokable]
        fn select_similar(self: Pin<&mut Self>, path: &QString) -> QStringList;

        /// Every member of the link set containing `path`; empty when unlinked; a linked set becomes the target.
        #[qinvokable]
        fn select_linked(self: Pin<&mut Self>, path: &QString) -> QStringList;

        /// Link (`on`) or unlink the listed layers, joining every set they touch.
        #[qinvokable]
        fn link_layers(self: Pin<&mut Self>, paths: &QStringList, on: bool) -> i32;
        /// Whether every listed layer resolves to one shared link set.
        #[qinvokable]
        fn linked_together(&self, paths: &QStringList) -> bool;

        /// Hide the listed layers; records one "Hide Layers" state.
        #[qinvokable]
        fn hide_layers(self: Pin<&mut Self>, paths: &QStringList) -> i32;

        /// Delete every effectively hidden layer; records one "Delete Hidden Layers" state when any is removed.
        #[qinvokable]
        fn delete_hidden_layers(self: Pin<&mut Self>) -> i32;

        /// Append a solid-color fill layer for `rgba` (`0xAARRGGBB`); records "Color Fill".
        #[qinvokable]
        fn add_solid_fill(self: Pin<&mut Self>, rgba: u32) -> QString;

        /// Append a black-to-white Linear gradient fill layer; records "Gradient Fill".
        #[qinvokable]
        fn add_gradient_fill(self: Pin<&mut Self>) -> QString;

        /// Whether the layer at `path` is a decodable solid-color fill layer.
        #[qinvokable]
        fn layer_is_fill_content(&self, path: &QString) -> bool;

        /// `Rasterize Fill Content`: bake `path`'s fill into pixels.
        #[qinvokable]
        fn rasterize_fill_content(self: Pin<&mut Self>, path: &QString) -> bool;

        /// `Rasterize Layer` on a fill-content or type layer.
        #[qinvokable]
        fn rasterize_layer(self: Pin<&mut Self>, path: &QString) -> bool;

        /// `Rasterize Type`: materialize the type layer at `path`.
        #[qinvokable]
        fn rasterize_type(self: Pin<&mut Self>, path: &QString) -> bool;

        /// Whether the layer at `path` is a type layer. Read-only.
        #[qinvokable]
        fn layer_is_type(&self, path: &QString) -> bool;

        /// `Rasterize All Layers`: bake every fill-content or type layer.
        #[qinvokable]
        fn rasterize_all_layers(self: Pin<&mut Self>) -> i32;

        /// Whether `path` is a convertible raster pixel layer (not a group,
        /// adjustment, Background, or smart layer; positive rect). Read-only.
        #[qinvokable]
        fn layer_can_convert_to_smart_object(&self, path: &QString) -> bool;

        /// `Convert to Smart Object`: author an embedded source from `path`'s
        /// raster and attach it, keeping the raster proxy. Records one state on
        /// success; false (no state) for an ineligible target.
        #[qinvokable]
        fn convert_to_smart_object(self: Pin<&mut Self>, path: &QString) -> bool;

        /// `File > Place…`: read `file_path`, decode it as a PSD/PSB source, and
        /// append it as a topmost channel-less embedded smart-object layer.
        /// Records one "Place" state; empty and no state on an unreadable file.
        #[qinvokable]
        fn place_smart_object(self: Pin<&mut Self>, file_path: &QString) -> QString;

        /// `File > Place…` for a raster image: decode `file_path` with Qt, append
        /// a native-size raster layer, convert it to an embedded smart object,
        /// recomposite, and record one "Place" state. Empty on refusal.
        #[qinvokable]
        fn place_image(self: Pin<&mut Self>, file_path: &QString) -> QString;

        /// Whether `path` resolves to a rasterizable smart-object layer: not a
        /// group, no adjustment data, and a typed smart object. Read-only.
        #[qinvokable]
        fn layer_can_rasterize_smart_object(&self, path: &QString) -> bool;

        /// `Rasterize Smart Object`: consume the object at `path`, keeping its
        /// raster proxy or materializing the source into channels, then drop the
        /// preserved blocks and linked record. Records one state; false (no
        /// state) for an ineligible target or an undecodable payload.
        #[qinvokable]
        fn rasterize_smart_object(self: Pin<&mut Self>, path: &QString) -> bool;

        /// Whether `path` resolves to a replaceable smart-object layer: not a
        /// group, no adjustment data, and an embedded object with a payload.
        /// Read-only; mutates nothing.
        #[qinvokable]
        fn layer_can_replace_smart_object_contents(&self, path: &QString) -> bool;

        /// `Replace Contents…`: read `file_path`, swap the embedded source of
        /// the smart object at `path`, and keep the layer's transform. Records
        /// one "Replace Contents" state on success; false (no state) when the
        /// file is unreadable, not a PSD/PSB, or the target is ineligible.
        #[qinvokable]
        fn replace_smart_object_contents(
            self: Pin<&mut Self>,
            path: &QString,
            file_path: &QString,
        ) -> bool;

        /// `Edit Contents`: apply the edited `file_path` as the embedded source
        /// of the smart object at `path`, sharing `replace_smart_object_contents`'s
        /// engine path. Records one "Edit Contents" state on success; false (no
        /// state) on the same refusals.
        #[qinvokable]
        fn commit_smart_object_edit(
            self: Pin<&mut Self>,
            path: &QString,
            file_path: &QString,
        ) -> bool;

        /// Whether `path` resolves to a smart-object layer whose embedded
        /// payload parses as a PSD/PSB document, i.e. it can be opened as an
        /// in-app editor. Read-only; mutates nothing.
        #[qinvokable]
        fn layer_can_edit_smart_object_contents(&self, path: &QString) -> bool;

        /// Self-test probe: `<kind>:<payload-len>` for `path`'s smart object,
        /// or empty when the layer has none. Read-only.
        #[qinvokable]
        fn layer_smart_object_state(&self, path: &QString) -> QString;

        /// Whether `path` resolves to a layer carrying a non-empty embedded
        /// payload to export. Read-only; mutates nothing.
        #[qinvokable]
        fn layer_can_export_smart_object_contents(&self, path: &QString) -> bool;

        /// `Export Contents…`: write the embedded source of the smart object at
        /// `path` to `dest` byte-for-byte. Read-only, so it records no history
        /// state; true only when the write succeeds.
        #[qinvokable]
        fn export_smart_object_contents(&self, path: &QString, dest: &QString) -> bool;

        /// The full Pictura Raw settings attached to `path` in the bridge text
        /// form, or empty when the layer has no Pictura Raw smart filter.
        #[qinvokable]
        fn layer_pictura_raw_settings(&self, path: &QString) -> QString;

        /// Whether `Filter > Pictura Raw…` can act on `path`. Read-only.
        #[qinvokable]
        fn layer_can_apply_pictura_raw(&self, path: &QString) -> bool;

        /// `Filter > Pictura Raw…`: apply `settings_text` to `path`, one state.
        #[qinvokable]
        fn apply_pictura_raw_filter(
            self: Pin<&mut Self>,
            path: &QString,
            settings_text: &QString,
        ) -> bool;

        /// A downscaled preview of the layer at `path` rendered with
        /// `settings_text`, for the dialog's live preview. Read-only; a null
        /// image for an ineligible target.
        #[qinvokable]
        fn pictura_raw_preview(
            &self,
            path: &QString,
            settings_text: &QString,
            max_dim: i32,
        ) -> QImage;

        /// Set layer `i` visibility, recomposite, and emit [`changed`].
        #[qinvokable]
        fn set_layer_visible(self: Pin<&mut Self>, i: i32, visible: bool);

        /// Select the whole document, recomposite, and emit [`changed`].
        #[qinvokable]
        fn select_all(self: Pin<&mut Self>);

        /// Clear the active selection, recomposite, and emit [`changed`].
        #[qinvokable]
        fn deselect(self: Pin<&mut Self>);

        /// Flood-select around `(x, y)` within `tolerance`, combining per `mode`.
        #[qinvokable]
        fn magic_wand(
            self: Pin<&mut Self>,
            x: i32,
            y: i32,
            tolerance: i32,
            contiguous: bool,
            mode: &QString,
        ) -> bool;

        /// Build a document-sized selection from the alpha of the layer at
        /// `path`; missing alpha is opaque. Replaces the selection, one undo state.
        #[qinvokable]
        fn select_layer_alpha(self: Pin<&mut Self>, path: &QString) -> bool;

        /// Load the raster mask at `path` as a selection, combined per `mode`; one undo state.
        #[qinvokable]
        fn mask_selection(self: Pin<&mut Self>, path: &QString, mode: &QString) -> bool;

        /// Whether a selection is currently active.
        #[qinvokable]
        fn has_selection(&self) -> bool;

        /// Number of pixels with non-zero selection coverage (0 when none).
        #[qinvokable]
        fn selection_count(&self) -> i32;

        /// Selection coverage byte (0-255) at `(x, y)`; 0 when none/out of bounds.
        #[qinvokable]
        fn selection_coverage(&self, x: i32, y: i32) -> i32;

        /// Restore the most recently deselected selection; one undo state.
        #[qinvokable]
        fn reselect(self: Pin<&mut Self>) -> bool;

        /// Whether a deselected selection is stored for [`reselect`].
        #[qinvokable]
        fn has_deselected_selection(&self) -> bool;

        /// Replace the selection with its complement; one undo state.
        #[qinvokable]
        fn invert_selection(self: Pin<&mut Self>) -> bool;

        /// Apply `op` (`border`/`smooth`/`expand`/`contract`/`feather`) to the
        /// selection by `amount`; one undo state on success, none on refusal.
        #[qinvokable]
        fn modify_selection(self: Pin<&mut Self>, op: &QString, amount: f64) -> bool;

        /// Grow the selection to adjacent similar pixels within `tolerance`.
        #[qinvokable]
        fn grow_selection(self: Pin<&mut Self>, tolerance: i32) -> bool;

        /// Add every similar composite pixel within `tolerance`.
        #[qinvokable]
        fn similar_selection(self: Pin<&mut Self>, tolerance: i32) -> bool;

        /// Append the current selection to `doc.channels` as coverage.
        #[qinvokable]
        fn save_selection(self: Pin<&mut Self>, name: &QString) -> bool;

        /// Restore a selection from the named extra channel (`Alpha N`).
        #[qinvokable]
        fn load_selection(self: Pin<&mut Self>, name: &QString) -> bool;

        /// Number of document extra channels (saved selections).
        #[qinvokable]
        fn selection_channel_count(&self) -> i32;

        /// Every layer row path in panel order; read-only, no history.
        #[qinvokable]
        fn select_all_layers(&self) -> QStringList;

        /// Rectangle at `(x, y)`, softened by `feather` px (0-250). `mode` is `"new"`, `"add"`, `"subtract"`, or `"intersect"`; false without a doc.
        #[qinvokable]
        fn select_rect(
            self: Pin<&mut Self>,
            x: i32,
            y: i32,
            w: i32,
            h: i32,
            mode: &QString,
            feather: f64,
        ) -> bool;

        /// Ellipse in the `w`×`h` rect; `feather` px (0-250); `mode` as select_rect.
        #[qinvokable]
        fn select_ellipse(
            self: Pin<&mut Self>,
            x: i32,
            y: i32,
            w: i32,
            h: i32,
            mode: &QString,
            feather: f64,
        ) -> bool;

        /// Start a lasso selection with `mode` as [`select_rect`], clearing pending points.
        #[qinvokable]
        fn begin_lasso(self: Pin<&mut Self>, mode: &QString) -> bool;

        /// Append a point to the pending lasso path.
        #[qinvokable]
        fn lasso_add_point(self: Pin<&mut Self>, x: i32, y: i32);
        /// Fill the pending lasso polygon softened by `feather` px; false with <3 points.
        #[qinvokable]
        fn end_lasso(self: Pin<&mut Self>, feather: f64) -> bool;
        /// Discard any pending lasso path without touching the selection.
        #[qinvokable]
        fn cancel_lasso(self: Pin<&mut Self>);

        /// Crop to the `w`×`h` rect at `(x, y)`, clear the selection, recomposite.
        #[qinvokable]
        fn crop(self: Pin<&mut Self>, x: i32, y: i32, w: i32, h: i32) -> bool;

        /// Move the topmost pixel layer by `(dx, dy)`, recomposite, and emit [`changed`]; false without a pixel layer.
        #[qinvokable]
        fn translate_layer(self: Pin<&mut Self>, dx: i32, dy: i32) -> bool;

        /// Preview-move the topmost pixel layer by `(dx, dy)`: recomposite and emit
        /// [`changed`] without history or dirty. `ponytail:` slow path for the self-test.
        #[qinvokable]
        fn move_preview(self: Pin<&mut Self>, dx: i32, dy: i32) -> bool;

        /// Enter move-preview mode for the topmost raster layer; reuses the cached base when unchanged, false without one.
        #[qinvokable]
        fn begin_move_preview(self: Pin<&mut Self>) -> bool;
        #[qinvokable]
        fn begin_move_duplicate(self: Pin<&mut Self>) -> bool;

        /// Warm the move-preview cache without entering preview mode; true when valid, false without a document or raster layer.
        #[qinvokable]
        fn prepare_move_preview(self: Pin<&mut Self>) -> bool;

        /// Whether the last `begin_move_preview` reused the cached base instead of recomputing it.
        #[qinvokable]
        fn move_preview_cache_hit(&self) -> bool;

        /// The cached base image (layers with the moved layer hidden); null when not previewing.
        #[qinvokable]
        fn move_preview_base(&self) -> QImage;

        /// The moved layer's own image; null when not previewing or for a non-raster layer.
        #[qinvokable]
        fn move_preview_layer(&self) -> QImage;

        /// The moved layer's document-space left edge.
        #[qinvokable]
        fn move_preview_x(&self) -> i32;

        /// The moved layer's document-space top edge.
        #[qinvokable]
        fn move_preview_y(&self) -> i32;

        /// The moved layer's opacity in `0..=255`.
        #[qinvokable]
        fn move_preview_opacity(&self) -> i32;

        /// Leave preview mode and drop the cached images (no document change).
        #[qinvokable]
        fn end_move_preview(self: Pin<&mut Self>);

        /// Apply the real move once: shift the topmost raster layer by `(dx, dy)`,
        /// capture one "Move Layer" history state, mark dirty, refresh the image,
        /// and emit [`changed`]. Uses a single composite. Returns false when there
        /// is no document/raster layer.
        #[qinvokable]
        fn commit_move(self: Pin<&mut Self>, dx: i32, dy: i32) -> bool;

        /// No-argument commit kept for the existing C++ tool and self-test, which
        /// mutate the document through [`move_preview`] before recording. Records
        /// the already-previewed state and recomposites. False without a document.
        #[qinvokable]
        #[cxx_name = "commit_move"]
        fn commit_move_legacy(self: Pin<&mut Self>) -> bool;

        /// The composited pixel at `(x, y)` as `0xAARRGGBB`, or 0 when there is
        /// no document or the point is out of bounds.
        #[qinvokable]
        fn sample_argb(&self, x: i32, y: i32) -> u32;

        /// Self-test probe: the stored document composite's pixel at `(x, y)` as
        /// `0xAARRGGBB`, or 0 when there is no document or out of bounds.
        #[qinvokable]
        fn composite_argb(&self, x: i32, y: i32) -> u32;

        /// The selected pixels' bounding box as flat `[x, y, w, h]`; empty when nothing is selected.
        #[qinvokable]
        fn selection_bounds_extents(&self) -> Vec<i32>;

        /// The marching-ants outline loops encoded as `[n0, x,y, …, n1, …]`
        /// (loop point counts then integer pixel corners); empty when nothing is
        /// selected. View-only: no history, no recomposite.
        #[qinvokable]
        fn selection_contour_extents(&self) -> Vec<i32>;

        #[qinvokable]
        fn selection_revision(&self) -> u64;
        #[qinvokable]
        fn selection_contour_compute_count(&self) -> u64;

        /// Append an adjustment layer for `kind` (invert, posterize, threshold,
        /// brightness-contrast, hue-saturation), recomposite, and emit
        /// [`changed`]. When a selection is active the layer gets a raster mask
        /// from its coverage, so only selected pixels change. Returns false for
        /// an unknown kind or no document.
        #[qinvokable]
        fn add_adjustment(self: Pin<&mut Self>, kind: &QString) -> bool;

        /// Apply a destructive filter `kind` to the topmost pixel layer,
        /// confined by the active selection, then recomposite and emit
        /// [`changed`]. Returns false without a document, for an unknown kind,
        /// or when there is no pixel layer.
        #[qinvokable]
        fn apply_filter(self: Pin<&mut Self>, kind: &QString) -> bool;

        /// Begin a paint stroke. Colours are 0xAARRGGBB; `mode` is "normal" | "dissolve" | "behind" | "clear".
        #[qinvokable]
        fn begin_paint(
            self: Pin<&mut Self>,
            foreground: u32,
            background: u32,
            diameter: i32,
            hardness: i32,
            roundness: i32,
            angle: i32,
            opacity: i32,
            flow: i32,
            spacing: i32,
            mode: &QString,
            aliased: bool,
            auto_erase: bool,
        ) -> bool;

        /// Add a pointer sample to the active stroke; only the dab that opens a frame refreshes, the rest wait for `flush_present`.
        #[qinvokable]
        fn paint_dab(self: Pin<&mut Self>, x: f64, y: f64, pressure: f64) -> bool;
        /// Present the region accumulated since the last in-stroke present, or do nothing when none is pending.
        #[qinvokable]
        fn flush_present(self: Pin<&mut Self>) -> bool;

        /// Commit the active stroke as one history state, superseding any pending present, and mark dirty; false if nothing painted.
        #[qinvokable]
        fn end_paint(self: Pin<&mut Self>) -> bool;

        /// Drop the active stroke without committing; the document is unchanged.
        #[qinvokable]
        fn cancel_paint(self: Pin<&mut Self>);

        /// Whether a paint stroke is currently active.
        #[qinvokable]
        fn is_painting(&self) -> bool;
        /// The view-pyramid level the active stroke presents: the reduced preview level, or 0 when the zoom picks it. A GPU stroke presents the zoom-selected level.
        #[qinvokable]
        fn preview_present_level(&self) -> i32;

        /// Brush size/hardness step for a `[`/`]` key, or 0.
        #[qinvokable]
        fn brush_shortcut_delta(&self, key: i32, scan: u32, shift: bool, paint: bool) -> i32;

        /// Scale the document to `width`×`height`, recomposite, and emit
        /// [`changed`]. False without a document, for an unknown resample, or a
        /// dimension below 1.
        #[qinvokable]
        fn resize_image(self: Pin<&mut Self>, kind: &QString, width: i32, height: i32) -> bool;

        /// Place the document on a `width`×`height` canvas at `anchor`,
        /// recomposite, and emit [`changed`]. False without a document, for an
        /// unknown anchor, or a dimension below 1.
        #[qinvokable]
        fn resize_canvas(self: Pin<&mut Self>, anchor: &QString, width: i32, height: i32) -> bool;

        /// Rotate the document `quarter_turns` quarter turns clockwise (1-3),
        /// recomposite, and emit [`changed`]. False outside 1-3.
        #[qinvokable]
        fn rotate_doc(self: Pin<&mut Self>, quarter_turns: i32) -> bool;

        /// Mirror the document, recomposite, and emit [`changed`].
        #[qinvokable]
        fn flip_doc(self: Pin<&mut Self>, horizontal: bool) -> bool;

        /// Convert a 32-bit document to `bits` (16 or 8) with the HDR
        /// Conversion "Exposure & Gamma" method and record "HDR Conversion".
        #[qinvokable]
        fn convert_depth(self: Pin<&mut Self>, bits: i32, exposure_ev: f64, gamma: f64) -> bool;

        /// Whether `path` resolves to a transformable Free Transform target.
        #[qinvokable]
        fn layer_can_free_transform(&self, path: &QString) -> bool;

        /// Begin a Free Transform session; false for an untransformable target.
        #[qinvokable]
        fn begin_free_transform(self: Pin<&mut Self>, path: &QString) -> bool;

        /// Begin a Skew/Distort/Perspective session; same rules as Free.
        #[qinvokable]
        fn begin_transform_mode(self: Pin<&mut Self>, path: &QString, mode: &QString) -> bool;
        /// Clear the active Free Transform session without touching the document.
        #[qinvokable]
        fn cancel_transform(self: Pin<&mut Self>);

        /// Commit the session in one `"Free Transform"` state; false on identity.
        #[qinvokable]
        fn commit_transform(self: Pin<&mut Self>) -> bool;

        /// Begin a transform drag at `(x, y)`; returns the hit handle or -1.
        #[qinvokable]
        fn transform_press(
            self: Pin<&mut Self>,
            x: f64,
            y: f64,
            zoom: f64,
            shift: bool,
            alt: bool,
        ) -> i32;

        /// Hover hit-test for the transform overlay cursor; 0..=7 scale, 8 rotate, 9 move, -1 outside. Read-only.
        #[qinvokable]
        fn transform_hit_test(&self, x: f64, y: f64, zoom: f64) -> i32;

        /// Update the active drag from `(x, y)`; Shift locks aspect and snaps 15°.
        #[qinvokable]
        fn transform_move(
            self: Pin<&mut Self>,
            x: f64,
            y: f64,
            zoom: f64,
            shift: bool,
            alt: bool,
        ) -> bool;

        /// End the active transform drag, leaving the session open.
        #[qinvokable]
        fn transform_release(self: Pin<&mut Self>) -> bool;

        /// Session probes: active flag, path, live transform values, quad, rect.
        #[qinvokable]
        fn transform_session_active(&self) -> bool;
        #[qinvokable]
        fn transform_session_path(&self) -> QString;
        #[qinvokable]
        fn transform_scale_x(&self) -> f64;
        #[qinvokable]
        fn transform_scale_y(&self) -> f64;
        #[qinvokable]
        fn transform_angle(&self) -> f64;
        #[qinvokable]
        fn transform_dx(&self) -> f64;
        #[qinvokable]
        fn transform_dy(&self) -> f64;
        /// The handle the live transform drag grabbed; -1 when no drag is live.
        #[qinvokable]
        fn transform_drag_handle(&self) -> i32;
        #[qinvokable]
        fn undo(self: Pin<&mut Self>) -> bool;
        #[qinvokable]
        fn redo(self: Pin<&mut Self>) -> bool;
        #[qinvokable]
        fn can_undo(&self) -> bool;
        #[qinvokable]
        fn can_redo(&self) -> bool;
        #[qinvokable]
        fn history_depth(&self) -> i32;
        #[qinvokable]
        fn history_count(&self) -> i32;
        /// Position of the current history state, in `0..history_count()`.
        #[qinvokable]
        fn history_index(&self) -> i32;
        /// Label of history state `i`, or empty when out of range.
        #[qinvokable]
        fn history_label(&self, i: i32) -> QString;
        /// Restore history state `i`, recomposite, and emit [`changed`]; false when `i` is out of range.
        #[qinvokable]
        fn history_jump(self: Pin<&mut Self>, i: i32) -> bool;
        /// Capture the current state as a named restore point; false without a document.
        #[qinvokable]
        fn history_add_snapshot(self: Pin<&mut Self>, label: &QString) -> bool;
        /// Restore named restore point `i`, recomposite, and emit [`changed`]. Returns false when `i` is out of range.
        #[qinvokable]
        fn history_restore_snapshot(self: Pin<&mut Self>, i: i32) -> bool;
        /// Remove top-level layer `i`, recomposite, and emit [`changed`]. False (no history) for an out-of-range index, the Background, a fully locked layer, or the document's last layer.
        #[qinvokable]
        fn remove_layer(self: Pin<&mut Self>, i: i32) -> bool;
        /// Offscreen GPU spike: renders a gradient on Vulkan; 0 no GPU, 1 non-blank, 2 blank.
        #[qinvokable]
        fn render_gpu(self: Pin<&mut Self>) -> i32;
        /// Create a Vulkan device for the zero-copy interop probe and keep it
        /// alive. Returns false when no device is available.
        #[qinvokable]
        fn gpu_interop_prepare(self: Pin<&mut Self>) -> bool;
        /// Raw handles from `gpu_interop_prepare`; 0 when unavailable.
        #[qinvokable]
        fn gpu_vk_instance(&self) -> u64;
        #[qinvokable]
        fn gpu_vk_physical_device(&self) -> u64;
        #[qinvokable]
        fn gpu_vk_device(&self) -> u64;
        #[qinvokable]
        fn gpu_vk_queue_family(&self) -> u32;
        /// Raw VkImage of the wgpu offscreen texture; 0 when unavailable.
        #[qinvokable]
        fn gpu_vk_image(&self) -> u64;
        #[qinvokable]
        fn gpu_image_width(&self) -> u32;
        #[qinvokable]
        fn gpu_image_height(&self) -> u32;
        /// Set the GPU-compute preference and recomposite on the new backend.
        #[qinvokable]
        fn set_gpu_compute(self: Pin<&mut Self>, enabled: bool);
        /// The persisted GPU-compute preference (true by default).
        #[qinvokable]
        fn gpu_compute(&self) -> bool;
        /// Record a phase the Qt side timed into the `PICTURA_METRICS` totals;
        /// a no-op when metrics are off.
        #[qinvokable]
        fn record_metric_phase(&self, label: &QString, micros: i64);
        /// The metrics snapshot as JSON (phases, counters, recent edits), or
        /// empty when metrics are off. `reset` clears the totals and records.
        #[qinvokable]
        fn metrics_snapshot(&self, reset: bool) -> QString;
        /// Whether a usable GPU adapter exists (cached capability probe).
        #[qinvokable]
        fn gpu_available(&self) -> bool;
        /// GPU adapter fields: name, device type, backend, driver, driver info.
        /// Empty when no usable adapter exists.
        #[qinvokable]
        fn gpu_adapter_info(&self) -> QStringList;
        /// Active backend label: `"GPU"`, `"CPU"`, or `"CPU (no GPU)"`.
        #[qinvokable]
        fn active_backend(&self) -> QString;
        /// Set the incoming-profile policy from its persistence code; unknown codes are ignored.
        #[qinvokable]
        fn set_color_policy(self: Pin<&mut Self>, code: i32);
        /// The incoming-profile policy code (0 Preserve, 1 Convert, 2 Off).
        #[qinvokable]
        fn color_policy(&self) -> i32;
    }

    impl cxx_qt::Threading for PictureView {}
}
