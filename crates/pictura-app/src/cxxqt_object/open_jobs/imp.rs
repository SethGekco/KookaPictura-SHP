use super::*;

use super::super::helpers::decode_import;
use super::super::helpers_composite::{current_buffer, store_composite};
use super::super::impl_core::format_for_path;
use super::super::state::PictureViewRust;
use crate::history::History;
use crate::layer_target::LayerTarget;
use core::pin::Pin;
use cxx_qt::{CxxQtThread, CxxQtType, Threading};
use cxx_qt_lib::{QString, QStringList};
use pictura_codec::pdf::import::{self as pdf_import, PdfImages, PdfRect, Rendered};
use pictura_codec::{eps, Ghostscript, Policy, Raster, RasterError, Resolution};
use pictura_core::{BitDepth, ColorMode, Document, Layer, LockFlags};
use std::path::Path;
use std::time::Duration;

/// What an open does to the view's file path.
pub(in crate::cxxqt_object) enum OpenedPath {
    /// Assign this path (a document open, or a path-less import).
    Set(Option<String>),
}

/// A decoded document and the view fields that belong with it.
pub(in crate::cxxqt_object) struct OpenedDocument {
    pub(in crate::cxxqt_object) doc: Document,
    pub(in crate::cxxqt_object) history: History,
    pub(in crate::cxxqt_object) target: LayerTarget,
    pub(in crate::cxxqt_object) path: OpenedPath,
    pub(in crate::cxxqt_object) source_format: String,
    pub(in crate::cxxqt_object) pdf_password: Option<String>,
}

/// A PDF probe's outcome.
pub(in crate::cxxqt_object) enum PdfOpen {
    Ok(Box<OpenedDocument>),
    None,
    Password,
    WrongPassword,
}

/// The import target as owned Rust data, so it can cross into a worker.
pub(in crate::cxxqt_object) struct ImportSpec {
    width: i32,
    height: i32,
    anti_alias: bool,
    mode: String,
    depth: i32,
    ppi: f64,
    per_cm: bool,
    name: String,
}

impl ImportSpec {
    pub(in crate::cxxqt_object) fn from_target(target: &ImportTarget) -> Self {
        Self {
            width: target.width,
            height: target.height,
            anti_alias: target.anti_alias,
            mode: target.mode.to_string(),
            depth: target.depth,
            ppi: target.ppi,
            per_cm: target.per_cm,
            name: target.name.to_string(),
        }
    }
}

/// Apply a decoded document to a view on the GUI thread.
pub(in crate::cxxqt_object) fn apply_opened(rust: &mut PictureViewRust, opened: OpenedDocument) {
    rust.replace_document(
        Some(opened.doc),
        opened.history,
        opened.target,
        opened.pdf_password,
    );
    let OpenedPath::Set(path) = opened.path;
    rust.path = path;
    rust.source_format = opened.source_format;
    rust.session.mark_saved();
}

/// `File > Revert`: the synchronous in-place reload.
pub(super) fn revert(view: Pin<&mut PictureView>, path: &QString) -> bool {
    view.revert(path)
}

pub(super) fn history_for(doc: &mut Document, target: &LayerTarget, label: &str) -> History {
    let mut history = History::default();
    history.capture_live(doc, &None, target, label);
    history
}

/// Cache the displayed composite after a decode, unless the file retained
/// native samples and a merged composite. A retained store is the conversion
/// source and keeps values above white; leaving the file's merged composite
/// (the store's 8-bit narrowing) in place lets `convert_depth` see that store
/// as current. A file without a merged composite still recomposites here.
pub(super) fn store_opened_composite(doc: &mut Document, gpu: bool) {
    if doc.source_planes.is_some() && doc.merged_composite_present {
        return;
    }
    let rendered = current_buffer(doc, gpu);
    store_composite(doc, &rendered);
}

/// The application's default open options. The working CMYK profile is the
/// bundled synthetic coated profile until Color Settings carries a choice.
pub(super) fn open_options(policy: Policy) -> pictura_codec::OpenOptions {
    pictura_codec::OpenOptions {
        policy,
        working_cmyk: Some(pictura_codec::Profile::coated_cmyk().to_icc()),
        conversion: pictura_codec::ConversionOptions::default(),
    }
}

/// Read and decode a PSD/PSB. `None` when the file is missing or unsupported.
pub(in crate::cxxqt_object) fn decode_psd(
    path: &str,
    policy: Policy,
    gpu: bool,
    progress: &dyn Fn(u8, &str),
) -> Option<OpenedDocument> {
    decode_psd_with(path, open_options(policy), gpu, progress)
}

pub(super) fn decode_psd_with(
    path: &str,
    options: pictura_codec::OpenOptions,
    gpu: bool,
    progress: &dyn Fn(u8, &str),
) -> Option<OpenedDocument> {
    progress(5, "Reading file");
    let bytes = std::fs::read(path).ok()?;
    progress(25, "Decoding");
    let mut doc = pictura_codec::read_psd_with_options(&bytes, &options).ok()?;
    progress(65, "Compositing");
    store_opened_composite(&mut doc, gpu);
    progress(90, "Preparing layers");
    let target = LayerTarget::for_opened(&doc, None);
    let history = history_for(&mut doc, &target, "Open");
    Some(OpenedDocument {
        doc,
        history,
        target,
        path: OpenedPath::Set(Some(path.to_string())),
        source_format: "psd".to_string(),
        pdf_password: None,
    })
}

/// Finish an imported raster document. When every decoded pixel is opaque the
/// single `from_rgba` layer becomes the locked `Background` and its redundant
/// alpha channel is dropped; otherwise the regular alpha layer named from the
/// file stem is left untouched.
pub(in crate::cxxqt_object) fn finalize_import(doc: &mut Document, rgba: &[u8]) {
    if !rgba.as_chunks::<4>().0.iter().all(|px| px[3] == 255) {
        return;
    }
    if let Some(layer) = doc.layers.last_mut() {
        make_background(layer);
    }
}

/// Turn the opaque `layer` into the locked `Background`, dropping its
/// redundant alpha channel.
pub(in crate::cxxqt_object) fn make_background(layer: &mut Layer) {
    layer.name = "Background".to_string();
    layer.background = true;
    layer.lock = LockFlags::default()
        .with(LockFlags::TRANSPARENCY, true)
        .with(LockFlags::POSITION, true);
    layer.channels.retain(|channel| channel.id != -1);
}

/// Apply a raster's embedded RGB ICC profile under the incoming policy. A
/// CMYK profile is not applied to the decoded RGB bytes; Qt has already
/// produced RGB values and the profile cannot describe them.
pub(super) fn apply_raster_profile(
    doc: &mut Document,
    bytes: &[u8],
    policy: Policy,
    options: pictura_codec::ConversionOptions,
) {
    let probe = pictura_codec::probe_raster_profile(bytes);
    let Some(icc) = probe.embedded else {
        return;
    };
    if probe.is_srgb || probe.channels != Some(3) {
        return;
    }
    match policy {
        Policy::Preserve => doc.document_icc = Some(icc.into()),
        Policy::Convert => {
            doc.document_icc = Some(icc.clone().into());
            let _ =
                pictura_codec::convert_document_with(doc, &pictura_codec::Profile::srgb(), options);
            doc.source_icc = Some(icc.into());
        }
        Policy::Off => {}
    }
}

/// Convert a placed raster's decoded pixels to the host sRGB working space
/// when it carries a non-sRGB RGB ICC profile. Place always converts, so the
/// resulting layer is in the document's working space.
pub(in crate::cxxqt_object) fn convert_placed_rgba(
    bytes: &[u8],
    rgba: Vec<u8>,
    width: u32,
    height: u32,
    policy: Policy,
) -> Vec<u8> {
    if policy == Policy::Off {
        return rgba;
    }
    let probe = pictura_codec::probe_raster_profile(bytes);
    let Some(icc) = probe.embedded else {
        return rgba;
    };
    if probe.is_srgb || probe.channels != Some(3) {
        return rgba;
    }
    let mut doc = Document::from_rgba("place", width, height, &rgba);
    doc.document_icc = Some(icc.into());
    if !pictura_codec::convert_document_with(
        &mut doc,
        &pictura_codec::Profile::srgb(),
        pictura_codec::ConversionOptions::default(),
    ) {
        return rgba;
    }
    super::super::helpers_composite::buffer_to_rgba_bytes(&doc.composite)
}

/// Read and decode a raster through the shared import path.
pub(in crate::cxxqt_object) fn decode_image(
    path: &str,
    gpu: bool,
    progress: &dyn Fn(u8, &str),
) -> Option<OpenedDocument> {
    decode_image_with_policy(path, Policy::Convert, gpu, progress)
}

pub(in crate::cxxqt_object) fn decode_image_with_policy(
    path: &str,
    policy: Policy,
    gpu: bool,
    progress: &dyn Fn(u8, &str),
) -> Option<OpenedDocument> {
    decode_image_with_options(
        path,
        policy,
        pictura_codec::ConversionOptions::default(),
        gpu,
        progress,
    )
}

pub(in crate::cxxqt_object) fn decode_image_with_options(
    path: &str,
    policy: Policy,
    options: pictura_codec::ConversionOptions,
    gpu: bool,
    progress: &dyn Fn(u8, &str),
) -> Option<OpenedDocument> {
    let name = Path::new(path)
        .file_stem()
        .map(|stem| stem.to_string_lossy().into_owned())
        .unwrap_or_default();
    progress(5, "Reading file");
    let bytes = super::super::helpers::safe_io::read_capped(
        path,
        super::super::helpers::safe_io::import_file_limit(),
    )?;
    // An SHP opens as a sheet before any Qt image plugin can claim it.
    if let Some(opened) = super::super::shp::decode_shp(path, &bytes, gpu, progress) {
        return opened;
    }
    progress(25, "Decoding");
    let (rgba, width, height) = decode_import(&bytes)?;
    progress(70, "Compositing");
    let mut doc = Document::from_rgba(&name, width, height, &rgba);
    apply_raster_profile(&mut doc, &bytes, policy, options);
    finalize_import(&mut doc, &rgba);
    if let Some(section) = pictura_codec::raster_image_resources(&bytes) {
        doc.image_resources = section.into();
    }
    store_opened_composite(&mut doc, gpu);
    progress(90, "Preparing layers");
    let target = LayerTarget::for_opened(&doc, Some("0".to_string()));
    let history = history_for(&mut doc, &target, "Open");
    Some(OpenedDocument {
        doc,
        history,
        target,
        path: OpenedPath::Set(Some(path.to_string())),
        source_format: format_for_path(path),
        pdf_password: None,
    })
}

/// Decode `path` for an in-place Revert without touching the view. A PDF uses
/// the stored password; a missing, corrupt, or unreadable file returns `None`.
pub(in crate::cxxqt_object) fn decode_for_revert(
    path: &str,
    pdf_password: Option<&str>,
    policy: Policy,
    gpu: bool,
) -> Option<OpenedDocument> {
    // Match the old C++ routing by the file's own extension: a path without a
    // known extension stays a raster import, not the PSD default used by Save.
    let suffix = Path::new(path)
        .extension()
        .and_then(|extension| extension.to_str())
        .map(str::to_ascii_lowercase);
    match suffix.as_deref() {
        Some("psd" | "psb") => decode_psd(path, policy, gpu, &|_, _| {}),
        Some("pdf" | "pdp") => match decode_pdf(path, pdf_password.unwrap_or(""), policy, gpu) {
            PdfOpen::Ok(opened) => Some(*opened),
            PdfOpen::None | PdfOpen::Password | PdfOpen::WrongPassword => None,
        },
        _ => decode_image(path, gpu, &|_, _| {}),
    }
}

/// Read and embed `path` as a smart-object document.
pub(in crate::cxxqt_object) fn decode_smart_object(
    path: &str,
    gpu: bool,
    progress: &dyn Fn(u8, &str),
) -> Option<OpenedDocument> {
    let name = Path::new(path)
        .file_stem()
        .map(|stem| stem.to_string_lossy().into_owned())
        .unwrap_or_default();
    progress(5, "Reading file");
    let bytes = std::fs::read(path).ok();
    progress(35, "Decoding");
    let mut doc = match bytes
        .as_deref()
        .and_then(|bytes| pictura_render::open_as_smart_object(&name, bytes))
    {
        Some(doc) => doc,
        None => {
            let (rgba, width, height) = bytes.as_deref().and_then(decode_import)?;
            let mut doc = Document::from_rgba(&name, width, height, &rgba);
            if !pictura_render::convert_to_smart_object(&mut doc, "0") {
                return None;
            }
            doc
        }
    };
    progress(70, "Compositing");
    store_opened_composite(&mut doc, gpu);
    progress(90, "Preparing layers");
    let target = LayerTarget::for_opened(&doc, Some("0".to_string()));
    let history = history_for(&mut doc, &target, "Open As Smart Object");
    Some(OpenedDocument {
        doc,
        history,
        target,
        path: OpenedPath::Set(None),
        source_format: "psd".to_string(),
        pdf_password: None,
    })
}

pub(super) fn decode_pdf_psd(
    psd: &[u8],
    path: &str,
    password: &str,
    options: pictura_codec::OpenOptions,
    gpu: bool,
    progress: &dyn Fn(u8, &str),
) -> Option<OpenedDocument> {
    progress(35, "Decoding");
    let mut doc = pictura_codec::read_psd_with_options(psd, &options).ok()?;
    progress(65, "Compositing");
    store_opened_composite(&mut doc, gpu);
    progress(90, "Preparing layers");
    let target = LayerTarget::for_opened(&doc, None);
    let history = history_for(&mut doc, &target, "Open");
    Some(OpenedDocument {
        doc,
        history,
        target,
        path: OpenedPath::Set(Some(path.to_string())),
        source_format: "pdf".to_string(),
        pdf_password: (!password.is_empty()).then(|| password.to_string()),
    })
}

/// Probe a Photoshop PDF and decode its editing data. A decode failure reads as
/// `None`, so the caller falls back to the import dialog, as the synchronous
/// `pdf_open` did.
pub(in crate::cxxqt_object) fn decode_pdf(
    path: &str,
    password: &str,
    policy: Policy,
    gpu: bool,
) -> PdfOpen {
    let bytes = std::fs::read(path).unwrap_or_default();
    match pictura_codec::pdf::editing_psd(&bytes, password) {
        pictura_codec::pdf::EditingPsd::Psd(psd) => {
            match decode_pdf_psd(&psd, path, password, open_options(policy), gpu, &|_, _| {}) {
                Some(opened) => PdfOpen::Ok(Box::new(opened)),
                None => PdfOpen::None,
            }
        }
        pictura_codec::pdf::EditingPsd::Absent => PdfOpen::None,
        pictura_codec::pdf::EditingPsd::Locked if password.is_empty() => PdfOpen::Password,
        pictura_codec::pdf::EditingPsd::Locked => PdfOpen::WrongPassword,
    }
}

/// Render PDF page `page` (1-based) cropped to `area`.
pub(in crate::cxxqt_object) fn render_pdf_page(
    path: &str,
    password: &str,
    page: i32,
    area: &str,
    spec: &ImportSpec,
) -> Result<Rendered, RasterError> {
    let numbers: Vec<f64> = area
        .split_whitespace()
        .filter_map(|token| token.parse().ok())
        .collect();
    let [x0, y0, x1, y1, view_height] = numbers[..] else {
        return Err(RasterError::Failed("bad page area".into()));
    };
    Ghostscript::locate().and_then(|gs| {
        pdf_import::render_area(
            &gs,
            Path::new(path),
            (!password.is_empty()).then_some(password),
            page.max(1) as u32,
            PdfRect::new(x0, y0, x1, y1),
            view_height,
            (spec.width.max(1) as u32, spec.height.max(1) as u32),
            spec.anti_alias,
        )
    })
}

/// Render embedded PDF image `index` at its native size.
pub(in crate::cxxqt_object) fn render_pdf_image(
    path: &str,
    index: i32,
    _spec: &ImportSpec,
) -> Result<Rendered, RasterError> {
    Ghostscript::locate().and_then(|gs| {
        let found = std::fs::read(path)
            .ok()
            .as_deref()
            .and_then(PdfImages::read)
            .ok_or_else(|| RasterError::Failed("no readable images".into()))?;
        found.render(&gs, index.max(0) as usize, None)
    })
}

/// Rasterize an EPS's bounding box.
pub(in crate::cxxqt_object) fn render_eps(
    path: &str,
    spec: &ImportSpec,
) -> Result<Rendered, RasterError> {
    Ghostscript::locate().and_then(|gs| {
        let bounds = std::fs::read(path)
            .ok()
            .as_deref()
            .and_then(eps::bounding_box)
            .ok_or_else(|| RasterError::Failed("no bounding box".into()))?;
        eps::render_eps(
            &gs,
            Path::new(path),
            bounds,
            (spec.width.max(1) as u32, spec.height.max(1) as u32),
            spec.anti_alias,
        )
    })
}

/// Build the untitled document an import's raster becomes.
pub(in crate::cxxqt_object) fn imported_document(
    raster: &Raster,
    spec: &ImportSpec,
    gpu: bool,
    progress: &dyn Fn(u8, &str),
) -> Option<OpenedDocument> {
    progress(80, "Preparing layers");
    let mut doc = Document::from_rgba(&spec.name, raster.width, raster.height, &raster.rgba);
    if let Some(layer) = doc.layers.last_mut() {
        layer.name = "Layer 1".to_string();
    }
    pictura_codec::set_document_resolution(
        &mut doc,
        Resolution {
            ppi: if spec.ppi > 0.0 { spec.ppi } else { 72.0 },
            per_cm: spec.per_cm,
        },
    );
    let mode = match spec.mode.as_str() {
        "grayscale" => ColorMode::Grayscale,
        "cmyk" => ColorMode::Cmyk,
        "lab" => ColorMode::Lab,
        _ => ColorMode::Rgb,
    };
    if mode != ColorMode::Rgb {
        pictura_render::convert_mode(&mut doc, mode).ok()?;
    }
    if spec.depth == 16 {
        pictura_render::convert_bit_depth(&mut doc, BitDepth::Sixteen).ok()?;
    }
    progress(90, "Compositing");
    store_opened_composite(&mut doc, gpu);
    progress(95, "Preparing layers");
    let target = LayerTarget::for_opened(&doc, Some("0".to_string()));
    let history = history_for(&mut doc, &target, "Open");
    Some(OpenedDocument {
        doc,
        history,
        target,
        path: OpenedPath::Set(None),
        source_format: "psd".to_string(),
        pdf_password: None,
    })
}

pub(super) fn warnings_list(warnings: Vec<String>) -> QStringList {
    let mut list = QStringList::default();
    for warning in warnings {
        list.append(QString::from(warning.as_str()));
    }
    list
}

impl PictureViewRust {
    /// Start an open request and return its id. A result from an earlier
    /// request is stale and must not install.
    pub(in crate::cxxqt_object) fn begin_open_request(&mut self) -> u64 {
        self.open_cancelled = false;
        self.open_request = self.open_request.wrapping_add(1);
        self.open_request
    }
}

/// Apply a result unless the view was cancelled or the request is stale. True
/// when it was applied.
pub(super) fn apply_if_current(
    rust: &mut PictureViewRust,
    request: u64,
    result: Option<OpenedDocument>,
) -> bool {
    if rust.open_cancelled || request != rust.open_request {
        return false;
    }
    if let Some(opened) = result {
        apply_opened(rust, opened);
    }
    true
}

/// Queue the worker's result onto the GUI thread. A cancelled or destroyed
/// view drops it.
pub(super) fn finish_on_gui(
    qt_thread: CxxQtThread<PictureView>,
    request: u64,
    result: Option<OpenedDocument>,
    status: String,
    warnings: Vec<String>,
) {
    qt_thread
        .queue(move |mut view| {
            {
                let mut rust = view.as_mut().rust_mut();
                if !apply_if_current(&mut rust, request, result) {
                    return;
                }
            }
            view.as_mut().session_cancelled();
            view.open_finished(QString::from(status.as_str()), warnings_list(warnings));
        })
        .ok();
}

pub(super) fn delay(ms: u64) {
    if ms > 0 {
        std::thread::sleep(Duration::from_millis(ms));
    }
}

pub(super) fn error_status(error: &RasterError) -> String {
    format!("error:{error}")
}

pub(super) fn start(
    view: &mut Pin<&mut PictureView>,
    work: impl FnOnce(&dyn Fn(u8, &str)) -> (Option<OpenedDocument>, String, Vec<String>)
        + Send
        + 'static,
) {
    let request = view.as_mut().rust_mut().begin_open_request();
    let delay_ms = view.rust().open_delay_for_test;
    let qt_thread = view.as_ref().get_ref().qt_thread();
    let progress_thread = qt_thread.clone();
    let progress = move |percent: u8, phase: &str| {
        let phase = phase.to_string();
        progress_thread
            .queue(move |view| {
                if !view.rust().open_cancelled {
                    view.open_progress(i32::from(percent), QString::from(phase.as_str()));
                }
            })
            .ok();
    };
    std::thread::spawn(move || {
        let (result, status, warnings) = work(&progress);
        delay(delay_ms);
        finish_on_gui(qt_thread, request, result, status, warnings);
    });
}

pub fn open_psd_start(mut view: Pin<&mut PictureView>, path: &QString) {
    let path = path.to_string();
    let (options, gpu) = {
        let mut rust = view.as_mut().rust_mut();
        let policy = rust
            .open_policy_override
            .take()
            .unwrap_or(rust.color_policy);
        let options = pictura_codec::OpenOptions {
            policy,
            working_cmyk: Some(rust.working_cmyk.clone()),
            conversion: rust.conversion_options,
        };
        (options, rust.gpu_compute)
    };
    start(&mut view, move |progress| {
        match decode_psd_with(&path, options, gpu, progress) {
            Some(opened) => (Some(opened), "ok".to_string(), Vec::new()),
            None => (None, "error".to_string(), Vec::new()),
        }
    });
}

pub fn open_image_start(mut view: Pin<&mut PictureView>, path: &QString) {
    let path = path.to_string();
    let (policy, options, gpu, shp) = {
        let mut rust = view.as_mut().rust_mut();
        let policy = rust
            .open_policy_override
            .take()
            .unwrap_or(rust.color_policy);
        let shp = rust.shp_open.take();
        (policy, rust.conversion_options, rust.gpu_compute, shp)
    };
    start(
        &mut view,
        move |progress| match super::super::shp::with_open_request(shp, || {
            decode_image_with_options(&path, policy, options, gpu, progress)
        }) {
            Some(opened) => (Some(opened), "ok".to_string(), Vec::new()),
            None => (None, "error".to_string(), Vec::new()),
        },
    );
}

pub fn open_smart_object_start(mut view: Pin<&mut PictureView>, path: &QString) {
    let path = path.to_string();
    let gpu = view.rust().gpu_compute;
    start(&mut view, move |progress| {
        match decode_smart_object(&path, gpu, progress) {
            Some(opened) => (Some(opened), "ok".to_string(), Vec::new()),
            None => (None, "error".to_string(), Vec::new()),
        }
    });
}

pub fn open_pdf_start(mut view: Pin<&mut PictureView>, path: &QString, password: &QString) {
    let path = path.to_string();
    let password = password.to_string();
    let options = {
        let rust = view.rust();
        pictura_codec::OpenOptions {
            policy: rust.color_policy,
            working_cmyk: Some(rust.working_cmyk.clone()),
            conversion: rust.conversion_options,
        }
    };
    let gpu = view.rust().gpu_compute;
    let request = view.as_mut().rust_mut().begin_open_request();
    let delay_ms = view.rust().open_delay_for_test;
    let qt_thread = view.as_ref().get_ref().qt_thread();
    let detected = qt_thread.clone();
    let progress_thread = qt_thread.clone();
    let progress = move |percent: u8, phase: &str| {
        let phase = phase.to_string();
        progress_thread
            .queue(move |view| {
                if !view.rust().open_cancelled {
                    view.open_progress(i32::from(percent), QString::from(phase.as_str()));
                }
            })
            .ok();
    };
    std::thread::spawn(move || {
        progress(5, "Reading file");
        let bytes = std::fs::read(&path).unwrap_or_default();
        progress(15, "Checking PDF");
        match pictura_codec::pdf::editing_psd(&bytes, &password) {
            pictura_codec::pdf::EditingPsd::Psd(psd) => {
                detected
                    .queue(|view| {
                        if !view.rust().open_cancelled {
                            view.open_detected();
                        }
                    })
                    .ok();
                let opened = decode_pdf_psd(&psd, &path, &password, options, gpu, &progress);
                let status = if opened.is_some() {
                    "ok".to_string()
                } else {
                    "none".to_string()
                };
                delay(delay_ms);
                finish_on_gui(qt_thread, request, opened, status, Vec::new());
            }
            pictura_codec::pdf::EditingPsd::Absent => {
                delay(delay_ms);
                finish_on_gui(qt_thread, request, None, "none".to_string(), Vec::new());
            }
            pictura_codec::pdf::EditingPsd::Locked if password.is_empty() => {
                delay(delay_ms);
                finish_on_gui(qt_thread, request, None, "password".to_string(), Vec::new());
            }
            pictura_codec::pdf::EditingPsd::Locked => {
                delay(delay_ms);
                finish_on_gui(
                    qt_thread,
                    request,
                    None,
                    "wrongpassword".to_string(),
                    Vec::new(),
                );
            }
        }
    });
}

pub fn open_pdf_page_start(
    mut view: Pin<&mut PictureView>,
    path: &QString,
    password: &QString,
    page: i32,
    area: &QString,
    target: &ImportTarget,
) {
    let path = path.to_string();
    let password = password.to_string();
    let area = area.to_string();
    let spec = ImportSpec::from_target(target);
    let gpu = view.rust().gpu_compute;
    start(&mut view, move |progress| {
        progress(10, "Rendering page");
        match render_pdf_page(&path, &password, page, &area, &spec) {
            Ok(rendered) => match imported_document(&rendered.raster, &spec, gpu, progress) {
                Some(opened) => (Some(opened), "ok".to_string(), rendered.warnings),
                None => (
                    None,
                    "error:the image could not be converted".to_string(),
                    Vec::new(),
                ),
            },
            Err(error) => (None, error_status(&error), Vec::new()),
        }
    });
}

pub fn open_pdf_image_start(
    mut view: Pin<&mut PictureView>,
    path: &QString,
    index: i32,
    target: &ImportTarget,
) {
    let path = path.to_string();
    let spec = ImportSpec::from_target(target);
    let gpu = view.rust().gpu_compute;
    start(&mut view, move |progress| {
        progress(10, "Rendering image");
        match render_pdf_image(&path, index, &spec) {
            Ok(rendered) => match imported_document(&rendered.raster, &spec, gpu, progress) {
                Some(opened) => (Some(opened), "ok".to_string(), rendered.warnings),
                None => (
                    None,
                    "error:the image could not be converted".to_string(),
                    Vec::new(),
                ),
            },
            Err(error) => (None, error_status(&error), Vec::new()),
        }
    });
}

pub fn open_eps_start(mut view: Pin<&mut PictureView>, path: &QString, target: &ImportTarget) {
    let path = path.to_string();
    let spec = ImportSpec::from_target(target);
    let gpu = view.rust().gpu_compute;
    start(&mut view, move |progress| {
        progress(10, "Rendering EPS");
        match render_eps(&path, &spec) {
            Ok(rendered) => match imported_document(&rendered.raster, &spec, gpu, progress) {
                Some(opened) => (Some(opened), "ok".to_string(), rendered.warnings),
                None => (
                    None,
                    "error:the image could not be converted".to_string(),
                    Vec::new(),
                ),
            },
            Err(error) => (None, error_status(&error), Vec::new()),
        }
    });
}

pub fn open_cancel(mut view: Pin<&mut PictureView>) {
    view.as_mut().rust_mut().open_cancelled = true;
}

pub fn open_set_delay(mut view: Pin<&mut PictureView>, ms: u64) {
    view.as_mut().rust_mut().open_delay_for_test = ms;
}

#[cfg(test)]
mod tests;
