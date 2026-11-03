# Proposal

## Why

The KookaPictura-SHP fork edits Westwood SHP sprites (SHP-PLAN.md phases
0-3). Tiberian Sun and Red Alert 2 terrain is stored in a second format, TMP
isometric templates, under the same theater suffixes (`.tem`, `.sno`, `.urb`,
`.ubn`, `.des`, `.lun`) as theater SHPs. SHP-PLAN.md phase 4 asks for TMP
editing as its own change.

## What Changes

- **A TMP codec** in `crates/pictura-shp` (`tmp.rs`): sniff, lenient decode,
  and a writer that patches the original file in place, so an untouched tile
  keeps every byte (uninitialised header fields, gaps and tails included).
- **TMP documents**: tiles at their template pixel positions on a Tiles
  layer, extras on Extras layers, z-data on hidden Z layers (Kooka's
  document channels cannot be painted); the `iso<theater>.pal` palette; a
  Tiles panel in the SHP dock with tile outlines on the canvas; Save in
  place behind a gate (Snap / Clip / Show Me / Cancel, refused headless);
  editable height, land, ramp and radar colors.

## Capabilities

### New Capabilities

- `codec/westwood-tmp`: reading and byte-preserving writing of TMP terrain
  templates, and their documents.

### Modified Capabilities

None.

## Impact

- `crates/pictura-shp/src/tmp.rs` and its tests (`tests/tmp.rs`, the
  ignored corpus gate `tests/tmp_corpus.rs`).
- `crates/pictura-shp/src/terrain.rs` (placement, layers, read-back, the
  `KookaTMP` resource) and `tests/terrain.rs`.
- The app glue `crates/pictura-app/src/cxxqt_object/shp/tmp.rs`, the shell's
  `panels/tmp_tiles_panel`, `shp_support_tmp.cpp` and `tests/tst_tmp.cpp`,
  and the upstream hooks listed in `crates/pictura-shp/HOOKS.md`.
