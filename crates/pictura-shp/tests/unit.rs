//! Unit-sprite conventions: the shadow split, its check, the cast shadow,
//! and a census of the split over a private corpus (ignored by default).

use pictura_shp::palette::Palette;
use pictura_shp::unit::{
    classify, file_frame_use, house_ramp, silhouette, split, violations, ShadowParams, Use, SHADOW,
};
use pictura_shp::{build, sniff};
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
