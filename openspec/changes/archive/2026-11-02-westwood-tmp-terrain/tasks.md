# Tasks

## 1. Codec

- [x] 1.1 Write the synthetic codec tests (`crates/pictura-shp/tests/tmp.rs`) and the ignored corpus gate (`tests/tmp_corpus.rs`).
- [x] 1.2 Port `cnctmp.py` to `crates/pictura-shp/src/tmp.rs` with the in-place writer.
- [x] 1.3 Run the corpus gate over the owner's 4,789 theater files.

## 2. Open

- [x] 2.1 Write the layer tests (`crates/pictura-shp/tests/terrain.rs`), the app tests (`shp/tmp_tests.rs`) and `tst_tmp`.
- [x] 2.2 Place templates on Tiles, Extras and hidden Z layers (`crates/pictura-shp/src/terrain.rs`) with the `KookaTMP` resource; open them from `decode_shp` when the SHP sniffer refuses.
- [x] 2.3 Add the Tiles panel, tile outlines, terrain palette resolution and the open filters.

## 3. Save

- [x] 3.1 Write the save tests (`shp/tmp_tests.rs`, `tst_tmp`).
- [x] 3.2 Route the theater suffixes and `.tmp` to `shp::save_westwood`; read the layers back and write in place.
- [x] 3.3 Add the TMP save gate (`shp_support_tmp.cpp`), the Save As filter and the path helpers.
- [x] 3.4 Run the app over a 301-file TMP sample, 50 theater SHPs and the SHP regression sample; verify a one-pixel edit with Ochre's reader.

## 4. Fields

- [x] 4.1 Write the field tests (`shp/tmp_tests.rs`, `tst_tmp`).
- [x] 4.2 Add `tmp_set_tile_fields` (a recorded metadata edit of the resource's field table).
- [x] 4.3 Make the Tiles panel's Height, Land, Ramp and Radar cells editable.
