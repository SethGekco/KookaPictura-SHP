# Tasks

## 1. New TMP from Image

- [x] 1.1 Write the codec tests (`crates/pictura-shp/tests/tmp_image.rs`), the app tests (`shp/tmp_image_tests.rs`) and the `tst_tmp` cases.
- [x] 1.2 Add `tmp_image` (cells, positions, flat z, extras) to `crates/pictura-shp`.
- [x] 1.3 Add `tmp_from_image` and `tmp_image_cells`, the options dialog, File > New TMP from Image and Save As TMP of an image.
- [x] 1.4 Compare the made templates with Ochre's reader (`tmpedit-e2e.py new`).

## 2. TMP to SHP

- [x] 2.1 Write the app tests (`shp/tmp_shp_tests.rs`) and the `tst_tmp` cases.
- [x] 2.2 Add `tmp_to_shp` (frames per tile or one frame, extras over the tiles).
- [x] 2.3 Add the TmpToShpDialog and route Save As SHP of a template through it.
