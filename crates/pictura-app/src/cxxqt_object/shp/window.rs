// SPDX-License-Identifier: GPL-3.0-or-later
//! Paged SHP documents: the sheet holds a **window** of frames, sized so
//! memory follows the frames loaded rather than the file, and the frames
//! outside it live in the `KookaSHP` resource (the original file plus the
//! **frame store** of edited frames, `pictura_shp::resource`).
//!
//! Also the size limit every SHP document is held to, the allowance the
//! shell can grant past it (the Advanced preferences), and the floor no
//! setting removes: the estimated real footprint must fit the memory
//! available right now.

use super::*;

use crate::cxxqt_object::helpers_composite::{buffer_to_rgba_bytes, current_buffer};
use crate::history::History;
use pictura_shp::frame_canvas;
use pictura_shp::resource::{encode_parts, stored, stored_canvas};
use pictura_shp::sheet::{window_frames, window_start, Layout, DEFAULT_GUTTER};
use std::collections::BTreeMap;
use std::sync::atomic::{AtomicU64, Ordering};

/// The most sheet RGBA one SHP document holds before it pages (and the size
/// limit the shell's Advanced allowance lifts). Measured 2026-11-02 (peak
/// RSS, headless app): about 0.5 GB of base plus 9.6x the sheet's RGBA at
/// open, 12.5x after two fills, and 18x on `sppnuke.shp`'s 185 MB window
/// after a fill and two saves (3.9 GB). 256 MiB is so about 2.6 GB at open
/// and under 5 GB at worst. Every SHP of the owner's 4,810-file corpus but
/// 21 (the largest 343 MiB, `sppnuke.shp` 3.1 GiB) opens whole under it.
pub(in crate::cxxqt_object) const WINDOW_BUDGET: u64 = 256 << 20;

/// Real resident memory per byte of sheet RGBA, rounded up from the worst
/// measured case (18x: a window edited and saved twice, see
/// [`WINDOW_BUDGET`]). The estimate shown, the floor no setting removes
/// (estimate within `MemAvailable`) and the window budget when memory is short
/// (`MemAvailable / FOOTPRINT_FACTOR`) use it.
pub(in crate::cxxqt_object) const FOOTPRINT_FACTOR: u64 = 20;

static TEST_LIMIT: AtomicU64 = AtomicU64::new(0);
static TEST_AVAILABLE: AtomicU64 = AtomicU64::new(0);

/// Test seam: a size limit and available memory in bytes instead of the real
/// ones (0 keeps the real value).
pub(in crate::cxxqt_object) fn set_test_limits(limit: u64, available: u64) {
    TEST_LIMIT.store(limit, Ordering::Relaxed);
    TEST_AVAILABLE.store(available, Ordering::Relaxed);
}

/// `MemAvailable` from `/proc/meminfo`, in bytes; `None` where it cannot be
/// read.
fn mem_available() -> Option<u64> {
    let info = std::fs::read_to_string("/proc/meminfo").ok()?;
    let line = info.lines().find(|l| l.starts_with("MemAvailable:"))?;
    let kb: u64 = line.split_whitespace().nth(1)?.parse().ok()?;
    Some(kb * 1024)
}

/// The size limit and the memory available, read when needed.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(in crate::cxxqt_object) struct Limits {
    pub(in crate::cxxqt_object) limit: u64,
    pub(in crate::cxxqt_object) available: Option<u64>,
}

impl Limits {
    pub(in crate::cxxqt_object) fn current() -> Limits {
        let limit = match TEST_LIMIT.load(Ordering::Relaxed) {
            0 => WINDOW_BUDGET,
            v => v,
        };
        let available = match TEST_AVAILABLE.load(Ordering::Relaxed) {
            0 => mem_available(),
            v => Some(v),
        };
        Limits { limit, available }
    }

    /// The bytes a window opened or paged without asking may take: the
    /// limit, or less when memory is short.
    pub(in crate::cxxqt_object) fn window_budget(&self) -> u64 {
        match self.available {
            Some(available) => self.limit.min(available / FOOTPRINT_FACTOR),
            None => self.limit,
        }
    }

    /// Where `needed` bytes of sheet stand against the limit.
    pub(in crate::cxxqt_object) fn assess(&self, needed: u64) -> Need {
        let stage = if needed <= self.limit {
            0
        } else if needed.saturating_mul(10) <= self.limit.saturating_mul(11) {
            1
        } else {
            2
        };
        let estimate = needed.saturating_mul(FOOTPRINT_FACTOR);
        Need {
            needed,
            stage,
            estimate,
            floor_ok: self.available.is_none_or(|a| estimate <= a),
        }
    }

    /// May a sheet of `needed` bytes be held, with `allowance` bytes granted
    /// past the limit by the user? The reason when not.
    pub(in crate::cxxqt_object) fn admit(&self, needed: u64, allowance: u64) -> Result<(), String> {
        let need = self.assess(needed);
        if !need.floor_ok {
            return fail(format!(
                "It needs about {} of memory and {} is available now.",
                mib(need.estimate),
                mib(self.available.unwrap_or(0))
            ));
        }
        if need.stage > 0 && needed > allowance {
            return fail(format!(
                "{} of sheet is over the {} SHP size limit.",
                mib(needed),
                mib(self.limit)
            ));
        }
        Ok(())
    }
}

fn mib(bytes: u64) -> String {
    format!("{:.0} MiB", bytes as f64 / f64::from(1u32 << 20))
}

/// A size measured against the limit.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(in crate::cxxqt_object) struct Need {
    pub(in crate::cxxqt_object) needed: u64,
    /// 0 within the limit, 1 within 10% over it, 2 further.
    pub(in crate::cxxqt_object) stage: u8,
    /// The estimated real footprint.
    pub(in crate::cxxqt_object) estimate: u64,
    /// The estimate fits the memory available (or that is unknown).
    pub(in crate::cxxqt_object) floor_ok: bool,
}

/// The plan for a window of `count` frames from `first` (clamped to the
/// file), or `None` when no sheet can hold it.
pub(in crate::cxxqt_object) fn plan(
    width: usize,
    height: usize,
    total: usize,
    first: usize,
    count: usize,
    limits: &Limits,
) -> Option<ShpPlan> {
    if first >= total {
        return None;
    }
    let count = count.clamp(1, total - first);
    let layout = Layout::new(width, height, count, DEFAULT_GUTTER)?;
    let need = limits.assess(layout.sheet_bytes());
    Some(ShpPlan {
        valid: true,
        width: width as i32,
        height: height as i32,
        total: total as i32,
        first: first as i32,
        count: count as i32,
        needed: need.needed,
        limit: limits.limit,
        available: limits.available.unwrap_or(0),
        estimate: need.estimate,
        stage: i32::from(need.stage),
        floor_ok: need.floor_ok,
    })
}

/// The window an SHP opens with: from frame 0, as many frames as the window
/// budget holds, or as `allowance` (bytes of sheet the user agreed to past
/// the limit) holds when that is more. Header only; nothing is decoded.
pub(in crate::cxxqt_object) fn open_plan(
    bytes: &[u8],
    limits: &Limits,
    allowance: u64,
) -> Option<ShpPlan> {
    let field = |at: usize| {
        Some(usize::from(u16::from_le_bytes([
            *bytes.get(at)?,
            *bytes.get(at + 1)?,
        ])))
    };
    let (width, height, total) = (field(2)?, field(4)?, field(6)?);
    if width == 0 || height == 0 || total == 0 {
        return None;
    }
    let budget = limits.window_budget().max(allowance);
    let count = window_frames(width, height, total, DEFAULT_GUTTER, budget);
    plan(width, height, total, 0, count, limits)
}

pub(in crate::cxxqt_object) fn invalid_plan() -> ShpPlan {
    ShpPlan {
        valid: false,
        width: 0,
        height: 0,
        total: 0,
        first: 0,
        count: 0,
        needed: 0,
        limit: 0,
        available: 0,
        estimate: 0,
        stage: 0,
        floor_ok: false,
    }
}

/// The document for a window: `planes` (full-canvas indices of the frames
/// in `layout`, in order) drawn with the palette, the resource `data`, a
/// fresh history whose first state is `label`.
pub(in crate::cxxqt_object) fn window_document(
    name: &str,
    layout: &Layout,
    planes: &[Vec<u8>],
    palette: &Palette,
    data: &[u8],
    gpu: bool,
    label: &str,
) -> (Document, History, LayerTarget) {
    let unique = unique_colours(palette);
    let rgba = sheet::to_sheet(layout, planes, &unique);
    let (sw, sh) = (layout.sheet_width() as u32, layout.sheet_height() as u32);
    // No finalize_import: an all-opaque sheet must keep its alpha channel,
    // or index 0 could never be painted. No ICC either: the sheet is sRGB.
    let mut doc = Document::from_rgba(name, sw, sh, &rgba);
    drop(rgba);
    doc.image_resources =
        set_block(&[], resource::RESOURCE_ID, resource::RESOURCE_NAME, data).into();
    let rendered = current_buffer(&doc, gpu);
    crate::cxxqt_object::helpers_composite::store_composite(&mut doc, &rendered);
    drop(rendered);
    let target = LayerTarget::for_opened(&doc, Some("0".to_string()));
    let mut history = History::default();
    history.capture_live(&mut doc, &None, &target, label);
    (doc, history, target)
}

/// The history label of a window's first state.
pub(in crate::cxxqt_object) fn window_label(header: &Header) -> String {
    if header.paged() {
        format!(
            "Frames {}-{} of {}",
            header.first,
            header.first + header.layout.frames - 1,
            header.total
        )
    } else {
        "Open".to_string()
    }
}

/// Frame `frame`'s full canvas as the document holds it now: the frame
/// store's copy when it was edited off the sheet, else the original file's.
/// Not for frames on the sheet.
pub(in crate::cxxqt_object) fn stored_or_original(
    original: &[u8],
    stored: &[(usize, &[u8])],
    frame: usize,
    width: usize,
    height: usize,
) -> Vec<u8> {
    match stored.iter().find(|(f, _)| *f == frame) {
        Some((_, data)) => stored_canvas(data, width, height),
        None => frame_canvas(original, frame),
    }
}

/// The checks every recorded paging needs: one plain sheet layer whose
/// pixels all map to palette indices, with clean gutters.
fn clean_window(doc: &Document) -> Result<(Header, sheet::Scan), String> {
    let header = checked_header(doc)?;
    plain_sheet_layer(doc)?;
    let rgba = buffer_to_rgba_bytes(&current_buffer(doc, false));
    let unique = unique_colours(&header.palette);
    let scan = sheet::from_sheet(&header.layout, &rgba, &sheet::inverse(&unique));
    if scan.off_palette_total() > 0 {
        return fail(format!(
            "{} pixels are not palette colours; snap them to the palette before changing frames.",
            scan.off_palette_total()
        ));
    }
    if scan.gutter > 0 {
        return fail(format!(
            "{} pixels lie in the gaps between frames; clear the gutters before changing frames.",
            scan.gutter
        ));
    }
    Ok((header, scan))
}

/// Show the window of `span` frames that holds `frame` (windows are
/// aligned to multiples of `span`; 0 keeps the document's window size). The outgoing frames that differ from
/// the original go into the frame store, the incoming ones come out of it,
/// and history starts again at the new window: holding every window's
/// pixels for undo would hold the whole file, which is what paging avoids.
/// `allowance` is how far past the limit the user agreed to go.
pub(in crate::cxxqt_object) fn page_rust(
    rust: &mut PictureViewRust,
    frame: usize,
    span: usize,
    limits: &Limits,
    allowance: u64,
) -> Result<(), String> {
    let doc = rust.session.doc.as_ref().ok_or("no document")?;
    let header = header(doc).ok_or("This document is not an SHP sheet.")?;
    let (w, h, total) = (header.layout.width, header.layout.height, header.total);
    if frame >= total {
        return fail(format!("There is no frame {frame}."));
    }
    let span = if span == 0 { header.span } else { span }.clamp(1, total);
    let first = window_start(frame, span);
    let count = span.min(total - first);
    let layout = Layout::new(w, h, count, header.layout.gutter)
        .ok_or("That many frames do not fit one sheet.")?;
    limits.admit(layout.sheet_bytes(), allowance)?;
    let (header, scan) = clean_window(doc)?;
    let block = block(doc).ok_or("missing SHP data")?;
    let (_, palette_path, original) = resource::parts(block).ok_or("missing SHP data")?;
    if original.is_empty() {
        return fail("This SHP sheet has no source file to page through.");
    }
    let mut store: BTreeMap<usize, Vec<u8>> = stored(block)
        .ok_or("damaged SHP data")?
        .into_iter()
        .map(|(f, d)| (f, d.to_vec()))
        .collect();
    for (i, plane) in scan.planes.iter().enumerate() {
        let f = header.first + i;
        if *plane == frame_canvas(original, f) {
            store.remove(&f);
        } else {
            store.insert(f, pictura_shp::encode_rle(plane, w));
        }
    }
    drop(scan);
    let window = first..first + count;
    let planes: Vec<Vec<u8>> = window
        .clone()
        .map(|f| match store.get(&f) {
            Some(data) => stored_canvas(data, w, h),
            None => frame_canvas(original, f),
        })
        .collect();
    store.retain(|f, _| !window.contains(f));
    let next = Header {
        layout,
        palette: header.palette.clone(),
        first,
        total,
        span,
    };
    let data = encode_parts(
        &next,
        &palette_path,
        original,
        store.iter().map(|(f, d)| (*f, &d[..])),
    );
    drop(store);
    let name = doc.layers[0].name.clone();

    // The outgoing window and its history go before the next one is built,
    // so the two never sit in memory together.
    let (path, format, dirty, gpu) = (
        rust.path.clone(),
        rust.source_format.clone(),
        rust.session.dirty,
        rust.gpu_compute,
    );
    rust.session.doc = None;
    rust.session.history = History::default();
    let label = window_label(&next);
    let (doc, history, target) =
        window_document(&name, &layout, &planes, &next.palette, &data, gpu, &label);
    drop(planes);
    rust.replace_document(Some(doc), history, target, None);
    rust.path = path;
    rust.source_format = format;
    if dirty {
        rust.session.mark_unsaved();
    }
    Ok(())
}

/// A small picture of frame `index` for the Frames panel, at most `side`
/// pixels a side (nearest neighbour, unique palette colours), decoded from
/// the frame store or the original file. Empty for a frame on the sheet:
/// the panel reads those from the canvas.
pub(in crate::cxxqt_object) fn thumbnail(doc: &Document, index: usize, side: usize) -> ShpThumb {
    let empty = ShpThumb {
        width: 0,
        height: 0,
        argb: Vec::new(),
    };
    let Some(block) = block(doc) else {
        return empty;
    };
    let Some((header, _, original)) = resource::parts(block) else {
        return empty;
    };
    let l = &header.layout;
    if index >= header.total || header.window().contains(&index) || original.is_empty() {
        return empty;
    }
    let stored = stored(block).unwrap_or_default();
    let plane = stored_or_original(original, &stored, index, l.width, l.height);
    let side = side.max(1);
    let scale = (side as f64 / l.width as f64)
        .min(side as f64 / l.height as f64)
        .min(1.0);
    let tw = ((l.width as f64 * scale) as usize).max(1);
    let th = ((l.height as f64 * scale) as usize).max(1);
    let unique = unique_colours(&header.palette);
    let mut argb = Vec::with_capacity(tw * th);
    for y in 0..th {
        let sy = (y * l.height / th).min(l.height - 1);
        for x in 0..tw {
            let sx = (x * l.width / tw).min(l.width - 1);
            let i = plane[sy * l.width + sx];
            argb.push(if i == 0 {
                0
            } else {
                0xFF00_0000 | unique.argb[usize::from(i)]
            });
        }
    }
    ShpThumb {
        width: tw as i32,
        height: th as i32,
        argb,
    }
}

/// What frame `index`'s thumbnail depends on besides the palette: `None`
/// for a frame on the sheet (or none at all), 0 for an original frame, and a
/// hash of the frame store's copy for an edited one.
pub(in crate::cxxqt_object) fn frame_stamp(doc: &Document, index: usize) -> Option<u64> {
    let block = block(doc)?;
    let header = Header::read(block)?;
    if index >= header.total || header.window().contains(&index) {
        return None;
    }
    let stored = stored(block)?;
    Some(match stored.iter().find(|(f, _)| *f == index) {
        // FNV-1a, nudged off 0.
        Some((_, data)) => data
            .iter()
            .fold(0xcbf2_9ce4_8422_2325u64, |h, &b| {
                (h ^ u64::from(b)).wrapping_mul(0x0100_0000_01b3)
            })
            .max(1),
        None => 0,
    })
}
