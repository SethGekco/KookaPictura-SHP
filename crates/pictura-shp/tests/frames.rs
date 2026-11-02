//! Frame operations: the new frame order, the sheet reflow, the frame
//! origins in the resource, and the writer keeping moved frames' bytes.

mod common;

use common::{assemble, header, sprite_plane, table, Rng};
use pictura_shp::frames::FrameOp;
use pictura_shp::palette::{fallback_palette, unique_colours};
use pictura_shp::resource::{identity_origins, origins, Header, ShpResource};
use pictura_shp::sheet::{
    cell_rgba, from_sheet, gutter_pixels, inverse, reflow, reframe, to_sheet, Layout,
};
use pictura_shp::{
    build_mapped, encode_rle, parse, sniff, FrameHeader, FramePlane, Source, MAX_FRAMES,
};
use std::borrow::Cow;

#[test]
fn every_operation_gives_its_order_and_refuses_what_does_not_apply() {
    let ids = |o: Vec<Option<usize>>| {
        o.into_iter()
            .map(|v| v.map_or(-1, |k| k as i32))
            .collect::<Vec<_>>()
    };
    let order = |op: FrameOp| op.order(5).map(ids);
    assert_eq!(
        order(FrameOp::Insert { at: 2, count: 2 }),
        Some(vec![0, 1, -1, -1, 2, 3, 4])
    );
    assert_eq!(
        order(FrameOp::Insert { at: 5, count: 1 }),
        Some(vec![0, 1, 2, 3, 4, -1])
    );
    assert_eq!(
        order(FrameOp::Duplicate { frame: 4 }),
        Some(vec![0, 1, 2, 3, 4, 4])
    );
    assert_eq!(
        order(FrameOp::Delete { first: 1, count: 3 }),
        Some(vec![0, 4])
    );
    assert_eq!(
        order(FrameOp::Move { from: 0, to: 3 }),
        Some(vec![1, 2, 3, 0, 4])
    );
    assert_eq!(
        order(FrameOp::Move { from: 4, to: 1 }),
        Some(vec![0, 4, 1, 2, 3])
    );
    assert_eq!(
        order(FrameOp::Reverse { first: 1, count: 3 }),
        Some(vec![0, 3, 2, 1, 4])
    );
    for refused in [
        FrameOp::Insert { at: 6, count: 1 },
        FrameOp::Insert { at: 0, count: 0 },
        FrameOp::Duplicate { frame: 5 },
        FrameOp::Delete { first: 0, count: 5 },
        FrameOp::Delete { first: 4, count: 2 },
        FrameOp::Move { from: 2, to: 2 },
        FrameOp::Move { from: 1, to: 5 },
        FrameOp::Reverse { first: 2, count: 1 },
        FrameOp::Reverse { first: 3, count: 3 },
    ] {
        assert_eq!(order(refused), None, "{refused:?}");
    }
    let full = usize::from(MAX_FRAMES);
    assert!(FrameOp::Insert { at: 0, count: 1 }.order(full).is_none());
    assert_eq!(FrameOp::Delete { first: 4, count: 1 }.focus(4), 3);
    assert_eq!(FrameOp::Duplicate { frame: 2 }.focus(6), 3);
    assert_eq!(
        FrameOp::parse("move", 1, 0, 3),
        Some(FrameOp::Move { from: 1, to: 3 })
    );
    assert_eq!(FrameOp::parse("rotate", 1, 0, 3), None);
}

#[test]
fn reflow_moves_cells_exactly_and_leaves_gutters_clear() {
    let unique = unique_colours(&fallback_palette());
    let (w, h) = (5usize, 4usize);
    let planes: Vec<Vec<u8>> = (0..5u8)
        .map(|k| {
            (0..w * h)
                .map(|i| if i % 3 == 0 { 0 } else { 10 + k })
                .collect()
        })
        .collect();
    let old = Layout::new(w, h, 5, 1).unwrap();
    let mut rgba = to_sheet(&old, &planes, &unique);
    // A half-transparent pixel in frame 2 travels with it, unchanged.
    let (x2, y2) = old.cell_origin(2);
    let at = ((y2 + 1) * old.sheet_width() + x2 + 1) * 4;
    rgba[at..at + 4].copy_from_slice(&[9, 8, 7, 100]);
    let order = FrameOp::Move { from: 2, to: 0 }.order(5).unwrap();
    let order: Vec<Option<usize>> = order.into_iter().chain([None]).collect();
    let new = Layout::new(w, h, 6, 1).unwrap();
    let out = reflow(&old, &rgba, &new, &order);
    assert_eq!(out.len(), new.sheet_width() * new.sheet_height() * 4);
    assert_eq!(gutter_pixels(&new, &out), 0);
    assert_eq!(cell_rgba(&new, &out, 0), cell_rgba(&old, &rgba, 2));
    assert_eq!(cell_rgba(&new, &out, 1), cell_rgba(&old, &rgba, 0));
    assert!(
        cell_rgba(&new, &out, 5).iter().all(|&v| v == 0),
        "blank cell"
    );
    let scan = from_sheet(&new, &out, &inverse(&unique));
    assert_eq!(scan.off_palette, vec![1, 0, 0, 0, 0, 0]);
    assert_eq!(scan.planes[3], planes[3]);
    assert_eq!(scan.planes[4], planes[4]);

    // A gutter pixel is counted.
    let mut dirty = rgba.clone();
    let gx = old.width;
    dirty[gx * 4 + 3] = 255;
    assert_eq!(gutter_pixels(&old, &dirty), 1);
}

#[test]
fn origins_are_kept_only_once_they_differ_from_the_identity() {
    let layout = Layout::new(8, 8, 4, 1).unwrap();
    let mut file = vec![0u8; 8];
    file[6] = 4; // a header claiming 4 frames
    let mut resource = ShpResource {
        header: Header::whole(layout, fallback_palette()),
        palette_path: String::new(),
        original: file,
        stored: Vec::new(),
        origins: identity_origins(4, 4),
    };
    let data = resource.encode();
    assert_eq!(&data[4..6], &[0, 1], "the identity keeps version 1");
    assert_eq!(origins(&data), Some(identity_origins(4, 4)));

    resource.origins = vec![Some(1), Some(0), None, Some(3)];
    let data = resource.encode();
    assert_eq!(&data[4..6], &[0, 3]);
    assert_eq!(ShpResource::decode(&data), Some(resource.clone()));
    assert_eq!(Header::read(&data), Some(resource.header.clone()));
    assert!(!resource.header.paged());

    // Frames past the file's count are new in the identity.
    assert_eq!(identity_origins(3, 2), vec![Some(0), Some(1), None]);
}

/// Four frames: raw, RLE with a radar colour, empty, RLE; garbage gaps and a
/// tail.
fn source_file() -> (Vec<u8>, Vec<Vec<u8>>) {
    let mut rng = Rng::new(5);
    let (cw, ch) = (20usize, 12usize);
    let a = sprite_plane(&mut rng, 9, 6);
    let b = sprite_plane(&mut rng, 7, 8);
    let d = sprite_plane(&mut rng, 11, 5);
    let blocks = [a.clone(), encode_rle(&b, 7), encode_rle(&d, 11)];
    let mut offsets = Vec::new();
    let mut at = table(4) + 3;
    for block in &blocks {
        offsets.push(at);
        at += block.len() + 5;
    }
    let headers = [
        header(1, 2, 9, 6, 0x0000_0001, offsets[0] as u32),
        header(10, 1, 7, 8, 0x0055_0003, offsets[1] as u32),
        header(0, 0, 0, 0, 0, 0),
        header(4, 6, 11, 5, 0x00AB_0003, offsets[2] as u32),
    ];
    let file = assemble(
        cw as u16,
        ch as u16,
        &headers,
        &[
            (offsets[0], blocks[0].clone()),
            (offsets[1], blocks[1].clone()),
            (offsets[2], blocks[2].clone()),
        ],
        0x0F,
        b"END",
    );
    let canvases = Source::read(file.clone()).unwrap().canvases();
    (file, canvases)
}

fn payload(file: &[u8], i: usize, len: usize) -> &[u8] {
    let at = FrameHeader::read(file, i).offset as usize;
    &file[at..at + len]
}

#[test]
fn moved_and_duplicated_frames_keep_their_bytes_through_their_origins() {
    let (file, canvases) = source_file();
    let source = Source::read(file.clone()).unwrap();
    // Reverse everything, then duplicate frame 1 (old 2, empty) and 0 (old 3).
    let origins = [Some(3), Some(3), Some(2), Some(1), Some(0), None];
    let out = build_mapped(
        20,
        12,
        origins.len(),
        Some(&source),
        |i| origins[i],
        |i| match origins[i] {
            Some(_) => FramePlane::Unchanged,
            None => FramePlane::Pixels(Cow::Owned(vec![0; 20 * 12])),
        },
    )
    .unwrap();
    assert!(sniff(&out, Some(out.len())));
    let back = parse(&out).unwrap();
    for (i, o) in origins.iter().enumerate() {
        let want = o.map_or_else(|| vec![0; 20 * 12], |j| canvases[j].clone());
        assert_eq!(back.canvas(i), want, "frame {i}");
    }
    for (i, j) in [(0usize, 3usize), (1, 3), (3, 1), (4, 0)] {
        let (was, now) = (FrameHeader::read(&file, j), FrameHeader::read(&out, i));
        assert_eq!(
            (now.x, now.y, now.w, now.h, now.flags, now.radar),
            (was.x, was.y, was.w, was.h, was.flags, was.radar),
            "frame {i} from {j}"
        );
        let len = source.payload(j).len();
        assert_eq!(payload(&out, i, len), source.payload(j), "frame {i} bytes");
        assert_eq!(now.offset % 8, 0, "a moved frame is aligned");
    }
    assert_ne!(
        FrameHeader::read(&out, 0).offset,
        FrameHeader::read(&out, 1).offset,
        "a duplicate gets its own copy"
    );
    assert!(out.ends_with(b"END"));
}

#[test]
fn a_frame_moved_by_hand_is_found_by_its_content() {
    let (file, canvases) = source_file();
    let source = Source::read(file.clone()).unwrap();
    // Positional pairing, but frames 0 and 3 swapped as plain pixels.
    let planes = [&canvases[3], &canvases[1], &canvases[2], &canvases[0]];
    let out = build_mapped(20, 12, 4, Some(&source), Some, |i| {
        FramePlane::Pixels(Cow::Borrowed(&planes[i][..]))
    })
    .unwrap();
    let back = parse(&out).unwrap();
    for (i, plane) in planes.iter().enumerate() {
        assert_eq!(&back.canvas(i), *plane);
    }
    for (i, j) in [(0usize, 3usize), (3, 0)] {
        let len = source.payload(j).len();
        assert_eq!(
            payload(&out, i, len),
            source.payload(j),
            "frame {i} keeps {j}'s bytes"
        );
        assert_eq!(
            FrameHeader::read(&out, i).flags,
            FrameHeader::read(&file, j).flags
        );
    }
    // Frame 1 stayed in place: offset and gap bytes as in the file.
    assert_eq!(
        FrameHeader::read(&out, 1).offset,
        FrameHeader::read(&file, 1).offset
    );
}

#[test]
fn reframe_places_each_frame_by_the_anchor() {
    let unique = unique_colours(&fallback_palette());
    // Frame k: a 4x3 frame whose pixel (x, y) is index 1 + 12k + 4y + x.
    let old = Layout::new(4, 3, 3, 1).unwrap();
    let planes: Vec<Vec<u8>> = (0..3)
        .map(|k| (0..12).map(|i| (1 + 12 * k + i) as u8).collect())
        .collect();
    let rgba = to_sheet(&old, &planes, &unique);
    let inv = inverse(&unique);

    // Grow to 6x5, centred: one column and row of padding before.
    let grown = Layout::new(6, 5, 3, 1).unwrap();
    let scan = from_sheet(&grown, &reframe(&old, &rgba, &grown, (1, 1)), &inv);
    assert_eq!(scan.off_palette_total(), 0);
    for (k, plane) in scan.planes.iter().enumerate() {
        assert_eq!(plane[6 + 1], (1 + 12 * k) as u8, "old (0,0) lands at (1,1)");
        assert_eq!(plane[0], 0);
        assert_eq!(
            plane[3 * 6 + 4],
            (1 + 12 * k + 8 + 3) as u8,
            "old (3,2) at (4,3)"
        );
    }

    // Shrink to 2x2 at the bottom right: the frame's last two columns and rows.
    let small = Layout::new(2, 2, 3, 1).unwrap();
    let scan = from_sheet(&small, &reframe(&old, &rgba, &small, (2, 2)), &inv);
    assert_eq!(
        scan.planes[2],
        vec![1 + 24 + 6, 1 + 24 + 7, 1 + 24 + 10, 1 + 24 + 11]
    );
    assert_eq!(
        gutter_pixels(&small, &reframe(&old, &rgba, &small, (2, 2))),
        0
    );

    // Top-left keeps the corner.
    let scan = from_sheet(&small, &reframe(&old, &rgba, &small, (0, 0)), &inv);
    assert_eq!(scan.planes[0], vec![1, 2, 5, 6]);
}
