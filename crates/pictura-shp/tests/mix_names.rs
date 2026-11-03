//! Builds the MIX name lists from names that can be proven: every candidate
//! counts only when its ID is an entry of an archive in the corpus and the
//! entry's content is the kind its suffix says. Ignored; it reads a private
//! corpus and writes two lists:
//!
//! ```text
//! PICTURA_MIX_CORPUS_LIST=/path/mixlist0    NUL-separated archive paths
//! PICTURA_MIX_STOCK_DIR=/path/game          a stock game folder
//! PICTURA_MIX_LOOSE=/path/shplist0:/path/tmplist0   NUL-separated loose files
//! PICTURA_MIX_NAMES_SHIPPED=crates/pictura-shp/data/mix-names.txt
//! PICTURA_MIX_NAMES_LOCAL=$XDG_STATE_HOME/kooka-pictura/mix-names.txt
//! cargo test -p pictura-shp --release --test mix_names -- --ignored --nocapture
//! ```
//!
//! The shipped list holds only names from the stock game: read from the
//! stock archives' own INI files and from the stock game executables'
//! strings, each kept when its ID is in a stock archive (the files named in
//! `STOCK`, in the stock folder). The local list (never committed) holds
//! everything else that matched anywhere in the corpus: names from mods'
//! INI files, from local mix databases and from the loose files' names.

use pictura_shp::mix::{self, ids, names, IdKind, Kind, Node};
use std::collections::{BTreeSet, HashMap, HashSet};
use std::fs::File;
use std::io::{Read, Seek, SeekFrom};
use std::os::unix::ffi::OsStrExt;
use std::path::{Path, PathBuf};

/// The archives of a stock Red Alert 2 + Yuri's Revenge install (with the
/// official 1.001 patch's `expandmd01.mix`).
const STOCK: [&str; 12] = [
    "ra2.mix",
    "language.mix",
    "multi.mix",
    "maps01.mix",
    "maps02.mix",
    "theme.mix",
    "ra2md.mix",
    "langmd.mix",
    "multimd.mix",
    "mapsmd03.mix",
    "thememd.mix",
    "expandmd01.mix",
];

/// The stock executables whose strings name files.
const STOCK_EXES: [&str; 4] = ["ra2.exe", "game.exe", "ra2md.exe", "gamemd.exe"];

fn paths(list: &Path) -> Vec<PathBuf> {
    std::fs::read(list)
        .unwrap_or_default()
        .split(|&b| b == 0)
        .filter(|p| !p.is_empty())
        .map(|p| PathBuf::from(std::ffi::OsStr::from_bytes(p)))
        .collect()
}

/// The INI files whose names the candidates are read from.
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

#[derive(Default)]
struct Found {
    /// ID -> the kinds of the entries holding it.
    kinds: HashMap<u32, HashSet<Kind>>,
    candidates: BTreeSet<String>,
    lmd: BTreeSet<String>,
}

fn read_entry(file: &mut File, at: u64, len: u64) -> Vec<u8> {
    let mut buf = vec![0u8; len as usize];
    file.seek(SeekFrom::Start(at)).unwrap();
    file.read_exact(&mut buf).unwrap();
    buf
}

fn collect(path: &Path, found: &mut Found) {
    let Ok(mut file) = File::open(path) else {
        return;
    };
    let len = file.metadata().map(|m| m.len()).unwrap_or(0);
    let Ok(root) = mix::scan(&file, 0, len) else {
        return;
    };
    let inis = ini_names();
    for node in root.walk() {
        visit(node, &mut file, &inis, found);
    }
}

fn visit(node: &Node, file: &mut File, inis: &[String], found: &mut Found) {
    for (i, e) in node.index.entries.iter().enumerate() {
        let kind = node.kinds[i];
        found.kinds.entry(e.id).or_default().insert(kind);
        let at = node.start + node.index.start(e);
        if kind == Kind::Lmd {
            let bytes = read_entry(file, at, u64::from(e.size));
            found
                .lmd
                .extend(names::parse_lmd(&bytes).unwrap_or_default());
        }
        if matches!(kind, Kind::Ini | Kind::Other) {
            if let Some(ini) = inis.iter().find(|n| ids::ts_id(n) == e.id) {
                let bytes = read_entry(file, at, u64::from(e.size));
                names::harvest_ini(ini, &String::from_utf8_lossy(&bytes), &mut found.candidates);
                found.candidates.insert(ini.clone());
            }
        }
    }
}

/// File names in an executable's printable strings; `%02d` and `*` stand
/// for 00..99, `%s` for every theater name and suffix.
fn exe_names(path: &Path, out: &mut BTreeSet<String>) {
    let Ok(bytes) = std::fs::read(path) else {
        return;
    };
    let suffixes = [
        ".mix", ".pal", ".ini", ".shp", ".vxl", ".hva", ".tem", ".sno", ".urb", ".ubn", ".des",
        ".lun", ".csf", ".fnt", ".pcx", ".wav",
    ];
    for run in bytes.split(|b| !(0x20..0x7F).contains(b)) {
        let text = String::from_utf8_lossy(run).to_ascii_lowercase();
        for word in text.split(|c: char| !(c.is_ascii_alphanumeric() || "._%*-".contains(c))) {
            if !suffixes.iter().any(|s| word.ends_with(s)) || word.len() > 40 {
                continue;
            }
            let mut forms = vec![word.to_owned()];
            for (pattern, fills) in [
                (
                    "%02d",
                    (0..100).map(|i| format!("{i:02}")).collect::<Vec<_>>(),
                ),
                ("*", (0..100).map(|i| format!("{i:02}")).collect()),
                (
                    "%s",
                    names::THEATERS
                        .iter()
                        .flat_map(|(n, s, _)| [n.to_string(), s.to_string()])
                        .collect(),
                ),
            ] {
                forms = forms
                    .into_iter()
                    .flat_map(|f| {
                        if f.contains(pattern) {
                            fills.iter().map(|x| f.replacen(pattern, x, 1)).collect()
                        } else {
                            vec![f]
                        }
                    })
                    .collect();
            }
            out.extend(forms.into_iter().filter(|f| !f.contains('%')));
        }
    }
}

fn proven(candidates: &BTreeSet<String>, kinds: &HashMap<u32, HashSet<Kind>>) -> BTreeSet<String> {
    candidates
        .iter()
        .filter(|name| {
            kinds
                .get(&ids::ts_id(name))
                .is_some_and(|k| k.iter().any(|&k| names::suffix_fits(name, k)))
        })
        .cloned()
        .collect()
}

#[test]
#[ignore = "builds the name lists from a private corpus"]
fn build_the_name_lists() {
    let var = |name: &str| std::env::var(name).unwrap_or_else(|_| panic!("set {name}"));
    let stock_dir = PathBuf::from(var("PICTURA_MIX_STOCK_DIR"));
    let corpus = paths(Path::new(&var("PICTURA_MIX_CORPUS_LIST")));

    // The stock game: its archives' INIs and its executables' strings.
    let mut stock = Found::default();
    let mut stock_files = Vec::new();
    for entry in std::fs::read_dir(&stock_dir).unwrap().flatten() {
        let name = entry.file_name().to_string_lossy().to_ascii_lowercase();
        if STOCK.contains(&name.as_str()) {
            stock_files.push(name.clone());
            collect(&entry.path(), &mut stock);
        }
        if STOCK_EXES.contains(&name.as_str()) {
            exe_names(&entry.path(), &mut stock.candidates);
        }
    }
    stock_files.sort();
    let shipped = proven(&stock.candidates, &stock.kinds);
    println!(
        "stock: {} archives ({:?}), {} candidates, {} proven",
        stock_files.len(),
        stock_files,
        stock.candidates.len(),
        shipped.len()
    );

    // Everything else in the corpus.
    let mut all = Found::default();
    for path in &corpus {
        collect(path, &mut all);
    }
    for list in var("PICTURA_MIX_LOOSE").split(':') {
        for path in paths(Path::new(list)) {
            if let Some(name) = path.file_name() {
                all.candidates
                    .insert(name.to_string_lossy().to_ascii_lowercase());
            }
        }
    }
    for path in &corpus {
        if let Some(name) = path.file_name() {
            all.candidates
                .insert(name.to_string_lossy().to_ascii_lowercase());
        }
    }
    all.candidates
        .extend(all.lmd.iter().map(|n| n.to_ascii_lowercase()));
    all.candidates.extend(shipped.iter().cloned());
    let local: BTreeSet<String> = proven(&all.candidates, &all.kinds)
        .difference(&shipped)
        .cloned()
        .collect();
    let ids_total = all.kinds.len();
    let mut named = names::Names::new();
    for n in shipped.iter().chain(&local) {
        named.add(n);
    }
    let named_ids = all
        .kinds
        .keys()
        .filter(|id| named.get(IdKind::Crc, **id).is_some())
        .count();
    println!(
        "corpus: {} archives, {} distinct IDs, {} candidates, {} LMD names, local list {} names; {} of the IDs named",
        corpus.len(),
        ids_total,
        all.candidates.len(),
        all.lmd.len(),
        local.len(),
        named_ids
    );

    let header = "# Names of entries in the stock Red Alert 2 and Yuri's Revenge MIX archives.\n\
                  # Read from the stock archives' INI files and the stock executables' strings;\n\
                  # each kept only because its ID is an entry of a stock archive whose content\n\
                  # is the kind its suffix says (crates/pictura-shp/tests/mix_names.rs).\n";
    std::fs::write(
        var("PICTURA_MIX_NAMES_SHIPPED"),
        format!("{header}{}", names::to_list(&shipped)),
    )
    .unwrap();
    let local_path = PathBuf::from(var("PICTURA_MIX_NAMES_LOCAL"));
    if let Some(dir) = local_path.parent() {
        std::fs::create_dir_all(dir).unwrap();
    }
    std::fs::write(
        &local_path,
        format!(
            "# MIX entry names found in this machine's own files (not shipped).\n{}",
            names::to_list(&local)
        ),
    )
    .unwrap();
}
