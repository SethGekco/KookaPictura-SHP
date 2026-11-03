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

## 3. Reshaping in the Tiles panel

- [x] 3.1 Write the codec tests (append, move, remove, extras), the resource test, the app tests (`shp/tmp_edit_tests.rs`) and the `tst_tmp` cases.
- [x] 3.2 Patch with appends in `tmp::build_with_layout`; the version 2 `KookaTMP` block with shapes.
- [x] 3.3 Add `tmp_edit_tiles` (relayout, size check, one History step) and `tmp_save_layout`.
- [x] 3.4 Add the panel buttons and the cell and extra-block dialogs; say the layout in the save status.
- [x] 3.5 Edit a copy of a real template and compare in Ochre's reader (`tmpedit-e2e.py edit`).
