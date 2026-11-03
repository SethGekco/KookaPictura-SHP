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

## Capabilities

### New Capabilities

None.

### Modified Capabilities

- `codec/westwood-tmp`: templates made from images; templates converted
  to SHP.

## Impact

- `crates/pictura-shp/src/tmp_image.rs` and `tests/tmp_image.rs`.
- The app glue `crates/pictura-app/src/cxxqt_object/shp/tmp_image.rs`,
  `tmp_shp.rs` and their tests; the shell's `tmp_authoring.{h,cpp}`, `shp_support_tmp.cpp`,
  the save gate in `shp_support.cpp` and `tests/tst_tmp.cpp`; the hooks
  listed in `crates/pictura-shp/HOOKS.md`.
