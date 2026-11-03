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
- **TMP documents** (later commits of this change): tiles at their template
  pixel positions on a Tiles layer, extras on Extras layers, z-data on
  hidden Z layers; a Tiles panel in the SHP dock; the save gate; editable
  height, land, ramp and radar colors.

## Capabilities

### New Capabilities

- `codec/westwood-tmp`: reading and byte-preserving writing of TMP terrain
  templates, and their documents.

### Modified Capabilities

None.

## Impact

- `crates/pictura-shp/src/tmp.rs` and its tests (`tests/tmp.rs`, the
  ignored corpus gate `tests/tmp_corpus.rs`).
- The app and shell files the later commits list in `crates/pictura-shp/HOOKS.md`.
