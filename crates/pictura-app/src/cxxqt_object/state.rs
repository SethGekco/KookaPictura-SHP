use crate::layer_target::LayerTarget;
use cxx_qt_lib::QImage;
use pictura_core::{PixelBuffer, PsdRect};
use pictura_render::{CanvasDamage, ViewPyramid};
use std::cell::{Cell, RefCell};
use std::collections::HashMap;

/// A filter dialog's reduced canvas copy and what it was built from.
pub(super) struct CanvasProxy {
    /// The document region, the scale's bits and the `content_revision`.
    pub(super) key: (PsdRect, u64, u64, u64),
    pub(super) proxy: pictura_render::FilterProxy,
}

/// Backing Rust state for [`super::qobject::PictureView`].
pub struct PictureViewRust {
    /// The document, the selection, and the history: the state the document
    /// session owns.
    pub(super) session: crate::cxxqt_object::document_session::DocumentSession,
    /// The encoded committed selection outline and the selection revision it
    /// was built for. `None` until the outline is first requested.
    pub(super) selection_contour_cache: RefCell<Option<(u64, Vec<i32>)>>,
    /// How many times the committed outline has been traced, for the cache test.
    pub(super) selection_contour_computes: Cell<u64>,
    pub(super) path: Option<String>,
    /// The selected layer paths and the active path. The shared resolver turns
    /// this into the single tool-edit target.
    pub(super) layer_target: LayerTarget,
    /// The path calls edit the active shape layer's outline, not the Work
    /// Path (`paths.rs`).
    pub(super) path_on_layer: bool,
    /// The Paths panel row the path calls edit when not on a shape layer.
    pub(super) active_path: super::paths::ActivePath,
    /// Output format remembered from the import source, used to preselect the
    /// Save As filter: a lowercased extension (`"png"`) or `"psd"` when native.
    pub(super) source_format: String,
    pub(super) interop: Option<crate::gpu::InteropState>,
    /// The layer whose raster mask is the edit target, so the brush, fill, and
    /// filters write its coverage; `None` edits the layer's pixels.
    pub(super) mask_edit_target: Option<String>,
    /// The layer whose raster mask is shown alone on the canvas (view-only, no
    /// history); `None` shows the composite. See `impl_core/display.rs`.
    pub(super) mask_solo: Option<String>,
    /// The layer whose mask is shown as the rubylith overlay, if any. View
    /// state: it never reaches the composite.
    pub(super) mask_overlay: Option<String>,
    /// Layer Mask Display Options: the rubylith colour (ARGB) and opacity (%).
    pub(super) mask_display: (u32, u8),
    /// Bumped whenever the overlay or its options change, so the shell knows
    /// to re-render it.
    pub(super) mask_overlay_revision: u64,
    /// The isolated set while the Layers panel's Selected filter is on: the
    /// panel paths Auto-Select may pick. `None` leaves Auto-Select unscoped.
    pub(super) isolation: Option<Vec<String>>,
    /// Every layer path's own visibility when the current solo began. The
    /// snapshot belongs to this document and is dropped by a structural edit.
    pub(super) solo_snapshot: Option<HashMap<String, bool>>,
    /// The expanded group paths, per document.
    pub(super) expanded_groups: Vec<String>,
    pub(super) move_base: Option<QImage>,
    pub(super) move_layer: Option<QImage>,
    pub(super) move_x: i32,
    pub(super) move_y: i32,
    pub(super) move_opacity: i32,
    pub(super) move_prepared_revision: u64,
    /// The move-group key (its paths joined by `\n`) the move-preview base was
    /// built from; empty when no whole-layer preview is prepared.
    pub(super) move_prepared_layer: String,
    /// The move group's union rect when the moving image is a rendered group.
    /// A rendered group is composited at the layers' positions and clipped to
    /// the canvas, so a move makes the cached image stale; the cache is reused
    /// only while this rect is unchanged.
    pub(super) move_prepared_union: Option<(i32, i32, i32, i32)>,
    pub(super) move_preview_cache_hit: bool,
    /// The last committed similarity transform, for `Edit > Transform > Again`.
    /// Projective and Warp commits clear it (they are not repeatable this way).
    pub(super) last_transform: Option<super::document_session::RepeatTransform>,
    /// Whether the live transform belongs to the Move tool's per-gesture
    /// controls box. Such a session is never modal and does not lock edits.
    pub(super) transform_controls: bool,
    /// The panel previews that are applied but not committed yet.
    pub(super) pending_previews: super::document_session::PendingPreviews,
    pub(super) gpu_compute: bool,
    pub(super) color_policy: pictura_codec::Policy,
    pub(super) cmyk_policy: pictura_codec::Policy,
    pub(super) working_cmyk: Vec<u8>,
    pub(super) conversion_options: pictura_codec::ConversionOptions,
    pub(super) ask_mismatch_open: bool,
    pub(super) ask_missing_open: bool,
    pub(super) picker_choice: i32,
    pub(super) open_policy_override: Option<pictura_codec::Policy>,
    /// The `.pal` the next raster open uses when the file is an SHP (taken
    /// by `open_image_start`; set by `shp_set_open_palette`).
    pub(super) shp_open_palette: Option<String>,
    /// The last Embed Color Profile choice accepted for this document.
    pub(super) save_embed_profile: Option<bool>,
    /// The raster format options the next save of this document uses, set by the
    /// per-format options dialog (`save_format.rs`); `None` uses the defaults.
    pub(super) save_raster_options: Option<super::save_format::SaveRasterOptions>,
    /// Outstanding display damage; the canvas redraws the region `take` reports.
    /// No longer consumed by present: it keys on [`Self::canvas_revision`].
    pub(super) damage: CanvasDamage,
    /// Non-consuming revision of the displayed canvas pixels, bumped whenever
    /// `image` or the pyramid is rebuilt. Lets the shell cache without draining
    /// the damage account before `image` reads it.
    pub(super) canvas_revision: u64,
    /// Bumped when the level-0 frame is rebuilt whole **or the canvas display
    /// source changes** (mask solo enter/exit); a region change always reaches
    /// the shell as a `region_damaged`. A shell whose copy of the frame carries
    /// this revision needs no full image on `changed`.
    pub(super) frame_revision: u64,
    /// The interactive render session: the preview backdrop and reduced document.
    pub(super) render_session: pictura_render::RenderSession,
    /// The live display-only preview, if one is in progress.
    pub(super) display_preview: Option<super::display_preview::DisplayPreview>,
    /// The view-pyramid level and document rectangle the canvas last presented.
    pub(super) presented_level: i32,
    pub(super) presented_rect: PsdRect,
    /// Bumped by every committed edit, undo/redo, and non-display preview, so
    /// the session drops a reduced document the gesture no longer matches.
    pub(super) non_preview_revision: u64,
    /// The halved-level view of the composite, fed by `recomposite` and
    /// `refresh_region`.
    pub(super) pyramid: ViewPyramid,
    /// The 4-plane straight **sRGB** level-0 frame the display image, pyramid,
    /// and canvas crops all share, so every path shows the working-space
    /// conversion rather than the raw composite. The display `QImage` is built
    /// from this frame on demand (`PictureView::image`) rather than cached, so
    /// only one full-resolution frame is held.
    pub(super) level0: Option<PixelBuffer>,
    /// The solo mask's document-sized grayscale plane and its view pyramid,
    /// built while `mask_solo` is set and dropped when it clears. The canvas
    /// presents these instead of the composite (`impl_core/display.rs`); no
    /// other display consumer reads them.
    pub(super) mask_level0: Option<PixelBuffer>,
    pub(super) mask_pyramid: ViewPyramid,
    pub(super) link_sets: HashMap<String, u32>,
    /// The Ruler tool's measuring line: view state, never saved or undone.
    pub(super) ruler: Option<pictura_core::Ruler>,
    /// The filter dialog's reduced copy of the canvas.
    pub(super) canvas_proxy: Option<CanvasProxy>,
    /// Filter work running on worker threads (`filter_tools::jobs`).
    pub(super) filter_jobs: super::filter_tools::jobs::FilterJobs,
    /// The Auto-Blend Layers worker (`auto_blend`).
    pub(super) auto_blend: super::auto_blend::AutoBlendJob,
    /// The last committed filter's kind and slot values, for Last Filter and
    /// Last Filter Settings.
    pub(super) last_filter: Option<(String, Vec<f64>)>,
    /// The Smart Filter being re-edited, `(layer path, index)`: its dialog
    /// previews the chain up to it and OK replaces it in place.
    pub(super) smart_filter_edit: Option<(String, usize)>,
    /// Why the most recent filter apply/preview was refused, for surfacing the
    /// failure to the user. Cleared at the start of every attempt.
    pub(super) filter_error: Option<String>,
    /// Layers Panel Options: name a duplicate `"<name> copy"` (CS6 "Add copy to
    /// Copied Layers and Groups", default on).
    pub(super) add_copy: bool,
    /// Layers Panel Options: a fill layer created with an active selection
    /// takes that selection as a layer mask (CS6 "Use Default Masks on Fill
    /// Layers", default on).
    pub(super) use_default_masks: bool,
    /// Adjustments panel menu: a new adjustment layer receives a mask (CS6
    /// "Add Mask by Default", default on).
    pub(super) add_mask_by_default: bool,
    /// The Copy Shape Attributes clipboard: the last shape layer's fill and
    /// stroke, pasted onto another shape layer. Session state, never saved.
    pub(super) shape_attributes: Option<pictura_render::ShapeAttributes>,
    /// The background crash-recovery snapshot write in flight, if any
    /// (`impl_core/recovery.rs`); its result is whether the file landed.
    pub(super) recovery_write: Option<std::thread::JoinHandle<bool>>,
    /// The native save worker in flight, if any (`impl_core/save_jobs.rs`).
    pub(super) save_job: Option<super::impl_core::save_jobs::SaveJob>,
    /// Test seam: hold a started save's completion for this many milliseconds.
    pub(super) save_delay_for_test: u64,
    /// The adjustment dialog's worker jobs (`image_adjust/jobs.rs`).
    pub(super) adjust_jobs: super::image_adjust::jobs::AdjustJobs,
    /// Set when a loading tab's open is cancelled; the worker's queued result
    /// is dropped instead of applied (`open_jobs`).
    pub(super) open_cancelled: bool,
    /// The id of the latest open request. A result from an earlier request is
    /// stale: it must not install over a newer document (`open_jobs`).
    pub(super) open_request: u64,
    /// Test seam: hold a started open's completion for this many milliseconds.
    pub(super) open_delay_for_test: u64,
    /// The Warp Text dialog's layer path and that layer as it was before the
    /// dialog's first preview, which Cancel puts back.
    pub(super) warp_text_original: Option<(String, pictura_core::Layer)>,
}

impl Default for PictureViewRust {
    fn default() -> Self {
        Self {
            session: Default::default(),
            selection_contour_cache: RefCell::new(None),
            selection_contour_computes: Cell::new(0),
            path: None,
            layer_target: LayerTarget::default(),
            path_on_layer: false,
            active_path: super::paths::ActivePath::Work,
            source_format: "psd".to_string(),
            interop: None,
            mask_edit_target: None,
            mask_solo: None,
            mask_overlay: None,
            mask_display: (0xFFFF_0000, 50),
            mask_overlay_revision: 0,
            isolation: None,
            solo_snapshot: None,
            expanded_groups: Vec::new(),
            move_base: None,
            move_layer: None,
            move_x: 0,
            move_y: 0,
            move_opacity: 0,
            move_prepared_revision: 0,
            move_prepared_layer: String::new(),
            move_prepared_union: None,
            move_preview_cache_hit: false,
            last_transform: None,
            transform_controls: false,
            pending_previews: Default::default(),
            gpu_compute: true,
            color_policy: pictura_codec::Policy::Preserve,
            cmyk_policy: pictura_codec::Policy::Preserve,
            working_cmyk: pictura_codec::Profile::coated_cmyk().to_icc(),
            conversion_options: pictura_codec::ConversionOptions::default(),
            ask_mismatch_open: false,
            ask_missing_open: false,
            picker_choice: 0,
            open_policy_override: None,
            shp_open_palette: None,
            save_embed_profile: None,
            save_raster_options: None,
            damage: CanvasDamage::default(),
            canvas_revision: 0,
            frame_revision: 0,
            render_session: pictura_render::RenderSession::default(),
            display_preview: None,
            presented_level: 0,
            presented_rect: PsdRect {
                top: 0,
                left: 0,
                bottom: 0,
                right: 0,
            },
            non_preview_revision: 0,
            pyramid: ViewPyramid::default(),
            level0: None,
            mask_level0: None,
            mask_pyramid: ViewPyramid::default(),
            link_sets: HashMap::new(),
            ruler: None,
            canvas_proxy: None,
            filter_jobs: Default::default(),
            auto_blend: Default::default(),
            last_filter: None,
            smart_filter_edit: None,
            filter_error: None,
            add_copy: true,
            use_default_masks: true,
            add_mask_by_default: true,
            shape_attributes: None,
            recovery_write: None,
            save_job: None,
            save_delay_for_test: 0,
            adjust_jobs: Default::default(),
            open_cancelled: false,
            open_request: 0,
            open_delay_for_test: 0,
            warp_text_original: None,
        }
    }
}

impl PictureViewRust {
    /// Replace the target, clearing a mask edit target that named the previous
    /// active path.
    pub(super) fn set_layer_target(&mut self, target: crate::layer_target::LayerTarget) {
        // The mask target follows its layer: keep it whenever the new anchor is
        // that layer, even when the previous anchor was stale.
        if self.mask_edit_target.as_deref() != target.anchor() {
            self.mask_edit_target = None;
        }
        self.layer_target = target;
    }

    /// Select one layer while keeping the mask-edit invariant.
    pub(super) fn select_only_layer(&mut self, path: &str) {
        let mut target = self.layer_target.clone();
        target.select_only(path);
        self.set_layer_target(target);
    }
}
