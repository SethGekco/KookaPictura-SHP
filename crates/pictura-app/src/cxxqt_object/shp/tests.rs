// SPDX-License-Identifier: GPL-3.0-or-later
//! SHP documents end to end through the app glue. Every sprite and palette
//! here is generated; no game file is used.

use super::*;
use crate::cxxqt_object::helpers_composite::{buffer_to_rgba_bytes, current_buffer};
use crate::cxxqt_object::open_jobs::apply_opened;
use pictura_paint::spacing::SpacingMode;
use pictura_paint::{PaintMode, Rgba, StrokeConfig, StrokeSample};
use pictura_shp::palette::write_vga;
use pictura_shp::{parse, FRAME_HEADER, HEADER};

const W: usize = 12;
const H: usize = 10;
const FRAMES: usize = 10;

/// Frame `k`: a filled rectangle of index `40 + k` with a dot of `k + 1`,
/// in a different place per frame, so frames differ and some are raw.
fn planes() -> Vec<Vec<u8>> {
    (0..FRAMES)
        .map(|k| {
            let mut p = vec![0u8; W * H];
            for y in 1 + k % 3..H - 1 {
                for x in 2..W - 1 - k % 4 {
                    p[y * W + x] = 40 + k as u8;
                }
            }
            p[(k % H) * W + k % W] = k as u8 + 1;
            p
        })
        .collect()
}

/// An SHP with what real files carry: a raw frame with a size in its flag
/// word, garbage padding, and a tail.
fn sprite() -> Vec<u8> {
    let mut bytes = pictura_shp::build(W as u16, H as u16, &planes()).expect("build");
    bytes.extend_from_slice(b"TAIL\x01\x02");
    assert!(pictura_shp::sniff(&bytes, Some(bytes.len())));
    bytes
}

/// A palette with repeated colours (four indices per colour, as game
/// palettes repeat), the remap ramp included.
fn palette() -> Palette {
    let mut argb = [0u32; 256];
    for (i, c) in argb.iter_mut().enumerate() {
        let v = (i as u32 / 4) * 4;
        *c = 0xFF00_0000 | (v << 16) | ((255 - v) << 8) | (v / 2);
    }
    argb[0] &= 0x00FF_FFFF;
    // 6-bit precision, as a .pal stores it.
    let vga = write_vga(&Palette {
        argb,
        name: String::new(),
    });
    let mut p = read_palette(&vga).expect("vga");
    p.name = "unittem.pal".into();
    p
}

fn temp_pal(name: &str, palette: &Palette) -> String {
    let dir = std::env::temp_dir().join(format!("pictura-shp-{}-{name}", std::process::id()));
    std::fs::create_dir_all(&dir).expect("temp dir");
    let path = dir.join("unittem.pal");
    std::fs::write(&path, write_vga(palette)).expect("write pal");
    path.to_string_lossy().into_owned()
}

fn opened(bytes: Vec<u8>, choice: PaletteChoice) -> PictureViewRust {
    let opened = open_sheet("/tmp/fixture.shp", bytes, choice, false, &|_, _| {}).expect("opens");
    let mut rust = PictureViewRust::default();
    apply_opened(&mut rust, opened);
    rust
}

fn choice() -> PaletteChoice {
    PaletteChoice {
        palette: palette(),
        path: "/pal/unittem.pal".into(),
    }
}

fn doc(rust: &PictureViewRust) -> &Document {
    rust.session.doc.as_ref().expect("document")
}

fn layout(rust: &PictureViewRust) -> Layout {
    header(doc(rust)).expect("resource").layout
}

fn argb(c: u32) -> Rgba {
    Rgba {
        r: (c >> 16) as u8,
        g: (c >> 8) as u8,
        b: c as u8,
        a: 255,
    }
}

/// One dab at `(x, y)`, committed the way `end_paint` does.
fn dab(rust: &mut PictureViewRust, cfg: StrokeConfig, x: f32, y: f32) {
    assert!(rust.begin_paint(cfg, "Pencil"));
    let rect = {
        let mut session = rust.session.live.take_stroke_session().expect("session");
        let mut stroke = session.stroke.take().expect("stroke");
        let doc = rust.session.doc.as_mut().expect("doc");
        stroke.sample(
            doc,
            StrokeSample {
                x,
                y,
                pressure: 1.0,
            },
        );
        stroke.finish().expect("painted")
    };
    rust.apply_edit("Pencil", Edit::regions(vec![rect], false));
}

fn pencil(colour: u32) -> StrokeConfig {
    StrokeConfig {
        color: argb(colour),
        background: argb(0xFFFF_FFFF),
        diameter: 1,
        hardness: 100,
        spacing: SpacingMode::Fixed(25),
        opacity: 100,
        flow: 100,
        mode: PaintMode::Normal,
        aliased: true,
        ..StrokeConfig::default()
    }
}

fn frame_bytes(file: &[u8], i: usize) -> (&[u8], &[u8]) {
    let header = &file[HEADER + FRAME_HEADER * i..HEADER + FRAME_HEADER * (i + 1)];
    let f = pictura_shp::FrameHeader::read(file, i);
    let span = pictura_shp::frame_span(file, &f);
    let start = f.offset as usize;
    (header, &file[start..start + span])
}

#[test]
fn an_shp_opens_as_a_sheet_with_its_resource_and_keeps_alpha() {
    let rust = opened(sprite(), choice());
    let l = layout(&rust);
    assert_eq!((l.width, l.height, l.frames, l.cols), (W, H, FRAMES, 4));
    let d = doc(&rust);
    assert_eq!(
        (d.width as usize, d.height as usize),
        (l.sheet_width(), l.sheet_height())
    );
    assert!(d.document_icc.is_none());
    let (h, path, original) = resource::parts(block(d).unwrap()).unwrap();
    assert_eq!(h.palette, palette());
    assert_eq!(path, "/pal/unittem.pal");
    assert_eq!(original, &sprite()[..]);
    assert_eq!(rust.source_format, "shp");

    // A single all-opaque frame: still a transparent layer, not a locked
    // Background without alpha (no finalize_import).
    let solid = pictura_shp::build(4, 4, &[vec![7u8; 16]]).unwrap();
    let rust = opened(solid, choice());
    let layer = &doc(&rust).layers[0];
    assert!(!layer.background);
    assert!(layer.channels.iter().any(|c| c.id == -1));
}

#[test]
fn the_cpu_composite_is_the_layer_itself() {
    let rust = opened(sprite(), choice());
    let d = doc(&rust);
    let plane = d.width as usize * d.height as usize;
    assert_eq!(
        buffer_to_rgba_bytes(&current_buffer(d, false)),
        layer_rgba(&d.layers[0], plane)
    );
}

#[test]
fn an_unchanged_save_is_byte_identical() {
    let rust = opened(sprite(), choice());
    assert_eq!(encode(doc(&rust)).expect("encodes"), sprite());
    let check = check(doc(&rust));
    assert!(check.error.is_empty());
    assert_eq!((check.off_palette, check.gutter), (0, 0));
}

#[test]
fn a_pencil_dab_of_a_palette_colour_saves_that_index_and_only_that_frame_changes() {
    let mut rust = opened(sprite(), choice());
    let unique = unique_colours(&palette());
    let l = layout(&rust);
    // Index 22 shares its colour with 20, 21 and 23 in the palette as read.
    let (x0, y0) = l.cell_origin(5);
    dab(
        &mut rust,
        pencil(unique.argb[22]),
        (x0 + 3) as f32 + 0.5,
        (y0 + 4) as f32 + 0.5,
    );
    let saved = encode(doc(&rust)).expect("an exact pencil dab saves");

    let sprite = parse(&saved).expect("parses");
    let mut expected = planes();
    expected[5][4 * W + 3] = 22;
    let canvases: Vec<Vec<u8>> = (0..FRAMES).map(|i| sprite.canvas(i)).collect();
    assert_eq!(canvases, expected);
    let original = self::sprite();
    for i in (0..FRAMES).filter(|&i| i != 5) {
        assert_eq!(
            frame_bytes(&saved, i).1,
            frame_bytes(&original, i).1,
            "frame {i} data"
        );
        let (a, b) = (frame_bytes(&saved, i).0, frame_bytes(&original, i).0);
        assert_eq!(a[..20], b[..20], "frame {i} header (offset aside)");
    }
    assert!(saved.ends_with(b"TAIL\x01\x02"));
}

#[test]
fn a_soft_brush_dab_is_reported_and_snap_fixes_it() {
    let mut rust = opened(sprite(), choice());
    let unique = unique_colours(&palette());
    let l = layout(&rust);
    let (x0, y0) = l.cell_origin(2);
    let mut soft = pencil(unique.argb[21] ^ 0x0000_0300);
    soft.aliased = false;
    soft.diameter = 5;
    soft.hardness = 0;
    dab(&mut rust, soft, (x0 + 5) as f32, (y0 + 5) as f32);
    let found = check(doc(&rust));
    assert!(found.off_palette > 0);
    assert_eq!(found.off_frames, vec![2]);
    assert_eq!(found.first_off.len(), 4);
    assert!(encode(doc(&rust)).is_err(), "never silently snapped");

    let changed = snap_rust(&mut rust).expect("snaps");
    assert_eq!(changed, found.off_palette as usize);
    rust.apply_edit("Snap to Palette", Edit::Full);
    assert_eq!(check(doc(&rust)).off_palette, 0);
    let saved = parse(&encode(doc(&rust)).expect("saves after the snap")).unwrap();
    assert_ne!(
        saved.canvas(2)[5 * W + 5],
        0,
        "an opaque centre stays opaque"
    );
    // Frames the dab never reached keep their pixels.
    for i in (0..FRAMES).filter(|&i| i != 2) {
        assert_eq!(saved.canvas(i), planes()[i]);
    }
}

#[test]
fn a_gutter_pixel_blocks_the_save_until_cleared() {
    let mut rust = opened(sprite(), choice());
    let unique = unique_colours(&palette());
    let l = layout(&rust);
    dab(
        &mut rust,
        pencil(unique.argb[50]),
        l.width as f32 + 0.5,
        0.5,
    );
    assert_eq!(check(doc(&rust)).gutter, 1);
    assert!(encode(doc(&rust)).is_err());
    assert_eq!(clear_gutter_rust(&mut rust), Ok(1));
    rust.apply_edit("Clear Frame Gutters", Edit::Full);
    assert_eq!(
        encode(doc(&rust)).expect("saves"),
        sprite(),
        "back to the original"
    );
}

#[test]
fn a_size_change_is_refused_at_save() {
    let mut rust = opened(sprite(), choice());
    let d = rust.session.doc.as_mut().unwrap();
    d.width += 1;
    let found = check(doc(&rust));
    assert!(found.error.to_string().contains("need"));
}

#[test]
fn undoing_change_palette_restores_palette_and_pixels_together() {
    let mut rust = opened(sprite(), choice());
    let before = doc(&rust).clone();
    let mut grey = fallback_palette();
    grey.name = "other.pal".into();
    let path = temp_pal("rebind", &grey);
    let new = PaletteChoice::try_load(&path).expect("loads");
    assert_eq!(rebind_rust(&mut rust, new.clone()), Ok(true));
    rust.apply_edit("Change Palette", Edit::Full);
    let after = doc(&rust);
    assert_eq!(header(after).unwrap().palette, new.palette);
    assert_ne!(after.layers[0].channels, before.layers[0].channels);
    // The same file, indices unchanged: saving still writes the original.
    assert_eq!(encode(after).expect("saves"), sprite());

    assert!(rust
        .session
        .restore(&mut rust.layer_target, History::undo_live, History::undo)
        .is_some());
    let undone = doc(&rust);
    assert_eq!(header(undone).unwrap().palette, palette());
    assert_eq!(undone.layers[0].channels, before.layers[0].channels);
    assert_eq!(undone.image_resources, before.image_resources);
    let _ = std::fs::remove_file(&path);
}

#[test]
fn the_resource_survives_a_psd_round_trip() {
    let rust = opened(sprite(), choice());
    let psd = pictura_codec::write_psd(doc(&rust)).expect("writes");
    let back = pictura_codec::read_psd(&psd).expect("reads");
    assert_eq!(header(&back), header(doc(&rust)));
    assert_eq!(encode(&back).expect("encodes"), sprite());
}

#[test]
fn the_open_palette_reaches_the_decode_and_revert_keeps_the_document_one() {
    let bytes = sprite();
    let path = temp_pal("open", &palette());
    let opened = with_open_palette(Some(path.clone()), || {
        decode_shp("/tmp/x.shp", &bytes, false, &|_, _| {})
    })
    .expect("an shp")
    .expect("opens");
    let (h, p, _) = resource::parts(block(&opened.doc).unwrap()).unwrap();
    assert_eq!(h.palette.argb, palette().argb);
    assert_eq!(p, path);

    let again = with_document_palette(Some(&opened.doc), || {
        decode_shp("/tmp/x.shp", &bytes, false, &|_, _| {})
    })
    .unwrap()
    .unwrap();
    assert_eq!(header(&again.doc), header(&opened.doc));

    // No palette chosen: the grey fallback, indices intact.
    let plain = decode_shp("/tmp/x.shp", &bytes, false, &|_, _| {})
        .unwrap()
        .unwrap();
    assert_eq!(header(&plain.doc).unwrap().palette, fallback_palette());
    assert_eq!(encode(&plain.doc).unwrap(), bytes);
    assert!(decode_shp("/tmp/x.png", b"\x89PNG\r\n", false, &|_, _| {}).is_none());
    let _ = std::fs::remove_file(&path);
}

/// The corpus gate again, through the app path: open as a sheet (with a real
/// palette when given), save unchanged, and save after one exact-colour
/// pixel edit on the sheet layer. Reads the corpus, writes nothing.
///
/// ```text
/// PICTURA_SHP_CORPUS_LIST=/path/list0 [PICTURA_SHP_PALETTE=/path/unittem.pal]
/// cargo test -p pictura_app --lib shp::imp::tests::corpus -- --ignored --nocapture
/// ```
#[test]
#[ignore = "needs PICTURA_SHP_CORPUS_LIST (a private corpus of real SHPs)"]
fn corpus_through_the_sheet() {
    let list = std::env::var("PICTURA_SHP_CORPUS_LIST").expect("PICTURA_SHP_CORPUS_LIST");
    let palette = std::env::var("PICTURA_SHP_PALETTE").unwrap_or_default();
    let choice = PaletteChoice::load(&palette);
    let unique = unique_colours(&choice.palette);
    let names = std::fs::read(list).expect("list");
    let (mut files, mut shps, mut unchanged, mut edited, mut edit_ok) = (0, 0, 0, 0, 0);
    let mut failures = Vec::new();
    for name in names.split(|&b| b == 0).filter(|n| !n.is_empty()) {
        let path = String::from_utf8_lossy(name).into_owned();
        files += 1;
        let Ok(bytes) = std::fs::read(&path) else {
            continue;
        };
        if !pictura_shp::sniff(&bytes, Some(bytes.len())) {
            continue;
        }
        shps += 1;
        let Some(mut opened) = open_sheet(&path, bytes.clone(), choice.clone(), false, &|_, _| {})
        else {
            failures.push(format!("open: {path}"));
            continue;
        };
        match encode(&opened.doc) {
            Ok(out) if out == bytes => unchanged += 1,
            Ok(_) => failures.push(format!("unchanged differs: {path}")),
            Err(e) => failures.push(format!("unchanged refused ({e}): {path}")),
        }
        // One pixel in the middle drawn frame becomes another exact colour.
        let l = header(&opened.doc).unwrap().layout;
        let source = Source::read(bytes.clone()).unwrap();
        let drawn: Vec<usize> = (0..l.frames)
            .filter(|&i| source.frames()[i].frame.pixels.is_some())
            .collect();
        let Some(&k) = drawn.get(drawn.len() / 2) else {
            continue;
        };
        let h = *source.frames()[k].header();
        let (cx, cy) = (usize::from(h.x + h.w / 2), usize::from(h.y + h.h / 2));
        let mut planes = source.canvases();
        let was = planes[k][cy * l.width + cx];
        let now = if was == 200 { 201 } else { 200 };
        planes[k][cy * l.width + cx] = now;
        let (x0, y0) = l.cell_origin(k);
        let at = (y0 + cy) * opened.doc.width as usize + x0 + cx;
        let c = unique.argb[usize::from(now)];
        for (id, v) in [
            (0i16, (c >> 16) as u8),
            (1, (c >> 8) as u8),
            (2, c as u8),
            (-1, 255),
        ] {
            let channel = opened.doc.layers[0]
                .channels
                .iter_mut()
                .find(|ch| ch.id == id)
                .unwrap();
            channel.data[at] = v;
        }
        edited += 1;
        let Ok(out) = encode(&opened.doc) else {
            failures.push(format!("edit refused: {path}"));
            continue;
        };
        let back = parse(&out).expect("the edit parses");
        let others_kept = (0..l.frames).filter(|&i| i != k).all(|i| {
            let (a, b) = (frame_bytes(&out, i), frame_bytes(&bytes, i));
            a.1 == b.1 && a.0[..20] == b.0[..20]
        });
        if (0..l.frames).all(|i| back.canvas(i) == planes[i]) && others_kept {
            edit_ok += 1;
        } else {
            failures.push(format!("edit wrong: {path}"));
        }
    }
    eprintln!(
        "files={files} shps={shps} unchanged_identical={unchanged} edited={edited} \
         edit_ok={edit_ok} failures={}",
        failures.len()
    );
    for f in failures.iter().take(30) {
        eprintln!("  {f}");
    }
    assert!(failures.is_empty());
}
