//! The TMP corpus gate over a private collection of real files (theater
//! suffixes, SHPs among them), against reference decode digests made by an
//! independent reader (Ochre's `cnctmp.py`). It reads the corpus and writes
//! nothing:
//!
//! ```text
//! PICTURA_TMP_CORPUS_LIST=/path/list0     NUL-separated file paths
//! PICTURA_TMP_REFERENCE=/path/ref.tsv     per file, one of:
//!     py<TAB>tiles<TAB>md5<TAB>path       (the digest is described below)
//!     py-not-tmp<TAB>-<TAB>path
//! cargo test -p pictura-shp --test tmp_corpus -- --ignored --nocapture
//! ```
//!
//! The digest: md5 over the live tiles in slot order of slot (u32), x, y
//! (i32), height, land, ramp, radar low, radar high, the diamond as a
//! `cx * cy` rectangle (0 outside), then for z, extra and extra z a presence
//! byte and the bytes (z as a rectangle, 255 outside).

mod md5;

use pictura_shp::tmp::{self, diamond_rows, Template};
use std::collections::HashMap;
use std::os::unix::ffi::OsStrExt;
use std::path::Path;

fn digest(t: &Template) -> String {
    let mut m = md5::Md5::new();
    for (slot, tile) in t.live() {
        m.update(&(slot as u32).to_le_bytes());
        m.update(&tile.x.to_le_bytes());
        m.update(&tile.y.to_le_bytes());
        let f = tile.fields;
        m.update(&[f.height, f.land, f.ramp]);
        m.update(&f.radar_low);
        m.update(&f.radar_high);
        m.update(&tile.image);
        for block in [&tile.z, &tile.extra, &tile.extra_z] {
            match block {
                None => m.update(&[0]),
                Some(b) => {
                    m.update(&[1]);
                    m.update(b);
                }
            }
        }
    }
    m.hex()
}

#[derive(Default, Debug)]
struct Tally {
    files: usize,
    shp: usize,
    neither: usize,
    both: usize,
    tmp: usize,
    sniff_agree: usize,
    digest_match: usize,
    in_place_identical: usize,
    fresh_identical: usize,
    edit_one_byte: usize,
    tiles: usize,
    with_z: usize,
    with_extra: usize,
    with_extra_z: usize,
    unreadable_slots: usize,
}

#[test]
#[ignore = "needs a private TMP corpus: set PICTURA_TMP_CORPUS_LIST and PICTURA_TMP_REFERENCE"]
fn every_real_template_decodes_and_saves_unchanged() {
    let list = std::env::var("PICTURA_TMP_CORPUS_LIST").expect("PICTURA_TMP_CORPUS_LIST");
    let reference: HashMap<Vec<u8>, Option<String>> =
        std::fs::read(std::env::var("PICTURA_TMP_REFERENCE").expect("PICTURA_TMP_REFERENCE"))
            .expect("reference")
            .split(|&b| b == b'\n')
            .filter(|l| !l.is_empty())
            .map(|line| {
                let f: Vec<&[u8]> = line.splitn(4, |&b| b == b'\t').collect();
                let md5 = (f[0] == b"py").then(|| String::from_utf8_lossy(f[2]).into_owned());
                (f[f.len() - 1].to_vec(), md5)
            })
            .collect();
    let paths = std::fs::read(&list).expect("list");
    let mut t = Tally::default();
    let mut failures = Vec::new();
    for path in paths.split(|&b| b == 0).filter(|p| !p.is_empty()) {
        let name = String::from_utf8_lossy(path).into_owned();
        let data = std::fs::read(Path::new(std::ffi::OsStr::from_bytes(path))).expect("read");
        t.files += 1;
        let is_tmp = tmp::sniff(&data);
        let is_shp = pictura_shp::sniff(&data, Some(data.len()));
        match (is_tmp, is_shp) {
            (true, true) => t.both += 1,
            (false, true) => t.shp += 1,
            (false, false) => t.neither += 1,
            (true, false) => t.tmp += 1,
        }
        let expected = reference
            .get(path)
            .expect("every listed file has a reference line");
        if is_tmp == expected.is_some() {
            t.sniff_agree += 1;
        } else {
            failures.push(format!("sniff {is_tmp} vs reference: {name}"));
        }
        if !is_tmp {
            continue;
        }
        let template = match tmp::parse(&data) {
            Ok(template) => template,
            Err(e) => {
                failures.push(format!("parse {e}: {name}"));
                continue;
            }
        };
        if expected.as_deref() == Some(digest(&template).as_str()) {
            t.digest_match += 1;
        } else {
            failures.push(format!("decode differs from the reference: {name}"));
        }
        for (_, tile) in template.live() {
            t.tiles += 1;
            t.with_z += usize::from(tile.z.is_some());
            t.with_extra += usize::from(tile.extra.is_some());
            t.with_extra_z += usize::from(tile.extra_z.is_some());
        }
        t.unreadable_slots += template.unreadable.len();
        if tmp::build(&template, Some(&data)).as_deref() == Ok(&data[..]) {
            t.in_place_identical += 1;
        } else {
            failures.push(format!("in-place save differs: {name}"));
        }
        if tmp::build(&template, None).as_deref() == Ok(&data[..]) {
            t.fresh_identical += 1;
        }
        // One pixel of the middle tile's diamond: exactly one byte changes,
        // inside that tile's diamond data.
        let live: Vec<usize> = template.live().map(|(s, _)| s).collect();
        let slot = live[live.len() / 2];
        let mut edited = template.clone();
        let tile = edited.tiles[slot].as_mut().unwrap();
        let record = tile.offset;
        let (row, start, width) = diamond_rows(template.cx, template.cy)[template.cy / 2];
        let at = row * template.cx + start + width / 2;
        tile.image[at] ^= 0x5A;
        let out = tmp::build(&edited, Some(&data)).expect("edited build");
        let diff: Vec<usize> = (0..data.len().max(out.len()))
            .filter(|&i| out.get(i) != data.get(i))
            .collect();
        let cb = tmp::diamond_bytes(template.cx, template.cy);
        let body = record + tmp::TILE_HEADER..record + tmp::TILE_HEADER + cb;
        if out.len() == data.len() && diff.len() == 1 && body.contains(&diff[0]) {
            t.edit_one_byte += 1;
        } else {
            failures.push(format!("edit changed {} bytes: {name}", diff.len()));
        }
    }
    eprintln!("{t:#?}");
    for f in failures.iter().take(40) {
        eprintln!("FAIL {f}");
    }
    assert!(failures.is_empty(), "{} failures", failures.len());
    assert_eq!(t.both, 0, "no file is both an SHP and a TMP");
}
