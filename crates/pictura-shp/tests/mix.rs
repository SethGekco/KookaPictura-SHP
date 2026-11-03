//! MIX archives: every archive here is generated in code. The encrypted one
//! uses a test RSA key pair (Westwood's private key is not public, so no
//! one can make a key source for the games' own key); the key recovery is
//! the same code the games' archives go through, and the corpus gate
//! (`mix_corpus.rs`) reads the real ones with Westwood's public key.

mod common;

use common::Rng;
use pictura_shp::mix::{
    self, ids, key_source_for, names, read_index, read_index_with, sha1, Data, Format, IdKind,
    Kind, Plan, PublicKey, FLAG_CHECKSUM, FLAG_ENCRYPTED,
};
use pictura_shp::tmp::{self, diamond_rows, Template, Tile};
use std::collections::BTreeSet;
use std::fs::File;
use std::io::Cursor;

/// The test key pair (320 bits, like Westwood's; generated for these tests
/// with Python, `~/Claude/shp-corpus/mix/rsa.py` on the author's machine).
const TEST_N: &str =
    "9f836b724bfad138838bdac9145ccc35cff70fa5e09400345e357081c02151e3598f46b19dff9e4f";
const TEST_D: &str =
    "48fdd51bef22adbc638d8aa6fab76006bbcc3d0f1c95bccac891dca650b20b762ca3b3574cee68c9";
/// Python's `pow(m, d, n)` of the little-endian bytes 1..=39, as 40
/// little-endian bytes: a known answer from another implementation.
const TEST_C: &str =
    "25f0918e5ea08871c28fb91f82a6e4b776f8e3e15dbe0453fbee38824bf1e42821e052252cef526f";

fn hex(text: &str) -> Vec<u8> {
    (0..text.len())
        .step_by(2)
        .map(|i| u8::from_str_radix(&text[i..i + 2], 16).unwrap())
        .collect()
}

fn test_key() -> PublicKey {
    PublicKey::new(&hex(TEST_N), &[0x01, 0x00, 0x01])
}

/// The header and index of a new-format archive (`flags`), sorted by signed
/// ID, with the body packed in the order given.
fn index_bytes(entries: &[(u32, Vec<u8>)]) -> Vec<u8> {
    let mut rows = Vec::new();
    let mut offset = 0u32;
    for (id, data) in entries {
        rows.push((*id, offset, data.len() as u32));
        offset += data.len() as u32;
    }
    rows.sort_by_key(|r| r.0 as i32);
    let mut out = Vec::new();
    out.extend_from_slice(&(entries.len() as u16).to_le_bytes());
    out.extend_from_slice(&offset.to_le_bytes());
    for (id, off, size) in rows {
        out.extend_from_slice(&id.to_le_bytes());
        out.extend_from_slice(&off.to_le_bytes());
        out.extend_from_slice(&size.to_le_bytes());
    }
    out
}

fn body(entries: &[(u32, Vec<u8>)]) -> Vec<u8> {
    entries.iter().flat_map(|(_, d)| d.clone()).collect()
}

fn new_archive(entries: &[(u32, Vec<u8>)], checksum: bool) -> Vec<u8> {
    let flags = if checksum { FLAG_CHECKSUM } else { 0 };
    let mut out = flags.to_le_bytes().to_vec();
    out.extend(index_bytes(entries));
    let body = body(entries);
    out.extend_from_slice(&body);
    if checksum {
        out.extend_from_slice(&sha1(&body));
    }
    out
}

fn old_archive(entries: &[(u32, Vec<u8>)]) -> Vec<u8> {
    let mut out = index_bytes(entries);
    out.extend(body(entries));
    out
}

const BF_KEY: [u8; 56] = {
    let mut k = [0u8; 56];
    let mut i = 0;
    while i < 56 {
        k[i] = (i as u8).wrapping_mul(37).wrapping_add(11);
        i += 1;
    }
    k
};

fn encrypted_archive(entries: &[(u32, Vec<u8>)]) -> Vec<u8> {
    let mut out = FLAG_ENCRYPTED.to_le_bytes().to_vec();
    out.extend(key_source_for(&test_key(), &hex(TEST_D), &BF_KEY));
    let mut index = index_bytes(entries);
    index.resize(index.len().div_ceil(8) * 8, 0xA5);
    mix::blowfish(&BF_KEY, &mut index, false);
    out.extend(index);
    out.extend(body(entries));
    out
}

fn shp_bytes(seed: u64) -> Vec<u8> {
    let mut rng = Rng::new(seed);
    let planes: Vec<Vec<u8>> = (0..3)
        .map(|_| (0..12 * 9).map(|_| rng.below(4) as u8 * 60).collect())
        .collect();
    pictura_shp::build(12, 9, &planes).unwrap()
}

fn tmp_bytes() -> Vec<u8> {
    let (cx, cy) = (60, 30);
    let mut tile = Tile::new(0, 0, cx, cy);
    for (row, start, width) in diamond_rows(cx, cy) {
        tile.image[row * cx + start..row * cx + start + width].fill(7);
    }
    let t = Template {
        blocks_x: 1,
        blocks_y: 1,
        cx,
        cy,
        tiles: vec![Some(tile)],
        unreadable: Vec::new(),
    };
    tmp::build(&t, None).unwrap()
}

fn pal_bytes() -> Vec<u8> {
    (0..768).map(|i| (i % 64) as u8).collect()
}

fn id(name: &str) -> u32 {
    ids::ts_id(name)
}

fn scratch(name: &str) -> std::path::PathBuf {
    let dir = std::path::Path::new(env!("CARGO_TARGET_TMPDIR")).join("mix-tests");
    std::fs::create_dir_all(&dir).unwrap();
    dir.join(name)
}

#[test]
fn ts_ids_match_names_found_in_the_stock_archives() {
    // Each of these IDs is an entry of the stock Yuri's Revenge archives
    // whose content is what the name says (checked by the corpus probe).
    for (name, want) in [
        ("local mix database.dat", 0x366E_051F),
        ("rules.ini", 0xF025_A96C),
        ("rulesmd.ini", 0x8218_F9F4),
        ("artmd.ini", 0x5B47_D8D5),
        ("cache.mix", 0x3B5A_96DE),
        ("local.mix", 0xA854_8FD9),
        ("unittem.pal", 0x63DA_7359),
        ("isotem.pal", 0x5F9D_97B9),
    ] {
        assert_eq!(ids::ts_id(name), want, "{name}");
        assert_eq!(
            ids::ts_id(&name.to_uppercase()),
            want,
            "{name} in upper case"
        );
    }
}

#[test]
fn td_ids_rotate_and_add() {
    // XCC's own ID for its names file in Tiberian Dawn archives, and two
    // computed by an independent Python version of the hash.
    assert_eq!(ids::td_id("local mix database.dat"), 0x54C2_D545);
    assert_eq!(ids::td_id("rules.ini"), 0xB1C3_B238);
    assert_eq!(ids::td_id("CONQUER.MIX"), 0xA236_1104);
}

#[test]
fn crc32_and_sha1_match_their_check_values() {
    assert_eq!(ids::crc32(b"123456789"), 0xCBF4_3926);
    assert_eq!(
        sha1(b"abc").to_vec(),
        hex("a9993e364706816aba3e25717850c26c9cd0d89d")
    );
    assert_eq!(
        sha1(b"").to_vec(),
        hex("da39a3ee5e6b4b0d3255bfef95601890afd80709")
    );
    let long = vec![b'a'; 1_000_000];
    let mut s = mix::Sha1::new();
    for chunk in long.chunks(7919) {
        s.update(chunk);
    }
    assert_eq!(
        s.finish().to_vec(),
        hex("34aa973cd4c4daa4f61eeb2bdbad27316534016f")
    );
}

#[test]
fn blowfish_matches_published_vectors() {
    // Eric Young's vectors, and one made with OpenSSL's bf-ecb.
    for (key, plain, cipher) in [
        ("0000000000000000", "0000000000000000", "4ef997456198dd78"),
        ("ffffffffffffffff", "ffffffffffffffff", "51866fd5b85ecb8a"),
        (
            "000102030405060708090a0b0c0d0e0f",
            "0123456789abcdef",
            "5b2c1ca4d5528ad2",
        ),
    ] {
        let mut data = hex(plain);
        mix::blowfish(&hex(key), &mut data, false);
        assert_eq!(data, hex(cipher), "key {key}");
        mix::blowfish(&hex(key), &mut data, true);
        assert_eq!(data, hex(plain));
    }
}

#[test]
fn the_key_recovery_matches_another_implementation() {
    let key = test_key();
    assert_eq!(key.key_source_len(), 80);
    let mut source = hex(TEST_C);
    source.extend(hex(TEST_C));
    let out = key.blowfish_key(&source);
    let expect: Vec<u8> = (1..=39u8).chain(1..=17u8).collect();
    assert_eq!(out.to_vec(), expect);
    // And a key source made here for a key comes back as that key.
    let made = key_source_for(&key, &hex(TEST_D), &BF_KEY);
    assert_eq!(made.len(), 80);
    assert_eq!(key.blowfish_key(&made), BF_KEY);
}

fn sample_entries() -> Vec<(u32, Vec<u8>)> {
    vec![
        (id("a.shp"), shp_bytes(1)),
        (id("b.pal"), pal_bytes()),
        (id("rules.ini"), b"[General]\r\nName=x\r\n".to_vec()),
        (0x8000_0001, vec![1, 2, 3]),
        (id("empty.bin"), Vec::new()),
    ]
}

fn assert_entries(data: &[u8], index: &mix::Index, entries: &[(u32, Vec<u8>)]) {
    assert_eq!(index.entries.len(), entries.len());
    assert!(index.consistent());
    assert_eq!(index.trailing(), 0);
    for (id, bytes) in entries {
        let e = index.entries[index.find(*id).unwrap()];
        let start = index.start(&e) as usize;
        assert_eq!(&data[start..start + e.size as usize], &bytes[..]);
    }
}

#[test]
fn an_old_format_archive_reads() {
    let entries = sample_entries();
    let data = old_archive(&entries);
    let index = read_index(&mut Cursor::new(&data), 0, data.len() as u64).unwrap();
    assert_eq!(index.format, Format::Old);
    assert_entries(&data, &index, &entries);
    assert!(mix::probe(&mut Cursor::new(&data), 0, data.len() as u64).is_some());
}

#[test]
fn an_unencrypted_archive_reads_and_rebuilds_byte_identical() {
    let entries = sample_entries();
    let data = new_archive(&entries, false);
    let index = read_index(&mut Cursor::new(&data), 0, data.len() as u64).unwrap();
    assert_eq!(index.format, Format::New { flags: 0 });
    assert_entries(&data, &index, &entries);
    let plan = Plan::from_index(&index, 0).unwrap();
    assert_eq!(plan.len(), data.len() as u64);
    let mut out = Vec::new();
    plan.write(&mut Cursor::new(&data), &mut out).unwrap();
    assert_eq!(out, data);
    // The old layout is kept too.
    let old = old_archive(&entries);
    let index = read_index(&mut Cursor::new(&old), 0, old.len() as u64).unwrap();
    let mut out = Vec::new();
    Plan::from_index(&index, 0)
        .unwrap()
        .write(&mut Cursor::new(&old), &mut out)
        .unwrap();
    assert_eq!(out, old);
}

#[test]
fn a_checksummed_archive_verifies_and_keeps_its_checksum_on_write() {
    let entries = sample_entries();
    let data = new_archive(&entries, true);
    let mut r = Cursor::new(&data);
    let index = read_index(&mut r, 0, data.len() as u64).unwrap();
    assert!(index.has_checksum());
    assert_entries(&data, &index, &entries);
    let (stored, now) = mix::checksums(&mut r, 0, &index).unwrap().unwrap();
    assert_eq!(stored, now);
    let mut plan = Plan::from_index(&index, 0).unwrap();
    let mut out = Vec::new();
    plan.write(&mut r, &mut out).unwrap();
    assert_eq!(out, data);
    plan.set(id("b.pal"), Data::Bytes(vec![9; 768]));
    let mut out = Vec::new();
    plan.write(&mut r, &mut out).unwrap();
    let mut w = Cursor::new(&out);
    let index = read_index(&mut w, 0, out.len() as u64).unwrap();
    let (stored, now) = mix::checksums(&mut w, 0, &index).unwrap().unwrap();
    assert_eq!(stored, now);
    assert_ne!(out, data);
}

#[test]
fn an_encrypted_archive_reads_with_its_key_and_writes_unencrypted() {
    let entries = sample_entries();
    let data = encrypted_archive(&entries);
    let mut r = Cursor::new(&data);
    let index = read_index_with(&mut r, 0, data.len() as u64, &test_key()).unwrap();
    assert!(index.encrypted());
    // 6 + 5 * 12 = 66 bytes of index, padded to 72, after 4 + 80.
    assert_eq!(index.body_start, 4 + 80 + 72);
    assert_entries(&data, &index, &entries);
    // Westwood's key reads it as nonsense, which is refused, not trusted.
    assert!(mix::probe(&mut Cursor::new(&data), 0, data.len() as u64).is_none());
    let mut out = Vec::new();
    Plan::from_index(&index, 0)
        .unwrap()
        .write(&mut r, &mut out)
        .unwrap();
    let written = read_index(&mut Cursor::new(&out), 0, out.len() as u64).unwrap();
    assert_eq!(written.flags(), 0);
    assert_entries(&out, &written, &entries);
}

/// `outer.mix` holding `inner.mix` (an SHP, a TMP and a palette) and
/// loose entries, with a checksum on the inner one.
fn nested() -> (Vec<u8>, Vec<(u32, Vec<u8>)>) {
    let inner_entries = vec![
        (id("unit.shp"), shp_bytes(7)),
        (id("clear01.tem"), tmp_bytes()),
        (id("unittem.pal"), pal_bytes()),
    ];
    let inner = new_archive(&inner_entries, true);
    let outer = new_archive(
        &[
            (id("readme.ini"), b"; hi\n[A]\nB=1\n".to_vec()),
            (id("inner.mix"), inner),
            (id("other.shp"), shp_bytes(8)),
            (0x1234_5678, b"opaque".to_vec()),
        ],
        false,
    );
    (outer, inner_entries)
}

#[test]
fn nested_archives_scan_with_kinds() {
    let (outer, _) = nested();
    let path = scratch("nested-scan.mix");
    std::fs::write(&path, &outer).unwrap();
    let file = File::open(&path).unwrap();
    let node = mix::scan(&file, 0, outer.len() as u64).unwrap();
    let kind = |n: &mix::Node, name: &str| n.kinds[n.index.find(id(name)).unwrap()];
    assert_eq!(kind(&node, "readme.ini"), Kind::Ini);
    assert_eq!(kind(&node, "inner.mix"), Kind::Mix);
    assert_eq!(kind(&node, "other.shp"), Kind::Shp);
    assert_eq!(
        node.kinds[node.index.find(0x1234_5678).unwrap()],
        Kind::Other
    );
    let inner = node
        .child(node.index.find(id("inner.mix")).unwrap())
        .unwrap();
    assert!(inner.index.has_checksum());
    assert_eq!(kind(inner, "unit.shp"), Kind::Shp);
    assert_eq!(kind(inner, "clear01.tem"), Kind::Tmp);
    assert_eq!(kind(inner, "unittem.pal"), Kind::Pal);
    assert_eq!(node.walk().len(), 2);
}

#[test]
fn member_paths_read_through_nesting() {
    let (outer, inner_entries) = nested();
    let path = scratch("members.mix");
    std::fs::write(&path, &outer).unwrap();
    let base = path.to_str().unwrap();
    for (name, bytes) in [
        ("inner.mix/unittem.pal", &inner_entries[2].1),
        ("inner.mix/UNIT.SHP", &inner_entries[0].1),
    ] {
        let member = format!("{base}/{name}");
        assert_eq!(&mix::read_path(&member).unwrap(), bytes, "{name}");
        assert!(mix::path_exists(&member));
    }
    let by_id = format!("{base}/#12345678");
    assert_eq!(mix::read_path(&by_id).unwrap(), b"opaque");
    assert!(!mix::path_exists(&format!("{base}/inner.mix/missing.pal")));
    assert!(mix::split_member_path(base).is_none());
    let (file, members) = mix::split_member_path(&format!("{base}/inner.mix/x.pal")).unwrap();
    assert_eq!(file, path);
    assert_eq!(members, ["inner.mix", "x.pal"]);
    // A chain of IDs finds the same range.
    let mut f = File::open(&path).unwrap();
    let (start, len) = mix::locate(
        &mut f,
        outer.len() as u64,
        &[id("inner.mix"), id("unittem.pal")],
    )
    .unwrap();
    assert_eq!(
        &outer[start as usize..(start + len) as usize],
        &inner_entries[2].1[..]
    );
}

#[test]
fn replacing_a_nested_entry_rebuilds_only_its_chain() {
    let (outer, inner_entries) = nested();
    let new_shp = shp_bytes(99);
    let plan = Plan::replacing(
        &mut Cursor::new(&outer),
        outer.len() as u64,
        &[id("inner.mix"), id("unit.shp")],
        Data::Bytes(new_shp.clone()),
    )
    .unwrap();
    let mut out = Vec::new();
    plan.write(&mut Cursor::new(&outer), &mut out).unwrap();
    assert_eq!(plan.len(), out.len() as u64);
    let mut r = Cursor::new(&out);
    let read = |r: &mut Cursor<&Vec<u8>>, chain: &[u32]| {
        let (s, l) = mix::locate(r, out.len() as u64, chain).unwrap();
        out[s as usize..(s + l) as usize].to_vec()
    };
    assert_eq!(read(&mut r, &[id("inner.mix"), id("unit.shp")]), new_shp);
    for (eid, bytes) in &inner_entries[1..] {
        assert_eq!(&read(&mut r, &[id("inner.mix"), *eid]), bytes);
    }
    assert_eq!(read(&mut r, &[id("other.shp")]), shp_bytes(8));
    assert_eq!(read(&mut r, &[0x1234_5678]), b"opaque");
    // The rebuilt inner archive still has a valid checksum.
    let (s, l) = mix::locate(&mut r, out.len() as u64, &[id("inner.mix")]).unwrap();
    let inner = read_index(&mut r, s, l).unwrap();
    let (stored, now) = mix::checksums(&mut r, s, &inner).unwrap().unwrap();
    assert_eq!(stored, now);
    // A missing entry is refused.
    assert!(Plan::replacing(
        &mut Cursor::new(&outer),
        outer.len() as u64,
        &[id("inner.mix"), id("nothere.shp")],
        Data::Bytes(Vec::new())
    )
    .is_err());
}

#[test]
fn entries_outside_the_archive_are_broken_and_refuse_a_plan() {
    let mut data = new_archive(&sample_entries(), false);
    // Grow one entry's size past the end.
    let at = 10 + 8;
    data[at..at + 4].copy_from_slice(&0x00FF_FFFFu32.to_le_bytes());
    let index = read_index(&mut Cursor::new(&data), 0, data.len() as u64).unwrap();
    assert!(!index.consistent());
    assert!(Plan::from_index(&index, 0).is_err());
    assert!(mix::probe(&mut Cursor::new(&data), 0, data.len() as u64).is_none());
    let path = scratch("broken.mix");
    std::fs::write(&path, &data).unwrap();
    let file = File::open(&path).unwrap();
    let node = mix::scan(&file, 0, data.len() as u64).unwrap();
    assert_eq!(node.kinds.iter().filter(|k| **k == Kind::Broken).count(), 1);
}

#[test]
fn small_sprites_are_not_taken_for_archives() {
    // An SHP 1 to 3 pixels wide starts like a flags word (checksum,
    // encrypted, both); a scan still calls it an SHP.
    let sprites: Vec<(u32, Vec<u8>)> = (1..=3u16)
        .map(|w| {
            let planes = vec![vec![5u8; usize::from(w) * 4]; 2];
            (u32::from(w), pictura_shp::build(w, 4, &planes).unwrap())
        })
        .collect();
    let data = new_archive(&sprites, false);
    let path = scratch("sprites.mix");
    std::fs::write(&path, &data).unwrap();
    let file = File::open(&path).unwrap();
    let node = mix::scan(&file, 0, data.len() as u64).unwrap();
    assert_eq!(node.kinds, vec![Kind::Shp; 3]);
    assert!(node.children.is_empty());
}

#[test]
fn names_come_from_lmds_inis_and_lists() {
    let mut lmd = b"XCC by Olaf van der Spek\x1a\x04\x17\x27\x10\x19\x80\x00".to_vec();
    let names_blob = b"gi.shp\0unittem.pal\0";
    for v in [52 + names_blob.len() as u32, 0, 0, 5, 2] {
        lmd.extend_from_slice(&v.to_le_bytes());
    }
    lmd.extend_from_slice(names_blob);
    assert_eq!(mix::sniff_kind(&lmd, lmd.len() as u64), Kind::Lmd);
    assert_eq!(names::parse_lmd(&lmd).unwrap(), ["gi.shp", "unittem.pal"]);

    let mut found = BTreeSet::new();
    names::harvest_ini(
        "temperatmd.ini",
        "[TileSet0001]\nFileName=clear\nTilesInSet=2 ; two\n[GI]\nCameo=giicon\nPalette=lib\n",
        &mut found,
    );
    for name in [
        "clear01.tem",
        "clear02.tem",
        "clear02b.tem",
        "gi.shp",
        "giicon.shp",
        "libtem.pal",
        "lib.pal",
    ] {
        assert!(found.contains(name), "{name}");
    }
    assert!(!found.contains("clear03.tem"));

    let mut list = names::Names::new();
    list.add_list("# comment\ngi.shp\n\nGI.SHP\n");
    assert_eq!(list.len(), 1);
    assert_eq!(list.get(IdKind::Crc, ids::ts_id("gi.shp")), Some("gi.shp"));
    assert_eq!(
        list.get(IdKind::Classic, ids::td_id("gi.shp")),
        Some("gi.shp")
    );
    let data = old_archive(&[(ids::td_id("gi.shp"), vec![0])]);
    let index = read_index(&mut Cursor::new(&data), 0, data.len() as u64).unwrap();
    assert_eq!(list.kind_for(&index), IdKind::Classic);
    assert!(names::suffix_fits("x.tem", Kind::Tmp));
    assert!(!names::suffix_fits("x.pal", Kind::Shp));
    // The shipped list is a list of names, one per line.
    assert!(!names::SHIPPED.contains('\r'));
}
