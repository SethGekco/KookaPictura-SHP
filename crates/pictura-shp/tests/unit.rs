//! Unit-sprite conventions: the shadow split, its check, the cast shadow,
//! and a census of the split over a private corpus (ignored by default).

use pictura_shp::palette::Palette;
use pictura_shp::unit::{
    average_color, classify, file_frame_use, house_ramp, recompute_radar, silhouette, split,
    violations, ShadowParams, Use, SHADOW,
};
use pictura_shp::{build, sniff, FrameHeader};
use std::os::unix::ffi::OsStrExt;
use std::path::Path;

const W: usize = 7;
const H: usize = 6;

/// A small "tank": a filled block of index `v` with a column of 17 (remap),
/// standing on the bottom row minus one.
fn art(v: u8) -> Vec<u8> {
    let mut p = vec![0u8; W * H];
    for y in 1..H - 1 {
        for x in 2..5 {
            p[y * W + x] = v;
        }
    }
    p[2 * W + 3] = 17;
    p
}

fn shadow_of(plane: &[u8]) -> Vec<u8> {
    plane.iter().map(|&v| u8::from(v != 0)).collect()
}

#[test]
fn classify_tells_empty_shadow_and_art_apart() {
    assert_eq!(classify(&[0, 0, 0]), Use::Empty);
    assert_eq!(classify(&[0, 1, 0]), Use::Shadow);
    assert_eq!(classify(&[1, 1, 2]), Use::Art);
    assert_eq!(classify(&[]), Use::Empty);
}

#[test]
fn the_split_needs_an_even_count_a_clean_second_half_and_some_art() {
    use Use::*;
    assert_eq!(split(&[Art, Art, Shadow, Shadow]), Some(2));
    // Cleared shadows are still the shadow half.
    assert_eq!(split(&[Art, Art, Shadow, Empty]), Some(2));
    assert_eq!(split(&[Art, Empty, Empty, Empty]), Some(2));
    // Odd counts, art in the second half, nothing but shadows: not a unit.
    assert_eq!(split(&[Art, Art, Shadow]), None);
    assert_eq!(split(&[Art, Art, Art, Shadow]), None);
    assert_eq!(split(&[Shadow, Shadow, Shadow, Shadow]), None);
    assert_eq!(split(&[Empty, Empty]), None);
    assert_eq!(split(&[Art]), None);
    assert_eq!(split(&[]), None);
}

#[test]
fn violations_are_the_shadow_frames_holding_other_indices() {
    use Use::*;
    let uses = [Art, Art, Art, Shadow, Art, Empty];
    assert_eq!(violations(&uses, 3), vec![4]);
    assert_eq!(violations(&uses, 2), vec![2, 4]);
    assert!(violations(&uses, 6).is_empty());
    assert!(violations(&uses, 9).is_empty());
}

#[test]
fn a_straight_silhouette_is_the_art_as_index_1() {
    let a = art(40);
    let s = silhouette(&a, W, H, ShadowParams::STRAIGHT);
    assert_eq!(s, shadow_of(&a));
    assert_eq!(classify(&s), Use::Shadow);
    assert!(s.iter().all(|&v| v == 0 || v == SHADOW));
}

#[test]
fn the_offset_moves_the_silhouette_and_drops_what_leaves_the_canvas() {
    let a = art(40);
    let s = silhouette(
        &a,
        W,
        H,
        ShadowParams {
            dx: 3,
            dy: 1,
            skew: 0.0,
        },
    );
    for y in 0..H {
        for x in 0..W {
            let from = (x >= 3 && y >= 1).then(|| a[(y - 1) * W + x - 3]);
            let want = u8::from(from.is_some_and(|v| v != 0));
            assert_eq!(s[y * W + x], want, "({x}, {y})");
        }
    }
    // Column 4 of the art moved to 7: off the canvas.
    let kept = s.iter().filter(|&&v| v != 0).count();
    let art_px = a.iter().filter(|&&v| v != 0).count();
    assert!(kept < art_px);
    // Pushed off entirely: an empty frame.
    let gone = silhouette(
        &a,
        W,
        H,
        ShadowParams {
            dx: -10,
            dy: 0,
            skew: 0.0,
        },
    );
    assert_eq!(classify(&gone), Use::Empty);
}

#[test]
fn the_skew_leans_rows_above_the_base_and_keeps_the_base_in_place() {
    let a = art(40);
    let base = H - 2; // the art's lowest row
    let s = silhouette(
        &a,
        W,
        H,
        ShadowParams {
            dx: 0,
            dy: 0,
            skew: 1.0,
        },
    );
    for y in 0..H {
        let shift = base.saturating_sub(y);
        for x in 0..W {
            let from = x
                .checked_sub(shift)
                .filter(|_| y <= base)
                .map(|sx| a[y * W + sx]);
            let want = u8::from(from.is_some_and(|v| v != 0));
            assert_eq!(s[y * W + x], want, "({x}, {y})");
        }
    }
    // Half a pixel per row rounds away from zero every other row; negative
    // leans left.
    let left = silhouette(
        &a,
        W,
        H,
        ShadowParams {
            dx: 0,
            dy: 0,
            skew: -0.5,
        },
    );
    let row =
        |p: &[u8], y: usize| -> Vec<usize> { (0..W).filter(|&x| p[y * W + x] != 0).collect() };
    assert_eq!(row(&left, base), vec![2, 3, 4]);
    assert_eq!(row(&left, base - 1), vec![1, 2, 3]); // -0.5 rounds to -1
    assert_eq!(row(&left, base - 2), vec![1, 2, 3]);
    assert_eq!(row(&left, base - 3), vec![0, 1, 2]); // -1.5 rounds to -2
}

#[test]
fn an_empty_frame_casts_no_shadow() {
    assert_eq!(
        silhouette(&[0u8; W * H], W, H, ShadowParams::STRAIGHT),
        vec![0u8; W * H]
    );
}

#[test]
fn file_frames_are_classified_from_their_stored_rectangle() {
    let mut planes = vec![art(40), art(41)];
    planes.push(shadow_of(&planes[0]));
    planes.push(vec![0u8; W * H]);
    let bytes = build(W as u16, H as u16, &planes).expect("build");
    assert!(sniff(&bytes, Some(bytes.len())));
    let uses: Vec<Use> = (0..4).map(|i| file_frame_use(&bytes, i)).collect();
    assert_eq!(uses, vec![Use::Art, Use::Art, Use::Shadow, Use::Empty]);
    assert_eq!(split(&uses), Some(2));
}

#[test]
fn the_house_ramp_follows_the_palette_shading() {
    let mut argb = [0xFF00_0000u32; 256];
    // A remap ramp from bright red down to dark red, as unit palettes have.
    for i in 16..32u32 {
        let v = 255 - (i - 16) * 15;
        argb[i as usize] = 0xFF00_0000 | (v << 16);
    }
    let palette = Palette {
        argb,
        name: "unittem.pal".into(),
    };
    let gold = 0x00FF_C800;
    let ramp = house_ramp(&palette, 16..=31, gold);
    assert_eq!(ramp.len(), 16);
    assert_eq!(
        ramp[0],
        0xFF00_0000 | gold,
        "the brightest entry is the color itself"
    );
    let green = |c: u32| (c >> 8) & 0xFF;
    assert!(
        ramp.windows(2).all(|w| green(w[1]) < green(w[0])),
        "darker down the ramp"
    );
    // 255 - 15 * 15 = 30 of 255: the last entry keeps that share.
    assert_eq!(green(ramp[15]), (200.0f64 * 30.0 / 255.0).round() as u32);
    assert!(ramp.iter().all(|c| c >> 24 == 0xFF && c & 0xFF == 0));
    // A black range still gives a usable ramp.
    let black = Palette {
        argb: [0xFF00_0000; 256],
        name: String::new(),
    };
    let even = house_ramp(&black, 16..=31, 0x0010_2030);
    assert_eq!(even[0], 0xFF10_2030);
    assert!(even[15] & 0xFF_FFFF < 0x0010_2030 & 0xFF_FFFF);
}

fn gray_palette() -> Palette {
    let mut argb = [0u32; 256];
    for (i, c) in argb.iter_mut().enumerate() {
        let v = i as u32;
        *c = 0xFF00_0000 | (v << 16) | ((255 - v) << 8) | (v / 3);
    }
    Palette {
        argb,
        name: String::new(),
    }
}

#[test]
fn the_radar_color_is_the_mean_of_the_drawn_pixels_rounded_down() {
    let palette = gray_palette();
    // Indices 10 and 13: red (10 + 13) / 2 = 11.5 -> 11, green 245 - 1.5 -> 243,
    // blue (3 + 4) / 2 = 3.5 -> 3. Transparent pixels do not count.
    assert_eq!(
        average_color(&[0, 10, 0, 13, 0], &palette),
        Some([11, 243, 3])
    );
    assert_eq!(average_color(&[7], &palette), Some([7, 248, 2]));
    assert_eq!(average_color(&[0, 0], &palette), None);
}

#[test]
fn recomputing_the_radar_rewrites_drawn_frames_only() {
    let palette = gray_palette();
    let planes = vec![art(40), vec![0u8; W * H], art(41)];
    let mut bytes = build(W as u16, H as u16, &planes).expect("build");
    let before = bytes.clone();
    assert_eq!(recompute_radar(&mut bytes, &palette), 2);
    for (i, plane) in planes.iter().enumerate() {
        let radar = FrameHeader::read(&bytes, i).radar;
        match average_color(plane, &palette) {
            Some([r, g, b]) => assert_eq!(radar, [r, g, b, 0], "frame {i}"),
            None => assert_eq!(radar, FrameHeader::read(&before, i).radar, "frame {i} kept"),
        }
    }
    // Only the radar fields changed, and a second pass changes nothing.
    let differing: Vec<usize> = (0..bytes.len())
        .filter(|&k| bytes[k] != before[k])
        .collect();
    assert!(differing
        .iter()
        .all(|&k| k >= 8 && (k - 8) % 24 >= 12 && (k - 8) % 24 < 16 && k < 8 + 24 * 3));
    assert_eq!(recompute_radar(&mut bytes, &palette), 0);
}

/// The shadow-split census over a private corpus (nothing is written):
///
/// ```text
/// PICTURA_SHP_CORPUS_LIST=/path/list0   NUL-separated file paths
/// cargo test -p pictura-shp --test unit -- --ignored --nocapture census
/// ```
///
/// Prints one TSV line per SHP (`frames`, the split or `-`, how many shadow
/// frames hold index 1, the path) and a summary.
#[test]
#[ignore = "needs PICTURA_SHP_CORPUS_LIST, a private corpus"]
fn census_of_the_shadow_split() {
    let Ok(list) = std::env::var("PICTURA_SHP_CORPUS_LIST") else {
        eprintln!("PICTURA_SHP_CORPUS_LIST is not set");
        return;
    };
    let names = std::fs::read(list).expect("corpus list");
    let (mut files, mut shp, mut even, mut shadowed, mut drawn) = (0, 0, 0, 0, 0);
    for name in names.split(|&b| b == 0).filter(|n| !n.is_empty()) {
        files += 1;
        let path = Path::new(std::ffi::OsStr::from_bytes(name));
        let Ok(bytes) = std::fs::read(path) else {
            continue;
        };
        if !sniff(&bytes, Some(bytes.len())) {
            continue;
        }
        shp += 1;
        let count = usize::from(u16::from_le_bytes([bytes[6], bytes[7]]));
        even += usize::from(count >= 2 && count.is_multiple_of(2));
        let uses: Vec<Use> = (0..count).map(|i| file_frame_use(&bytes, i)).collect();
        let found = split(&uses);
        let with_ones = found.map_or(0, |h| {
            uses[h..].iter().filter(|u| **u == Use::Shadow).count()
        });
        if found.is_some() {
            shadowed += 1;
            drawn += usize::from(with_ones > 0);
        }
        println!(
            "census\t{count}\t{}\t{with_ones}\t{}",
            found.map_or_else(|| "-".to_string(), |h| h.to_string()),
            path.display()
        );
    }
    println!(
        "census-summary files={files} shp={shp} even={even} shadowed={shadowed} \
         shadowed_with_index1={drawn}"
    );
}

/// The end-to-end check on a real unit sprite (ignored by default): with
/// `PICTURA_SHP_E2E_ORIGINAL` and `PICTURA_SHP_E2E_EDITED` set to a sprite
/// and the app's save after Clear All then Generate All (straight shadows),
/// the art frames keep their headers and pixels and every shadow frame is
/// exactly its art frame's silhouette in index 1 (or empty, for the save
/// after Clear All with `PICTURA_SHP_E2E_EXPECT=cleared`).
#[test]
#[ignore = "needs a real sprite and the app's edit of it"]
fn a_real_sprite_regenerated_changes_only_its_shadow_frames() {
    let (Ok(original), Ok(edited)) = (
        std::env::var("PICTURA_SHP_E2E_ORIGINAL"),
        std::env::var("PICTURA_SHP_E2E_EDITED"),
    ) else {
        eprintln!("PICTURA_SHP_E2E_ORIGINAL / _EDITED are not set");
        return;
    };
    let a = std::fs::read(original).expect("original");
    let b = std::fs::read(edited).expect("edited");
    let (sa, sb) = (
        pictura_shp::parse(&a).expect("parses"),
        pictura_shp::parse(&b).expect("parses"),
    );
    assert_eq!((sa.width, sa.height), (sb.width, sb.height));
    let count = sa.frames.len();
    assert_eq!(sb.frames.len(), count);
    let uses: Vec<Use> = (0..count).map(|i| file_frame_use(&a, i)).collect();
    let half = split(&uses).expect("the original has a shadow half");
    let (w, h) = (usize::from(sa.width), usize::from(sa.height));
    // PICTURA_SHP_E2E_EXPECT=cleared: the save after Clear All only.
    let cleared = std::env::var("PICTURA_SHP_E2E_EXPECT").is_ok_and(|v| v == "cleared");
    let mut changed = 0;
    for i in 0..count {
        let (ha, hb) = (FrameHeader::read(&a, i), FrameHeader::read(&b, i));
        let (ca, cb) = (sa.canvas(i), sb.canvas(i));
        if i < half {
            assert_eq!(ca, cb, "art frame {i} pixels");
            assert_eq!(
                (ha.x, ha.y, ha.w, ha.h, ha.flags, ha.radar, ha.reserved),
                (hb.x, hb.y, hb.w, hb.h, hb.flags, hb.radar, hb.reserved),
                "art frame {i} header"
            );
        } else {
            assert!(cb.iter().all(|&v| v <= SHADOW), "shadow frame {i} is 0/1");
            let want = if cleared {
                vec![0; w * h]
            } else {
                silhouette(&sa.canvas(i - half), w, h, ShadowParams::STRAIGHT)
            };
            assert_eq!(cb, want, "shadow frame {i} (cleared: {cleared})");
            changed += usize::from(ca != cb);
        }
    }
    println!("e2e frames={count} half={half} shadow_frames_changed={changed}");
}
