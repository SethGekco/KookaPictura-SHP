// SPDX-License-Identifier: GPL-3.0-or-later
//! TMP terrain documents (`pictura_shp::terrain`): every tile at its
//! template pixel position on the Tiles layer, extra blocks on Extras
//! layers, z-data as grey on hidden Z layers. The palette, the original file
//! and the tile fields live in the private `KookaTMP` resource, so undo,
//! redo and a PSD round trip carry them with the pixels.

use super::*;

use crate::history::History;
use pictura_core::LayerStack;
use pictura_shp::terrain::{
    clip_outside, fields_of, read_back, render, snap, tile_at, LayerSpec, Placement, Scan,
    TmpResource,
};
use pictura_shp::tmp::{self, land_label, ramp_label, Template, TileFields};

use crate::cxxqt_object::shp::ffi::{TmpCheck, TmpInfo, TmpTile};

/// The largest TMP file read: real templates are under 100 KB.
const MAX_FILE: u64 = 64 << 20;

/// The document's `KookaTMP` block, when it is a TMP document.
pub(in crate::cxxqt_object) fn tmp_resource(doc: &Document) -> Option<TmpResource> {
    TmpResource::find(&doc.image_resources)
}

/// The template a TMP document's resource holds (the original tiles with
/// the current fields) and where it lands on the canvas.
pub(in crate::cxxqt_object) fn template_of(
    doc: &Document,
) -> Option<(TmpResource, Template, Placement)> {
    let resource = tmp_resource(doc)?;
    let template = resource.template()?;
    let placement = Placement::new(&template)?;
    Some((resource, template, placement))
}

/// `Some(result)` when `bytes` is a TMP (after the SHP sniffer refused it).
pub(in crate::cxxqt_object) fn decode_tmp(
    path: &str,
    bytes: &[u8],
    gpu: bool,
    progress: &dyn Fn(u8, &str),
) -> Option<Option<OpenedDocument>> {
    if !tmp::sniff(bytes) {
        return None;
    }
    let choice = OPEN_PALETTE
        .with(|p| p.borrow().clone())
        .unwrap_or_else(|| PaletteChoice::load(""));
    Some(open_template(
        path,
        bytes,
        choice,
        gpu,
        progress,
        &Limits::current(),
    ))
}

/// The TMP document for a template's bytes.
pub(in crate::cxxqt_object) fn open_template(
    path: &str,
    bytes: &[u8],
    choice: PaletteChoice,
    gpu: bool,
    progress: &dyn Fn(u8, &str),
    limits: &Limits,
) -> Option<OpenedDocument> {
    progress(30, "Decoding tiles");
    let template = match tmp::parse(bytes) {
        Ok(t) => t,
        Err(e) => {
            set_error(&format!("not a readable TMP: {e}"));
            return None;
        }
    };
    let Some(placement) = Placement::new(&template) else {
        set_error("The template's tiles lie too far apart to fit one canvas.");
        return None;
    };
    let specs = placement.layers();
    let needed = (placement.width * placement.height * 4 * specs.len()) as u64;
    if let Err(reason) = limits.admit(needed, 0) {
        set_error(&format!(
            "A {}x{} template canvas of {} layers: {reason}",
            placement.width,
            placement.height,
            specs.len()
        ));
        return None;
    }
    let unique = unique_colours(&choice.palette);
    let layers = render(&template, &placement, &unique);
    progress(70, "Preparing layers");
    let resource = TmpResource {
        palette: choice.palette,
        palette_path: choice.path,
        original: bytes.to_vec(),
        fields: fields_of(&template),
    };
    let name = Path::new(path)
        .file_name()
        .map(|n| n.to_string_lossy().into_owned())
        .unwrap_or_default();
    let (doc, history, target) =
        template_document(&name, &placement, &specs, layers, &resource, gpu);
    Some(OpenedDocument {
        doc,
        history,
        target,
        path: OpenedPath::Set(Some(path.to_string())),
        source_format: "tmp".to_string(),
        pdf_password: None,
    })
}

/// A document of the given layers (bottom first; z layers hidden), the
/// resource, and a fresh history whose first state is "Open".
pub(in crate::cxxqt_object) fn template_document(
    name: &str,
    placement: &Placement,
    specs: &[LayerSpec],
    layers: Vec<Vec<u8>>,
    resource: &TmpResource,
    gpu: bool,
) -> (Document, History, LayerTarget) {
    let (w, h) = (placement.width as u32, placement.height as u32);
    let mut stack = Vec::with_capacity(specs.len());
    let mut doc = None;
    for (spec, rgba) in specs.iter().zip(layers) {
        // No finalize_import: index 0 must stay paintable as transparency.
        let one = Document::from_rgba(&spec.name, w, h, &rgba);
        let mut layer = one.layers[0].clone();
        layer.visible = !spec.plane.is_z();
        stack.push(layer);
        doc.get_or_insert(one);
    }
    let mut doc = doc.unwrap_or_else(|| Document::from_rgba(name, w, h, &[]));
    doc.layers = LayerStack::from_layers(stack);
    doc.image_resources = resource.store(&[]).into();
    let rendered = current_buffer(&doc, gpu);
    crate::cxxqt_object::helpers_composite::store_composite(&mut doc, &rendered);
    drop(rendered);
    let target = LayerTarget::for_opened(&doc, Some("0".to_string()));
    let mut history = History::default();
    history.capture_live(&mut doc, &None, &target, "Open");
    (doc, history, target)
}

/// Recolour every colour layer by index for a new palette and keep it in
/// the resource. `Ok(false)` when it is the same palette.
pub(in crate::cxxqt_object) fn rebind_template(
    rust: &mut PictureViewRust,
    choice: PaletteChoice,
) -> Result<bool, String> {
    let doc = rust.session.doc.as_mut().ok_or("no document")?;
    let mut resource = tmp_resource(doc).ok_or("This document is not a TMP template.")?;
    if resource.palette == choice.palette && resource.palette_path == choice.path {
        return Ok(false);
    }
    let old = unique_colours(&resource.palette);
    let new = unique_colours(&choice.palette);
    let plane = doc.width as usize * doc.height as usize;
    let colour_layers: Vec<String> =
        Placement::new(&resource.template().ok_or("damaged TMP data")?)
            .ok_or("damaged TMP data")?
            .layers()
            .into_iter()
            .filter(|s| !s.plane.is_z())
            .map(|s| s.name)
            .collect();
    let (w, h) = (doc.width, doc.height);
    for i in 0..doc.layers.len() {
        let layer = &mut doc.layers[i];
        if !colour_layers.contains(&layer.name) || !full_canvas(layer, w, h) {
            continue;
        }
        let mut rgba = layer_rgba(layer, plane);
        if sheet::rebind(&mut rgba, &old, &new) > 0 {
            store_layer_rgba(layer, &rgba);
        }
    }
    resource.palette = choice.palette;
    resource.palette_path = choice.path;
    doc.image_resources = resource.store(&doc.image_resources).into();
    Ok(true)
}

/// Does `layer` cover exactly the `width` x `height` canvas?
pub(in crate::cxxqt_object) fn full_canvas(layer: &Layer, width: u32, height: u32) -> bool {
    let r = &layer.rect;
    (r.left, r.top, r.right, r.bottom) == (0, 0, width as i32, height as i32)
}

pub(in crate::cxxqt_object) fn tmp_sniff_file(path: &QString) -> bool {
    read_capped(path.to_string(), MAX_FILE).is_some_and(|bytes| tmp::sniff(&bytes))
}

fn view_template(view: &PictureView) -> Option<(TmpResource, Template, Placement)> {
    view.rust().session.doc.as_ref().and_then(template_of)
}

pub(in crate::cxxqt_object) fn tmp_info(view: &PictureView) -> TmpInfo {
    match view_template(view) {
        Some((resource, t, p)) => TmpInfo {
            valid: true,
            blocks_x: t.blocks_x as i32,
            blocks_y: t.blocks_y as i32,
            cx: t.cx as i32,
            cy: t.cy as i32,
            tiles: t.live().count() as i32,
            width: p.width as i32,
            height: p.height as i32,
            palette_name: QString::from(resource.palette.name.as_str()),
            palette_path: QString::from(resource.palette_path.as_str()),
        },
        None => TmpInfo {
            valid: false,
            blocks_x: 0,
            blocks_y: 0,
            cx: 0,
            cy: 0,
            tiles: 0,
            width: 0,
            height: 0,
            palette_name: QString::default(),
            palette_path: QString::default(),
        },
    }
}

fn rgb(c: [u8; 3]) -> u32 {
    u32::from_be_bytes([0, c[0], c[1], c[2]])
}

pub(in crate::cxxqt_object) fn tmp_tiles(view: &PictureView) -> Vec<TmpTile> {
    view.rust()
        .session
        .doc
        .as_ref()
        .map_or_else(Vec::new, tiles_of)
}

/// [`tmp_tiles`] of a document.
pub(in crate::cxxqt_object) fn tiles_of(doc: &Document) -> Vec<TmpTile> {
    let Some((resource, t, p)) = template_of(doc) else {
        return Vec::new();
    };
    // Whether a tile holds z-data now: painting its Z layer can give it some.
    let scan = read_layers(doc, &p).ok().map(|layers| {
        let refs: Vec<&[u8]> = layers.iter().map(Vec::as_slice).collect();
        let unique = unique_colours(&resource.palette);
        read_back(&t, &p, &refs, &sheet::inverse(&unique))
    });
    let now = scan
        .as_ref()
        .and_then(|s| s.template.as_ref())
        .unwrap_or(&t);
    t.live()
        .map(|(slot, tile)| {
            let (x, y) = p.tile_origin(tile);
            let (ex, ey) = p.extra_origin(tile);
            let has_extra = tile.extra.is_some();
            let current = now.tiles[slot].as_ref().unwrap_or(tile);
            TmpTile {
                slot: slot as i32,
                col: (slot % t.blocks_x) as i32,
                row: (slot / t.blocks_x) as i32,
                x: x as i32,
                y: y as i32,
                height: i32::from(tile.fields.height),
                land: i32::from(tile.fields.land),
                ramp: i32::from(tile.fields.ramp),
                radar_low: rgb(tile.fields.radar_low),
                radar_high: rgb(tile.fields.radar_high),
                flags: tile.flags,
                has_z: current.z.is_some(),
                has_extra,
                has_extra_z: current.extra_z.is_some(),
                extra_x: if has_extra { ex as i32 } else { 0 },
                extra_y: if has_extra { ey as i32 } else { 0 },
                extra_w: tile.extra_w as i32,
                extra_h: tile.extra_h as i32,
                extra_layer: p.extra_group[slot].map_or(-1, |g| g as i32),
            }
        })
        .collect()
}

pub(in crate::cxxqt_object) fn tmp_tile_at(view: &PictureView, x: i32, y: i32) -> i32 {
    let (Some((_, t, p)), Ok(x), Ok(y)) =
        (view_template(view), usize::try_from(x), usize::try_from(y))
    else {
        return -1;
    };
    tile_at(&t, &p, x, y).map_or(-1, |slot| slot as i32)
}

pub(in crate::cxxqt_object) fn tmp_land_label(land: i32) -> QString {
    QString::from(land_label(land.clamp(0, 255) as u8))
}

pub(in crate::cxxqt_object) fn tmp_ramp_label(ramp: i32) -> QString {
    QString::from(ramp_label(ramp.clamp(0, 255) as u8))
}

/// Each layer of `p.layers()` as canvas RGBA, read from the document's
/// layer of that name; the reason when one is missing or is not a plain
/// pixel layer, or when other layers would be lost.
pub(in crate::cxxqt_object) fn read_layers(
    doc: &Document,
    p: &Placement,
) -> Result<Vec<Vec<u8>>, String> {
    let specs = p.layers();
    let names: Vec<&str> = specs.iter().map(|s| s.name.as_str()).collect();
    if (doc.width as usize, doc.height as usize) != (p.width, p.height) {
        return fail(format!(
            "The canvas is {}x{} px but the template needs {}x{} px. Undo the size change \
             before saving as TMP.",
            doc.width, doc.height, p.width, p.height
        ));
    }
    if let Some(other) = doc
        .layers
        .iter()
        .find(|l| !names.contains(&l.name.as_str()))
    {
        return fail(format!(
            "The layer \"{}\" is not one a TMP keeps ({}). Merge it into one of them or \
             delete it first.",
            other.name,
            names.join(", ")
        ));
    }
    specs
        .iter()
        .map(|spec| {
            let mut found = doc.layers.iter().filter(|l| l.name == spec.name);
            let (Some(layer), None) = (found.next(), found.next()) else {
                return fail(format!(
                    "A TMP needs exactly one layer named \"{}\" (layers: {}).",
                    spec.name,
                    names.join(", ")
                ));
            };
            if layer.is_group || layer.adjustment.is_some() || layer.mask.is_some() {
                return fail(format!(
                    "The layer \"{}\" must be a plain pixel layer without a mask.",
                    spec.name
                ));
            }
            Ok(canvas_rgba(layer, p.width, p.height))
        })
        .collect()
}

/// A pixel layer's RGBA placed on a `width` x `height` canvas (its rect may
/// be smaller, larger or offset).
fn canvas_rgba(layer: &Layer, width: usize, height: usize) -> Vec<u8> {
    let mut out = vec![0u8; width * height * 4];
    let r = &layer.rect;
    let lw = (r.right - r.left).max(0) as usize;
    let lh = (r.bottom - r.top).max(0) as usize;
    let alpha = layer.channels.iter().any(|c| c.id == -1);
    for (k, id) in [0i16, 1, 2, -1].into_iter().enumerate() {
        let channel = layer.channels.iter().find(|c| c.id == id);
        for y in 0..lh {
            let cy = r.top as i64 + y as i64;
            if !(0..height as i64).contains(&cy) {
                continue;
            }
            for x in 0..lw {
                let cx = r.left as i64 + x as i64;
                if !(0..width as i64).contains(&cx) {
                    continue;
                }
                let v = match channel {
                    Some(c) => c.data.get(y * lw + x).copied().unwrap_or(0),
                    None if id == -1 && !alpha => 255,
                    None => 0,
                };
                out[(cy as usize * width + cx as usize) * 4 + k] = v;
            }
        }
    }
    out
}

/// Read a TMP document's layers back the way Save does.
pub(in crate::cxxqt_object) fn scan_document(
    doc: &Document,
) -> Result<(TmpResource, Placement, Scan), String> {
    let (resource, t, p) = template_of(doc)
        .ok_or("This document is not a TMP template, or its TMP data is damaged.")?;
    let layers = read_layers(doc, &p)?;
    let refs: Vec<&[u8]> = layers.iter().map(Vec::as_slice).collect();
    let unique = unique_colours(&resource.palette);
    let scan = read_back(&t, &p, &refs, &sheet::inverse(&unique));
    Ok((resource, p, scan))
}

/// Why `scan` cannot be saved as it is, if it cannot.
fn refusal(scan: &Scan) -> Option<String> {
    let at = |p: Option<(usize, usize)>| {
        p.map_or_else(String::new, |(x, y)| format!(" (first at {x}, {y})"))
    };
    if scan.off_palette > 0 || scan.off_scale > 0 {
        return Some(format!(
            "{} pixel(s) are not palette colours and {} z pixel(s) are not depths 0-31{}. Snap \
             them before saving.",
            scan.off_palette,
            scan.off_scale,
            at(scan.first_off)
        ));
    }
    if scan.outside > 0 {
        return Some(format!(
            "{} pixel(s) lie outside every tile's diamond and extra block{} and would be lost. \
             Clip them before saving.",
            scan.outside,
            at(scan.first_outside)
        ));
    }
    None
}

/// The bytes Save writes for a TMP document: every block over the original
/// file where the layout allows, so untouched tiles keep every byte.
pub(in crate::cxxqt_object) fn encode_template(doc: &Document) -> Result<Vec<u8>, String> {
    let (resource, _, scan) = scan_document(doc)?;
    if let Some(reason) = refusal(&scan) {
        return fail(reason);
    }
    let template = scan.template.ok_or("damaged TMP data")?;
    tmp::build(&template, Some(&resource.original))
        .map_err(|e| format!("Cannot write the template: {e}"))
}

/// Save a TMP document to `path`.
pub(in crate::cxxqt_object) fn save_tmp(mut view: Pin<&mut PictureView>, path: &str) -> bool {
    let encoded = match view.rust().session.doc.as_ref() {
        Some(doc) => encode_template(doc),
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

/// Is `suffix` (lower case) one a TMP template is saved under?
pub(in crate::cxxqt_object) fn tmp_suffix(suffix: &str) -> bool {
    matches!(
        suffix,
        "tem" | "sno" | "urb" | "ubn" | "des" | "lun" | "tmp"
    )
}

pub(in crate::cxxqt_object) fn tmp_check(view: &PictureView) -> TmpCheck {
    let pair =
        |p: Option<(usize, usize)>| p.map_or_else(Vec::new, |(x, y)| vec![x as i32, y as i32]);
    let clamp = |n: usize| n.min(i32::MAX as usize) as i32;
    match view.rust().session.doc.as_ref().map(scan_document) {
        Some(Ok((_, _, scan))) => TmpCheck {
            error: QString::default(),
            off_palette: clamp(scan.off_palette),
            off_scale: clamp(scan.off_scale),
            off_slots: scan.off_slots.iter().map(|&s| s as i32).collect(),
            first_off: pair(scan.first_off),
            outside: clamp(scan.outside),
            first_outside: pair(scan.first_outside),
        },
        other => TmpCheck {
            error: QString::from(match other {
                Some(Err(e)) => e,
                _ => "no document".to_string(),
            }),
            off_palette: 0,
            off_scale: 0,
            off_slots: Vec::new(),
            first_off: Vec::new(),
            outside: 0,
            first_outside: Vec::new(),
        },
    }
}

/// Apply `f` to the document's layers (canvas RGBA, `Placement::layers`
/// order) and store the ones it changed; the pixels changed.
fn with_layers<F>(rust: &mut PictureViewRust, f: F) -> Result<usize, String>
where
    F: FnOnce(&TmpResource, &Template, &Placement, &mut [Vec<u8>]) -> usize,
{
    let doc = rust.session.doc.as_mut().ok_or("no document")?;
    let (resource, t, p) = template_of(doc)
        .ok_or("This document is not a TMP template, or its TMP data is damaged.")?;
    let before = read_layers(doc, &p)?;
    let mut layers = before.clone();
    let changed = f(&resource, &t, &p, &mut layers);
    if changed > 0 {
        let (w, h) = (doc.width, doc.height);
        for ((spec, old), new) in p.layers().iter().zip(&before).zip(&layers) {
            if old == new {
                continue;
            }
            if let Some(layer) = doc.layers.iter_mut().find(|l| l.name == spec.name) {
                store_canvas_rgba(layer, w, h, new);
            }
        }
    }
    Ok(changed)
}

/// Replace a pixel layer's pixels with canvas RGBA (its rect becomes the
/// canvas; every other property is kept).
fn store_canvas_rgba(layer: &mut Layer, width: u32, height: u32, rgba: &[u8]) {
    let plane = width as usize * height as usize;
    let whole = full_canvas(layer, width, height)
        && [0i16, 1, 2, -1].iter().all(|id| {
            layer
                .channels
                .iter()
                .any(|c| c.id == *id && c.data.len() == plane)
        });
    if whole {
        store_layer_rgba(layer, rgba);
        return;
    }
    let fresh = Document::from_rgba(&layer.name, width, height, rgba);
    let fresh = &fresh.layers[0];
    layer.rect = fresh.rect;
    layer.channels = fresh.channels.clone();
}

/// The mutation half of TMP "Snap to Palette".
pub(in crate::cxxqt_object) fn snap_template_rust(
    rust: &mut PictureViewRust,
) -> Result<usize, String> {
    with_layers(rust, |resource, t, p, layers| {
        let unique = unique_colours(&resource.palette);
        snap(t, p, layers, &resource.palette, &unique)
    })
}

/// The mutation half of "Clip to Tiles".
pub(in crate::cxxqt_object) fn clip_template_rust(
    rust: &mut PictureViewRust,
) -> Result<usize, String> {
    with_layers(rust, |_, t, p, layers| clip_outside(t, p, layers))
}

pub(in crate::cxxqt_object) fn tmp_snap(view: Pin<&mut PictureView>) -> i32 {
    commit(view, "Snap to Palette", snap_template_rust)
}

pub(in crate::cxxqt_object) fn tmp_clip(view: Pin<&mut PictureView>) -> i32 {
    commit(view, "Clip to Tiles", clip_template_rust)
}

/// The mutation half of "Tile Fields": set slot `slot`'s height, land and
/// ramp types and radar colors in the resource (what Save writes into its
/// header). `Ok(false)` when nothing changed.
pub(in crate::cxxqt_object) fn set_tile_fields_rust(
    rust: &mut PictureViewRust,
    slot: usize,
    fields: TileFields,
) -> Result<bool, String> {
    let doc = rust.session.doc.as_mut().ok_or("no document")?;
    let mut resource = tmp_resource(doc).ok_or("This document is not a TMP template.")?;
    let entry = resource
        .fields
        .iter_mut()
        .find(|(s, _)| *s == slot)
        .ok_or_else(|| format!("The template has no tile in slot {slot}."))?;
    if entry.1 == fields {
        return Ok(false);
    }
    entry.1 = fields;
    doc.image_resources = resource.store(&doc.image_resources).into();
    Ok(true)
}

pub(in crate::cxxqt_object) fn tmp_set_tile_fields(
    view: Pin<&mut PictureView>,
    slot: i32,
    height: i32,
    land: i32,
    ramp: i32,
    radar_low: u32,
    radar_high: u32,
) -> bool {
    let byte = |v: i32, what: &str| {
        u8::try_from(v).map_err(|_| format!("The {what} must be 0 to 255 (got {v})."))
    };
    let colour = |c: u32| [(c >> 16) as u8, (c >> 8) as u8, c as u8];
    let fields = (|| {
        Ok::<_, String>(TileFields {
            height: byte(height, "height")?,
            land: byte(land, "land type")?,
            ramp: byte(ramp, "ramp type")?,
            radar_low: colour(radar_low),
            radar_high: colour(radar_high),
        })
    })();
    let (fields, slot) = match (fields, usize::try_from(slot)) {
        (Ok(f), Ok(s)) => (f, s),
        (Err(e), _) => {
            set_error(&e);
            return false;
        }
        (_, Err(_)) => {
            set_error("no such tile");
            return false;
        }
    };
    let mut outcome: Result<bool, String> = fail("no document");
    view.edit("Tile Fields", |rust| {
        outcome = set_tile_fields_rust(rust, slot, fields);
        match outcome {
            Ok(true) => Edit::Meta,
            _ => Edit::None,
        }
    });
    match outcome {
        Ok(_) => {
            set_error("");
            true
        }
        Err(e) => {
            set_error(&e);
            false
        }
    }
}
