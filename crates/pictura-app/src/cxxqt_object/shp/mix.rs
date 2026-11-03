// SPDX-License-Identifier: GPL-3.0-or-later
//! MIX archives in the app: the browser's listing, extracting an entry to a
//! file, writing a copy of an archive with one entry replaced, and the
//! palette searches. The codec is `pictura_shp::mix`.
//!
//! Names come from the list shipped with the codec, the local list the
//! user's own files gave (`$XDG_STATE_HOME/kooka-pictura/mix-names.txt`, never
//! shipped), the local mix databases inside the archives listed, and the
//! game INI files inside them (each name kept only when its ID is an entry
//! of the archive and the entry is the kind its suffix says). What is
//! learned from an archive serves every later listing of the session.

use super::set_error;
use crate::cxxqt_object::shp::ffi::{MixListing, MixRow};
use cxx_qt_lib::QString;
use pictura_shp::mix::{self, ids, names, Data, IdKind, Kind, Node, Plan};
use std::collections::{BTreeSet, VecDeque};
use std::fs::File;
use std::io::{BufWriter, Read, Seek, SeekFrom, Write};
use std::path::{Path, PathBuf};
use std::sync::{Mutex, MutexGuard, OnceLock};

/// The local name list (never part of the repository).
pub(in crate::cxxqt_object) fn local_names_path() -> PathBuf {
    let base = std::env::var_os("XDG_STATE_HOME")
        .filter(|v| !v.is_empty())
        .map(PathBuf::from)
        .unwrap_or_else(|| {
            PathBuf::from(std::env::var_os("HOME").unwrap_or_default()).join(".local/state")
        });
    base.join("kooka-pictura/mix-names.txt")
}

/// The session's names: the shipped list, the local list (as it was when
/// first needed) and what listings learned since.
fn names() -> MutexGuard<'static, names::Names> {
    static NAMES: OnceLock<Mutex<names::Names>> = OnceLock::new();
    NAMES
        .get_or_init(|| {
            let mut n = names::Names::shipped();
            if let Ok(text) = std::fs::read_to_string(local_names_path()) {
                n.add_list(&text);
            }
            Mutex::new(n)
        })
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner())
}

/// The INI files names are learned from.
fn ini_names() -> Vec<String> {
    let mut out = Vec::new();
    for stem in ["rules", "art"] {
        out.push(format!("{stem}.ini"));
        out.push(format!("{stem}md.ini"));
    }
    for (name, _, _) in names::THEATERS {
        out.push(format!("{name}.ini"));
        out.push(format!("{name}md.ini"));
    }
    out
}

/// INI files over this size are not read for names.
const MAX_INI: u32 = 8 << 20;

fn read_entry(file: &mut File, at: u64, len: u64) -> std::io::Result<Vec<u8>> {
    let mut buf = vec![0u8; len as usize];
    file.seek(SeekFrom::Start(at))?;
    file.read_exact(&mut buf)?;
    Ok(buf)
}

/// Learn names from the local mix databases and game INI files in `root`.
fn learn(root: &Node, file: &mut File) {
    let inis = ini_names();
    let mut found: Vec<String> = Vec::new();
    let mut candidates = BTreeSet::new();
    for node in root.walk() {
        for (i, e) in node.index.entries.iter().enumerate() {
            let kind = node.kinds[i];
            let at = node.start + node.index.start(e);
            if kind == Kind::Lmd && e.size <= MAX_INI {
                if let Ok(bytes) = read_entry(file, at, u64::from(e.size)) {
                    found.extend(names::parse_lmd(&bytes).unwrap_or_default());
                }
            } else if matches!(kind, Kind::Ini | Kind::Other) && e.size <= MAX_INI {
                if let Some(ini) = inis.iter().find(|n| ids::ts_id(n) == e.id) {
                    if let Ok(bytes) = read_entry(file, at, u64::from(e.size)) {
                        names::harvest_ini(ini, &String::from_utf8_lossy(&bytes), &mut candidates);
                    }
                }
            }
        }
    }
    let kinds: Vec<(u32, Kind)> = root
        .walk()
        .iter()
        .flat_map(|n| {
            n.index
                .entries
                .iter()
                .map(|e| e.id)
                .zip(n.kinds.iter().copied())
        })
        .collect();
    let mut by_id: std::collections::HashMap<u32, Vec<Kind>> = std::collections::HashMap::new();
    for (id, kind) in kinds {
        by_id.entry(id).or_default().push(kind);
    }
    found.extend(candidates.into_iter().filter(|name| {
        by_id
            .get(&ids::ts_id(name))
            .is_some_and(|k| k.iter().any(|&k| names::suffix_fits(name, k)))
    }));
    let mut session = names();
    for name in found {
        session.add(&name);
    }
}

fn format_label(index: &mix::Index) -> String {
    let mut parts = vec![match index.format {
        mix::Format::Old => "Tiberian Dawn layout",
        mix::Format::New { .. } => "Red Alert / Tiberian Sun / Red Alert 2 layout",
    }
    .to_owned()];
    if index.encrypted() {
        parts.push("encrypted index".into());
    }
    if index.has_checksum() {
        parts.push("SHA-1 checksum".into());
    }
    parts.push(format!("{} entries", index.entries.len()));
    parts.join(", ")
}

fn add_rows(node: &Node, parent: i32, chain: &[u32], names: &names::Names, rows: &mut Vec<MixRow>) {
    let kind = names.kind_for(&node.index);
    for (i, e) in node.index.entries.iter().enumerate() {
        let mut here = chain.to_vec();
        here.push(e.id);
        let entry_kind = node.kinds[i];
        rows.push(MixRow {
            parent,
            id: e.id,
            name: QString::from(names.get(kind, e.id).unwrap_or("")),
            kind: QString::from(entry_kind.label()),
            suffix: QString::from(entry_kind.suffix()),
            size: u64::from(e.size),
            chain: here.clone(),
        });
        if let Some(child) = node.child(i) {
            let row = rows.len() as i32 - 1;
            add_rows(child, row, &here, names, rows);
        }
    }
}

/// List the archive at `path`: every entry of it and of the archives
/// nested in it, depth first (a nested archive's entries follow its row).
pub(in crate::cxxqt_object) fn mix_list(path: &QString) -> MixListing {
    let mut listing = MixListing {
        error: QString::default(),
        format: QString::default(),
        rows: Vec::new(),
    };
    let path = path.to_string();
    let result = (|| -> Result<Node, mix::MixError> {
        let mut file = File::open(&path)?;
        let len = file.metadata()?.len();
        let root = mix::scan(&file, 0, len)?;
        learn(&root, &mut file);
        Ok(root)
    })();
    match result {
        Ok(root) => {
            listing.format = QString::from(format_label(&root.index).as_str());
            add_rows(&root, -1, &[], &names(), &mut listing.rows);
        }
        Err(e) => listing.error = QString::from(format!("{path}: {e}").as_str()),
    }
    listing
}

/// True when the file at `path` is a MIX archive (its index accounts for
/// the file).
pub(in crate::cxxqt_object) fn mix_sniff_file(path: &QString) -> bool {
    let Ok(mut file) = File::open(path.to_string()) else {
        return false;
    };
    let len = file.metadata().map(|m| m.len()).unwrap_or(0);
    mix::probe(&mut file, 0, len).is_some()
}

/// Copy the entry a chain of IDs names out of the archive at `path` into
/// the file `out`. False with the reason in `shp_last_error`.
pub(in crate::cxxqt_object) fn mix_extract(path: &QString, chain: &[u32], out: &QString) -> bool {
    let result = (|| -> Result<(), mix::MixError> {
        let mut file = File::open(path.to_string())?;
        let len = file.metadata()?.len();
        let (start, size) = mix::locate(&mut file, len, chain)?;
        let mut w = BufWriter::new(File::create(out.to_string())?);
        mix::copy_range(&mut file, start, size, &mut w)?;
        w.flush()?;
        Ok(())
    })();
    result.map_err(|e| set_error(&e.to_string())).is_ok()
}

/// Write a copy of the archive at `path` to `out` with the entry a chain of
/// IDs names replaced by the bytes of the file `entry`. Every archive on the
/// chain is rebuilt without encryption; everything else is copied byte for
/// byte. The copy is written beside `out` and renamed over it when
/// complete, so `out` may be `path` itself (the caller confirms that).
pub(in crate::cxxqt_object) fn mix_replace(
    path: &QString,
    chain: &[u32],
    entry: &QString,
    out: &QString,
) -> bool {
    let out = PathBuf::from(out.to_string());
    let partial = out.with_file_name(format!(
        "{}.kooka-partial",
        out.file_name()
            .map(|n| n.to_string_lossy())
            .unwrap_or_default()
    ));
    let result = (|| -> Result<(), mix::MixError> {
        let bytes = std::fs::read(entry.to_string())?;
        let mut src = File::open(path.to_string())?;
        let len = src.metadata()?.len();
        let plan = Plan::replacing(&mut src, len, chain, Data::Bytes(bytes))?;
        let file = File::create(&partial)?;
        let mut w = BufWriter::with_capacity(1 << 20, file);
        plan.write(&mut src, &mut w)?;
        let file = w.into_inner().map_err(|e| mix::MixError(e.to_string()))?;
        file.sync_all()?;
        std::fs::rename(&partial, &out)?;
        Ok(())
    })();
    if let Err(e) = result {
        let _ = std::fs::remove_file(&partial);
        set_error(&e.to_string());
        return false;
    }
    true
}

/// Visit every entry of the archive at `path`, shallowest first, with its
/// member path (`ra2.mix/cache.mix/unittem.pal`) and name. Nested archives
/// are entered only when a name says they are one (`*.mix`), so a search
/// reads indexes, not entries.
fn visit_named(path: &str, mut visit: impl FnMut(&str, Option<&str>, u32) -> bool) {
    let Ok(mut file) = File::open(path) else {
        return;
    };
    let len = file.metadata().map(|m| m.len()).unwrap_or(0);
    let names = names();
    let mut queue = VecDeque::from([(0u64, len, path.to_owned(), 0usize)]);
    while let Some((start, size, prefix, depth)) = queue.pop_front() {
        let Ok(index) = mix::read_index(&mut file, start, size) else {
            continue;
        };
        let kind = names.kind_for(&index);
        for e in &index.entries {
            let name = names.get(kind, e.id);
            let member = format!("{prefix}/{}", mix::component(name, e.id));
            if !visit(&member, name, e.id) {
                return;
            }
            let nested = name.is_some_and(|n| n.to_ascii_lowercase().ends_with(".mix"));
            if nested && depth < mix::MAX_DEPTH && index.in_bounds(e) {
                let at = start + index.start(e);
                if mix::probe(&mut file, at, u64::from(e.size)).is_some() {
                    queue.push_back((at, u64::from(e.size), member, depth + 1));
                }
            }
        }
    }
}

/// The member path of the first entry called `name` in the archive at
/// `path` or the archives nested in it (shallowest first), or empty.
pub(in crate::cxxqt_object) fn mix_find(path: &QString, name: &QString) -> QString {
    let name = name.to_string();
    let wanted = [IdKind::Crc.id(&name), IdKind::Classic.id(&name)];
    let mut found = String::new();
    visit_named(&path.to_string(), |member, _, id| {
        if wanted.contains(&id) {
            found = member
                .rsplit_once('/')
                .map_or(member.to_owned(), |(dir, _)| format!("{dir}/{name}"));
            return false;
        }
        true
    });
    QString::from(found.as_str())
}

/// The member path of the entry whose name ends in `suffix` and whose stem
/// is the longest one that starts `stem` (any case), or empty.
pub(in crate::cxxqt_object) fn mix_find_prefix(
    path: &QString,
    stem: &QString,
    suffix: &QString,
) -> QString {
    let stem = stem.to_string().to_ascii_lowercase();
    let suffix = suffix.to_string().to_ascii_lowercase();
    let mut best = (0usize, String::new());
    visit_named(&path.to_string(), |member, name, _| {
        if let Some(name) = name.map(str::to_ascii_lowercase) {
            if let Some(entry_stem) = name.strip_suffix(&suffix) {
                if entry_stem.len() > best.0 && stem.starts_with(entry_stem) {
                    best = (entry_stem.len(), member.to_owned());
                }
            }
        }
        true
    });
    QString::from(best.1.as_str())
}

/// True when `path` is a file or an entry inside a MIX
/// (`ra2.mix/cache.mix/unittem.pal`).
pub(in crate::cxxqt_object) fn mix_path_exists(path: &QString) -> bool {
    mix::path_exists(&path.to_string())
}

/// The bytes of a palette at `path`, a file or a MIX member, when it is at
/// most `cap` bytes.
pub(in crate::cxxqt_object) fn read_small(path: &str, cap: u64) -> Option<Vec<u8>> {
    if Path::new(path).is_file() {
        return crate::cxxqt_object::helpers::safe_io::read_capped(path, cap);
    }
    let (mut file, start, len) = mix::locate_member(path).ok()?;
    (len <= cap).then(|| read_entry(&mut file, start, len).ok())?
}
