<!-- Fork planning document: KookaPictura-SHP. Not part of upstream Kooka Pictura. -->

# Native Westwood SHP editing for the Kooka Pictura fork: implementation plan

Paths are relative to `/home/rex/Claude/KookaPictura-SHP` unless they are absolute. Every claim about Kooka below comes from reading the tree at `91575671`.

---

## 0. Findings that shape the plan

1. **A data-loss bug exists today, and it is not where the brief puts it.**
   - `isNativeDocumentPath()` (`crates/pictura-app/cpp/frame_open.cpp:578`) returns true only for `psd`/`psb`.
   - On **open**, an unknown suffix goes to `startOpenImagePath`, which is the Qt raster import.
   - The PSD default is on **save**:
     - `format_for_path` (`crates/pictura-app/src/cxxqt_object/impl_core.rs:23`) returns `"shp"`.
     - `raster_writer_for_suffix` (`:33`) returns `None` for it.
     - `save()` (`impl_core/save.rs:16`) then calls `save_start_rust`, which writes **PSD bytes** (`save_jobs/imp.rs:55-72`).
   - `savePathNeedsFormatDialog` (`dialogs.cpp:289`) only shows the dialog for a layered document. A sheet opened through the qshp plugin is one pixel layer, so `documentIsFlat` (`dialogs.cpp:48`) is true.
   - Result: **Ctrl+S on an `.shp` opened with qshp silently replaces it with a PSD.** Phase 0 fixes this first. (Confirmed 2026-11-01 against the released 0.3.0 `.deb`: Save As `saved.shp` wrote a file starting `8BPS`.)
2. **Kooka's "Indexed" mode is never a working mode.**
   - `read.rs` `normalize()` turns an Indexed file into `ColorMode::Rgb` and keeps the palette only in `doc.source_palette`.
   - `pictura_render::convert_to_indexed` (`crates/pictura-render/src/document_ops/mode/flat.rs:51`) stores working RGB plus retained index planes (`SourceChannels`).
   - `write.rs` (~l.406/578) writes Indexed only while every layer still expands to the same RGB: "an edit already chose the RGB mode".
   - So upstream already uses the pattern "RGB working document, retained indices, verify on save". The plan builds on that pattern.
3. **The engine assumes RGBA channels everywhere.**
   - `Layer.channels` uses ids 0/1/2/-1 (`crates/pictura-core/src/lib.rs:1015`).
   - There are about 780 `.channels` accesses: render 409, app 184, paint 103, filters 59, ops 15, select 9.
   - History has fixed plane kinds: `PlaneKind::{Composite, LayerChannel, Mask, DocumentChannel}` (`crates/pictura-app/src/history/planes.rs:35`).
   - The GPU compositor is a separate path again.
4. **Layer visibility is history-tracked document data.**
   - `set_layer_visible` calls `edit("Layer Visibility")` (`impl_layers.rs:85`), and `solo_set` records "Solo Visibility" (`layer_target_bridge/imp.rs:69`).
   - History cannot be bypassed safely. `History::capture_live_with` (`history.rs:241`) diffs the live document against its own `current` copy, limited to the edit's region `hint` (`Edit::Regions`, `cxxqt_object/edit.rs`).
   - If a composite change is made outside an edit (for example "show frame 7") and a paint stroke is then committed elsewhere, those composite tiles are never captured, and undo restores a stale composite.
5. **No animation or layer-comp machinery exists to reuse.** Layer comps are open (ROADMAP #265). Video and the timeline are out of scope (ROADMAP l.588).
6. **What can be reused:**
   - The Pencil is an aliased tip with binary coverage (`pictura-paint/src/tip.rs`, test `pencil_is_binary`), and the Eraser has a Pencil mode.
   - The eyedropper samples a point by scope (`eyedropper_sample.rs`).
   - `SwatchGrid` (`cpp/panels/swatches_panel.h`) is a reflowing swatch grid.
   - `Document.image_resources` is kept byte-for-byte, rides history as metadata, and is written back to PSD as-is.
7. **qshp's `shp::build()` is not byte-preserving.**
   - It re-crops, forces RLE-when-smaller, uses 8-byte alignment, and writes the radar colour as 0.
   - The byte-for-byte rules live only in the Python `cncshp.py`: `payload`, `pad`, `tail`, verbatim `flags`/`reserved`/`radar`, `_unchanged()`, and span helpers.
   - The port therefore needs **qshp's sniffer and decoder plus Python's preserving writer**.

---

## 1. Document model: how to keep exact palette indices

### Options compared

| | (a) RGB + unique palette + snap on save | (b) True indexed mode | (c) Hybrid (recommended) |
|---|---|---|---|
| What it is | qshp approach: RGBA document; nearest colour on save | Index plane + palette in core; index-aware tools | RGBA document whose colours are a **one-to-one map** of the palette (`uniqueColours`); palette lives inside the document; index recovery is exact; off-palette pixels are **reported, never silently snapped**; untouched frames copied byte-for-byte |
| Exact indices for untouched pixels | Yes (unique colours) | Yes | Yes |
| Painting a new pixel | Snapped silently on save | Exact by construction | Exact with Pencil + palette click; anything else is flagged |
| Filters, blur, AA, transforms | Snapped silently | Must be disabled or reimplemented | Work; results are gated |
| Change to Kooka | Small | New working mode across compositor (CPU+GPU), ~780 channel sites, history `PlaneKind`, paint, select, eyedropper, PSD writer | Small; no core change |
| Upstream-merge cost | Low | Very high, permanent | Low |
| Effort | Days | Months | 1.5–3 weeks for the MVP |

**Recommendation: (c).**
- (b) fights finding 3. It would touch the parts of Kooka that change most often upstream.
- (a) loses silently: an antialiased edge becomes some palette index, possibly in the 16–31 remap ramp.

How (c) keeps indices exact:
- **On open,** each index `i` becomes the unique RGB `U[i]` at alpha 255. Index 0 becomes alpha 0.
- **On save,** a pixel with alpha 0 is index 0. A pixel with alpha 255 is looked up in `U⁻¹`, a 256-entry exact `HashMap`. Every other pixel is **off-palette**.
- An opaque pixel equal to `U[0]` is also off-palette, because the format cannot store index 0 as an opaque pixel.

### Where the SHP state lives (undo-safe)

All SHP state goes in **one private PSD image resource** in `doc.image_resources`:
- ID 4848 (the public PSD spec sets aside 4000–4999 for plug-ins), `8BIM` signature, name "KookaSHP".
- Contents:
  - format version;
  - canvas W×H and frame count;
  - sheet layout (columns, gutter);
  - 256-entry source palette, its name/path and kind (`unit`/`iso`/`anim`/…);
  - the **original file bytes**.

Why the resource is the right place:
- It is document metadata, so undo/redo restores palette, layout and pixels **together**. A "Change Palette" or "Insert Frame" undo stays consistent.
- `Plane` is refcounted, so history shares the original bytes instead of copying them per state.
- It survives Save As PSD → reopen → Save As SHP (still byte-exact).
- The view state needs no new field for it.

The Rust crate reads and writes it with its own small 8BIM section parser/rebuilder that keeps other resources. The codec's `rebuild_resources` is `pub(crate)` (`pictura-codec/src/icc.rs:243`), and this avoids widening the codec API.

### Saving, frame by frame (Python `build_shp` rules)

1. **Whole file unchanged** (same frame count and canvas, every frame's indices equal to the original decode): write the original bytes verbatim. This is trivially byte-identical for 100% of files.
2. **Frame `k` unchanged:**
   - Match by position first, then by content hash of the cropped index plane, so reorders keep payloads.
   - Reuse the original payload, `flags`, `reserved` and `radar`.
   - Reuse the original pad bytes when the offset lines up.
   - Keep sharing when original frames shared an offset (some writers do this; Python duplicates such payloads, so that is not covered by Python's guarantee).
3. **Changed frame that was in the file:** re-crop. Keep the old rect only if the content still fits (`_meta_rect` rule). Keep the original compression family (2→prefixed, RLE bit→RLE, else raw with the original flag word).
4. **New frame:** RLE-Zero only when smaller (XCC rule), align to 8.
5. **Tail:** keep the original tail.

### Rules for painting

- **Exact painting** means: Pencil or Eraser in Pencil mode, opacity/flow 100%, Normal blend mode, colour set from the Palette panel.
- **Fill** needs anti-alias off.
- **Transforms** need nearest-neighbour interpolation.
- **Everything else** works, but its results go through the **palette gate** at save:
  - A dialog shows "N off-palette pixels in frames 3, 7, 12".
  - Choices: [Snap to nearest and save] / [Show me] / [Cancel].
  - "Snap" is a recorded edit that uses qshp `nearest()` with the remap ramp searched last and alpha <128 → index 0. What gets saved is what is on screen.
- Later phases add a live off-palette overlay and a "Snap Frame to Palette" command.

**Risks to verify in tests:**
- (i) The display uses the GPU compositor (`store_opened_composite(doc, gpu)`). **Save must read pixels through the CPU path** (`current_buffer(doc, false)` in `helpers_composite.rs`). Add a test that a single Normal 100% layer composites to exactly its own bytes.
- (ii) A foreground colour set from the palette panel must reach the layer unchanged. The document has no ICC profile, so the working space is sRGB and this should be an identity; prove it with a test.
- (iii) Do not call `finalize_import` (`tests.rs:1116`). It turns an all-opaque import into a locked Background with no alpha, and then index 0 could not be painted.

---

## 2. Frames

### Options compared

- **Frames as layers** (Ochre's model):
  - Every frame step is a recorded visibility edit, which floods history and marks the document dirty (finding 4).
  - Doing it out-of-band corrupts undo (finding 4).
  - Hundreds of rows in the layers panel. Infantry SHPs commonly run to hundreds of frames.
  - Per-frame identity would be nice, but the content-hash matching above makes it unnecessary.
- **Layer comps:** do not exist (finding 5).
- **Separate document per frame:** breaks the shared canvas, cross-frame copy, and the single save.
- **Sheet in one document (recommended):**
  - Frame `i` sits in cell `(i % cols, i / cols)` of one pixel layer.
  - Cell size is canvas W×H, with a configurable gutter (default 1 px, transparent).
  - `cols` = 8 when N is a multiple of 8 (one facing set per row; qshp `autoColumns`). Increase it when needed to stay under the 30000-px side budget (`decode_image.cpp` `kMaxDimension`).
  - Shadow frames (second half) fall into the lower half of rows by construction.

**Why the sheet:**
- **No change to core, history or compositor.** Every tool, selection, filter, copy/paste between frames, and undo just works.
- Every frame is visible at once.
- Frame navigation is **view-only C++ state**, so it never touches the document.

### "One frame at a time" feel, all view-only

- **Frames panel** (thumbnail strip):
  - Clicking frame `k` zooms/scrolls `ImageView` to the cell.
  - It optionally sets a marquee selection to the cell, so strokes cannot bleed. A selection change is a normal, cheap history entry.
  - `,` and `.` step frames.
  - A "Focus" toggle clips `ImageView` painting to the active cell and dims the rest.
- **Grid lines and frame numbers:** an `ImageView` overlay.
- **Onion skin** (later phase): the overlay draws cell `k-1`/`k+1` pixels shifted into cell `k` at about 30%. It is not part of the composite, so a sampled colour stays exact.
- **Playback preview panel:** loops cells from the CPU composite, read-only, with a shadow option.
- **Gutter check at save:** non-transparent gutter pixels mean bleed. Report them and drop on confirm.
- **Geometry guard:** Image Size, Canvas Size, Crop and Rotate break the layout. Save refuses when the document size ≠ the layout size; later phases disable those commands for SHP documents.

### Undo across frames

- History is document-wide, as in Kooka and in most SHP editors.
- Later phases use the `Restored::Region` rectangle that undo/redo returns (`history.rs`) to auto-focus the frame that changed.
- Per-frame undo stacks would need one document per frame: rejected.

### Frame operations

Insert blank, duplicate, delete, move, and reverse range are recorded edits:
- They reflow cell pixels; a canvas-height change is a geometry edit (a history anchor).
- They rewrite the KookaSHP resource in the **same** `edit()` call.
- Restriction: they require exactly one pixel layer, otherwise they offer to flatten. Moving cells across a multi-layer stack is not worth it.

---

## 3. Palettes

- **Parsing** (port of `readPalette`, `crates/pictura-shp/src/palette.rs`):
  - Check for JASC-PAL first. Reading a JASC file as binary gives a palette of confident nonsense (cncpal.py note).
  - Otherwise 768-byte 6-bit VGA, expanded as `(v<<2)|(v>>4)`. Never `v<<2` alone, which turns 63 into 252.
  - Index 0 is transparent.
  - Also port `uniqueColours`, `nearest` (remap ramp searched last) and `fallbackPalette` (grey, magenta 0, red 16–31).
  - Writing: `v>>2` for VGA, plus JASC.
- **Picking the palette on open** (`crates/pictura-app/cpp/shp_palettes.cpp`, C++ because the settings are `QSettings`). Resolve before the worker starts, inside `startOpenImagePath` (the single entry point for menu, drag-drop, recent files and the control server, which all go through `startOpenDocumentAtPath`), and pass it with a new bridge call `shp_set_open_palette(view, path)`. This mirrors `set_open_policy_override`, which `open_image_start` already `take()`s. Order:
  1. A remembered folder choice: `Shp/FolderPalette/<canonical dir>`.
  2. A stem match among `.pal` files in `Shp/PaletteFolders`, recursive, longest name first (Python `_pick_palette`).
  3. `Shp/DefaultPalette`.
  4. The built-in grey fallback.

  A wrong guess costs nothing: indices are kept, and rebinding is one undoable edit.
- **Palette panel** (`cpp/panels/shp_palette_panel.{h,cpp}`):
  - A 16×16 `SwatchGrid` of `U[i]`.
  - Click → foreground; Alt-click → background. Set through an **exact RGB setter**, not HSB.
  - Hover shows index, original 8-bit RGB and role. Roles come from a small table ported from Ochre `data/palettes.ini`: 0 transparent, 1 shadow, 16–31 house-colour remap, 240–254 unlit (unit palettes); only 0 for `iso*`.
  - Shows the foreground's index (or "off-palette").
  - "Load Palette…" offers "use for this folder" (checked by default).
  - **Rebind** recolours by index, as a recorded "Change Palette" edit, and updates the resource in the same edit.
- **House-colour preview** (later phase): view-only, like onion skin.
  - The overlay recolours pixels whose index is 16–31, using a ramp built from a chosen colour and the brightness of the palette's own 16–31 entries.
  - **Matching the game's exact remap formula is an open question**, so it is labelled as a preview.
- **Do not ship game palettes.** They are EA/Westwood assets. The user points `Shp/PaletteFolders` at their own extracted files.

---

## 4. Open/save integration (small surface)

**New crate `crates/pictura-shp`:** pure Rust, std only, no Qt, no `pictura-*` dependencies, so it is reusable and its tests are fast.
- `sniff` (full-table validation, port of qshp), `parse` (lenient), `canvas`.
- `Source` (payload, pad, span, tail, shared offsets).
- `build_preserving` (Python rules plus whole-file verbatim).
- `palette.rs`.
- `sheet.rs` (layout, index↔RGBA via `U`, gutter check, off-palette scan, snap).
- `resource.rs` (KookaSHP 8BIM block).

**App glue:** `crates/pictura-app/src/cxxqt_object/shp.rs` + `shp/imp.rs` (a new bridge), turning sheet⇄`Document`. Hook points, each a few lines:

| File | Change |
|---|---|
| `Cargo.toml`, `crates/pictura-app/Cargo.toml` | Workspace member and dependency |
| `crates/pictura-app/build.rs`, `src/cxxqt_object.rs` | Register `shp.rs` bridge; `mod shp;` |
| `src/cxxqt_object/open_jobs/imp.rs` | In `decode_image_with_options` (l.260), after `read_capped`: if `pictura_shp::sniff`, return `shp::open(...)` **before** `decode_import`, so it wins over an installed qshp plugin. Uses `Document::from_rgba`, skips `finalize_import`/ICC, sets the resource, `source_format "shp"`. `open_image_start` (l.642) takes the palette override. `decode_for_revert` already goes through `decode_image`; Revert keeps the document's current palette. |
| `src/cxxqt_object/state.rs` | One field: `shp_open_palette: Option<String>` |
| `src/cxxqt_object/impl_core/save.rs` | `if suffix == "shp"`, call `shp::save` (sync; encoding is fast) before the PSD fallback |
| `src/cxxqt_object/impl_core/save_jobs/imp.rs` | `save_start_rust`: refuse any suffix that is not psd/psb (not just shp). This is the generic fix for finding 1. |
| `cpp/frame_open.cpp` | `saveRunsOnWorker` false for `shp`; palette resolution in `startOpenImagePath`; add `isShpPath()`. Leave `isNativeDocumentPath` alone (Place and drop routing use it to pick the PSD decoder). |
| `cpp/frame.cpp:556` | For an `.shp` path, skip `savePathNeedsFormatDialog` (SHP save flattens and gates by itself). Same in `cpp/control_server_documents.cpp:29`. |
| `cpp/dialogs.cpp` | `saveFileFilters` (l.246): insert `Westwood SHP (TS/RA2) (*.SHP)` **just before "All Formats"** (index 8; `pathForSaveFilter` derives All Formats as `size()-1`). `defaultSuffixForFilter` case 8 → `.shp`; `defaultSaveFilterIndex` "shp" → 8; `isKnownImageSuffix` += shp. `openFileFilters` (l.262): add `*.SHP` to All Formats plus a "Westwood SHP" entry (theater suffixes come with TMP in Phase 4). Update `tst_open_save.cpp:74` and `selftest_session.cpp:583` if they assume the count. |
| `cpp/frame_build.cpp:284-297` | `registerPanel` the Frames, Palette and (later) Preview panels |
| `cpp/image_view.{h,cpp}` (`paintEvent` l.544) | One call into `image_view_shp.cpp` for the overlays (grid, focus clip, onion, house colour, off-palette) |
| `CMakeLists.txt`, `cpp/tests/CMakeLists.txt` | List the new sources and the `tst_shp` suite (no globbing) |

Save As SHP for a document without a KookaSHP resource (a PNG sheet) is a later phase. It uses an options dialog (columns/frames/palette) in the style of `cpp/frame_save_options.cpp`. Until then it is refused with a message.

---

## 5. Phases

### Phase 0: Codec and the save hazard (2–4 days)

**Changes:**
- `crates/pictura-shp/{Cargo.toml,src/lib.rs,src/codec.rs,src/source.rs,src/palette.rs,README.md}`.
- The `save_start_rust` guard and the C++ suffix routing that stop PSD bytes going into foreign suffixes.

**Rust tests** (std `#[test]`, fixtures built in code, no binaries):
- RLE runs clamp at 255; the line length counts itself; a literal 0 is impossible.
- Compression 0/1/2/3, with flags carrying upper bits.
- Empty frames with a non-zero offset; unaligned offsets; garbage pad bytes; tail bytes; shared offsets.
- Truncated lines and over-long runs (lenient decode).
- `uniqueColours` is one-to-one, and `nearest` prefers non-remap indices on ties.
- VGA `63→255`; JASC parses; a JASC file is never read as binary.
- Deterministic-PRNG round trip: planes → build → parse.
- `pictura-app`: saving to `x.shp` / `x.foo` never emits `8BPS`.

**Corpus gate** (`#[ignore]`, `PICTURA_SHP_CORPUS=/path` plus `PICTURA_SHP_REFERENCE=manifest.tsv`):
- The manifest (file, frame, x,y,w,h,flags, md5(plane)) is produced once by qshp's codec, e.g. a frame-dump mode added to `/home/rex/Claude/Ochre/extras/qshp/tests/readcheck.cpp`.
- **Pass criteria:**
  - Sniff agrees with qshp on all 4,819 files.
  - Decode md5 matches frame-for-frame on all 4,810 valid files.
  - Unchanged save is byte-identical on 100% of files.
  - For each file, editing one pixel in frame k keeps every other frame's header and payload bytes identical. The output re-sniffs and decodes in qshp to the edited planes.
- Never commit the corpus, and do not reference `reference-fixtures/` from tracked code (`scripts/guard.sh` blocks it).

### Phase 1: MVP (1.5–3 weeks)

**Scope:**
- Open a multi-frame SHP as a sheet with the resolved palette.
- Frames panel: thumbnails, click to focus/select cell, step keys.
- Palette panel with exact foreground setting and Load/Rebind.
- Grid overlay.
- Save and Save As SHP with the palette gate, gutter check, geometry guard and verbatim-unchanged rule.

**New files:**
- `src/cxxqt_object/shp.rs`, `shp/imp.rs`.
- `pictura-shp/src/{sheet.rs,resource.rs}`.
- `cpp/shp_palettes.{h,cpp}`, `cpp/panels/shp_frames_panel.{h,cpp}`, `cpp/panels/shp_palette_panel.{h,cpp}`, `cpp/image_view_shp.cpp`.

**Tests:**
- **Rust (`pictura-app`):**
  - `decode_image_with_options` on a generated SHP gives the sheet size and resource; skipping `finalize_import` keeps alpha.
  - Unchanged save is byte-identical.
  - A Pencil dab of `U[i]` saves index i.
  - A soft-brush dab is reported off-palette.
  - Snap maps to non-remap indices on ties.
  - CPU composite == layer bytes.
  - Undoing "Change Palette" restores palette and pixels together.
  - Resource survives a PSD write/read.
- **Qt Test `cpp/tests/tst_shp.cpp`:**
  - `openDocumentAtPath` shows N thumbnails.
  - Frame click focuses the cell.
  - Swatch click sets the exact foreground.
  - Ctrl+S writes a sniffable SHP with no dialog.
  - Save As filter index 8 ↔ `.shp`.
  - Off-palette save shows the gate.
- **Not in the C++ self-test:** its budget only goes down (`scripts/check-selftest-budget.sh`).
- **Manual check:** the corpus gate rerun through the app path, plus opening the output in XCC Mixer or the game.

### Phase 2: Frames and authoring (2–3 weeks)

**Scope:**
- Frame ops: insert, duplicate, delete, move, reverse.
- File ▸ New SHP (canvas, frames, palette).
- Save As SHP from any sheet (options dialog).
- Onion skin (prev/next, opacity).
- Playback preview panel with FPS and range.
- Auto-focus the frame changed by undo.
- Live off-palette overlay and "Snap Frame to Palette".
- Disable Image Size/Canvas Size/Crop/Rotate/Mode for SHP documents.
- Hover index readout.

**Tests:**
- Rust: frame-op reflow and resource consistency under undo/redo; content-hash payload reuse after a move.
- Qt: onion overlay does not change `sample_argb_scoped`; playback steps; disabled commands.

### Phase 3: Unit-specific tools (1–2 weeks)

**Scope:**
- Shadow tools:
  - Detect the split: even N, and the second half uses only {0,1} (port of `SplitShadowsCommand`).
  - Validate that shadows are only 0/1.
  - Generate a shadow from art (silhouette → index 1, optional offset/skew).
  - Clear shadows.
  - Preview with shadow underlay.
- House-colour preview (approximate, labelled as such).
- Radar colour: kept by default; "Recompute radar" option (palette-average, Python `_average_colour`).
- Palette-kind presets: unit/iso/anim/cameo, with roles per kind.

**Tests:** Rust tests for the shadow validator and generator and the radar average; Qt test that the house-colour overlay does not change the document.

### Phase 4: TMP terrain (2–3 weeks; separate openspec change)

- Port `cnctmp.py`: 52-byte tile header, diamond rows, extra/z offsets relative to the tile record, 255 = no z.
- Layout: tiles at template pixel positions on layer 1; extras on layer 2; z-data in a **document channel** (`doc.channels`, editable in the Channels panel).
- The theater suffixes (`.tem/.sno/.urb/.ubn/.des/.lun`) are shared by SHP and TMP; the sniffer decides (1392 of 2108 measured were SHPs).

**Out of scope:** VXL/HVA; Tiberian Dawn/RA1 SHPs (the 9 refused files) unless asked for later.

---

## 6. Upstream-merge hygiene, licensing, process

- **Footprint:**
  - New code lives in new files (`crates/pictura-shp`, `shp*`, `image_view_shp.cpp`, `panels/shp_*`).
  - Upstream files get only the hook lines in §4.
  - List those hooks in `crates/pictura-shp/HOOKS.md` so a conflict after `git merge upstream/master` is easy to resolve.
  - Do not edit `docs/`: `scripts/guard.sh` requires a `TASK-ALLOWS-DOCS` marker.
- **Nothing is submitted upstream** (owner's decision): no pull requests,
  issues or posts. Generic fixes such as the "unknown suffix saves PSD"
  guard (finding 1) are kept as small, self-contained commits so Kooka's
  maintainers can take them from this public fork if they choose.
- **Keep the gates green:** `bash scripts/verify-fast.sh`:
  - fmt;
  - clippy `-D warnings`;
  - file-size cap of 1200 lines of code (`scripts/check-file-size.sh`), so split panels early;
  - colour-literal check (swatch colours are data, but use theme tokens for greys);
  - `check-milestone-names.py` (no "Phase/M1" in `crates/` code);
  - `cargo deny check` (no new dependencies planned);
  - conventional-commit messages (`commitlint.config.mjs`).
- **OpenSpec:** worth following, because the gates enforce it anyway (`openspec validate --all --strict`, `check-openspec-archived.sh` zero-open policy, `check-test-plan.py`).
  - Use one change per phase, e.g. `openspec/changes/shp-sprite-codec`, `shp-sheet-editing`.
  - Capabilities sit under existing domains (`codec/shp-sprites`, `ui/shp-frames-palette`).
  - Each change has a `test-plan.md` naming each test.
  - Archive in the same PR.
  - Specs are new files only, so they do not conflict with upstream.
- **Licensing and provenance:**
  - Kooka, Ochre and qshp are all GPL-3.0-or-later and qshp/Ochre are by the fork owner. Porting is fine.
  - Give the new files headers saying they are derived from Ochre `addons/cnc/cncshp.py`/`cncpal.py` and qshp `shpcodec.cpp`, and add a NOTICE.md line.
  - CONTRIBUTING's attestation is about Adobe material and does not touch SHP. Still, cite the public SHP sources in the crate README (ModdingWiki SHP(TS), ModEnc PAL, XCC) and say that the real-file rules come from black-box measurement of the user's own files.
  - The resource ID comes from the public PSD spec's plug-in range.
  - **No EA/Westwood assets** (palettes, SHPs) in the repo, fixtures or packages. Fixtures are generated in tests; `assets/PROVENANCE.md` needs no change unless an icon is added.

---

### Critical Files for Implementation
- /home/rex/Claude/KookaPictura-SHP/crates/pictura-app/src/cxxqt_object/open_jobs/imp.rs
- /home/rex/Claude/KookaPictura-SHP/crates/pictura-app/src/cxxqt_object/impl_core/save.rs
- /home/rex/Claude/KookaPictura-SHP/crates/pictura-app/cpp/dialogs.cpp
- /home/rex/Claude/KookaPictura-SHP/crates/pictura-app/cpp/frame_open.cpp
- /home/rex/Claude/Ochre/addons/cnc/cncshp.py (the byte-preserving writer to port, alongside /home/rex/Claude/Ochre/extras/qshp/src/shpcodec.cpp for sniff and decode)
---

## 7. Implementation notes (deviations found while building)

Phase 1, recorded as each slice landed:

- **The open palette reaches the worker through a scoped thread-local.**
  `open_image_start` takes `shp_open_palette` (the planned state field) and
  runs the existing decode inside `shp::with_open_palette`; the SHP hook in
  `decode_image_with_options` reads it. No upstream signature changed.
  Revert does the same with the document's current palette
  (`shp::with_document_palette`, one hook in `impl_core.rs::revert`), so a
  reverted SHP keeps its palette as §4 intended.
- **Snap, Clear Frame Gutters and Change Palette work on one plain
  full-canvas pixel layer** (Normal, 100%, no mask) and otherwise ask the
  user to flatten first, like the planned frame operations. Save itself reads
  the CPU composite, so a multi-layer sheet whose result is on-palette saves.
- **Save does not rewrite the resource's original bytes.** The writer pairs
  frames with the file the document was opened from; a second save of the
  same pixels produces the same bytes, and history never sees a metadata
  change that no edit recorded (finding 4).
- **Snap matches against the palette as read, then draws `U[i]`.** Matching
  against the unique colours would break the remap-ramp tie rule, because
  `unique_colours` nudges duplicates apart.
- **One bottom dock instead of two registered column panels.** The panel
  column's workspace presets (Essentials regrouping, session layout) would
  need new command ids and preset entries in several upstream files to host
  new panels. The Frames and Palette panels share one `QDockWidget` ("SHP
  Frames & Palette", registered through the existing `registerPanel`) that
  shows itself when an SHP document becomes active. The palette grid is a
  small fixed 16 x 16 widget rather than `SwatchGrid`, whose clicks delete
  and add swatches.
- **Palette choice order** gained a step: a `.pal` beside the SHP with the
  same stem comes right after the remembered folder choice. "Stem match"
  means the longest palette stem that starts the SHP's stem. The choices
  live in `$XDG_STATE_HOME/kooka-pictura/shp.ini` (QSettings INI) rather
  than in the session store, so no upstream session code changes.
- **The frame grid is drawn in the view frame** (one screen pixel at any
  zoom) around each cell, in the gutter, with the focused cell highlighted.
- **`,` and `.` are workspace keys.** The frame-stepping shortcuts are listed
  in `workspaceKeys()`, so Edit > Keyboard Shortcuts refuses to give them to
  a command (`tst_keyboard_shortcuts` holds every window shortcut to that).
- **The save gate runs inside `saveActiveAs`**, the one place every save of
  a `.shp` path passes (Ctrl+S, Save As, the control server), as a single
  hook into `ShpSupport::saveGate`. Without an interactive platform it never
  fixes anything silently: the save is refused with the reason. The control
  server always runs it without dialogs and answers `refused` with that
  reason (`io_error` with the writer's message if the write itself fails).
- **Saving a non-SHP document as `.shp` is refused, not converted.**
  `savePathNeedsFormatDialog` sends a plain Save of such a document to Save
  As, and the gate refuses the `.shp` path with the reason. Importing an
  arbitrary image as a new SHP stays out of Phase 1. (Superseded in Phase
  2: Save As SHP converts it, interactively; see "Save As SHP" below.)
- **An SHP whose sheet is over the import budget is refused on open**, from
  its header, before any frame is decoded. The budget is the one every
  other format meets (`ImageBudget`: one RGBA allocation of at most 2 GiB
  and three quarters of available memory, 30 000 px per side). Found by
  measuring the corpus: a 136-frame 2052x2816 `sppnuke.shp` lays out at
  28741x28169, 3.2 GB of RGBA before the composite and history copies (7
  of 4,819 files; the next largest sheet is 360 MB). Opening it would have
  exhausted the machine's memory. Phase 2's one-frame-at-a-time view is the
  way to edit such sprites. (Superseded in Phase 2: such sprites now open
  as a frame window, below.)

Phase 2, the frame window (paged frames):

- **Big sprites open as a window of frames.** The document holds the
  sheet of frames `first..first+span`; the Frames panel lists every frame
  of the file. Clicking a frame off the sheet, or stepping past the window
  edge with `,` / `.`, pages the window. Opening reads only the 8-byte
  header to plan the window and decodes only its frames (`frame_canvas`);
  a whole-file decode never happens.
- **The window budget is 256 MiB of sheet RGBA** (`WINDOW_BUDGET`), or
  `MemAvailable / FOOTPRINT_FACTOR` when less memory is free. Measured
  2026-11-02, peak RSS of the headless app: about 0.5 GB of base plus 9.6x
  the sheet at open (slope over 67 MB and 269 MB generated sheets), 12.5x
  after two fills, and 18x on `sppnuke.shp`'s 185 MB window after a fill
  and two saves (3.9 GB peak). `FOOTPRINT_FACTOR` is 20, rounded up from
  that worst case. Over the owner's 4,810 valid SHPs, 4,789 sheets fit
  256 MiB and open whole exactly as in Phase 1 (the largest whole sheet is
  211 MiB); 21 page: the 7 `sppnuke.shp` copies (3,088 MiB each) and 14
  explosion and `ambusher.shp` sheets of 257-343 MiB. A budget of 256 MiB
  keeps every open within about 2.6 GB on this 16 GB machine, which has
  frozen twice under memory pressure. The window holds as many frames as
  fit, in whole facing sets of 8 when the count is a multiple of 8, and at
  least one frame. `sppnuke.shp` (136 frames of 2052x2816) opens frames
  0-7 as a 16423x2816 sheet (185 MB), peak RSS 2.6 GB at open (3.9 GB
  after an edit and two saves) instead of the 3.2 GB sheet plus copies
  that Phase 1 refused.
- **The frame store lives in the KookaSHP resource, not beside the
  document** (a deviation from the brief's "outside the document"). The
  resource gained a version 2: the window (`first`, `total`, `span`) and,
  after the original file bytes, the edited frames that are off the sheet,
  as RGBA-free palette indices (each frame's full canvas in RLE-Zero
  lines). The pixel layers hold only the window. Keeping the store in the
  resource keeps undo, revert, duplicate and a PSD Save As consistent with
  no new state field and no external store that could go stale; the
  resource is re-encoded only when the window pages. A whole document still
  writes the version 1 block, byte for byte as before. Untouched frames
  save from the original bytes verbatim (`build_with` /
  `FramePlane::Unchanged`, no decode); a stored frame that matches its
  original again is dropped from the store.
- **Paging is the edge of undo history** (paged files only). A page change
  replaces the document with the new window and starts a fresh history
  whose first state is "Frames a-b of n"; edits made before it are kept in
  the frame store but can no longer be undone. Holding undo for frames that
  are no longer in memory would mean keeping their sheets, which is exactly
  the memory paging exists to save. The status bar says so on every page
  change ("Frames a-b of n loaded. Loading other frames starts a new undo
  history.") and the Frames panel's label tooltip repeats it. A modified
  document stays modified across a page change; a clean one stays clean.
- **Paging runs the save gate's clean check first.** Leaving a window with
  off-palette pixels or drawn gutters offers the same Snap / Clear choices
  as a save ("...and Continue"), or refuses with the reason when there is
  no interactive platform. So every frame in the store is exact palette
  indices with clean gutters, and the save gate covers off-sheet edits by
  construction: saving encodes window frames from the sheet, stored frames
  from the store, and the rest verbatim.
- **Off-sheet thumbnails are drawn from the store or the original bytes**,
  one frame at a time, nearest-neighbour, at most the thumbnail side,
  cached by a stamp (0 for the original, a hash of the stored data) so an
  edited frame's thumbnail refreshes after paging.
- **The open request reaches the worker as one value.** Phase 1's
  `shp_open_palette` state field became `shp_open` (an `OpenRequest`:
  palette and size-limit allowance), and `with_open_palette` became
  `with_open_request`.
- **The paging edit check on the real `sppnuke.shp` is a Rust test**
  (`window_tests::a_real_big_sprite_pages_an_edit_far_from_frame_0`,
  ignored, `SHP_BIG`/`SHP_BIG_OUT`), checked by the independent decoder in
  the e2e script. The control server cannot click the Frames panel: window-
  space pointer events and keys need the admin tier, which the CLI does
  not grant. The app e2e covers the window open, an unchanged save, an
  in-window one-pixel edit and peak RSS.

The size limit and its allowance (Preferences > Advanced):

- **The SHP size limit is the window budget, 256 MiB of sheet.** Opening a
  sprite whose whole sheet is over it is not refused: it opens as a window
  (with a status-bar note). What the limit gates is holding more than it at
  once: a whole over-limit sprite, a window of one frame that is itself
  over it, a bigger window ("Frames loaded" in the Frames panel), Image
  Size and Canvas Size, and (when they land) frame inserts, which must call
  `ShpSupport::allowSize` too. No real SHP frame is over the limit by
  itself (the format caps a side at 4096, 64 MiB of sheet), so in practice
  the open prompt is "Open Whole / Open as Window / Cancel" for sheets up
  to 10% over. Of the owner's 21 paged files, the 14 that are not
  `sppnuke.shp` are 0.4-34% over (3 of them within 10%); `sppnuke.shp` is about
  1,100% over and its whole-sheet estimate (60 GB) never passes the floor.
- **Two settings on a new Advanced page, never saved**: "Allow SHP files
  over the size limit (until Kooka closes)" and "Remove the 10% ceiling on
  that allowance (until Kooka closes)", enabled only while the first is
  on. They are members of `ShpSupport` (one per main window, one main
  window per process), so a restart always starts with both off.
- **Stages.** Within the limit nothing ever asks. Up to 10% over: refused
  (naming the setting) unless the first setting is on; then a prompt
  showing the image memory needed, the limit, the percent over, the
  estimated footprint and `MemAvailable` now, with an unticked "I
  understand this switches off a safety check for this file, and Kooka
  could run this computer out of memory." box; the go button stays
  disabled until it is ticked, and Cancel is the default. Past 10%:
  refused unless the second setting is on too; then the same prompt and a
  second, separate one with its own box. A view remembers the stage it
  confirmed and the bytes it was granted: paging windows of that size
  never asks again, a growth into a higher stage asks for that stage only
  (the second prompt again past 10%).
- **The floor no setting removes**: the estimate (`needed x
  FOOTPRINT_FACTOR`, 20) must fit `MemAvailable` now, checked in the shell
  before asking and again in Rust (`Limits::admit`) when the window is
  built.
- **No dialogs without a person.** Offscreen, and inside any control
  request (`ShpSupport::Quiet` in `ControlServer::dispatch`), a step that
  needs confirming is refused with the reason; a control request answers
  `refused` with it. Nothing is ever confirmed automatically.
- **Tests inject the limit and the memory** (`shp_set_test_limits`): a
  6x5 frame is 120 bytes, so 110 bytes is 9% over and 100 bytes 20% over;
  nothing large is allocated.

Phase 2, authoring (recorded as each piece landed):

- **Frame operations** (insert blank, duplicate, delete, move earlier /
  later or by dragging a thumbnail, reverse the selected range) live in the
  Frames panel (buttons and the list's context menu). The plan's rule "they
  require exactly one pixel layer, otherwise offer to flatten" is kept as a
  refusal naming Layer > Flatten Image, like Snap and Change Palette.
- **Frame origins instead of content hashing alone.** The resource gained
  a version 3: after the frame store, every frame's origin (the original
  file's frame it shows, or `0xFFFF` for an added one). It is written only
  once the order differs from the identity, so untouched documents keep
  their version 1 or 2 block byte for byte. Save pairs each frame with its
  origin (`build_mapped`): a moved frame keeps its payload, flags, radar and
  rectangle; an edited moved frame keeps its own compression family. The
  plan's content hash is there too, as the fallback: a frame whose pixels
  equal some source frame's decode exactly (moved by copy and paste, say)
  is written as that frame. Origins are needed anyway for paged sprites,
  whose untouched off-sheet frames are never decoded.
- **A frame keeps its original offset only at its own position.** The
  writer's "place data at the original offset while it is still ahead"
  rule assumed file order; for a moved frame it would leave holes of zeros,
  so moved frames are aligned to 8. A duplicate gets its own copy of the
  bytes; only frames that shared a data block in the source keep sharing.
- **Whole sheets: one recorded edit.** The sheet is reflowed cell by cell
  (pixels copied exactly, off-palette work included, so no snap is
  needed first) and the resource is rewritten in the same edit; a
  frame-count change that changes the sheet size is a geometry edit
  (`resize_canvas_document` top-left, then the layer is replaced). Pixels
  in the gutters would be dropped by the reflow, so they refuse the
  operation (the shell offers the recorded Clear first).
- **Paged sprites: an operation is a history edge, like paging.** It is
  applied to the whole frame list (window frames from the sheet, stored
  frames from the store, everything else by origin, nothing decoded that
  is not shown), then the window that holds the focused frame is rebuilt
  and History starts again, with the status-bar notice. Recording it would
  keep a copy of the resource, which holds the whole original file
  (`sppnuke.shp`: 203 MB), in every undo state. The window must pass the
  paging gate first (on the palette, clean gutters), because its frames may
  move into the frame store.
- **Inserts and duplicates go through the size check.** The resulting sheet
  (or window) is measured like "Frames loaded": over the limit it asks or
  refuses by the same stages and the same allowance; a whole sheet that
  would outgrow the limit is not turned into a paged one.
- **File > New SHP** (frame width and height, frame count, palette: a
  `.pal` path with Browse, empty for the built-in grey; the default palette
  setting fills it in) makes a whole sheet of blank frames with no source
  file, so every frame saves fresh (cropped, RLE-Zero only when smaller,
  data aligned to 8). It goes through the size limit like an open. The
  entry is inserted into the File menu right after New by `ShpSupport`
  (it is not a command-table entry), so no upstream menu code changed.
- **Save As SHP of an image that is not an SHP sheet converts it into a
  new document** rather than changing the image. The options dialog takes
  the frame size or columns x rows (kept consistent with the image), a gap
  between cells, the frame count (cells read row by row), the palette
  (pre-filled by the same resolution an open of the target path would use)
  and how other colours map: nearest palette index with alpha under 128 as
  index 0 (exactly Snap to Palette), or exact colours only, leaving the rest
  to the save gate. The flattened CPU composite is cut into a new SHP
  document (a new tab, size-checked), which is then saved through the
  normal gate; the original image stays open, unchanged, and is not saved
  (`saveActiveAs` returns false for it). Without dialogs (offscreen, the
  control server) it is still refused with the reason, as in Phase 1.
  Items 2 and 3 of the scope landed in one commit: they share the new
  dialogs file and the "new sheet with no source" document path.

- **Onion skin** (Frames panel: "Onion skin" and its opacity, default 35%)
  is drawn by `ImageView::paintShpOnion` over the composite, in the view
  only: the frames before and after the current one are tinted (red
  before, blue after), ghosted at the opacity, and cut out wherever the
  current frame is opaque, so they read as behind it and its own pixels
  show as they are. The document, the eyedropper's samples and the saved
  bytes are untouched (tested). **At the edge of a paged window the
  neighbour off the sheet is simply not shown** (decoding it from the
  store on every paint would put frame-store reads in the paint path for a
  ghost); frame 0 and the last frame do not wrap either. Cells over 1
  megapixel are not ghosted, so a paint never copies megabytes.
- The SHP Qt tests are split: `tst_shp` (documents, saving, limits, frame
  operations, authoring) and `tst_shp_view` (onion skin, playback,
  overlays, readouts, disabled commands), sharing the generated sprites in
  `cpp/tests/shp_fixtures.h`, to stay under the 1,400-line test-file cap.
