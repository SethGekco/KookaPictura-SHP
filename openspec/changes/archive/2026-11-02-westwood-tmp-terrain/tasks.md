# Tasks

## 1. Codec

- [x] 1.1 Write the synthetic codec tests (`crates/pictura-shp/tests/tmp.rs`) and the ignored corpus gate (`tests/tmp_corpus.rs`).
- [x] 1.2 Port `cnctmp.py` to `crates/pictura-shp/src/tmp.rs` with the in-place writer.
- [x] 1.3 Run the corpus gate over the owner's 4,789 theater files.

## 2. Open

- [x] 2.1 Write the layer tests (`crates/pictura-shp/tests/terrain.rs`), the app tests (`shp/tmp_tests.rs`) and `tst_tmp`.
- [x] 2.2 Place templates on Tiles, Extras and hidden Z layers (`crates/pictura-shp/src/terrain.rs`) with the `KookaTMP` resource; open them from `decode_shp` when the SHP sniffer refuses.
- [x] 2.3 Add the Tiles panel, tile outlines, terrain palette resolution and the open filters.
