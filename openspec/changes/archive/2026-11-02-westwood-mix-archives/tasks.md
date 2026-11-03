# Tasks

## 1. Codec

- [x] 1.1 Write the synthetic codec tests (`crates/pictura-shp/tests/mix.rs`): both layouts, encryption with a test key pair, checksums, IDs, nesting, write round-trips.
- [x] 1.2 Write `src/mix/` (index, Blowfish, RSA key recovery, SHA-1, IDs, kinds, scan, member paths, writer).
- [x] 1.3 Build the name lists (`tests/mix_names.rs`): stock list shipped, local list kept in the state folder.
- [x] 1.4 Run the corpus gate (`tests/mix_corpus.rs`) over the owner's 681 archives.

## 2. App

- [x] 2.1 Write the app tests (`shp/mix_tests.rs`) and `tst_mix`.
- [x] 2.2 Add the app glue (`shp/mix.rs`): listing, extract, replace in a copy, palette search.
- [x] 2.3 Add the MIX browser dock, File > Open routing, entry documents and their titles, the save prompt, export.

## 3. Palettes

- [x] 3.1 Search the archives in SHP and TMP palette resolution; load palettes from member paths.
- [x] 3.2 Add Preferences > MIX Archives.
