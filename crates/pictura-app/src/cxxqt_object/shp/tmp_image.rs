// SPDX-License-Identifier: GPL-3.0-or-later
//! New TMP templates from an image (`pictura_shp::tmp_image`): File > New
//! TMP from Image, and Save As TMP of a document that is not a template.
//! The template is built, written out (the fresh layout) and opened from
//! those bytes as a new document, so it behaves exactly like a template read
//! from a file: Save patches it in place from then on.

use super::*;

use pictura_shp::tmp_image::{average_radar, default_cells, from_image, ImageSpec, Made, ZData};

/// The source document's composite as 8-bit RGBA.
fn source_rgba(source: &Document) -> Result<(Vec<u8>, usize, usize), String> {
    let image = buffer_to_rgba_bytes(&current_buffer(source, false));
    let (iw, ih) = (source.width as usize, source.height as usize);
    if image.len() != iw * ih * 4 {
        return fail("The image could not be read as 8-bit RGBA.");
    }
    Ok((image, iw, ih))
}

/// The template spec from the shell's integers: tile width and height,
/// blocks x and y, height, land, ramp, nearest (0/1), z-data (0 none, 1
/// flat), extras (0/1); `cells` one 0/1 per slot.
pub(in crate::cxxqt_object) fn image_spec(
    params: &[i32],
    cells: &[i32],
) -> Option<(ImageSpec, bool)> {
    let n = |k: usize| params.get(k).and_then(|&v| usize::try_from(v).ok());
    let byte = |k: usize| params.get(k).and_then(|&v| u8::try_from(v).ok());
    let spec = ImageSpec {
        cx: n(0)?,
        cy: n(1)?,
        blocks_x: n(2)?,
        blocks_y: n(3)?,
        cells: cells.iter().map(|&c| c != 0).collect(),
        fields: TileFields {
            height: byte(4)?,
            land: byte(5)?,
            ramp: byte(6)?,
            radar_low: [0; 3],
            radar_high: [0; 3],
        },
        z: if n(8)? == 1 { ZData::Flat } else { ZData::None },
        extras: n(9)? == 1,
    };
    Some((spec, n(7)? == 1))
}

/// The template document `spec` makes of `source`, its colours mapped to
/// `choice` (with `nearest`, as Snap to Palette does), and what was made.
pub(in crate::cxxqt_object) fn template_from_image(
    source: &Document,
    name: &str,
    spec: &ImageSpec,
    choice: &PaletteChoice,
    nearest: bool,
    gpu: bool,
    limits: &Limits,
    allowance: u64,
) -> Result<(OpenedDocument, Made), String> {
    let (image, iw, ih) = source_rgba(source)?;
    let mut made = from_image(&image, iw, ih, spec)?;
    drop(image);
    let p = &made.placement;
    let specs = p.layers();
    limits.admit((p.width * p.height * 4 * specs.len()) as u64, allowance)?;
    let unique = unique_colours(&choice.palette);
    if nearest {
        snap(
            &made.template,
            p,
            &mut made.layers,
            &choice.palette,
            &unique,
        );
    }
    let refs: Vec<&[u8]> = made.layers.iter().map(Vec::as_slice).collect();
    let mut template = read_back(&made.template, p, &refs, &sheet::inverse(&unique))
        .template
        .ok_or("damaged TMP data")?;
    for tile in template.tiles.iter_mut().flatten() {
        (tile.fields.radar_low, tile.fields.radar_high) = average_radar(tile, &choice.palette);
    }
    let bytes =
        tmp::build(&template, None).map_err(|e| format!("Cannot build the template: {e}"))?;
    let resource = TmpResource {
        palette: choice.palette.clone(),
        palette_path: choice.path.clone(),
        original: bytes,
        fields: fields_of(&template),
        shapes: None,
    };
    let check = resource
        .template()
        .and_then(|t| Placement::new(&t))
        .ok_or("The built template does not read back.")?;
    if check != made.placement {
        return fail("The built template does not read back to the same canvas.");
    }
    let layers = std::mem::take(&mut made.layers);
    let (doc, history, target) =
        template_document(name, &made.placement, &specs, layers, &resource, gpu);
    made.template = template;
    Ok((
        OpenedDocument {
            doc,
            history,
            target,
            path: OpenedPath::Set(None),
            source_format: "tmp".to_string(),
            pdf_password: None,
        },
        made,
    ))
}

/// The default cells (`tmp_image_cells`) of `source` for `params`, as 0/1.
#[cfg(test)]
pub(in crate::cxxqt_object) fn image_cells_of(source: &Document, params: &[i32; 10]) -> Vec<i32> {
    let n = |k: usize| usize::try_from(params[k]).unwrap_or(0);
    let (image, iw, ih) = source_rgba(source).expect("rgba");
    default_cells(&image, iw, ih, n(0), n(1), n(2), n(3))
        .into_iter()
        .map(i32::from)
        .collect()
}

pub(in crate::cxxqt_object) fn tmp_image_cells(
    source: &PictureView,
    cx: i32,
    cy: i32,
    blocks_x: i32,
    blocks_y: i32,
) -> Vec<bool> {
    let n = |v: i32| usize::try_from(v).unwrap_or(0);
    let (cx, cy, bx, by) = (n(cx), n(cy), n(blocks_x), n(blocks_y));
    let valid = pictura_shp::tmp_image::valid_tile_size(cx, cy)
        && (1..=tmp::MAX_BLOCKS).contains(&bx)
        && (1..=tmp::MAX_BLOCKS).contains(&by);
    match source.rust().session.doc.as_ref().map(source_rgba) {
        Some(Ok((image, iw, ih))) if valid => default_cells(&image, iw, ih, cx, cy, bx, by),
        _ => Vec::new(),
    }
}

/// Make the template in `view`; `[tiles, outside, dropped, width, height]`,
/// or empty with the reason in `shp_last_error`.
#[allow(clippy::too_many_arguments)]
pub(in crate::cxxqt_object) fn tmp_from_image(
    view: Pin<&mut PictureView>,
    source: &PictureView,
    name: &QString,
    params: &[i32],
    cells: &[i32],
    palette: &QString,
    allowance: u64,
) -> Vec<i32> {
    let Some((spec, nearest)) = image_spec(params, cells) else {
        set_error("The template options are out of range.");
        return Vec::new();
    };
    let choice = PaletteChoice::load(&palette.to_string());
    let gpu = view.rust().gpu_compute;
    let made = match source.rust().session.doc.as_ref() {
        Some(doc) => template_from_image(
            doc,
            &name.to_string(),
            &spec,
            &choice,
            nearest,
            gpu,
            &Limits::current(),
            allowance,
        ),
        None => fail("no document"),
    };
    let clamp = |n: usize| n.min(i32::MAX as usize) as i32;
    let summary = made.as_ref().ok().map(|(_, m)| {
        vec![
            clamp(m.template.live().count()),
            clamp(m.outside),
            clamp(m.dropped),
            clamp(m.placement.width),
            clamp(m.placement.height),
        ]
    });
    if super::super::authoring::install(view, made.map(|(opened, _)| opened), true) {
        summary.unwrap_or_default()
    } else {
        Vec::new()
    }
}
