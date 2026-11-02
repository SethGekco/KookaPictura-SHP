// SPDX-License-Identifier: GPL-3.0-or-later
//! New SHP documents and Save As SHP's conversion.

use super::authoring::*;
use super::tests::{choice, dab, doc, palette, pencil, roomy};
use super::*;
use pictura_shp::parse;

fn blank(width: usize, height: usize, frames: usize) -> PictureViewRust {
    let opened =
        blank_sheet("new", width, height, frames, &choice(), false, &roomy(), 0).expect("makes");
    let mut rust = PictureViewRust::default();
    crate::cxxqt_object::open_jobs::apply_opened(&mut rust, opened);
    rust
}

#[test]
fn a_new_shp_saves_what_was_drawn_and_reads_back() {
    let mut rust = blank(10, 8, 3);
    let h = header(doc(&rust)).unwrap();
    assert_eq!((h.layout.width, h.layout.height, h.total), (10, 8, 3));
    let block = block(doc(&rust)).unwrap();
    assert_eq!(&block[4..6], &[0, 1], "a new sheet is a version 1 block");
    assert!(
        resource::parts(block).unwrap().2.is_empty(),
        "no source file"
    );

    // Empty frames save as an SHP of three empty frames.
    let bytes = encode(doc(&rust)).unwrap();
    assert!(pictura_shp::sniff(&bytes, Some(bytes.len())));
    assert_eq!(parse(&bytes).unwrap().frames.len(), 3);

    // One dab of a different index per frame.
    let unique = unique_colours(&palette());
    let mut want = vec![vec![0u8; 80]; 3];
    for (k, plane) in want.iter_mut().enumerate() {
        let (x0, y0) = h.layout.cell_origin(k);
        let (x, y) = (2 + k, 3);
        dab(
            &mut rust,
            pencil(unique.argb[50 + k]),
            (x0 + x) as f32 + 0.5,
            (y0 + y) as f32 + 0.5,
        );
        plane[y * 10 + x] = 50 + k as u8;
    }
    let bytes = encode(doc(&rust)).unwrap();
    let sprite = parse(&bytes).unwrap();
    assert_eq!((sprite.width, sprite.height), (10, 8));
    for (k, plane) in want.iter().enumerate() {
        assert_eq!(&sprite.canvas(k), plane, "frame {k}");
    }
    for k in 0..3 {
        let offset = pictura_shp::FrameHeader::read(&bytes, k).offset;
        assert_eq!(offset % 8, 0, "frame {k} data is aligned to 8");
    }
}

#[test]
fn a_new_shp_is_held_to_the_format_and_the_size_limit() {
    let tight = Limits {
        limit: 1000,
        available: None,
    };
    let err = blank_sheet("x", 40, 40, 4, &choice(), false, &tight, 0)
        .err()
        .unwrap();
    assert!(err.contains("size limit"), "{err}");
    let plan = shp_plan_sheet(40, 40, 4);
    assert!(plan.valid);
    assert!(blank_sheet("x", 40, 40, 4, &choice(), false, &tight, plan.needed).is_ok());
    for (w, h, n) in [(0, 5, 1), (5, 4097, 1), (5, 5, 0), (5, 5, 4097)] {
        assert!(blank_sheet("x", w, h, n, &choice(), false, &roomy(), 0).is_err());
        assert!(!shp_plan_sheet(w as i32, h as i32, n as i32).valid);
    }
}

#[test]
fn an_image_is_cut_into_frames_and_mapped_to_the_palette() {
    let p = palette();
    let unique = unique_colours(&p);
    // A 2 x 2 grid of 3x2 cells with a 1 px gutter: 7 x 5 pixels.
    let (iw, ih) = (7usize, 5usize);
    let mut image = vec![0u8; iw * ih * 4];
    let mut put = |x: usize, y: usize, px: [u8; 4]| {
        image[(y * iw + x) * 4..(y * iw + x) * 4 + 4].copy_from_slice(&px)
    };
    let exact = unique.argb[60];
    let rgb = |c: u32| [(c >> 16) as u8, (c >> 8) as u8, c as u8];
    let [r, g, b] = rgb(exact);
    put(0, 0, [r, g, b, 255]); // frame 0: an exact palette colour
    let [r2, g2, b2] = rgb(p.argb[100]);
    put(4 + 1, 1, [r2, g2 ^ 1, b2, 255]); // frame 1: close to index 100's colour
    put(0, 3 + 1, [200, 10, 10, 90]); // frame 2: mostly transparent
    put(4 + 2, 3, [r, g, b, 255]); // frame 3
    let cut = Cut {
        width: 3,
        height: 2,
        cols: 2,
        rows: 2,
        gutter: 1,
        frames: 4,
    };
    let (layout, sheet) = cut_sheet(&image, iw, ih, cut, &p, true).unwrap();
    assert_eq!((layout.frames, layout.width, layout.height), (4, 3, 2));
    let scan = sheet::from_sheet(&layout, &sheet, &sheet::inverse(&unique));
    assert_eq!(scan.off_palette_total(), 0);
    assert_eq!(scan.planes[0][0], 60);
    assert_eq!(
        scan.planes[1][3 + 1],
        pictura_shp::palette::nearest(&p, p.argb[100] ^ 0x100)
    );
    assert!(
        scan.planes[2].iter().all(|&i| i == 0),
        "alpha under 128 is index 0"
    );
    assert_eq!(scan.planes[3][2], 60);

    // Exact only: the near colour stays off-palette for the gate.
    let (_, sheet) = cut_sheet(&image, iw, ih, cut, &p, false).unwrap();
    let scan = sheet::from_sheet(&layout, &sheet, &sheet::inverse(&unique));
    assert_eq!(scan.off_frames(), vec![1, 2]);

    // Three frames from the same grid; a grid bigger than the image fails.
    let three = Cut { frames: 3, ..cut };
    assert_eq!(
        cut_sheet(&image, iw, ih, three, &p, true).unwrap().0.frames,
        3
    );
    let wide = Cut { cols: 3, ..cut };
    assert!(cut_sheet(&image, iw, ih, wide, &p, true).is_err());
}

#[test]
fn a_converted_document_saves_as_a_valid_shp() {
    let source = Document::from_rgba("plain", 8, 4, &{
        let mut px = vec![0u8; 8 * 4 * 4];
        for (i, v) in px.as_chunks_mut::<4>().0.iter_mut().enumerate() {
            if i % 3 == 0 {
                v.copy_from_slice(&[(i * 7) as u8, 90, 200, 255]);
            }
        }
        px
    });
    let cut = Cut {
        width: 4,
        height: 4,
        cols: 2,
        rows: 1,
        gutter: 0,
        frames: 2,
    };
    let opened = convert(&source, "plain", cut, &choice(), true, false, &roomy(), 0).unwrap();
    let bytes = encode(&opened.doc).unwrap();
    let sprite = parse(&bytes).unwrap();
    assert_eq!(
        (sprite.width, sprite.height, sprite.frames.len()),
        (4, 4, 2)
    );
    assert!(sprite.canvas(0).iter().any(|&i| i != 0));
}
