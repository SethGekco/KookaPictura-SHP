//! The writer: round trips, and what an edit keeps from the source file.

mod common;

use common::{assemble, header, header_bytes, place, sprite_plane, table, Rng};
use pictura_shp::{
    build, build_frames, build_preserving, build_with, encode_prefixed, encode_rle, frame_canvas,
    parse, sniff, Error, FrameHeader, FramePlane, Source, FRAME_HEADER, HEADER,
};
use std::borrow::Cow;

fn frame_header(file: &[u8], i: usize) -> FrameHeader {
    FrameHeader::read(file, i)
}

#[test]
fn random_planes_round_trip_through_build_and_parse() {
    for seed in 0..60u64 {
        let mut rng = Rng::new(seed);
        let cw = 1 + rng.below(300) as usize;
        let ch = 1 + rng.below(120) as usize;
        let n = 1 + rng.below(12) as usize;
        let planes: Vec<Vec<u8>> = (0..n)
            .map(|_| {
                if rng.below(5) == 0 {
                    return vec![0u8; cw * ch]; // an empty frame
                }
                let w = 1 + rng.below(cw as u64) as usize;
                let h = 1 + rng.below(ch as u64) as usize;
                let x = rng.below((cw - w + 1) as u64) as usize;
                let y = rng.below((ch - h + 1) as u64) as usize;
                place(cw, ch, x, y, w, &sprite_plane(&mut rng, w, h))
            })
            .collect();
        let file = build(cw as u16, ch as u16, &planes).expect("builds");
        assert!(sniff(&file, Some(file.len())), "seed {seed}");
        let sprite = parse(&file).expect("parses");
        for (i, plane) in planes.iter().enumerate() {
            assert_eq!(&sprite.canvas(i), plane, "seed {seed} frame {i}");
        }
        // And through a source: unchanged is verbatim, an edit stays exact.
        let source = Source::read(file.clone()).unwrap();
        assert_eq!(
            build_preserving(cw as u16, ch as u16, &planes, Some(&source)).unwrap(),
            file
        );
        let mut edited = planes.clone();
        let k = rng.below(n as u64) as usize;
        let p = rng.below((cw * ch) as u64) as usize;
        edited[k][p] = edited[k][p].wrapping_add(1);
        let out = build_preserving(cw as u16, ch as u16, &edited, Some(&source)).unwrap();
        let back = parse(&out).expect("edited parses");
        for (i, plane) in edited.iter().enumerate() {
            assert_eq!(&back.canvas(i), plane, "seed {seed} edited frame {i}");
        }
    }
}

#[test]
fn new_frames_use_rle_only_when_smaller_and_align_to_8() {
    let (cw, ch) = (20usize, 4usize);
    let dense: Vec<u8> = (0..cw * ch).map(|i| 1 + (i % 250) as u8).collect();
    let mut sparse = vec![0u8; cw * ch];
    sparse[cw + 3] = 9;
    sparse[2 * cw + 15] = 9;
    let odd = place(cw, ch, 0, 0, 3, &[5, 6, 7]);
    let file = build(
        cw as u16,
        ch as u16,
        &[odd, dense, sparse, vec![0; cw * ch]],
    )
    .unwrap();
    let h0 = frame_header(&file, 0);
    let h1 = frame_header(&file, 1);
    let h2 = frame_header(&file, 2);
    let h3 = frame_header(&file, 3);
    assert_eq!(
        h0.flags, 0,
        "3 literals: RLE (5 bytes) is not smaller than raw (3)"
    );
    assert_eq!(h1.flags, 0, "no transparency: raw wins");
    assert_eq!(h2.flags, 3, "mostly transparent: RLE-Zero");
    assert_eq!(
        (h2.x, h2.y, h2.w, h2.h),
        (3, 1, 13, 2),
        "cropped to content"
    );
    assert_eq!(h3, FrameHeader::default(), "empty: a zero header, no data");
    for h in [h0, h1, h2] {
        assert_eq!(h.offset % 8, 0);
        assert_eq!(h.radar, [0; 4]);
        assert_eq!(h.reserved, 0);
    }
}

/// A source with one frame of each stored family, unaligned, garbage gaps,
/// flags with upper bits, a tail.
fn mixed_source() -> (Vec<u8>, Vec<Vec<u8>>) {
    let mut rng = Rng::new(11);
    let (cw, ch) = (24usize, 16usize);
    let a = sprite_plane(&mut rng, 10, 6);
    let b = sprite_plane(&mut rng, 7, 9);
    let c = sprite_plane(&mut rng, 12, 5);
    let blocks = [a.clone(), encode_prefixed(&b, 7), encode_rle(&c, 12)];
    let t = table(4);
    let mut offsets = Vec::new();
    let mut at = t + 3;
    for block in &blocks {
        offsets.push(at);
        at += block.len() + 5;
    }
    let headers = [
        header(2, 3, 10, 6, 0x0000_0001, offsets[0] as u32),
        header(10, 1, 7, 9, 0x0123_0002, offsets[1] as u32),
        header(0, 0, 0, 0, 0x0000_0003, 0), // empty
        header(5, 9, 12, 5, 0x00AB_0003, offsets[2] as u32),
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
        b"\x1e\x0f",
    );
    let source = Source::read(file.clone()).unwrap();
    (file, source.canvases())
}

#[test]
fn unchanged_is_verbatim_through_both_writers() {
    let (file, planes) = mixed_source();
    let source = Source::read(file.clone()).unwrap();
    assert_eq!(
        build_preserving(24, 16, &planes, Some(&source)).unwrap(),
        file
    );
    assert_eq!(build_frames(24, 16, &planes, Some(&source)).unwrap(), file);
}

#[test]
fn an_edited_frame_keeps_its_family_and_the_others_their_bytes() {
    let (file, planes) = mixed_source();
    let source = Source::read(file.clone()).unwrap();
    for k in [0usize, 1, 3] {
        let mut edited = planes.clone();
        let h = frame_header(&file, k);
        let p = (usize::from(h.y) + 1) * 24 + usize::from(h.x) + 1;
        edited[k][p] = if edited[k][p] == 77 { 78 } else { 77 };
        let out = build_preserving(24, 16, &edited, Some(&source)).unwrap();
        assert!(sniff(&out, Some(out.len())));
        let back = parse(&out).unwrap();
        for (i, plane) in edited.iter().enumerate() {
            assert_eq!(&back.canvas(i), plane, "edit {k}: frame {i}");
        }
        let now = frame_header(&out, k);
        assert_eq!(now.flags, h.flags, "edit {k}: the flag word is kept whole");
        assert_eq!((now.radar, now.reserved), (h.radar, h.reserved));
        assert_eq!(
            (now.x, now.y, now.w, now.h),
            (h.x, h.y, h.w, h.h),
            "rect kept"
        );
        for j in [0usize, 1, 2, 3].into_iter().filter(|&j| j != k) {
            assert_eq!(
                &header_bytes(&out, j)[..20],
                &header_bytes(&file, j)[..20],
                "edit {k}: frame {j} header"
            );
            let span = source.payload(j).len();
            let at = frame_header(&out, j).offset as usize;
            if span > 0 {
                assert_eq!(
                    &out[at..at + span],
                    source.payload(j),
                    "edit {k}: frame {j} data"
                );
            }
        }
        assert!(out.ends_with(b"\x1e\x0f"), "the tail is kept");
    }
}

#[test]
fn the_old_rect_is_kept_while_the_content_fits_and_dropped_when_it_grows() {
    let (file, planes) = mixed_source();
    let source = Source::read(file.clone()).unwrap();
    // Clear frame 0 down to one pixel: the stored 10x6 rect still holds it.
    let h = frame_header(&file, 0);
    let mut shrunk = planes.clone();
    shrunk[0].fill(0);
    shrunk[0][4 * 24 + 4] = 5;
    let out = build_preserving(24, 16, &shrunk, Some(&source)).unwrap();
    let now = frame_header(&out, 0);
    assert_eq!((now.x, now.y, now.w, now.h), (h.x, h.y, h.w, h.h));
    // Paint outside it: re-cropped to the new content.
    let mut grown = planes.clone();
    grown[0][15 * 24 + 23] = 6;
    let out = build_preserving(24, 16, &grown, Some(&source)).unwrap();
    let now = frame_header(&out, 0);
    let (mut x0, mut y0, mut x1, mut y1) = (24, 16, 0, 0);
    for (i, &v) in grown[0].iter().enumerate() {
        if v != 0 {
            let (x, y) = ((i % 24) as u16, (i / 24) as u16);
            (x0, y0, x1, y1) = (x0.min(x), y0.min(y), x1.max(x), y1.max(y));
        }
    }
    assert_eq!(
        (now.x, now.y, now.w, now.h),
        (x0, y0, x1 - x0 + 1, y1 - y0 + 1)
    );
    assert_eq!((x1, y1), (23, 15));
    assert_eq!(parse(&out).unwrap().canvas(0), grown[0]);
}

#[test]
fn an_empty_frame_that_gains_content_is_stored_like_a_new_one() {
    let (file, planes) = mixed_source();
    let source = Source::read(file.clone()).unwrap();
    let mut edited = planes.clone();
    edited[2][3 * 24 + 4] = 12;
    let out = build_preserving(24, 16, &edited, Some(&source)).unwrap();
    let h = frame_header(&out, 2);
    assert_eq!((h.x, h.y, h.w, h.h, h.flags), (4, 3, 1, 1, 0));
    assert_eq!(h.offset % 8, 0);
    let was = frame_header(&file, 2);
    assert_eq!((h.radar, h.reserved), (was.radar, was.reserved));
    assert_eq!(parse(&out).unwrap().canvas(2), edited[2]);
}

#[test]
fn an_appended_frame_is_new_and_the_rest_keep_their_bytes() {
    let (file, mut planes) = mixed_source();
    let source = Source::read(file.clone()).unwrap();
    let mut extra = vec![0u8; 24 * 16];
    extra[0] = 1;
    planes.push(extra);
    let out = build_preserving(24, 16, &planes, Some(&source)).unwrap();
    let back = parse(&out).unwrap();
    assert_eq!(back.frames.len(), 5);
    for (i, plane) in planes.iter().enumerate() {
        assert_eq!(&back.canvas(i), plane);
    }
    for j in 0..4 {
        let span = source.payload(j).len();
        let at = frame_header(&out, j).offset as usize;
        assert_eq!(&out[at..at + span], source.payload(j));
    }
    assert!(out.len() > HEADER + FRAME_HEADER * 5);
}

#[test]
fn a_shrunken_frame_leaves_later_frames_on_their_offsets() {
    // Frame 0 is RLE-Zero; blanking most of it shrinks its data, and the gap
    // before frame 1 is zero-filled so frame 1 does not move.
    let mut rng = Rng::new(5);
    let a = sprite_plane(&mut rng, 8, 8);
    let rle = encode_rle(&a, 8);
    let t = table(2);
    let ob = t + rle.len();
    let headers = [
        header(0, 0, 8, 8, 3, t as u32),
        header(2, 2, 2, 1, 0, ob as u32),
    ];
    let file = assemble(10, 10, &headers, &[(t, rle), (ob, vec![4, 5])], 0, &[]);
    let source = Source::read(file.clone()).unwrap();
    let mut edited = source.canvases();
    edited[0].fill(0);
    edited[0][0] = 3;
    let out = build_preserving(10, 10, &edited, Some(&source)).unwrap();
    assert!(frame_header(&out, 0).offset == t as u32);
    assert_eq!(
        header_bytes(&out, 1),
        header_bytes(&file, 1),
        "frame 1 stays put"
    );
    let back = parse(&out).unwrap();
    assert_eq!(back.canvas(0), edited[0]);
    assert_eq!(back.canvas(1), edited[1]);
}

#[test]
fn a_different_canvas_is_not_paired_with_the_source() {
    let (file, _) = mixed_source();
    let source = Source::read(file).unwrap();
    let planes = vec![vec![1u8; 8 * 8]];
    let out = build_preserving(8, 8, &planes, Some(&source)).unwrap();
    assert_eq!(
        out,
        build(8, 8, &planes).unwrap(),
        "no source bytes leak in"
    );
}

#[test]
fn bad_input_is_refused() {
    assert!(matches!(
        build(4, 4, &[vec![0; 15]]),
        Err(Error::BadPlane {
            frame: 0,
            len: 15,
            expected: 16
        })
    ));
    assert!(matches!(
        build(0, 4, &[vec![]]),
        Err(Error::BadDimensions { .. })
    ));
    assert!(matches!(build(4, 4, &[]), Err(Error::BadDimensions { .. })));
    assert!(matches!(
        build(4097, 1, &[vec![0; 4097]]),
        Err(Error::BadDimensions { .. })
    ));
}

/// Handing `build_with` a frame as "unchanged" writes exactly what handing
/// it the frame's decoded canvas writes, without decoding it.
#[test]
fn unchanged_frames_build_like_their_canvases() {
    for seed in 0..40u64 {
        let mut rng = Rng::new(seed);
        let (file, planes) = if seed % 2 == 0 {
            mixed_source()
        } else {
            let cw = 1 + rng.below(60) as usize;
            let ch = 1 + rng.below(40) as usize;
            let n = 1 + rng.below(10) as usize;
            let planes: Vec<Vec<u8>> = (0..n)
                .map(|_| {
                    if rng.below(4) == 0 {
                        return vec![0u8; cw * ch];
                    }
                    let w = 1 + rng.below(cw as u64) as usize;
                    let h = 1 + rng.below(ch as u64) as usize;
                    let x = rng.below((cw - w + 1) as u64) as usize;
                    let y = rng.below((ch - h + 1) as u64) as usize;
                    place(cw, ch, x, y, w, &sprite_plane(&mut rng, w, h))
                })
                .collect();
            (build(cw as u16, ch as u16, &planes).unwrap(), planes)
        };
        let source = Source::read(file.clone()).unwrap();
        let (w, h) = (source.width(), source.height());
        for (i, plane) in planes.iter().enumerate() {
            assert_eq!(&frame_canvas(&file, i), plane, "seed {seed} frame {i}");
        }
        // One frame edited, the rest handed over as unchanged.
        let mut edited = planes.clone();
        let k = rng.below(planes.len() as u64) as usize;
        let p = rng.below(edited[k].len() as u64) as usize;
        edited[k][p] = edited[k][p] % 254 + 1;
        let expected = build_frames(w, h, &edited, Some(&source)).unwrap();
        let got = build_with(w, h, planes.len(), Some(&source), |i| {
            if i == k {
                FramePlane::Pixels(Cow::Borrowed(&edited[i]))
            } else {
                FramePlane::Unchanged
            }
        })
        .unwrap();
        assert_eq!(got, expected, "seed {seed}");
        // Nothing edited: frame by frame, every frame unchanged.
        let all = build_with(w, h, planes.len(), Some(&source), |_| FramePlane::Unchanged);
        assert_eq!(
            all.unwrap(),
            build_frames(w, h, &planes, Some(&source)).unwrap(),
            "seed {seed}"
        );
    }
    // "Unchanged" needs a source frame to stand for.
    assert!(matches!(
        build_with(4, 4, 1, None, |_| FramePlane::Unchanged),
        Err(Error::BadPlane { frame: 0, .. })
    ));
}
