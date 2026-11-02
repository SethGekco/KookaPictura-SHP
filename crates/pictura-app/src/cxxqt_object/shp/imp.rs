// SPDX-License-Identifier: GPL-3.0-or-later
//! The SHP document glue: open as a sheet, the palette queries, the
//! recorded snap / clear / rebind edits, and the byte-preserving save.

use super::ffi::{ShpCheck, ShpInfo};
use super::*;

use crate::cxxqt_object::edit::Edit;
use crate::cxxqt_object::helpers::safe_io::{read_capped, write_atomically};
use crate::cxxqt_object::helpers_composite::{
    buffer_to_rgba_bytes, current_buffer, store_composite,
};
use crate::cxxqt_object::open_jobs::{OpenedDocument, OpenedPath};
use crate::cxxqt_object::state::PictureViewRust;
use crate::history::History;
use crate::layer_target::LayerTarget;
use core::pin::Pin;
use cxx_qt::CxxQtType;
use cxx_qt_lib::QString;
use pictura_core::{BitDepth, BlendMode, ColorMode, Document, Layer};
use pictura_shp::palette::{fallback_palette, read_palette, role, unique_colours, Palette};
use pictura_shp::resource::{self, find_block, set_block, Header, ShpResource};
use pictura_shp::sheet::{self, Layout, DEFAULT_GUTTER};
use pictura_shp::{build_preserving, sniff, Source};
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

thread_local! {
    /// The palette an SHP decode on this thread uses; set around one decode
    /// by [`with_open_palette`] / [`with_document_palette`].
    static OPEN_PALETTE: RefCell<Option<PaletteChoice>> = const { RefCell::new(None) };
    static LAST_ERROR: RefCell<String> = const { RefCell::new(String::new()) };
}

fn fail<T>(message: impl Into<String>) -> Result<T, String> {
    Err(message.into())
}

fn set_error(message: &str) {
    LAST_ERROR.with(|e| *e.borrow_mut() = message.to_string());
}

/// Run `f` (a decode) with the palette at `path` (`None` or empty: the
/// fallback) as the one an SHP opens with.
pub(in crate::cxxqt_object) fn with_open_palette<T>(
    path: Option<String>,
    f: impl FnOnce() -> T,
) -> T {
    let choice = PaletteChoice::load(path.as_deref().unwrap_or(""));
    OPEN_PALETTE.with(|p| *p.borrow_mut() = Some(choice));
    let out = f();
    OPEN_PALETTE.with(|p| *p.borrow_mut() = None);
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
    Some(open_sheet(path, bytes.to_vec(), choice, gpu, progress))
}

/// The import budget every other format meets ([`ImageBudget`]): one RGBA
/// sheet must fit a single decoded allocation and the document share of
/// memory. A 136-frame 2052x2816 sprite lays out at 28741x28169 (3.2 GB).
///
/// [`ImageBudget`]: pictura_codec::ImageBudget
pub(in crate::cxxqt_object) fn sheet_fits_budget(layout: &Layout) -> bool {
    let budget = pictura_codec::ImageBudget::default();
    let (w, h) = (layout.sheet_width() as u64, layout.sheet_height() as u64);
    w.max(h) <= u64::from(budget.max_dimension)
        && w * h * 4 <= budget.max_alloc_bytes.min(budget.max_document_bytes)
}

/// The sheet document for an SHP's bytes.
pub(in crate::cxxqt_object) fn open_sheet(
    path: &str,
    bytes: Vec<u8>,
    choice: PaletteChoice,
    gpu: bool,
    progress: &dyn Fn(u8, &str),
) -> Option<OpenedDocument> {
    progress(30, "Decoding frames");
    // The sheet is laid out from the file header before any frame is decoded,
    // so an over-budget sheet costs nothing.
    let field = |at: usize| {
        Some(usize::from(u16::from_le_bytes([
            *bytes.get(at)?,
            *bytes.get(at + 1)?,
        ])))
    };
    let layout = Layout::new(field(2)?, field(4)?, field(6)?, DEFAULT_GUTTER)?;
    if !sheet_fits_budget(&layout) {
        set_error(&format!(
            "{} frames of {}x{} make a {}x{} sheet, over the import budget.",
            layout.frames,
            layout.width,
            layout.height,
            layout.sheet_width(),
            layout.sheet_height()
        ));
        return None;
    }
    let source = Source::read(bytes).ok()?;
    if (
        usize::from(source.width()),
        usize::from(source.height()),
        source.frames().len(),
    ) != (layout.width, layout.height, layout.frames)
    {
        return None;
    }
    let planes = source.canvases();
    let unique = unique_colours(&choice.palette);
    let rgba = sheet::to_sheet(&layout, &planes, &unique);
    progress(70, "Compositing");
    let name = Path::new(path)
        .file_stem()
        .map(|stem| stem.to_string_lossy().into_owned())
        .unwrap_or_default();
    let (sw, sh) = (layout.sheet_width() as u32, layout.sheet_height() as u32);
    // No finalize_import: an all-opaque sheet must keep its alpha channel,
    // or index 0 could never be painted. No ICC either: the sheet is sRGB.
    let mut doc = Document::from_rgba(&name, sw, sh, &rgba);
    let data = ShpResource {
        header: Header {
            layout,
            palette: choice.palette,
        },
        palette_path: choice.path,
        original: source.bytes().to_vec(),
    }
    .encode();
    doc.image_resources =
        set_block(&[], resource::RESOURCE_ID, resource::RESOURCE_NAME, &data).into();
    let rendered = current_buffer(&doc, gpu);
    store_composite(&mut doc, &rendered);
    progress(90, "Preparing layers");
    let target = LayerTarget::for_opened(&doc, Some("0".to_string()));
    let mut history = History::default();
    history.capture_live(&mut doc, &None, &target, "Open");
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
pub(in crate::cxxqt_object) fn encode(doc: &Document) -> Result<Vec<u8>, String> {
    let (header, rgba) = sheet_pixels(doc)?;
    let unique = unique_colours(&header.palette);
    let scan = sheet::from_sheet(&header.layout, &rgba, &sheet::inverse(&unique));
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
    let (_, _, original) = block(doc)
        .and_then(resource::parts)
        .ok_or("missing SHP data")?;
    let source = if original.is_empty() {
        None
    } else {
        Source::read(original.to_vec()).ok()
    };
    let l = &header.layout;
    build_preserving(
        l.width as u16,
        l.height as u16,
        &scan.planes,
        source.as_ref(),
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

/// The single full-canvas, plain pixel layer the recorded SHP edits work on.
fn sheet_layer(doc: &mut Document) -> Result<&mut Layer, String> {
    let (w, h) = (doc.width as i32, doc.height as i32);
    let plane = doc.width as usize * doc.height as usize;
    if doc.layers.len() != 1 {
        return fail(
            "Flatten the image first (Layer > Flatten Image): this works on a single layer.",
        );
    }
    let layer = &mut doc.layers[0];
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
    Ok(layer)
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
    view.as_mut().rust_mut().shp_open_palette = Some(path.to_string());
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
    let off_frames: Vec<i32> = scan.off_frames().into_iter().map(|i| i as i32).collect();
    let first_off = off_frames
        .first()
        .and_then(|&i| scan.off_bounds[i as usize])
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

pub(super) fn shp_last_error() -> QString {
    LAST_ERROR.with(|e| QString::from(e.borrow().as_str()))
}

#[cfg(test)]
#[path = "tests.rs"]
mod tests;
