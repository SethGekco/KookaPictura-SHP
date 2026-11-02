//! `.pal` palettes: VGA expansion, JASC detection, unique colours, nearest.

use pictura_shp::palette::{
    compress8, expand6, fallback_palette, is_jasc, nearest, read_palette, unique_colours,
    write_jasc, write_vga, Palette, VGA_BYTES,
};
use std::collections::HashSet;

fn vga(entries: impl Fn(usize) -> [u8; 3]) -> Vec<u8> {
    (0..256).flat_map(entries).collect()
}

#[test]
fn vga_expands_63_to_255_and_writes_back_exactly() {
    assert_eq!(expand6(63), 255, "not 252");
    assert_eq!(expand6(0), 0);
    assert_eq!(expand6(32), 130);
    for v in 0..64u8 {
        assert_eq!(compress8(expand6(v)), v);
    }
    let bytes = vga(|i| [(i % 64) as u8, 63, 0]);
    let palette = read_palette(&bytes).expect("768 bytes");
    assert_eq!(palette.argb[1], 0xFF04_FF00);
    assert_eq!(palette.argb[63], 0xFFFF_FF00);
    assert_eq!(palette.argb[0] >> 24, 0, "index 0 is transparent");
    assert_eq!(palette.argb[5] >> 24, 0xFF);
    assert_eq!(write_vga(&palette), bytes, "v >> 2 restores every byte");
}

#[test]
fn a_short_binary_palette_is_refused() {
    assert!(read_palette(&[0u8; VGA_BYTES - 1]).is_none());
}

#[test]
fn jasc_text_parses_and_is_never_read_as_binary() {
    let mut text = String::from("JASC-PAL\r\n0100\r\n256\r\n");
    for i in 0..256 {
        text.push_str(&format!("{} {} {}\r\n", i, 255 - i, 7));
    }
    assert!(text.len() >= VGA_BYTES, "long enough to pass as binary");
    assert!(is_jasc(text.as_bytes()));
    let palette = read_palette(text.as_bytes()).expect("JASC");
    assert_eq!(palette.argb[1], 0xFF01_FE07, "8-bit values taken as-is");
    assert_eq!(palette.argb[255], 0xFFFF_0007);
    assert_eq!(palette.argb[0] & 0xFF00_0000, 0);
    // Writing it back gives the same palette.
    assert_eq!(
        read_palette(write_jasc(&palette).as_bytes()).unwrap().argb,
        palette.argb
    );
}

#[test]
fn a_short_or_untidy_jasc_file_is_padded_and_skips_bad_lines() {
    let text = "JASC-PAL\n0100\n3\n10 20 30\nnot a colour\n\n1 2\n40 50 60 extra\n";
    let palette = read_palette(text.as_bytes()).expect("JASC");
    assert_eq!(palette.argb[0], 0x000A_141E);
    assert_eq!(palette.argb[1], 0xFF28_323C);
    assert_eq!(palette.argb[2], 0xFF00_0000, "padded with opaque black");
}

#[test]
fn unique_colours_is_one_to_one_and_moves_only_repeats() {
    let mut argb = [0xFF00_0000u32; 256];
    for (i, c) in argb.iter_mut().enumerate().skip(100) {
        *c = 0xFF00_0000 | ((i as u32) << 8);
    }
    // The ramp copied onto 200-202, as real unit palettes do.
    argb[200] = argb[20];
    argb[201] = argb[21];
    argb[202] = argb[22];
    let palette = Palette {
        argb,
        name: "test".into(),
    };
    let unique = unique_colours(&palette);
    let colours: HashSet<u32> = unique.argb.iter().map(|c| c & 0xFF_FFFF).collect();
    assert_eq!(colours.len(), 256, "every index has a colour of its own");
    for i in 0..256 {
        if i == 0 || (100..200).contains(&i) || i > 202 {
            assert_eq!(
                unique.argb[i], palette.argb[i],
                "index {i} was already unique"
            );
        }
        assert_eq!(unique.argb[i] >> 24, palette.argb[i] >> 24, "alpha kept");
    }
    // A repeat moves by the smallest step.
    let d = |a: u32, b: u32| {
        (0..3)
            .map(|k| (((a >> (8 * k)) & 0xFF) as i32 - ((b >> (8 * k)) & 0xFF) as i32).abs())
            .max()
            .unwrap()
    };
    assert_eq!(d(unique.argb[2], palette.argb[2]), 1);
}

#[test]
fn nearest_prefers_indices_outside_the_remap_ramp_on_ties() {
    let mut palette = fallback_palette();
    palette.argb[20] = 0xFF40_8040;
    palette.argb[40] = 0xFF40_8040;
    assert_eq!(nearest(&palette, 0xFF40_8040), 40, "tie: the ramp loses");
    palette.argb[40] = 0xFF41_8040;
    assert_eq!(
        nearest(&palette, 0xFF40_8040),
        20,
        "an exact ramp match still wins"
    );
    assert_eq!(nearest(&palette, 0xFFFF_FFFF), 255);
    for c in [0x00FF_00FFu32, 0, 0xFF00_0000] {
        assert_ne!(nearest(&palette, c), 0, "index 0 is never chosen");
    }
}

#[test]
fn the_fallback_marks_the_special_indices() {
    let palette = fallback_palette();
    assert_eq!(palette.argb[0] >> 24, 0);
    assert_eq!(palette.argb[1], 0xFF28_2828);
    assert_eq!(palette.argb[16], 0xFF3C_0000);
    assert_eq!(palette.argb[31], 0xFFF0_0000);
    assert_eq!(palette.argb[128], 0xFF80_8080);
}
