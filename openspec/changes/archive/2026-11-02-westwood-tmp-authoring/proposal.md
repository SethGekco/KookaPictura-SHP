# Proposal

## Why

The KookaPictura-SHP fork opens, edits and saves TMP terrain templates
(`2026-11-02-westwood-tmp-terrain`), but a template's shape is fixed: no
new template can be made, a template cannot become an SHP, and tiles and
extra blocks cannot be added, removed or moved. The owner approved the
three open TMP items.

## What Changes

- **New TMP from Image**: File > New TMP from Image and Save As TMP of an
  image make a new template from a picture through an options dialog.
- **TMP to SHP**: Save As SHP of a template converts its pixels into a new
  SHP document (one frame per tile or one frame), saying that tile headers
  and z-data are not carried over; the template is unchanged.
- **Reshaping in the Tiles panel**: move a tile to another cell, add or
  remove a tile, add or remove a tile's extra block, each one undoable step;
  Save keeps every untouched tile byte for byte by patching the original
  file and appending what is new.

## Capabilities

### New Capabilities

None.

### Modified Capabilities

- `codec/westwood-tmp`: templates made from images; templates converted
  to SHP; tiles and extra blocks moved, added and removed; the codec
  appends new blocks instead of laying the file out afresh.

## Impact

- `crates/pictura-shp/src/tmp_image.rs` and `tests/tmp_image.rs`; the
  writer in `src/tmp.rs` and the `KookaTMP` block (version 2, shapes) in
  `src/terrain.rs`, with their tests.
- The app glue `crates/pictura-app/src/cxxqt_object/shp/tmp_image.rs`,
  `tmp_shp.rs`, `tmp_edit.rs` and their tests; the shell's
  `tmp_authoring.{h,cpp}`, `panels/tmp_tiles_panel.{h,cpp}`, `shp_support_tmp.cpp`,
  the save gate in `shp_support.cpp` and `tests/tst_tmp.cpp`; the hooks
  listed in `crates/pictura-shp/HOOKS.md`.
