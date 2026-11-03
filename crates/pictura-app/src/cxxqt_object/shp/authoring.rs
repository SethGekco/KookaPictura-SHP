// SPDX-License-Identifier: GPL-3.0-or-later
//! New SHP documents: File > New SHP (blank frames in a palette), and the
//! conversion Save As SHP runs on any other image (cut into frames, colours
//! mapped to the palette). Both make a whole sheet with no source file, so
//! every frame is written fresh (RLE-Zero only when smaller, as XCC does).

use super::*;

use pictura_shp::sheet::DEFAULT_GUTTER;
use pictura_shp::{MAX_FRAMES, MAX_SIDE};

/// A blank sheet's (or a converted one's) layout, checked against the
/// format and the sheet budget.
fn new_layout(width: usize, height: usize, frames: usize) -> Result<Layout, String> {
    let side = usize::from(MAX_SIDE);
    if !(1..=side).contains(&width) || !(1..=side).contains(&height) {
        return fail(format!("An SHP frame is 1 to {side} pixels a side."));
    }
    if !(1..=usize::from(MAX_FRAMES)).contains(&frames) {
        return fail(format!("An SHP holds 1 to {MAX_FRAMES} frames."));
    }
    Layout::new(width, height, frames, DEFAULT_GUTTER)
        .ok_or_else(|| "That many frames of that size do not fit one sheet.".to_string())
}

/// The document of a new sheet: `rgba` on `layout`, the palette, no file.
fn new_document(
    name: &str,
    layout: &Layout,
    rgba: Vec<u8>,
    choice: &PaletteChoice,
    gpu: bool,
    label: &str,
) -> OpenedDocument {
    let header = Header::whole(*layout, choice.palette.clone());
    let data = resource::encode_parts(&header, &choice.path, &[], std::iter::empty(), None);
    let (doc, history, target) = sheet_document(name, layout, rgba, &data, gpu, label);
    OpenedDocument {
        doc,
        history,
        target,
        path: OpenedPath::Set(None),
        source_format: "shp".to_string(),
        pdf_password: None,
    }
}

/// File > New SHP: `frames` blank frames of `width` x `height`.
pub(in crate::cxxqt_object) fn blank_sheet(
    name: &str,
    width: usize,
    height: usize,
    frames: usize,
    choice: &PaletteChoice,
    gpu: bool,
    limits: &Limits,
    allowance: u64,
) -> Result<OpenedDocument, String> {
    let layout = new_layout(width, height, frames)?;
    limits.admit(layout.sheet_bytes(), allowance)?;
    let rgba = vec![0u8; layout.sheet_bytes() as usize];
    Ok(new_document(name, &layout, rgba, choice, gpu, "New SHP"))
}

/// How an image is cut into frames: cells of `width` x `height` with
/// `gutter` pixels between them, `cols` across, read row by row, the first
/// `frames` of them.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(in crate::cxxqt_object) struct Cut {
    pub(in crate::cxxqt_object) width: usize,
    pub(in crate::cxxqt_object) height: usize,
    pub(in crate::cxxqt_object) cols: usize,
    pub(in crate::cxxqt_object) rows: usize,
    pub(in crate::cxxqt_object) gutter: usize,
    pub(in crate::cxxqt_object) frames: usize,
}

/// The SHP sheet for an RGBA image `iw` x `ih` cut by `cut`, its colours
/// mapped to `palette`: with `nearest`, every pixel that is not already a
/// palette colour becomes its nearest index (alpha under 128 becomes index
/// 0), exactly as Snap to Palette does; otherwise colours are copied as they
/// are and the save gate reports the rest.
pub(in crate::cxxqt_object) fn cut_sheet(
    image: &[u8],
    iw: usize,
    ih: usize,
    cut: Cut,
    palette: &Palette,
    nearest: bool,
) -> Result<(Layout, Vec<u8>), String> {
    let Cut {
        width,
        height,
        cols,
        rows,
        gutter,
        frames,
    } = cut;
    if cols == 0 || rows == 0 || frames > cols * rows {
        return fail("The frame grid must hold every frame.");
    }
    let span = |n: usize, side: usize| n * side + n.saturating_sub(1) * gutter;
    if span(cols, width) > iw || span(rows, height) > ih {
        return fail(format!(
            "{cols} x {rows} frames of {width}x{height} do not fit the {iw}x{ih} image."
        ));
    }
    let layout = new_layout(width, height, frames)?;
    let sw = layout.sheet_width();
    let mut out = vec![0u8; layout.sheet_bytes() as usize];
    for k in 0..frames {
        let (sx, sy) = (
            (k % cols) * (width + gutter),
            (k / cols) * (height + gutter),
        );
        let (dx, dy) = layout.cell_origin(k);
        for y in 0..height {
            let src = ((sy + y) * iw + sx) * 4;
            let dst = ((dy + y) * sw + dx) * 4;
            out[dst..dst + width * 4].copy_from_slice(&image[src..src + width * 4]);
        }
    }
    if nearest {
        sheet::snap(&layout, &mut out, palette, &unique_colours(palette));
    }
    Ok((layout, out))
}

/// Save As SHP for an image that is not an SHP: the new sheet document.
pub(in crate::cxxqt_object) fn convert(
    source: &Document,
    name: &str,
    cut: Cut,
    choice: &PaletteChoice,
    nearest: bool,
    gpu: bool,
    limits: &Limits,
    allowance: u64,
) -> Result<OpenedDocument, String> {
    let layout = new_layout(cut.width, cut.height, cut.frames)?;
    limits.admit(layout.sheet_bytes(), allowance)?;
    let image = buffer_to_rgba_bytes(&current_buffer(source, false));
    let (iw, ih) = (source.width as usize, source.height as usize);
    if image.len() != iw * ih * 4 {
        return fail("The image could not be read as 8-bit RGBA.");
    }
    let (layout, rgba) = cut_sheet(&image, iw, ih, cut, &choice.palette, nearest)?;
    drop(image);
    Ok(new_document(
        name,
        &layout,
        rgba,
        choice,
        gpu,
        "Convert to SHP",
    ))
}

/// What a new sheet of `frames` frames of `width` x `height` needs.
pub(in crate::cxxqt_object::shp) fn shp_plan_sheet(
    width: i32,
    height: i32,
    frames: i32,
) -> ShpPlan {
    let n = |v: i32| usize::try_from(v).unwrap_or(0);
    let Ok(layout) = new_layout(n(width), n(height), n(frames)) else {
        return invalid_plan();
    };
    plan(
        layout.width,
        layout.height,
        layout.frames,
        0,
        layout.frames,
        &Limits::current(),
    )
    .unwrap_or_else(invalid_plan)
}

/// Put a made document in `view` (`unsaved`: it holds work not yet in a
/// file).
pub(super) fn install(
    mut view: Pin<&mut PictureView>,
    made: Result<OpenedDocument, String>,
    unsaved: bool,
) -> bool {
    match made {
        Ok(opened) => {
            crate::cxxqt_object::open_jobs::apply_opened(&mut view.as_mut().rust_mut(), opened);
            if unsaved {
                view.as_mut().rust_mut().session.mark_unsaved();
            }
            view.as_mut().session_cancelled();
            set_error("");
            true
        }
        Err(message) => {
            set_error(&message);
            false
        }
    }
}

#[allow(clippy::too_many_arguments)]
pub(in crate::cxxqt_object::shp) fn shp_new_document(
    view: Pin<&mut PictureView>,
    name: &QString,
    width: i32,
    height: i32,
    frames: i32,
    palette: &QString,
    allowance: u64,
) -> bool {
    let n = |v: i32| usize::try_from(v).unwrap_or(0);
    let choice = PaletteChoice::load(&palette.to_string());
    let gpu = view.rust().gpu_compute;
    let made = blank_sheet(
        &name.to_string(),
        n(width),
        n(height),
        n(frames),
        &choice,
        gpu,
        &Limits::current(),
        allowance,
    );
    install(view, made, false)
}

#[allow(clippy::too_many_arguments)]
pub(in crate::cxxqt_object::shp) fn shp_convert(
    view: Pin<&mut PictureView>,
    source: &PictureView,
    name: &QString,
    cut: &[i32],
    palette: &QString,
    nearest: bool,
    allowance: u64,
) -> bool {
    let n = |k: usize| {
        cut.get(k)
            .and_then(|&v| usize::try_from(v).ok())
            .unwrap_or(0)
    };
    let cut = Cut {
        width: n(0),
        height: n(1),
        cols: n(2),
        rows: n(3),
        gutter: n(4),
        frames: n(5),
    };
    let choice = PaletteChoice::load(&palette.to_string());
    let gpu = view.rust().gpu_compute;
    let made = match source.rust().session.doc.as_ref() {
        Some(doc) => convert(
            doc,
            &name.to_string(),
            cut,
            &choice,
            nearest,
            gpu,
            &Limits::current(),
            allowance,
        ),
        None => fail("no document"),
    };
    install(view, made, true)
}
