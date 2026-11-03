// SPDX-License-Identifier: GPL-3.0-or-later
//! Unit-sprite tools on SHP documents: the shadow half of a unit sprite
//! (`pictura_shp::unit`): find it, check it, generate shadows from the art,
//! clear them; the house-color preview's ramp; and the radar color (shown
//! per frame, recomputed on save only when asked).
//!
//! On a whole sheet, generating or clearing is one recorded edit; adding a
//! missing shadow half grows the sheet like Insert Frames, through the size
//! check, in the same edit. On a paged sheet it is applied like a paged
//! frame operation: the window must be clean, frames on the sheet are
//! changed there, the others go into the frame store, and History starts
//! again (a recorded edit would keep the resource, which holds the whole
//! original file, in every undo state).

use super::*;

use crate::cxxqt_object::shp::ffi::ShpRadar;
use pictura_shp::encode_rle;
use pictura_shp::frames::FrameOp;
use pictura_shp::resource::{encode_parts, origins, stored, stored_canvas};
use pictura_shp::sheet::{gutter_pixels, reflow};
use pictura_shp::unit::{self, ShadowParams, Use};
use std::cell::Cell;
use std::collections::BTreeMap;

fn rgb_of(px: &[u8; 4]) -> u32 {
    (u32::from(px[0]) << 16) | (u32::from(px[1]) << 8) | u32::from(px[2])
}

/// The opaque sheet color of the shadow index.
fn shadow_rgb(header: &Header) -> u32 {
    unique_colours(&header.palette).argb[usize::from(unit::SHADOW)] & 0xFF_FFFF
}

/// Each frame's use. Frames on the sheet are read from the composite:
/// transparent is index 0, the opaque color of index 1 a shadow pixel, and
/// anything else (off-palette pixels included) art. The others come from
/// the frame store or the original file, one frame at a time.
pub(in crate::cxxqt_object) fn frame_uses(doc: &Document) -> Result<Vec<Use>, String> {
    let (header, rgba) = sheet_pixels(doc)?;
    let l = header.layout;
    let shadow = shadow_rgb(&header);
    let sw = l.sheet_width();
    let mut uses = vec![Use::Empty; header.total];
    for c in 0..l.frames {
        let (x0, y0) = l.cell_origin(c);
        let mut found = Use::Empty;
        'cell: for y in 0..l.height {
            let at = ((y0 + y) * sw + x0) * 4;
            for px in rgba[at..at + l.width * 4].as_chunks::<4>().0 {
                if px[3] == 0 {
                    continue;
                }
                if px[3] == 255 && rgb_of(px) == shadow {
                    found = Use::Shadow;
                } else {
                    found = Use::Art;
                    break 'cell;
                }
            }
        }
        uses[header.first + c] = found;
    }
    drop(rgba);
    if header.paged() {
        let block = block(doc).ok_or("missing SHP data")?;
        let (_, _, original) = resource::parts(block).ok_or("missing SHP data")?;
        let stored = stored(block).ok_or("damaged SHP data")?;
        let origins = origins(block).ok_or("damaged SHP data")?;
        let window = header.window();
        for (f, found) in uses.iter_mut().enumerate() {
            if window.contains(&f) {
                continue;
            }
            *found = match stored.iter().find(|(s, _)| *s == f) {
                Some((_, data)) => unit::classify(&stored_canvas(data, l.width, l.height)),
                None if original.is_empty() => Use::Empty,
                None => origins[f].map_or(Use::Empty, |j| unit::file_frame_use(original, j)),
            };
        }
    }
    Ok(uses)
}

/// Is `half` the first shadow frame of `total` frames?
fn check_half(total: usize, half: usize) -> Result<(), String> {
    if half == 0 || half * 2 != total {
        return fail(format!(
            "The shadow half of {total} frames would start at frame {}; frame {half} is not it.",
            total / 2
        ));
    }
    Ok(())
}

/// Frame `cell`'s pixels as art presence: 1 where the sheet is not
/// transparent.
fn cell_mask(l: &Layout, rgba: &[u8], cell: usize) -> Vec<u8> {
    let sw = l.sheet_width();
    let (x0, y0) = l.cell_origin(cell);
    let mut out = Vec::with_capacity(l.width * l.height);
    for y in 0..l.height {
        let at = ((y0 + y) * sw + x0) * 4;
        out.extend(
            rgba[at..at + l.width * 4]
                .as_chunks::<4>()
                .0
                .iter()
                .map(|px| u8::from(px[3] != 0)),
        );
    }
    out
}

/// Draw a shadow plane (indices 0 and 1) into cell `cell`: 1 as the
/// opaque color of index 1, 0 transparent.
fn put_shadow(l: &Layout, rgba: &mut [u8], cell: usize, plane: &[u8], shadow: u32) {
    let sw = l.sheet_width();
    let (x0, y0) = l.cell_origin(cell);
    let on = [(shadow >> 16) as u8, (shadow >> 8) as u8, shadow as u8, 255];
    for y in 0..l.height {
        let at = ((y0 + y) * sw + x0) * 4;
        let row = &plane[y * l.width..(y + 1) * l.width];
        for (px, &v) in rgba[at..at + l.width * 4]
            .as_chunks_mut::<4>()
            .0
            .iter_mut()
            .zip(row)
        {
            *px = if v == 0 { [0; 4] } else { on };
        }
    }
}

/// Which shadow frame each generated or cleared frame is, and from which
/// art frame (`None`: cleared).
type Targets = Vec<(usize, Option<usize>)>;

/// The mutation half of "Generate Shadows": each art frame in `art` casts
/// its silhouette into its shadow frame (`half + a`). `half` 0 means the
/// sprite has no shadow half yet: as many blank frames as it has are added
/// first, through the size check. The frames written.
pub(in crate::cxxqt_object) fn generate_rust(
    rust: &mut PictureViewRust,
    half: usize,
    art: &[usize],
    params: ShadowParams,
    limits: &Limits,
    allowance: u64,
) -> Result<usize, String> {
    let doc = rust.session.doc.as_ref().ok_or("no document")?;
    let header = checked_header(doc)?;
    let total = header.total;
    let (half, grow) = if half == 0 {
        (total, true)
    } else {
        check_half(total, half)?;
        (half, false)
    };
    if art.is_empty() || art.iter().any(|&a| a >= half) {
        return fail(format!(
            "Shadows are generated from art frames 0-{}.",
            half - 1
        ));
    }
    let targets: Targets = art.iter().map(|&a| (half + a, Some(a))).collect();
    if header.paged() {
        if grow {
            let insert = FrameOp::Insert {
                at: total,
                count: total,
            };
            paged_op_rust(rust, insert, limits, allowance)?;
        }
        return paged_shadows(rust, &targets, params, "Generate Shadows");
    }
    plain_sheet_layer(doc)?;
    let plane = doc.width as usize * doc.height as usize;
    let rgba = layer_rgba(&doc.layers[0], plane);
    let shadow = shadow_rgb(&header);
    let l = header.layout;
    if !grow {
        let mut rgba = rgba;
        cast(&l, &mut rgba, &targets, params, shadow);
        let doc = rust.session.doc.as_mut().ok_or("no document")?;
        store_layer_rgba(&mut doc.layers[0], &rgba);
        return Ok(targets.len());
    }
    let layout = Layout::new(l.width, l.height, total * 2, l.gutter)
        .ok_or("Twice as many frames do not fit one sheet.")?;
    if layout.sheet_bytes() > l.sheet_bytes() {
        limits.admit(layout.sheet_bytes(), allowance)?;
    }
    let gutter = gutter_pixels(&l, &rgba);
    if gutter > 0 {
        return fail(format!(
            "{gutter} pixels lie in the gaps between frames; clear the gutters before adding \
             shadow frames."
        ));
    }
    let order: Vec<Option<usize>> = (0..total)
        .map(Some)
        .chain(std::iter::repeat_n(None, total))
        .collect();
    let mut next = reflow(&l, &rgba, &layout, &order);
    drop(rgba);
    cast(&layout, &mut next, &targets, params, shadow);
    let block = block(doc).ok_or("missing SHP data")?;
    let (_, path, original) = resource::parts(block).ok_or("missing SHP data")?;
    let original = original.to_vec();
    let next_origins: resource::Origins = origins(block)
        .ok_or("damaged SHP data")?
        .into_iter()
        .chain(std::iter::repeat_n(None, total))
        .collect();
    let palette = header.palette.clone();
    install_whole(
        rust,
        layout,
        palette,
        &next,
        &path,
        &original,
        &next_origins,
    )?;
    Ok(targets.len())
}

/// Cast every target on a whole sheet (file frame = cell). The art is read
/// before any shadow is drawn.
fn cast(l: &Layout, rgba: &mut [u8], targets: &Targets, params: ShadowParams, shadow: u32) {
    let planes: Vec<(usize, Vec<u8>)> = targets
        .iter()
        .map(|&(to, from)| {
            let plane = match from {
                Some(a) => unit::silhouette(&cell_mask(l, rgba, a), l.width, l.height, params),
                None => vec![0u8; l.width * l.height],
            };
            (to, plane)
        })
        .collect();
    for (to, plane) in planes {
        put_shadow(l, rgba, to, &plane, shadow);
    }
}

/// The mutation half of "Clear Shadows": the shadow frames in `frames`
/// (each at least `half`) become transparent.
pub(in crate::cxxqt_object) fn clear_rust(
    rust: &mut PictureViewRust,
    half: usize,
    frames: &[usize],
) -> Result<usize, String> {
    let doc = rust.session.doc.as_ref().ok_or("no document")?;
    let header = checked_header(doc)?;
    check_half(header.total, half)?;
    if frames.is_empty() || frames.iter().any(|&f| f < half || f >= header.total) {
        return fail(format!(
            "Only shadow frames ({half}-{}) are cleared.",
            header.total - 1
        ));
    }
    let targets: Targets = frames.iter().map(|&f| (f, None)).collect();
    if header.paged() {
        return paged_shadows(rust, &targets, ShadowParams::STRAIGHT, "Clear Shadows");
    }
    plain_sheet_layer(doc)?;
    let plane = doc.width as usize * doc.height as usize;
    let mut rgba = layer_rgba(&doc.layers[0], plane);
    cast(
        &header.layout,
        &mut rgba,
        &targets,
        ShadowParams::STRAIGHT,
        shadow_rgb(&header),
    );
    let doc = rust.session.doc.as_mut().ok_or("no document")?;
    store_layer_rgba(&mut doc.layers[0], &rgba);
    Ok(targets.len())
}

/// Generate or clear on a paged sheet: the window must be clean; every new
/// plane is computed from the frames as they are now, then written to the
/// sheet or the frame store, and the window is rebuilt with a fresh History
/// whose first state is `label`.
fn paged_shadows(
    rust: &mut PictureViewRust,
    targets: &Targets,
    params: ShadowParams,
    label: &str,
) -> Result<usize, String> {
    let doc = rust.session.doc.as_ref().ok_or("no document")?;
    let (header, mut scan) = clean_window(doc)?;
    let block = block(doc).ok_or("missing SHP data")?;
    let (_, path, original) = resource::parts(block).ok_or("missing SHP data")?;
    if original.is_empty() {
        return fail("This SHP sheet has no source file to page through.");
    }
    let origins = origins(block).ok_or("damaged SHP data")?;
    let mut store: BTreeMap<usize, Vec<u8>> = stored(block)
        .ok_or("damaged SHP data")?
        .into_iter()
        .map(|(f, d)| (f, d.to_vec()))
        .collect();
    let (w, h) = (header.layout.width, header.layout.height);
    let window = header.window();
    let first = header.first;
    let next: Vec<(usize, Vec<u8>)> = targets
        .iter()
        .map(|&(to, from)| {
            let plane = match from {
                Some(a) => {
                    let art = if window.contains(&a) {
                        scan.planes[a - first].clone()
                    } else if let Some(data) = store.get(&a) {
                        stored_canvas(data, w, h)
                    } else {
                        origin_canvas(original, origins[a], w, h)
                    };
                    unit::silhouette(&art, w, h, params)
                }
                None => vec![0u8; w * h],
            };
            (to, plane)
        })
        .collect();
    for (to, plane) in next {
        if window.contains(&to) {
            scan.planes[to - first] = plane;
        } else if plane == origin_canvas(original, origins[to], w, h) {
            store.remove(&to);
        } else {
            store.insert(to, encode_rle(&plane, w));
        }
    }
    let data = encode_parts(
        &header,
        &path,
        original,
        store.iter().map(|(f, d)| (*f, &d[..])),
        Some(&origins),
    );
    drop(store);
    let planes = std::mem::take(&mut scan.planes);
    drop(scan);
    let name = doc.layers[0].name.clone();
    let layout = header.layout;
    install_window(
        rust,
        &name,
        &layout,
        planes,
        &header.palette,
        &data,
        label,
        true,
    );
    Ok(targets.len())
}

fn frames_of(list: &[i32]) -> Option<Vec<usize>> {
    list.iter().map(|&v| usize::try_from(v).ok()).collect()
}

/// Run a shadow edit: one recorded step on a whole sheet, a history edge on
/// a paged one. The frames written, or -1 with the reason kept.
fn shadow_edit<F>(mut view: Pin<&mut PictureView>, label: &str, f: F) -> i32
where
    F: FnOnce(&mut PictureViewRust) -> Result<usize, String>,
{
    match view_header(&view).map(|h| h.paged()) {
        None => {
            set_error("This document is not an SHP sheet.");
            -1
        }
        Some(false) => commit(view, label, |rust| {
            if rust.modal_transform_blocks_edit() {
                return fail("Finish the transform first.");
            }
            f(rust)
        }),
        Some(true) => {
            let outcome = {
                let mut rust = view.as_mut().rust_mut();
                if rust.modal_transform_blocks_edit() {
                    Err("Finish the transform first.".to_string())
                } else {
                    f(&mut rust)
                }
            };
            match outcome {
                Ok(n) => {
                    set_error("");
                    view.as_mut().recomposite();
                    view.as_mut().session_cancelled();
                    n.min(i32::MAX as usize) as i32
                }
                Err(message) => {
                    set_error(&message);
                    -1
                }
            }
        }
    }
}

pub(in crate::cxxqt_object) fn shp_shadow_detect(view: &PictureView) -> i32 {
    view.rust()
        .session
        .doc
        .as_ref()
        .and_then(|doc| frame_uses(doc).ok())
        .and_then(|uses| unit::split(&uses))
        .map_or(-1, |half| half as i32)
}

pub(in crate::cxxqt_object) fn shp_shadow_violations(view: &PictureView, half: i32) -> Vec<i32> {
    let (Some(doc), Ok(half)) = (view.rust().session.doc.as_ref(), usize::try_from(half)) else {
        return Vec::new();
    };
    frame_uses(doc).map_or_else(
        |_| Vec::new(),
        |uses| {
            unit::violations(&uses, half)
                .into_iter()
                .map(|f| f as i32)
                .collect()
        },
    )
}

pub(in crate::cxxqt_object) fn shp_plan_shadow_half(view: &PictureView) -> ShpPlan {
    let doc = view.rust().session.doc.as_ref();
    doc.and_then(|doc| {
        let total = header(doc)?.total;
        frame_op_plan(
            doc,
            FrameOp::Insert {
                at: total,
                count: total,
            },
            &Limits::current(),
        )
    })
    .unwrap_or_else(invalid_plan)
}

pub(in crate::cxxqt_object) fn shp_generate_shadows(
    view: Pin<&mut PictureView>,
    half: i32,
    art: &[i32],
    dx: i32,
    dy: i32,
    skew: f64,
    allowance: u64,
) -> i32 {
    let (Ok(half), Some(art)) = (usize::try_from(half), frames_of(art)) else {
        set_error("no such frame");
        return -1;
    };
    let params = ShadowParams {
        dx,
        dy,
        skew: if skew.is_finite() { skew } else { 0.0 },
    };
    let limits = Limits::current();
    shadow_edit(view, "Generate Shadows", |rust| {
        generate_rust(rust, half, &art, params, &limits, allowance)
    })
}

pub(in crate::cxxqt_object) fn shp_clear_shadows(
    view: Pin<&mut PictureView>,
    half: i32,
    frames: &[i32],
) -> i32 {
    let (Ok(half), Some(frames)) = (usize::try_from(half), frames_of(frames)) else {
        set_error("no such frame");
        return -1;
    };
    shadow_edit(view, "Clear Shadows", |rust| {
        clear_rust(rust, half, &frames)
    })
}

pub(in crate::cxxqt_object) fn shp_house_ramp(
    view: &PictureView,
    rgb: u32,
    first: i32,
    last: i32,
) -> Vec<u32> {
    let (Some(header), Ok(first), Ok(last)) =
        (view_header(view), u8::try_from(first), u8::try_from(last))
    else {
        return Vec::new();
    };
    if first > last {
        return Vec::new();
    }
    unit::house_ramp(&header.palette, first..=last, rgb)
}

thread_local! {
    /// Recompute the radar colors on the next SHP save on this thread
    /// (`shp_set_save_radar`); taken by that save.
    static SAVE_RADAR: Cell<bool> = const { Cell::new(false) };
}

/// Take the "recompute radar colors" request of the next save.
pub(in crate::cxxqt_object) fn take_save_radar() -> bool {
    SAVE_RADAR.with(|r| r.replace(false))
}

pub(in crate::cxxqt_object) fn shp_set_save_radar(on: bool) {
    SAVE_RADAR.with(|r| r.set(on));
}

/// What Save writes: [`encode`], then, when asked, every drawn frame's radar
/// color recomputed as the average of its pixels in the palette as read.
/// Untouched, the radar fields are kept (the default).
pub(in crate::cxxqt_object) fn encode_for_save(
    doc: &Document,
    recompute: bool,
) -> Result<Vec<u8>, String> {
    let mut bytes = encode(doc)?;
    if recompute {
        let header = header(doc).ok_or("This document is not an SHP sheet.")?;
        unit::recompute_radar(&mut bytes, &header.palette);
    }
    Ok(bytes)
}

/// Cell `cell` of the sheet as palette indices (anything off the palette
/// reads as 0), read from the plain sheet layer when there is one, so a
/// readout never composites the whole sheet.
fn sheet_cell_plane(doc: &Document, header: &Header, cell: usize) -> Vec<u8> {
    let l = header.layout;
    let inverse = sheet::inverse(&unique_colours(&header.palette));
    let (x0, y0) = l.cell_origin(cell);
    let sw = l.sheet_width();
    let mut out = vec![0u8; l.width * l.height];
    let mut fill = |px: &dyn Fn(usize) -> [u8; 4]| {
        for y in 0..l.height {
            for x in 0..l.width {
                let p = px((y0 + y) * sw + x0 + x);
                if p[3] == 255 {
                    if let Some(&i) = inverse.get(&rgb_of(&p)) {
                        out[y * l.width + x] = i;
                    }
                }
            }
        }
    };
    if plain_sheet_layer(doc).is_ok() {
        let layer = &doc.layers[0];
        let channel = |id: i16| {
            layer
                .channels
                .iter()
                .find(|c| c.id == id)
                .map(|c| &c.data[..])
        };
        if let (Some(r), Some(g), Some(b), Some(a)) =
            (channel(0), channel(1), channel(2), channel(-1))
        {
            fill(&|i| [r[i], g[i], b[i], a[i]]);
        }
    } else if let Ok((_, rgba)) = sheet_pixels(doc) {
        fill(&|i| {
            [
                rgba[i * 4],
                rgba[i * 4 + 1],
                rgba[i * 4 + 2],
                rgba[i * 4 + 3],
            ]
        });
    }
    out
}

/// Frame `frame`'s radar color as the file has it (its original frame's
/// field; none for an added frame) and the average of its pixels now.
pub(in crate::cxxqt_object) fn radar_info(doc: &Document, frame: usize) -> ShpRadar {
    let mut out = ShpRadar {
        has_file: false,
        file: 0,
        has_average: false,
        average: 0,
    };
    let Some(block) = block(doc) else {
        return out;
    };
    let Some((header, _, original)) = resource::parts(block) else {
        return out;
    };
    let Some(origins) = origins(block) else {
        return out;
    };
    if frame >= header.total {
        return out;
    }
    let pack = |[r, g, b]: [u8; 3]| (u32::from(r) << 16) | (u32::from(g) << 8) | u32::from(b);
    if let Some(j) = origins[frame].filter(|_| !original.is_empty()) {
        let radar = pictura_shp::FrameHeader::read(original, j).radar;
        out.has_file = true;
        out.file = pack([radar[0], radar[1], radar[2]]);
    }
    let (w, h) = (header.layout.width, header.layout.height);
    let plane = if header.window().contains(&frame) {
        sheet_cell_plane(doc, &header, frame - header.first)
    } else {
        let stored = stored(block).unwrap_or_default();
        stored_or_original(original, &stored, origins[frame], frame, w, h)
    };
    if let Some(avg) = unit::average_color(&plane, &header.palette) {
        out.has_average = true;
        out.average = pack(avg);
    }
    out
}

pub(in crate::cxxqt_object) fn shp_radar(view: &PictureView, frame: i32) -> ShpRadar {
    match (view.rust().session.doc.as_ref(), usize::try_from(frame)) {
        (Some(doc), Ok(frame)) => radar_info(doc, frame),
        _ => ShpRadar {
            has_file: false,
            file: 0,
            has_average: false,
            average: 0,
        },
    }
}
