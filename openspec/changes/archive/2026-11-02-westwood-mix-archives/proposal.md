# Proposal

## Why

The KookaPictura-SHP fork edits Westwood SHP sprites and TMP terrain
templates, but the games keep almost all of them inside MIX archives
(`ra2.mix`, `ra2md.mix`, `expand*.mix` and the archives nested in them).
Until now a sprite had to be extracted with another tool first, and the
palettes the games draw with (`unittem.pal`, `isotem.pal`, ...) were found
only as loose files. SHP-PLAN.md section 7 asks for MIX archives as their own
change.

## What Changes

- **A MIX codec** in `crates/pictura-shp` (`src/mix/`): the Tiberian Dawn /
  Red Alert 1 layout (no flags) and the Red Alert / Tiberian Sun / Red Alert 2
  layout with flags, including the encrypted index (the Blowfish key
  recovered from the 80-byte key source with Westwood's public RSA key) and
  the SHA-1 checksum of the body; both ID hashes (rotate-add, CRC-32);
  nested archives; streaming reads; a writer that rebuilds the index
  (unencrypted), recomputes the checksum and copies untouched entries byte
  for byte.
- **Entry names** only from provable sources: a list shipped with the codec
  (built from the stock game archives' INI files and executables, each name
  kept only when its ID is a stock entry of the kind its suffix says), the
  local mix databases and INI files inside the archives opened, and a local
  list built from the user's own files (never shipped). Unnamed entries show
  by ID with a kind sniffed from their content.
- **A MIX browser** dock: File > Open of a `.mix` lists it (nested archives
  as a tree; name or ID, kind, size; a filter box). An SHP or TMP entry opens
  as a document titled "gi.shp in ra2.mix"; entries export to a folder.
  Save on such a document asks: Save As a loose file (the default) or Replace
  in a Copy of the MIX; writing over the archive itself needs a confirmation
  that names it.
- **Palettes from MIX archives**: SHP and TMP palette resolution also
  searches the archive the document came from, the archives open in the
  browser and a Preferences > MIX Archives list (default empty).

## Capabilities

### New Capabilities

- `codec/westwood-mix`: reading and writing MIX archives, their names, and
  their use in the shell.

### Modified Capabilities

None.

## Impact

- `crates/pictura-shp/src/mix/` (`mod.rs`, `write.rs`, `names.rs`, `kind.rs`,
  `ids.rs`, `key.rs`, `blowfish.rs`, `blowfish_tables.rs`, `sha1.rs`),
  `data/mix-names.txt`, the tests `tests/mix.rs` and the ignored
  `tests/mix_names.rs` (the name-list builder) and `tests/mix_corpus.rs`.
- The app glue `crates/pictura-app/src/cxxqt_object/shp/mix.rs` (with
  `mix_tests.rs`), the shell's `mix_support.{h,cpp}`,
  `panels/mix_browser_panel.{h,cpp}` and `tests/tst_mix.cpp`, and the
  upstream hooks listed in `crates/pictura-shp/HOOKS.md`.
