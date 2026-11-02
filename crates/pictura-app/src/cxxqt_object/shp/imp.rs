// SPDX-License-Identifier: GPL-3.0-or-later
//! The SHP document glue: open as a sheet, the palette queries, the
//! recorded snap / clear / rebind edits, and the byte-preserving save.

use super::ffi::{ShpCheck, ShpInfo, ShpOffPoints, ShpPlan, ShpThumb};
use super::*;

use crate::cxxqt_object::edit::Edit;
use crate::cxxqt_object::helpers::safe_io::{read_capped, write_atomically};
use crate::cxxqt_object::helpers_composite::{buffer_to_rgba_bytes, current_buffer};
use crate::cxxqt_object::open_jobs::{OpenedDocument, OpenedPath};
use crate::cxxqt_object::state::PictureViewRust;
use crate::layer_target::LayerTarget;
use core::pin::Pin;
use cxx_qt::CxxQtType;
use cxx_qt_lib::QString;
use pictura_core::{BitDepth, BlendMode, ColorMode, Document, Layer};
use pictura_shp::palette::{fallback_palette, read_palette, role, unique_colours, Palette};
use pictura_shp::resource::{self, find_block, set_block, Header, ShpResource};
use pictura_shp::sheet::{self, Layout};
use pictura_shp::{build_mapped, build_preserving, frame_canvas, sniff, FramePlane, Source};
use std::borrow::Cow;
use std::cell::RefCell;
use std::io::Read as _;
use std::path::Path;

/// A palette and the file it came from (empty for the fallback).
#[derive(Clone)]
pub(in crate::cxxqt_object) struct PaletteChoice {
    pub(in crate::cxxqt_object) palette: Palette,
    pub(in crate::cxxqt_object) path: String,
}

impl PaletteChoice {
    /// The palette at `path`, or the grey fallback when `path` is empty or
    /// does not hold a palette.
    pub(in crate::cxxqt_object) fn load(path: &str) -> PaletteChoice {
        Self::try_load(path).unwrap_or_else(|| PaletteChoice {
            palette: fallback_palette(),
            path: String::new(),
        })
    }

    fn try_load(path: &str) -> Option<PaletteChoice> {
        if path.is_empty() {
            return None;
        }
        let bytes = read_capped(path, 1 << 20)?;
        let mut palette = read_palette(&bytes)?;
        palette.name = Path::new(path)
            .file_name()
            .map(|n| n.to_string_lossy().into_owned())
            .unwrap_or_default();
        Some(PaletteChoice {
            palette,
            path: path.to_string(),
        })
    }
}

/// What the shell asks of the next SHP open on a view: the palette (`None`
/// or empty for the fallback) and how many bytes of sheet the user agreed to
/// hold past the size limit (0: none).
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub(in crate::cxxqt_object) struct OpenRequest {
    pub(in crate::cxxqt_object) palette: Option<String>,
    pub(in crate::cxxqt_object) allowance: u64,
}

thread_local! {
    /// The palette an SHP decode on this thread uses; set around one decode
    /// by [`with_open_request`] / [`with_document_palette`].
    static OPEN_PALETTE: RefCell<Option<PaletteChoice>> = const { RefCell::new(None) };
    /// The size allowance of that decode.
    static OPEN_ALLOWANCE: RefCell<u64> = const { RefCell::new(0) };
    static LAST_ERROR: RefCell<String> = const { RefCell::new(String::new()) };
}

fn fail<T>(message: impl Into<String>) -> Result<T, String> {
    Err(message.into())
}

fn set_error(message: &str) {
    LAST_ERROR.with(|e| *e.borrow_mut() = message.to_string());
}

/// Run `f` (a decode) with the request's palette (`None` or empty: the
/// fallback) and size allowance as the ones an SHP opens with.
pub(in crate::cxxqt_object) fn with_open_request<T>(
    request: Option<OpenRequest>,
    f: impl FnOnce() -> T,
) -> T {
    let request = request.unwrap_or_default();
    let choice = PaletteChoice::load(request.palette.as_deref().unwrap_or(""));
    OPEN_PALETTE.with(|p| *p.borrow_mut() = Some(choice));
    OPEN_ALLOWANCE.with(|a| *a.borrow_mut() = request.allowance);
    let out = f();
    OPEN_PALETTE.with(|p| *p.borrow_mut() = None);
    OPEN_ALLOWANCE.with(|a| *a.borrow_mut() = 0);
    out
}

/// Run `f` (Revert's decode) with the palette `doc` already uses, so a
/// reverted SHP keeps it.
pub(in crate::cxxqt_object) fn with_document_palette<T>(
    doc: Option<&Document>,
    f: impl FnOnce() -> T,
) -> T {
    let choice = doc
        .and_then(block)
        .and_then(resource::parts)
        .map(|(header, path, _)| PaletteChoice {
            palette: header.palette,
            path,
        });
    OPEN_PALETTE.with(|p| *p.borrow_mut() = choice);
    let out = f();
    OPEN_PALETTE.with(|p| *p.borrow_mut() = None);
    out
}

/// The document's `KookaSHP` block data.
fn block(doc: &Document) -> Option<&[u8]> {
    find_block(&doc.image_resources, resource::RESOURCE_ID)
}

fn header(doc: &Document) -> Option<Header> {
    block(doc).and_then(Header::read)
}

/// `Some(result)` when `bytes` is an SHP, so the caller never hands it to
/// another decoder (an installed Qt SHP plugin included); `None` otherwise.
pub(in crate::cxxqt_object) fn decode_shp(
    path: &str,
    bytes: &[u8],
    gpu: bool,
    progress: &dyn Fn(u8, &str),
) -> Option<Option<OpenedDocument>> {
    if !sniff(bytes, Some(bytes.len())) {
        return None;
    }
    let choice = OPEN_PALETTE
        .with(|p| p.borrow().clone())
        .unwrap_or_else(|| PaletteChoice::load(""));
    let allowance = OPEN_ALLOWANCE.with(|a| *a.borrow());
    Some(open_sheet(
        path,
        bytes,
        choice,
        gpu,
        progress,
        &Limits::current(),
        allowance,
    ))
}

/// The sheet document for an SHP's bytes (which pass `sniff`): the window
/// of its first frames that fits `limits` (every frame when they all fit).
/// Only the window's frames are decoded; the rest stay in the original file
/// the resource keeps.
pub(in crate::cxxqt_object) fn open_sheet(
    path: &str,
    bytes: &[u8],
    choice: PaletteChoice,
    gpu: bool,
    progress: &dyn Fn(u8, &str),
    limits: &Limits,
    allowance: u64,
) -> Option<OpenedDocument> {
    progress(30, "Decoding frames");
    // Planned from the file header before any frame is decoded.
    let plan = open_plan(bytes, limits, allowance)?;
    let (width, height, total, count) = (
        plan.width as usize,
        plan.height as usize,
        plan.total as usize,
        plan.count as usize,
    );
    let layout = Layout::new(width, height, count, sheet::DEFAULT_GUTTER)?;
    if let Err(reason) = limits.admit(layout.sheet_bytes(), allowance) {
        set_error(&format!(
            "{} frames of {}x{}: {reason}",
            total, width, height
        ));
        return None;
    }
    let planes: Vec<Vec<u8>> = (0..count).map(|i| frame_canvas(bytes, i)).collect();
    progress(70, "Compositing");
    let name = Path::new(path)
        .file_stem()
        .map(|stem| stem.to_string_lossy().into_owned())
        .unwrap_or_default();
    let header = Header {
        layout,
        palette: choice.palette,
        first: 0,
        total,
        span: count,
    };
    let data = resource::encode_parts(&header, &choice.path, bytes, std::iter::empty(), None);
    progress(90, "Preparing layers");
    let label = window_label(&header);
    let (doc, history, target) =
        window_document(&name, &layout, &planes, &header.palette, &data, gpu, &label);
    Some(OpenedDocument {
        doc,
        history,
        target,
        path: OpenedPath::Set(Some(path.to_string())),
        source_format: "shp".to_string(),
        pdf_password: None,
    })
}

/// The document's SHP header, once its mode and size still fit the layout.
fn checked_header(doc: &Document) -> Result<Header, String> {
    let Some(header) = header(doc) else {
        return fail(
            "This document is not an SHP sheet. Saving other images as SHP is not supported yet.",
        );
    };
    if doc.mode != ColorMode::Rgb || doc.depth != BitDepth::Eight {
        return fail("An SHP sheet must stay in RGB Color, 8 bits per channel.");
    }
    let l = &header.layout;
    if doc.width as usize != l.sheet_width() || doc.height as usize != l.sheet_height() {
        return fail(format!(
            "The sheet is {}x{} px but its {} frames of {}x{} need {}x{} px. Undo the size change \
             (Image Size, Canvas Size, Crop, Rotate) before saving as SHP.",
            doc.width,
            doc.height,
            l.frames,
            l.width,
            l.height,
            l.sheet_width(),
            l.sheet_height()
        ));
    }
    Ok(header)
}

/// The flattened sheet as straight RGBA, read through the CPU compositor
/// (the reference; the GPU path is only an accelerator).
fn sheet_pixels(doc: &Document) -> Result<(Header, Vec<u8>), String> {
    let header = checked_header(doc)?;
    let rgba = buffer_to_rgba_bytes(&current_buffer(doc, false));
    Ok((header, rgba))
}

/// Encode the document as SHP bytes: the original file verbatim when no
/// frame changed, else frame by frame keeping every untouched frame's bytes.
/// A paged document adds the frame store's edited frames and hands every
/// other frame off the sheet over as unchanged, never decoding them.
pub(in crate::cxxqt_object) fn encode(doc: &Document) -> Result<Vec<u8>, String> {
    let (header, rgba) = sheet_pixels(doc)?;
    let unique = unique_colours(&header.palette);
    let scan = sheet::from_sheet(&header.layout, &rgba, &sheet::inverse(&unique));
    drop(rgba);
    if scan.off_palette_total() > 0 {
        return fail(format!(
            "{} pixels are not palette colours; snap them to the palette first.",
            scan.off_palette_total()
        ));
    }
    if scan.gutter > 0 {
        return fail(format!(
            "{} pixels lie in the gaps between frames; clear the gutters first.",
            scan.gutter
        ));
    }
    let block = block(doc).ok_or("missing SHP data")?;
    let (_, _, original) = resource::parts(block).ok_or("missing SHP data")?;
    let stored = resource::stored(block).ok_or("damaged SHP data")?;
    let origins = resource::origins(block).ok_or("damaged SHP data")?;
    let in_order =
        origins == resource::identity_origins(header.total, resource::file_frames(original));
    let source = if original.is_empty() {
        None
    } else {
        Source::read(original.to_vec()).ok()
    };
    let l = &header.layout;
    let (w, h) = (l.width as u16, l.height as u16);
    if !header.paged() && stored.is_empty() && in_order {
        return build_preserving(w, h, &scan.planes, source.as_ref())
            .map_err(|e| format!("cannot write the SHP: {e:?}"));
    }
    if header.paged() && source.is_none() {
        return fail("This paged SHP sheet has lost its source file.");
    }
    let window = header.window();
    let unchanged = in_order
        && stored.is_empty()
        && source.is_some()
        && header.total == resource::file_frames(original)
        && window
            .clone()
            .all(|f| scan.planes[f - header.first] == frame_canvas(original, f));
    if unchanged {
        return Ok(original.to_vec());
    }
    let blank = || FramePlane::Pixels(Cow::Owned(vec![0u8; l.width * l.height]));
    build_mapped(
        w,
        h,
        header.total,
        source.as_ref(),
        |f| origins[f],
        |f| {
            if window.contains(&f) {
                FramePlane::Pixels(Cow::Borrowed(&scan.planes[f - header.first]))
            } else if let Some((_, data)) = stored.iter().find(|(s, _)| *s == f) {
                FramePlane::Pixels(Cow::Owned(resource::stored_canvas(data, l.width, l.height)))
            } else if origins[f].is_some() {
                FramePlane::Unchanged
            } else {
                blank()
            }
        },
    )
    .map_err(|e| format!("cannot write the SHP: {e:?}"))
}

/// Save the document to `path` as an SHP.
pub(in crate::cxxqt_object) fn save_shp(mut view: Pin<&mut PictureView>, path: &str) -> bool {
    let encoded = match view.rust().session.doc.as_ref() {
        Some(doc) => encode(doc),
        None => fail("no document"),
    };
    let bytes = match encoded {
        Ok(bytes) => bytes,
        Err(message) => {
            set_error(&message);
            return false;
        }
    };
    if let Err(e) = write_atomically(path, &bytes, true) {
        set_error(&format!("cannot write {path}: {e}"));
        return false;
    }
    set_error("");
    let mut rust = view.as_mut().rust_mut();
    rust.path = Some(path.to_string());
    rust.session.mark_saved();
    true
}

/// The document's only layer is a full-canvas, plain pixel layer: what the
/// recorded SHP edits and paging work on.
pub(in crate::cxxqt_object) fn plain_sheet_layer(doc: &Document) -> Result<(), String> {
    let (w, h) = (doc.width as i32, doc.height as i32);
    let plane = doc.width as usize * doc.height as usize;
    if doc.layers.len() != 1 {
        return fail(
            "Flatten the image first (Layer > Flatten Image): this works on a single layer.",
        );
    }
    let layer = &doc.layers[0];
    let plain = !layer.is_group
        && layer.adjustment.is_none()
        && layer.mask.is_none()
        && layer.blend == BlendMode::Normal
        && layer.opacity == 255
        && layer.fill == 255
        && (
            layer.rect.left,
            layer.rect.top,
            layer.rect.right,
            layer.rect.bottom,
        ) == (0, 0, w, h)
        && [0i16, 1, 2, -1].iter().all(|id| {
            layer
                .channels
                .iter()
                .any(|c| c.id == *id && c.data.len() == plane)
        });
    if !plain {
        return fail(
            "The sheet layer must be a plain full-canvas pixel layer with transparency \
             (Normal, 100% opacity, no mask).",
        );
    }
    Ok(())
}

/// The single full-canvas, plain pixel layer the recorded SHP edits work on.
fn sheet_layer(doc: &mut Document) -> Result<&mut Layer, String> {
    plain_sheet_layer(doc)?;
    Ok(&mut doc.layers[0])
}

fn layer_rgba(layer: &Layer, plane: usize) -> Vec<u8> {
    let mut rgba = vec![0u8; plane * 4];
    for (k, id) in [0i16, 1, 2, -1].into_iter().enumerate() {
        if let Some(channel) = layer.channels.iter().find(|c| c.id == id) {
            for (i, v) in channel.data.iter().enumerate() {
                rgba[i * 4 + k] = *v;
            }
        }
    }
    rgba
}

fn store_layer_rgba(layer: &mut Layer, rgba: &[u8]) {
    for (k, id) in [0i16, 1, 2, -1].into_iter().enumerate() {
        if let Some(channel) = layer.channels.iter_mut().find(|c| c.id == id) {
            let changed = channel
                .data
                .iter()
                .enumerate()
                .any(|(i, v)| *v != rgba[i * 4 + k]);
            if changed {
                let values: Vec<u8> = rgba.iter().skip(k).step_by(4).copied().collect();
                channel.data = values.into();
            }
        }
    }
}

/// Apply `f` to the sheet layer's RGBA; the number of pixels it changed.
fn with_sheet<F>(rust: &mut PictureViewRust, f: F) -> Result<usize, String>
where
    F: FnOnce(&Header, &mut [u8]) -> usize,
{
    let doc = rust.session.doc.as_mut().ok_or("no document")?;
    let header = checked_header(doc)?;
    let plane = doc.width as usize * doc.height as usize;
    let layer = sheet_layer(doc)?;
    let mut rgba = layer_rgba(layer, plane);
    let changed = f(&header, &mut rgba);
    if changed > 0 {
        store_layer_rgba(layer, &rgba);
    }
    Ok(changed)
}

/// The mutation half of "Snap to Palette".
pub(in crate::cxxqt_object) fn snap_rust(rust: &mut PictureViewRust) -> Result<usize, String> {
    with_sheet(rust, |header, rgba| {
        let unique = unique_colours(&header.palette);
        sheet::snap(&header.layout, rgba, &header.palette, &unique)
    })
}

/// The mutation half of "Snap Frame to Palette": file frame `frame`, which
/// must be on the sheet.
pub(in crate::cxxqt_object) fn snap_frame_rust(
    rust: &mut PictureViewRust,
    frame: usize,
) -> Result<usize, String> {
    let header = rust
        .session
        .doc
        .as_ref()
        .and_then(header)
        .ok_or("This document is not an SHP sheet.")?;
    if !header.window().contains(&frame) {
        return fail(format!("Frame {frame} is not on the sheet."));
    }
    with_sheet(rust, |header, rgba| {
        let unique = unique_colours(&header.palette);
        sheet::snap_frame(
            &header.layout,
            rgba,
            &header.palette,
            &unique,
            frame - header.first,
        )
    })
}

/// The mutation half of "Clear Frame Gutters".
pub(in crate::cxxqt_object) fn clear_gutter_rust(
    rust: &mut PictureViewRust,
) -> Result<usize, String> {
    with_sheet(rust, |header, rgba| {
        sheet::clear_gutter(&header.layout, rgba)
    })
}

/// The mutation half of "Change Palette": recolour by index and keep the
/// new palette in the resource. `Ok(false)` when it is the same palette.
pub(in crate::cxxqt_object) fn rebind_rust(
    rust: &mut PictureViewRust,
    choice: PaletteChoice,
) -> Result<bool, String> {
    let doc = rust.session.doc.as_mut().ok_or("no document")?;
    let (mut header, path, original) = block(doc)
        .and_then(resource::parts)
        .map(|(h, p, o)| (h, p, o.to_vec()))
        .ok_or("This document is not an SHP sheet.")?;
    let origins = block(doc)
        .and_then(resource::origins)
        .ok_or("damaged SHP data")?;
    let stored = block(doc)
        .and_then(resource::stored)
        .ok_or("damaged SHP data")?
        .into_iter()
        .map(|(frame, data)| resource::StoredFrame {
            frame,
            data: data.to_vec(),
        })
        .collect();
    if header.palette == choice.palette && path == choice.path {
        return Ok(false);
    }
    let old = unique_colours(&header.palette);
    let new = unique_colours(&choice.palette);
    let plane = doc.width as usize * doc.height as usize;
    let layer = sheet_layer(doc)?;
    let mut rgba = layer_rgba(layer, plane);
    if sheet::rebind(&mut rgba, &old, &new) > 0 {
        store_layer_rgba(layer, &rgba);
    }
    header.palette = choice.palette;
    let data = ShpResource {
        header,
        palette_path: choice.path,
        original,
        stored,
        origins,
    }
    .encode();
    doc.image_resources = set_block(
        &doc.image_resources,
        resource::RESOURCE_ID,
        resource::RESOURCE_NAME,
        &data,
    )
    .into();
    Ok(true)
}

/// Commit one recorded SHP edit; the count, or -1 with the error kept.
fn commit<F>(view: Pin<&mut PictureView>, label: &str, f: F) -> i32
where
    F: FnOnce(&mut PictureViewRust) -> Result<usize, String>,
{
    let mut outcome: Result<usize, String> = fail("no document");
    view.edit(label, |rust| {
        outcome = f(rust);
        match outcome {
            Ok(n) if n > 0 => Edit::Full,
            _ => Edit::None,
        }
    });
    match outcome {
        Ok(n) => {
            set_error("");
            n.min(i32::MAX as usize) as i32
        }
        Err(message) => {
            set_error(&message);
            -1
        }
    }
}

pub(super) fn shp_sniff_file(path: &QString) -> bool {
    let path = path.to_string();
    let Ok(mut file) = std::fs::File::open(&path) else {
        return false;
    };
    let Ok(size) = file.metadata().map(|m| m.len() as usize) else {
        return false;
    };
    let mut head = [0u8; pictura_shp::HEADER];
    if file.read_exact(&mut head).is_err() {
        return false;
    }
    let mut table = head.to_vec();
    table.resize(pictura_shp::table_bytes(&head), 0);
    if file.read_exact(&mut table[pictura_shp::HEADER..]).is_err() {
        return false;
    }
    sniff(&table, Some(size))
}

pub(super) fn shp_set_open_palette(mut view: Pin<&mut PictureView>, path: &QString) {
    view.as_mut()
        .rust_mut()
        .shp_open
        .get_or_insert_with(OpenRequest::default)
        .palette = Some(path.to_string());
}

pub(super) fn shp_info(view: &PictureView) -> ShpInfo {
    let parts = view
        .rust()
        .session
        .doc
        .as_ref()
        .and_then(block)
        .and_then(resource::parts);
    match parts {
        Some((header, path, original)) => ShpInfo {
            valid: true,
            width: header.layout.width as i32,
            height: header.layout.height as i32,
            frames: header.layout.frames as i32,
            cols: header.layout.cols as i32,
            gutter: header.layout.gutter as i32,
            palette_name: QString::from(header.palette.name.as_str()),
            palette_path: QString::from(path.as_str()),
            has_source: !original.is_empty(),
            first: header.first as i32,
            total: header.total as i32,
            span: header.span as i32,
        },
        None => ShpInfo {
            valid: false,
            width: 0,
            height: 0,
            frames: 0,
            cols: 0,
            gutter: 0,
            palette_name: QString::default(),
            palette_path: QString::default(),
            has_source: false,
            first: 0,
            total: 0,
            span: 0,
        },
    }
}

fn view_header(view: &PictureView) -> Option<Header> {
    view.rust().session.doc.as_ref().and_then(header)
}

pub(super) fn shp_palette_unique(view: &PictureView) -> Vec<u32> {
    view_header(view).map_or_else(Vec::new, |h| unique_colours(&h.palette).argb.to_vec())
}

pub(super) fn shp_palette_source(view: &PictureView) -> Vec<u32> {
    view_header(view).map_or_else(Vec::new, |h| h.palette.argb.to_vec())
}

pub(super) fn shp_index_of(view: &PictureView, rgb: u32) -> i32 {
    let Some(header) = view_header(view) else {
        return -1;
    };
    let unique = unique_colours(&header.palette);
    (1..256usize)
        .find(|&i| unique.argb[i] & 0xFF_FFFF == rgb & 0xFF_FFFF)
        .map_or(-1, |i| i as i32)
}

pub(super) fn shp_role(view: &PictureView, index: i32) -> QString {
    match (view_header(view), u8::try_from(index)) {
        (Some(header), Ok(index)) => QString::from(role(&header.palette.name, index).label()),
        _ => QString::default(),
    }
}

/// [`shp_check`] over a document, for tests.
pub(in crate::cxxqt_object) fn check(doc: &Document) -> ShpCheck {
    let empty = |error: &str| ShpCheck {
        error: QString::from(error),
        off_palette: 0,
        off_frames: Vec::new(),
        first_off: Vec::new(),
        gutter: 0,
    };
    let (header, rgba) = match sheet_pixels(doc) {
        Ok(found) => found,
        Err(message) => return empty(&message),
    };
    let unique = unique_colours(&header.palette);
    let scan = sheet::from_sheet(&header.layout, &rgba, &sheet::inverse(&unique));
    let first_frame = scan.off_frames().first().copied();
    let off_frames: Vec<i32> = scan
        .off_frames()
        .into_iter()
        .map(|i| (header.first + i) as i32)
        .collect();
    let first_off = first_frame
        .and_then(|i| scan.off_bounds[i])
        .map_or_else(Vec::new, |(x, y, w, h)| {
            vec![x as i32, y as i32, w as i32, h as i32]
        });
    ShpCheck {
        error: QString::default(),
        off_palette: scan.off_palette_total().min(i32::MAX as usize) as i32,
        off_frames,
        first_off,
        gutter: scan.gutter.min(i32::MAX as usize) as i32,
    }
}

pub(super) fn shp_check(view: &PictureView) -> ShpCheck {
    match view.rust().session.doc.as_ref() {
        Some(doc) => check(doc),
        None => ShpCheck {
            error: QString::from("no document"),
            off_palette: 0,
            off_frames: Vec::new(),
            first_off: Vec::new(),
            gutter: 0,
        },
    }
}

pub(super) fn shp_snap(view: Pin<&mut PictureView>) -> i32 {
    commit(view, "Snap to Palette", snap_rust)
}

pub(super) fn shp_snap_frame(view: Pin<&mut PictureView>, frame: i32) -> i32 {
    let Ok(frame) = usize::try_from(frame) else {
        set_error("no such frame");
        return -1;
    };
    commit(view, "Snap Frame to Palette", |rust| {
        snap_frame_rust(rust, frame)
    })
}

pub(super) fn shp_off_palette_points(view: &PictureView, limit: i32) -> ShpOffPoints {
    let found = view.rust().session.doc.as_ref().map(sheet_pixels);
    let Some(Ok((header, rgba))) = found else {
        return ShpOffPoints {
            total: 0,
            xy: Vec::new(),
        };
    };
    let unique = unique_colours(&header.palette);
    let limit = usize::try_from(limit).unwrap_or(0);
    let (total, points) =
        sheet::off_palette_points(&header.layout, &rgba, &sheet::inverse(&unique), limit);
    ShpOffPoints {
        total: total.min(i32::MAX as usize) as i32,
        xy: points
            .into_iter()
            .flat_map(|(x, y)| [x as i32, y as i32])
            .collect(),
    }
}

pub(super) fn shp_clear_gutter(view: Pin<&mut PictureView>) -> i32 {
    commit(view, "Clear Frame Gutters", clear_gutter_rust)
}

pub(super) fn shp_rebind_palette(view: Pin<&mut PictureView>, path: &QString) -> bool {
    let path = path.to_string();
    let Some(choice) = PaletteChoice::try_load(&path) else {
        set_error(&format!(
            "{path} is not a palette (768-byte VGA or JASC-PAL)."
        ));
        return false;
    };
    commit(view, "Change Palette", |rust| {
        rebind_rust(rust, choice).map(usize::from)
    }) >= 0
}

pub(super) fn shp_plan_file(path: &QString, count: i32) -> ShpPlan {
    let path = path.to_string();
    let mut head = [0u8; pictura_shp::HEADER];
    let read = std::fs::File::open(&path).and_then(|mut f| f.read_exact(&mut head));
    if read.is_err() || !shp_sniff_file(&QString::from(path.as_str())) {
        return invalid_plan();
    }
    let limits = Limits::current();
    let Some(default) = open_plan(&head, &limits, 0) else {
        return invalid_plan();
    };
    match usize::try_from(count) {
        Ok(count) if count > 0 => plan(
            default.width as usize,
            default.height as usize,
            default.total as usize,
            0,
            count,
            &limits,
        )
        .unwrap_or_else(invalid_plan),
        _ => default,
    }
}

pub(super) fn shp_plan_window(view: &PictureView, frame: i32, count: i32) -> ShpPlan {
    let (Some(header), Ok(frame), Ok(count)) = (
        view_header(view),
        usize::try_from(frame),
        usize::try_from(count),
    ) else {
        return invalid_plan();
    };
    if frame >= header.total {
        return invalid_plan();
    }
    let count = if count == 0 { header.span } else { count }.clamp(1, header.total);
    let l = &header.layout;
    let first = sheet::window_start(frame, count);
    plan(
        l.width,
        l.height,
        header.total,
        first,
        count,
        &Limits::current(),
    )
    .unwrap_or_else(invalid_plan)
}

pub(super) fn shp_set_open_allowance(mut view: Pin<&mut PictureView>, bytes: u64) {
    view.as_mut()
        .rust_mut()
        .shp_open
        .get_or_insert_with(OpenRequest::default)
        .allowance = bytes;
}

pub(super) fn shp_show_frames(
    mut view: Pin<&mut PictureView>,
    frame: i32,
    count: i32,
    allowance: u64,
) -> bool {
    let (Ok(frame), Ok(count)) = (usize::try_from(frame), usize::try_from(count)) else {
        set_error("no such frame");
        return false;
    };
    let outcome = {
        let mut rust = view.as_mut().rust_mut();
        if rust.modal_transform_blocks_edit() {
            Err("Finish the transform first.".to_string())
        } else {
            page_rust(&mut rust, frame, count, &Limits::current(), allowance)
        }
    };
    match outcome {
        Ok(()) => {
            set_error("");
            view.as_mut().recomposite();
            view.as_mut().session_cancelled();
            true
        }
        Err(message) => {
            set_error(&message);
            false
        }
    }
}

pub(super) fn shp_frame_thumbnail(view: &PictureView, index: i32, side: i32) -> ShpThumb {
    match (view.rust().session.doc.as_ref(), usize::try_from(index)) {
        (Some(doc), Ok(index)) => thumbnail(doc, index, usize::try_from(side).unwrap_or(1)),
        _ => ShpThumb {
            width: 0,
            height: 0,
            argb: Vec::new(),
        },
    }
}

pub(super) fn shp_frame_stamp(view: &PictureView, index: i32) -> u64 {
    match (view.rust().session.doc.as_ref(), usize::try_from(index)) {
        (Some(doc), Ok(index)) => frame_stamp(doc, index).unwrap_or(u64::MAX),
        _ => u64::MAX,
    }
}

pub(super) fn shp_set_test_limits(limit: u64, available: u64) {
    set_test_limits(limit, available);
}

pub(super) fn shp_footprint_factor() -> u64 {
    FOOTPRINT_FACTOR
}

pub(super) fn shp_size_limit() -> u64 {
    Limits::current().limit
}

pub(super) fn shp_last_error() -> QString {
    LAST_ERROR.with(|e| QString::from(e.borrow().as_str()))
}

pub(super) fn shp_plan_frames(
    view: &PictureView,
    op: &QString,
    first: i32,
    count: i32,
    to: i32,
) -> ShpPlan {
    let doc = view.rust().session.doc.as_ref();
    parse_op(&op.to_string(), first, count, to)
        .zip(doc)
        .and_then(|(op, doc)| frame_op_plan(doc, op, &Limits::current()))
        .unwrap_or_else(invalid_plan)
}

pub(super) fn shp_frame_op(
    mut view: Pin<&mut PictureView>,
    op: &QString,
    first: i32,
    count: i32,
    to: i32,
    allowance: u64,
) -> i32 {
    let Some(op) = parse_op(&op.to_string(), first, count, to) else {
        set_error("That frame operation does not apply here.");
        return -1;
    };
    let paged = view_header(&view).map(|h| h.paged());
    let limits = Limits::current();
    match paged {
        None => {
            set_error("This document is not an SHP sheet.");
            -1
        }
        Some(false) => {
            let mut focus = 0;
            let done = commit(view, op.label(), |rust| {
                if rust.modal_transform_blocks_edit() {
                    return fail("Finish the transform first.");
                }
                plain_sheet_layer(rust.session.doc.as_ref().ok_or("no document")?)?;
                focus = whole_op_rust(rust, op, &limits, allowance)?;
                Ok(1)
            });
            if done < 0 {
                -1
            } else {
                focus as i32
            }
        }
        Some(true) => {
            let outcome = {
                let mut rust = view.as_mut().rust_mut();
                if rust.modal_transform_blocks_edit() {
                    Err("Finish the transform first.".to_string())
                } else {
                    paged_op_rust(&mut rust, op, &limits, allowance)
                }
            };
            match outcome {
                Ok(focus) => {
                    set_error("");
                    view.as_mut().recomposite();
                    view.as_mut().session_cancelled();
                    focus as i32
                }
                Err(message) => {
                    set_error(&message);
                    -1
                }
            }
        }
    }
}

#[path = "window.rs"]
mod window;
use window::*;

#[path = "frames.rs"]
mod frames;
use frames::*;
pub(super) use frames::{shp_frame_size, shp_plan_frame_size};

#[path = "unit.rs"]
mod unit;
pub(super) use unit::{
    shp_clear_shadows, shp_generate_shadows, shp_plan_shadow_half, shp_shadow_detect,
    shp_shadow_violations,
};

#[path = "authoring.rs"]
mod authoring;
pub(super) use authoring::{shp_convert, shp_new_document, shp_plan_sheet};

#[cfg(test)]
#[path = "tests.rs"]
mod tests;

#[cfg(test)]
#[path = "window_tests.rs"]
mod window_tests;

#[cfg(test)]
#[path = "frames_tests.rs"]
mod frames_tests;

#[cfg(test)]
#[path = "authoring_tests.rs"]
mod authoring_tests;

#[cfg(test)]
#[path = "unit_tests.rs"]
mod unit_tests;
