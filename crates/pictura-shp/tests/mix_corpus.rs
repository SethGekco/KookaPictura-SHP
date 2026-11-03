//! The MIX corpus gate over a private collection of real archives. It reads
//! the corpus and writes nothing:
//!
//! ```text
//! PICTURA_MIX_CORPUS_LIST=/path/mixlist0       NUL-separated archive paths
//! PICTURA_MIX_NAMES_LOCAL=/path/mix-names.txt  optional: the local name list
//! cargo test -p pictura-shp --release --test mix_corpus -- --ignored --nocapture
//! ```
//!
//! Every archive with a header must parse (an empty file is counted apart);
//! nested archives are opened; every entry sniffed as an SHP or a TMP must
//! decode with this crate's codecs and save unchanged byte-identical. Entries
//! that lie outside their archive and stored SHA-1 checksums that do not
//! match the body are listed: real archives have both (damaged copies, mods
//! edited by tools that kept a stale checksum), and they are the files'
//! findings, not the reader's. It reports how many entries have a name.

use pictura_shp::mix::{self, names::Names, Kind, Node};
use pictura_shp::{build_preserving, tmp, Source};
use std::collections::BTreeMap;
use std::fs::File;
use std::io::{Read, Seek, SeekFrom};
use std::os::unix::ffi::OsStrExt;
use std::path::Path;

#[derive(Default)]
struct Tally {
    archives: usize,
    empty_files: usize,
    /// Files named .mix too short for any header (mods ship 4-byte stubs).
    stubs: Vec<String>,
    unreadable: Vec<String>,
    nested: usize,
    encrypted: usize,
    checksummed: usize,
    checksum_ok: usize,
    entries: usize,
    broken: Vec<String>,
    kinds: BTreeMap<&'static str, usize>,
    named_shipped: usize,
    named_all: usize,
    shp: usize,
    shp_identical: usize,
    tmp: usize,
    tmp_identical: usize,
    failures: Vec<String>,
    biggest_entry: u64,
}

fn read(file: &mut File, at: u64, len: u64) -> Vec<u8> {
    let mut buf = vec![0u8; len as usize];
    file.seek(SeekFrom::Start(at)).unwrap();
    file.read_exact(&mut buf).unwrap();
    buf
}

fn check_node(
    node: &Node,
    file: &mut File,
    shown: &str,
    shipped: &Names,
    all: &Names,
    t: &mut Tally,
) {
    let index = &node.index;
    t.encrypted += usize::from(index.encrypted());
    if index.has_checksum() {
        t.checksummed += 1;
        match mix::checksums(file, node.start, index) {
            Ok(Some((stored, now))) if stored == now => t.checksum_ok += 1,
            other => t.failures.push(format!(
                "checksum differs ({:?}): {shown} @{}",
                other.is_ok(),
                node.start
            )),
        }
    }
    // The archive's own names file counts as a name source too.
    let mut local = all.clone();
    for (i, e) in index.entries.iter().enumerate() {
        if node.kinds[i] == Kind::Lmd {
            let bytes = read(file, node.start + index.start(e), u64::from(e.size));
            for name in mix::names::parse_lmd(&bytes).unwrap_or_default() {
                local.add(&name);
            }
        }
    }
    let (kind_shipped, kind_all) = (shipped.kind_for(index), local.kind_for(index));
    for (i, e) in index.entries.iter().enumerate() {
        t.entries += 1;
        let kind = node.kinds[i];
        *t.kinds.entry(kind.label()).or_default() += 1;
        t.named_shipped += usize::from(shipped.get(kind_shipped, e.id).is_some());
        t.named_all += usize::from(local.get(kind_all, e.id).is_some());
        if !index.in_bounds(e) {
            t.broken
                .push(format!("{shown} @{} entry {:08X}", node.start, e.id));
            continue;
        }
        if !matches!(kind, Kind::Shp | Kind::Tmp) {
            continue;
        }
        let len = u64::from(e.size);
        t.biggest_entry = t.biggest_entry.max(len);
        let data = read(file, node.start + index.start(e), len);
        let what = format!("{shown} @{} entry {:08X}", node.start, e.id);
        if kind == Kind::Shp {
            t.shp += 1;
            let Ok(source) = Source::read(data.clone()) else {
                t.failures.push(format!("SHP does not decode: {what}"));
                continue;
            };
            let planes = source.canvases();
            match build_preserving(source.width(), source.height(), &planes, Some(&source)) {
                Ok(out) if out == data => t.shp_identical += 1,
                _ => t
                    .failures
                    .push(format!("SHP unchanged save differs: {what}")),
            }
        } else {
            t.tmp += 1;
            let Ok(template) = tmp::parse(&data) else {
                t.failures.push(format!("TMP does not decode: {what}"));
                continue;
            };
            match tmp::build(&template, Some(&data)) {
                Ok(out) if out == data => t.tmp_identical += 1,
                _ => t
                    .failures
                    .push(format!("TMP unchanged save differs: {what}")),
            }
        }
    }
}

#[test]
#[ignore = "reads a private corpus of real archives"]
fn every_real_archive_parses_and_its_sprites_save_unchanged() {
    let list = std::env::var("PICTURA_MIX_CORPUS_LIST").expect("set PICTURA_MIX_CORPUS_LIST");
    let shipped = Names::shipped();
    let mut all = shipped.clone();
    if let Ok(local) = std::env::var("PICTURA_MIX_NAMES_LOCAL") {
        all.add_list(&std::fs::read_to_string(local).unwrap_or_default());
    }
    let mut t = Tally::default();
    let paths = std::fs::read(&list).unwrap();
    for raw in paths.split(|&b| b == 0).filter(|p| !p.is_empty()) {
        let path = Path::new(std::ffi::OsStr::from_bytes(raw));
        let shown = path.display().to_string();
        let mut file = File::open(path).unwrap();
        let len = file.metadata().unwrap().len();
        if len == 0 {
            t.empty_files += 1;
            continue;
        }
        if len < 6 {
            t.stubs.push(format!("{len} bytes: {shown}"));
            continue;
        }
        let root = match mix::scan(&file, 0, len) {
            Ok(root) => root,
            Err(e) => {
                t.unreadable.push(format!("{e}: {shown}"));
                continue;
            }
        };
        t.archives += 1;
        for node in root.walk() {
            t.nested += usize::from(node.start != 0);
            check_node(node, &mut file, &shown, &shipped, &all, &mut t);
        }
    }
    println!(
        "archives parsed      {} (+{} empty files)",
        t.archives, t.empty_files
    );
    println!("stubs under 6 bytes  {}", t.stubs.len());
    println!("unreadable           {}", t.unreadable.len());
    println!("nested archives      {}", t.nested);
    println!("encrypted indexes    {}", t.encrypted);
    println!(
        "checksums            {} of {} match",
        t.checksum_ok, t.checksummed
    );
    println!("entries              {}", t.entries);
    println!("entries out of bounds {}", t.broken.len());
    println!("kinds                {:?}", t.kinds);
    println!("named (shipped list) {} of {}", t.named_shipped, t.entries);
    println!("named (all lists)    {} of {}", t.named_all, t.entries);
    println!(
        "SHP entries          {} decoded, {} unchanged save identical",
        t.shp, t.shp_identical
    );
    println!(
        "TMP entries          {} decoded, {} unchanged save identical",
        t.tmp, t.tmp_identical
    );
    println!("largest SHP/TMP entry {} bytes", t.biggest_entry);
    for line in t
        .stubs
        .iter()
        .chain(&t.unreadable)
        .chain(&t.broken)
        .chain(&t.failures)
        .take(60)
    {
        println!("  {line}");
    }
    assert!(t.unreadable.is_empty(), "unreadable archives");
    assert!(
        t.failures.iter().all(|f| f.starts_with("checksum")),
        "failures"
    );
    assert_eq!(t.shp_identical, t.shp);
    assert_eq!(t.tmp_identical, t.tmp);
}
