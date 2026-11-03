// SPDX-License-Identifier: GPL-3.0-or-later
//! MIX archives through the app glue: the browser listing, extracting,
//! replacing in a copy, the palette searches and loading a palette from
//! inside an archive. Every archive here is generated; no game file is used.

use super::*;
use crate::cxxqt_object::shp::ffi::{MixListing, MixRow};
use cxx_qt_lib::QString;
use pictura_shp::mix::{self, ids, Data, Plan};
use std::path::PathBuf;

fn scratch(name: &str) -> PathBuf {
    let dir = std::env::temp_dir().join(format!("pictura-mix-{}-{name}", std::process::id()));
    let _ = std::fs::remove_dir_all(&dir);
    std::fs::create_dir_all(&dir).expect("temp dir");
    dir
}

fn shp(seed: u8) -> Vec<u8> {
    let planes: Vec<Vec<u8>> = (0..2)
        .map(|f| {
            (0..8 * 6)
                .map(|i| ((i + f + usize::from(seed)) % 3) as u8 * 40)
                .collect()
        })
        .collect();
    pictura_shp::build(8, 6, &planes).expect("shp")
}

fn pal(shade: u8) -> Vec<u8> {
    (0..768)
        .map(|i| ((i / 3) as u8 % 64).max(shade % 64))
        .collect()
}

fn archive(entries: Vec<(&str, Data)>, checksum: bool) -> Vec<u8> {
    let plan = Plan {
        old_format: false,
        checksum,
        entries: entries
            .into_iter()
            .map(|(name, data)| (name_id(name), data))
            .collect(),
    };
    let mut out = Vec::new();
    plan.write(&mut std::io::Cursor::new(Vec::new()), &mut out)
        .expect("write");
    out
}

/// A name's ID, or a `#XXXXXXXX` ID itself.
fn name_id(name: &str) -> u32 {
    name.strip_prefix('#')
        .map(|hex| u32::from_str_radix(hex, 16).expect("hex"))
        .unwrap_or_else(|| ids::ts_id(name))
}

/// game.mix: gi.shp, an unnamed SHP, isotem.pal, and cache.mix holding
/// unittem.pal and anim.pal. Every name but the unnamed one is in the list
/// shipped with the codec.
fn game(dir: &std::path::Path) -> PathBuf {
    let cache = archive(
        vec![
            ("unittem.pal", Data::Bytes(pal(9))),
            ("anim.pal", Data::Bytes(pal(20))),
        ],
        false,
    );
    let bytes = archive(
        vec![
            ("gi.shp", Data::Bytes(shp(1))),
            ("#12345678", Data::Bytes(shp(2))),
            ("isotem.pal", Data::Bytes(pal(30))),
            ("cache.mix", Data::Bytes(cache)),
        ],
        true,
    );
    let path = dir.join("game.mix");
    std::fs::write(&path, bytes).expect("write mix");
    path
}

fn q(path: &std::path::Path) -> QString {
    QString::from(path.to_string_lossy().as_ref())
}

fn row<'a>(listing: &'a MixListing, name: &str) -> &'a MixRow {
    listing
        .rows
        .iter()
        .find(|r| r.name.to_string() == name)
        .unwrap_or_else(|| panic!("no row {name}"))
}

#[test]
fn a_listing_names_kinds_and_nests_the_entries() {
    let dir = scratch("list");
    let path = game(&dir);
    assert!(mix_sniff_file(&q(&path)));
    let listing = mix_list(&q(&path));
    assert!(listing.error.is_empty(), "{}", listing.error);
    assert!(listing.format.to_string().contains("SHA-1 checksum"));
    assert_eq!(listing.rows.len(), 6);
    let gi = row(&listing, "gi.shp");
    assert_eq!((gi.kind.to_string().as_str(), gi.parent), ("SHP", -1));
    assert_eq!(gi.chain, vec![ids::ts_id("gi.shp")]);
    let unnamed = listing
        .rows
        .iter()
        .find(|r| r.id == 0x1234_5678)
        .expect("unnamed");
    assert!(unnamed.name.is_empty());
    assert_eq!(unnamed.kind.to_string(), "SHP");
    assert_eq!(row(&listing, "isotem.pal").kind.to_string(), "PAL");
    let cache = row(&listing, "cache.mix");
    assert_eq!(cache.kind.to_string(), "MIX");
    let cache_row = listing.rows.iter().position(|r| r.id == cache.id).unwrap() as i32;
    let unittem = row(&listing, "unittem.pal");
    assert_eq!(unittem.parent, cache_row);
    assert_eq!(
        unittem.chain,
        vec![ids::ts_id("cache.mix"), ids::ts_id("unittem.pal")]
    );
    assert_eq!(unittem.size, 768);
}

#[test]
fn a_file_that_is_no_archive_lists_an_error() {
    let dir = scratch("bad");
    let path = dir.join("bad.mix");
    std::fs::write(&path, b"not an archive at all").unwrap();
    assert!(!mix_sniff_file(&q(&path)));
    assert!(!mix_list(&q(&path)).error.is_empty());
}

#[test]
fn entries_extract_byte_for_byte_and_a_bad_chain_says_why() {
    let dir = scratch("extract");
    let path = game(&dir);
    let out = dir.join("gi.shp");
    assert!(mix_extract(&q(&path), &[ids::ts_id("gi.shp")], &q(&out)));
    assert_eq!(std::fs::read(&out).unwrap(), shp(1));
    let nested = dir.join("unittem.pal");
    let chain = [ids::ts_id("cache.mix"), ids::ts_id("unittem.pal")];
    assert!(mix_extract(&q(&path), &chain, &q(&nested)));
    assert_eq!(std::fs::read(&nested).unwrap(), pal(9));
    assert!(!mix_extract(&q(&path), &[0xDEAD_BEEF], &q(&dir.join("x"))));
    assert!(!shp_last_error().is_empty());
}

#[test]
fn replacing_writes_a_new_archive_and_leaves_the_source_alone() {
    let dir = scratch("replace");
    let path = game(&dir);
    let before = std::fs::read(&path).unwrap();
    let entry = dir.join("unittem.pal");
    std::fs::write(&entry, pal(50)).unwrap();
    let out = dir.join("game-edited.mix");
    let chain = [ids::ts_id("cache.mix"), ids::ts_id("unittem.pal")];
    assert!(mix_replace(&q(&path), &chain, &q(&entry), &q(&out)));
    assert_eq!(std::fs::read(&path).unwrap(), before);
    assert!(!dir.join("game-edited.mix.kooka-partial").exists());
    let back = dir.join("back.pal");
    assert!(mix_extract(&q(&out), &chain, &q(&back)));
    assert_eq!(std::fs::read(&back).unwrap(), pal(50));
    // The untouched entries come out the same, and the checksum is new.
    let gi = dir.join("gi.shp");
    assert!(mix_extract(&q(&out), &[ids::ts_id("gi.shp")], &q(&gi)));
    assert_eq!(std::fs::read(&gi).unwrap(), shp(1));
    let mut file = std::fs::File::open(&out).unwrap();
    let len = file.metadata().unwrap().len();
    let index = mix::read_index(&mut file, 0, len).unwrap();
    assert!(!index.encrypted() && index.has_checksum());
    let (stored, now) = mix::checksums(&mut file, 0, &index).unwrap().unwrap();
    assert_eq!(stored, now);
}

#[test]
fn replacing_in_place_goes_through_a_partial_file() {
    let dir = scratch("in-place");
    let path = game(&dir);
    let entry = dir.join("gi.shp");
    std::fs::write(&entry, shp(7)).unwrap();
    assert!(mix_replace(
        &q(&path),
        &[ids::ts_id("gi.shp")],
        &q(&entry),
        &q(&path)
    ));
    let back = dir.join("back.shp");
    assert!(mix_extract(&q(&path), &[ids::ts_id("gi.shp")], &q(&back)));
    assert_eq!(std::fs::read(&back).unwrap(), shp(7));
    // A failed write leaves the archive and no partial file.
    let before = std::fs::read(&path).unwrap();
    assert!(!mix_replace(
        &q(&path),
        &[0xDEAD_BEEF],
        &q(&entry),
        &q(&path)
    ));
    assert_eq!(std::fs::read(&path).unwrap(), before);
    assert!(!dir.join("game.mix.kooka-partial").exists());
}

#[test]
fn palettes_are_found_by_name_and_by_prefix_through_nesting() {
    let dir = scratch("find");
    let path = game(&dir);
    let archive = path.to_string_lossy().into_owned();
    assert_eq!(
        mix_find(&q(&path), &QString::from("UNITTEM.PAL")).to_string(),
        format!("{archive}/cache.mix/UNITTEM.PAL")
    );
    assert_eq!(
        mix_find(&q(&path), &QString::from("isotem.pal")).to_string(),
        format!("{archive}/isotem.pal")
    );
    assert!(mix_find(&q(&path), &QString::from("isosno.pal")).is_empty());
    // The longest .pal stem that starts the sprite's stem.
    assert_eq!(
        mix_find_prefix(
            &q(&path),
            &QString::from("animfire"),
            &QString::from(".pal")
        )
        .to_string(),
        format!("{archive}/cache.mix/anim.pal")
    );
    assert!(mix_find_prefix(&q(&path), &QString::from("gi"), &QString::from(".pal")).is_empty());
    assert!(mix_path_exists(&QString::from(
        format!("{archive}/cache.mix/unittem.pal").as_str()
    )));
    assert!(!mix_path_exists(&QString::from(
        format!("{archive}/nope.pal").as_str()
    )));
}

#[test]
fn a_palette_loads_from_inside_an_archive() {
    let dir = scratch("load");
    let path = game(&dir);
    let member = format!("{}/cache.mix/unittem.pal", path.to_string_lossy());
    let choice = PaletteChoice::load(&member);
    assert_eq!(choice.path, member);
    assert_eq!(choice.palette.name, "unittem.pal");
    let loose = PaletteChoice::load(&format!("{}/cache.mix/missing.pal", path.to_string_lossy()));
    assert!(loose.path.is_empty());
}
