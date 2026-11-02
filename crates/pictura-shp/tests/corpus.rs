//! The corpus gate: sniff, decode, unchanged save and a one-pixel edit over a
//! private collection of real SHPs, against reference decode hashes.
//!
//! Nothing here ships a game file. The corpus stays wherever its owner keeps
//! it; the gate reads it and writes nothing:
//!
//! ```text
//! PICTURA_SHP_CORPUS_LIST=/path/list0     NUL-separated file paths
//! PICTURA_SHP_REFERENCE=/path/hashes.tsv  per file, one of:
//!     py<TAB>frames<TAB>md5<TAB>path      md5 over every frame's full canvas
//!     py-not-shp<TAB>...<TAB>path         (index 0 outside the frame rect)
//!     py-error<TAB>...<TAB>path
//! PICTURA_SHP_THREADS=3                   optional
//! cargo test -p pictura-shp --test corpus -- --ignored --nocapture
//! ```

mod md5;

use pictura_shp::{build_frames, build_preserving, parse, sniff, FrameHeader, Source};
use std::collections::HashMap;
use std::os::unix::ffi::OsStrExt;
use std::path::Path;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::Mutex;

#[derive(Clone, Debug)]
enum Reference {
    Shp { frames: usize, md5: String },
    NotShp,
}

fn read_reference(path: &str) -> HashMap<Vec<u8>, Reference> {
    let text = std::fs::read(path).expect("reference manifest");
    let mut out = HashMap::new();
    for line in text.split(|&b| b == b'\n').filter(|l| !l.is_empty()) {
        let fields: Vec<&[u8]> = line.splitn(4, |&b| b == b'\t').collect();
        match fields[0] {
            b"py" if fields.len() == 4 => {
                let frames = std::str::from_utf8(fields[1]).unwrap().parse().unwrap();
                let md5 = String::from_utf8_lossy(fields[2]).into_owned();
                out.insert(fields[3].to_vec(), Reference::Shp { frames, md5 });
            }
            _ => {
                // "py-not-shp<TAB>fmt<TAB>path" and "py-error<TAB>msg<TAB>path".
                let fields: Vec<&[u8]> = line.splitn(3, |&b| b == b'\t').collect();
                out.insert(fields[fields.len() - 1].to_vec(), Reference::NotShp);
            }
        }
    }
    out
}

#[derive(Default)]
struct Tally {
    files: usize,
    sniff_agree: usize,
    refused_both: usize,
    decode_match: usize,
    unchanged_identical: usize,
    frames_path_identical: usize,
    edited: usize,
    edit_ok: usize,
    edit_offsets_kept: usize,
    no_drawn_frame: usize,
    failures: Vec<String>,
    frames_path_diffs: Vec<String>,
}

fn canvas_md5(source: &Source) -> String {
    let mut m = md5::Md5::new();
    for i in 0..source.frames().len() {
        m.update(&source.canvas(i));
    }
    m.hex()
}

/// Edit one pixel in the middle drawn frame; check the other frames kept
/// their header fields and data, and the result decodes to the edit.
fn edit_check(path: &str, data: &[u8], source: &Source, planes: &[Vec<u8>], t: &mut Tally) {
    let drawn: Vec<usize> = (0..planes.len())
        .filter(|&i| source.frames()[i].frame.pixels.is_some())
        .collect();
    let Some(&k) = drawn.get(drawn.len() / 2) else {
        t.no_drawn_frame += 1;
        return;
    };
    t.edited += 1;
    let (w, h) = (source.width(), source.height());
    let head = *source.frames()[k].header();
    let p = (usize::from(head.y) + usize::from(head.h) / 2) * usize::from(w)
        + usize::from(head.x)
        + usize::from(head.w) / 2;
    let mut edited = planes.to_vec();
    edited[k][p] = edited[k][p] % 254 + 1;
    let out = match build_preserving(w, h, &edited, Some(source)) {
        Ok(out) => out,
        Err(e) => {
            t.failures.push(format!("edit build error {e}: {path}"));
            return;
        }
    };
    if !sniff(&out, Some(out.len())) {
        t.failures
            .push(format!("edit output does not sniff: {path}"));
        return;
    }
    let back = parse(&out).expect("sniffed");
    if back.frames.len() != edited.len()
        || edited.iter().enumerate().any(|(i, p)| back.canvas(i) != *p)
    {
        t.failures
            .push(format!("edit output decodes differently: {path}"));
        return;
    }
    let mut ok = true;
    let mut offsets_kept = true;
    for j in (0..planes.len()).filter(|&j| j != k) {
        let before = FrameHeader::read(data, j);
        let after = FrameHeader::read(&out, j);
        let fields = |f: &FrameHeader| {
            let mut b = f.to_bytes();
            b[20..].fill(0);
            b
        };
        if fields(&before) != fields(&after) {
            ok = false;
            t.failures
                .push(format!("edit changed frame {j} header (k={k}): {path}"));
            break;
        }
        let payload = source.payload(j);
        if !payload.is_empty() {
            let at = after.offset as usize;
            if out.get(at..at + payload.len()) != Some(payload) {
                ok = false;
                t.failures
                    .push(format!("edit changed frame {j} data (k={k}): {path}"));
                break;
            }
        } else if before != after {
            ok = false;
            t.failures
                .push(format!("edit changed empty frame {j} (k={k}): {path}"));
            break;
        }
        offsets_kept &= before.offset == after.offset;
    }
    let now = FrameHeader::read(&out, k);
    if now.flags != head.flags || now.radar != head.radar || now.reserved != head.reserved {
        ok = false;
        t.failures.push(format!(
            "edit lost frame {k}'s flags/radar/reserved: {path}"
        ));
    }
    if ok {
        t.edit_ok += 1;
        t.edit_offsets_kept += usize::from(offsets_kept);
    }
}

fn check_file(path: &[u8], reference: Option<&Reference>, t: &mut Tally) {
    let shown = String::from_utf8_lossy(path).into_owned();
    t.files += 1;
    let data = match std::fs::read(Path::new(std::ffi::OsStr::from_bytes(path))) {
        Ok(d) => d,
        Err(e) => {
            t.failures.push(format!("unreadable ({e}): {shown}"));
            return;
        }
    };
    let ours = sniff(&data, Some(data.len()));
    let theirs = matches!(reference, Some(Reference::Shp { .. }));
    if reference.is_none() {
        t.failures.push(format!("no reference line: {shown}"));
    }
    if ours == theirs {
        t.sniff_agree += 1;
    } else {
        t.failures
            .push(format!("sniff ours={ours} reference={theirs}: {shown}"));
    }
    if !ours {
        t.refused_both += usize::from(!theirs);
        return;
    }
    let source = Source::read(data.clone()).expect("sniffed");
    if let Some(Reference::Shp { frames, md5 }) = reference {
        if *frames == source.frames().len() && *md5 == canvas_md5(&source) {
            t.decode_match += 1;
        } else {
            t.failures.push(format!("decode md5 differs: {shown}"));
        }
    }
    let planes = source.canvases();
    let (w, h) = (source.width(), source.height());
    match build_preserving(w, h, &planes, Some(&source)) {
        Ok(out) if out == data => t.unchanged_identical += 1,
        _ => t.failures.push(format!("unchanged save differs: {shown}")),
    }
    // Informational: the frame-by-frame writer without the whole-file
    // shortcut, i.e. what a save keeps when some other frame changed.
    match build_frames(w, h, &planes, Some(&source)) {
        Ok(out) if out == data => t.frames_path_identical += 1,
        Ok(out) => {
            let first = out.iter().zip(&data).position(|(a, b)| a != b);
            t.frames_path_diffs.push(format!(
                "len {} vs {}, first diff {:?}: {shown}",
                out.len(),
                data.len(),
                first
            ));
        }
        Err(e) => t.frames_path_diffs.push(format!("error {e}: {shown}")),
    }
    edit_check(&shown, &data, &source, &planes, t);
}

#[test]
#[ignore = "needs a private SHP corpus: set PICTURA_SHP_CORPUS_LIST and PICTURA_SHP_REFERENCE"]
fn corpus_gate() {
    let list = std::env::var("PICTURA_SHP_CORPUS_LIST").expect("PICTURA_SHP_CORPUS_LIST");
    let reference =
        read_reference(&std::env::var("PICTURA_SHP_REFERENCE").expect("PICTURA_SHP_REFERENCE"));
    let threads: usize = std::env::var("PICTURA_SHP_THREADS")
        .ok()
        .and_then(|v| v.parse().ok())
        .unwrap_or(3);
    let list = std::fs::read(&list).expect("corpus list");
    let paths: Vec<&[u8]> = list.split(|&b| b == 0).filter(|p| !p.is_empty()).collect();
    let next = AtomicUsize::new(0);
    let total = Mutex::new(Tally::default());
    std::thread::scope(|scope| {
        for _ in 0..threads.max(1) {
            scope.spawn(|| {
                let mut t = Tally::default();
                loop {
                    let i = next.fetch_add(1, Ordering::Relaxed);
                    let Some(path) = paths.get(i) else { break };
                    check_file(path, reference.get(*path), &mut t);
                }
                let mut all = total.lock().unwrap();
                all.files += t.files;
                all.sniff_agree += t.sniff_agree;
                all.refused_both += t.refused_both;
                all.decode_match += t.decode_match;
                all.unchanged_identical += t.unchanged_identical;
                all.frames_path_identical += t.frames_path_identical;
                all.edited += t.edited;
                all.edit_ok += t.edit_ok;
                all.edit_offsets_kept += t.edit_offsets_kept;
                all.no_drawn_frame += t.no_drawn_frame;
                all.failures.extend(t.failures);
                all.frames_path_diffs.extend(t.frames_path_diffs);
            });
        }
    });
    let t = total.into_inner().unwrap();
    let accepted = t.files - t.refused_both;
    println!("files                      {}", t.files);
    println!("sniff agrees               {}", t.sniff_agree);
    println!("refused by both            {}", t.refused_both);
    println!(
        "decode md5 matches         {} of {accepted}",
        t.decode_match
    );
    println!(
        "unchanged save identical   {} of {accepted}",
        t.unchanged_identical
    );
    println!(
        "frame-by-frame identical   {} of {accepted} (informational)",
        t.frames_path_identical
    );
    println!(
        "one-pixel edit holds       {} of {} ({} with no drawn frame)",
        t.edit_ok, t.edited, t.no_drawn_frame
    );
    println!(
        "  ... with every other offset unmoved: {}",
        t.edit_offsets_kept
    );
    for line in &t.frames_path_diffs {
        println!("frame-by-frame differs: {line}");
    }
    for line in &t.failures {
        println!("FAIL {line}");
    }
    assert!(t.failures.is_empty(), "{} failures", t.failures.len());
    assert_eq!(t.sniff_agree, t.files);
    assert_eq!(t.decode_match, accepted);
    assert_eq!(t.unchanged_identical, accepted);
    assert_eq!(t.edit_ok, t.edited);
}
