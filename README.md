# Kooka Pictura -- SHP edition

**This is a fork of [Kooka Pictura](https://github.com/KookaStudio/KookaPictura)
for Command & Conquer sprite work.** Its goal is to make Kooka Pictura a good
editor for Westwood **SHP** sprites -- Tiberian Sun, Red Alert 2 and Yuri's
Revenge first, the rest of C&C after -- with multiple frames, game palettes and
exact palette indices, so modders can open, edit and save unit, building and
effect art directly.

- It is **not** part of the Kooka Pictura project, and nothing here is
  submitted to it. Kooka Pictura is a photo editor and game formats are outside
  its scope; this fork exists so both can stay what they are.
- Everything here stays **GPL-3.0-or-later**, like Kooka Pictura. Its
  maintainers, and anyone else, are welcome to take whatever is useful.
- **Status: first usable version** (build from source; no release packages
  yet). Open a TS / RA2 / YR SHP from File > Open and it appears as a sheet of
  frames in its game palette. The SHP dock has a Frames panel (thumbnails,
  `,` / `.` to step) and a Palette panel (click a swatch for the exact palette
  colour, load or swap palettes, remembered per folder). Ctrl+S saves the SHP
  in place: frames you did not touch are written back byte for byte, and
  pixels that are not palette colours are caught before saving instead of
  being changed silently. Sprites too big to hold whole (such as a
  136-frame 2052x2816 superweapon) open as a window of frames: the Frames
  panel lists them all, and clicking or stepping to another frame loads it,
  keeping your edits (loading another window starts a new undo history).
  For a rare sprite that must be held whole anyway, Edit > Preferences >
  Advanced can lift the size limit until Kooka closes, with a confirmation
  each time and a hard stop at the memory actually available.
  The Frames panel inserts, duplicates, deletes, moves (buttons or drag)
  and reverses frames; a moved frame keeps its stored bytes on save.
  File > New SHP starts a sprite from blank frames, and Save As SHP turns
  any image or sprite sheet into a new SHP document (frame size or grid,
  palette, nearest-colour mapping). Onion skin ghosts the previous and next
  frames behind the current one (view only), and a playback preview plays
  any range of frames at a chosen speed, once or looped. Undo and Redo bring
  the frame they changed into view. An Off-palette toggle marks pixels that
  are not palette colours as you paint, and Snap Frame fixes one frame.
  Checked against 4,819 real SHPs: every valid one saves back
  byte-identical. Next: shadow tools,
  house-colour preview (see [`SHP-PLAN.md`](SHP-PLAN.md)).
- For the unmodified Kooka Pictura there is also a drop-in Qt plugin,
  [`qshp`](https://github.com/SethGekco/Ochre/tree/master/extras/qshp), that
  opens SHPs as sprite sheets (a small converter saves them back). Do not
  press Save on an SHP in stock Kooka Pictura 0.3.0: it writes a PSD under
  the .shp name. This fork fixes that.
- The SHP code comes from [Ochre](https://github.com/SethGekco/Ochre), a
  separate GPL editor whose C&C addon it was first written for.

The rest of this page is Kooka Pictura's own README, unchanged.

---

# Kooka Pictura

A layered photo editor for Linux, built to edit photos well and to read and
write Photoshop (PSD/PSB) files faithfully.

[![CI](https://github.com/KookaStudio/KookaPictura/actions/workflows/ci.yml/badge.svg)](https://github.com/KookaStudio/KookaPictura/actions/workflows/ci.yml)
[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](LICENSE)
![Platform: Linux](https://img.shields.io/badge/platform-linux-lightgrey)

<img src="assets/icons/app.png" alt="Kooka Pictura icon" width="128">

![Kooka Pictura](assets/screenshot01.jpg)

## What it is

Kooka Pictura is an independent, from-scratch image editor for Linux. The engine
is Rust; the interface is Qt 6. It is a working application, not a mock-up: it
opens and saves PSD/PSB, composites layers on the CPU with an optional GPU path,
and ships a large slice of the classic Photoshop tool, layer, adjustment, and
filter surface.

The goal is a photo editor that handles the editing workflow really well, not a
clone of every corner of Photoshop. For what is in and out of scope, see
[`ROADMAP.md`](ROADMAP.md).

## Getting it

Prebuilt artifacts are published on the GitHub Releases page: AppImage,
`.deb`/`.rpm`, snap, Flatpak, Windows portable archive and installer, and an
Apple Silicon DMG. The macOS DMG is **unsigned and not notarized**; on first
launch allow it in **System Settings → Privacy & Security → Open Anyway**. The
snap is **unsigned** too; install it with
`sudo snap install --dangerous ./kooka-pictura_<version>_amd64.snap`.

You can also build it from source. Requirements: Rust 1.98, Qt 6, CMake, Ninja,
`lld`, and the Little CMS 2 headers. C++ links use `mold` when it is installed
and the compiler accepts it.

```bash
cmake -S . -B build -G Ninja
cmake --build build --parallel
./build/pictura
```

Full prerequisites, test commands, and packaging notes are in
[`DEVELOPING.md`](DEVELOPING.md).

## Quick start

1. `File > Open…` a PSD/PSB file, or any PNG, JPEG, GIF, BMP, TIFF, WebP, PDF,
   or EPS file.
2. Edit with the toolbox and panels: paint, select, crop, add adjustment and
   fill layers, apply filters. Can't find a command? Press `Shift+Space`, type
   its name, and press Enter to run it or Alt+Enter to see where it lives.
3. `File > Save` writes PSD/PSB, preserving the file's color mode, bit depth,
   layers, effects, smart objects, metadata, and profiles.

## Features

- **PSD/PSB round-trip.** Reads and writes Gray, RGB, CMYK, Lab, Indexed, and
  Bitmap at 8/16/32-bit, with raw, RLE, and ZIP compression, preserving blocks
  the engine does not model. `crates/pictura-codec/src/`.
- **Layers.** Groups and nesting, raster and vector masks, clipping masks, 27
  blend modes, layer styles and effects, fill and adjustment layers, shape and
  type layers, layer locks, color labels, and layer filtering.
  `crates/pictura-core/src/lib.rs`.
- **Smart objects.** Embedded smart objects with a bundled, non-destructive
  editing flow and smart filters. `crates/pictura-render/src/smart_filter.rs`.
- **The classic tool surface.** Move; the marquee and lasso families,
  including the magnetic lasso; magic wand and quick selection; crop and
  perspective crop; the healing family (spot healing, healing brush, patch,
  content-aware move, red eye); the erasers; gradient and paint bucket; clone
  and pattern stamp; the history brushes; blur, sharpen, and smudge; dodge,
  burn, and sponge; the pen and path tools; the type tools and type masks; the
  shape tools; the slice tools; rotate view; and the color sampler, ruler,
  note, and count. `crates/pictura-app/cpp/tool_catalog.cpp`.
- **Transforms.** Free Transform, plus skew, distort, perspective, and warp.
  `crates/pictura-render/src/document_ops/layer_ops/transform.rs`.
- **Pictura Raw.** The built-in raw processor (the equivalent role to Adobe
  Camera Raw, which is an add-on in Photoshop), reached through
  `Filter > Pictura Raw…`, with nine tabs. It applies to an already-decoded
  image; opening a camera raw file is not implemented yet.
  `crates/pictura-app/cpp/pictura_raw_dialog.cpp`.
- **Adjustments.** The classic families, from Levels and Curves to Match
  Color, with live preview and adjustment-layer variants. Each one that still
  lacks a control names it in the roadmap. `crates/pictura-adjust/src/`.
- **Filters.** 110 filters across the classic families — blur, sharpen, noise,
  stylize, pixelate, distort, render, sketch, texture, artistic, and brush
  strokes — plus Liquify, Oil Paint, the Blur Gallery, and the Filter Gallery.
  Three CS6 families have no engine yet — Adaptive Wide Angle, Lens Correction,
  and Vanishing Point — and the roadmap names them.
  `crates/pictura-filters/src/`.
- **Selections.** Marquee, ellipse, lasso, magnetic lasso, magic wand, and
  quick selection, with Refine Edge, Color Range, the Modify set, save/load to
  channels, selection-masked edits, and content move.
  `crates/pictura-select/src/`.
- **Color management.** ICC profiles, Assign and Convert Profile, and
  configurable handling of an incoming embedded profile.
  `crates/pictura-color/src/`.
- **Metadata.** EXIF/IPTC/XMP reading and editing, File Info, and metadata
  templates. `crates/pictura-codec/src/`.
- **Export.** PSD, PSB, PNG, JPEG, TIFF, WebP, BMP, GIF, SVG, and Photoshop
  PDF, plus Save for Web with palette reduction, dithering, and transparency.
  `crates/pictura-app/cpp/export_as_dialog.cpp`.
- **Print.** A print dialog over the system print stack, with a "Save as PDF"
  option. `crates/pictura-app/cpp/print_dialog.cpp`.
- **Fast.** A Vulkan (wgpu) compute path accelerates compositing and the heavy
  filters, with the CPU as the reference.
- **Search everything.** `Shift+Space` (Help > Search) finds any command, tool,
  panel, Preferences page, or key panel control by name, shows where it lives
  (`Edit > Transform > Warp`), and runs or reveals it. Not a CS6 feature.
  `crates/pictura-app/cpp/search_bar.cpp`.
- **Headless and scriptable.** A non-interactive `--headless` mode and an
  optional local control server for automated editing.

## What it is not

Kooka Pictura focuses on photo editing. It does not aim for Photoshop's product
family features — 3D, video, DICOM measurement, print fidelity, cloud services,
or Adobe plug-in compatibility. [`ROADMAP.md`](ROADMAP.md) lists these
explicitly. Adobe, Photoshop, and Camera Raw are trademarks of Adobe Inc.; this
project is independent and ships no Adobe code or assets.

## A note on AI

AI and LLM coding agents are part of how Kooka Pictura is built. This is an
agentic-coding project: agents write and review code alongside human
contributors, and that use is disclosed rather than hidden. The approach follows
the [Software Freedom Conservancy's recommendations for LLM-backed generative
AI in FOSS](https://sfconservancy.org/llm-gen-ai/llm-backed-generative-ai-recommendations.html):

- Every contribution is human-reviewed. Manual testing of fixes and features is
  highly recommended before they land.
- Using AI tools is optional; contributors who do not are equally welcome.
- All code stays GPL-3.0-or-later, and the provenance rules in
  [`CONTRIBUTING.md`](CONTRIBUTING.md) apply however it was written.

The agent tooling and the required spec workflow are in
[`DEVELOPING.md`](DEVELOPING.md#ai-assisted-development).

## Get help

Open an issue at
[github.com/KookaStudio/KookaPictura/issues](https://github.com/KookaStudio/KookaPictura/issues).

## For developers

- [`DEVELOPING.md`](DEVELOPING.md) — onboarding, build, test, and architecture.
- [`CONTRIBUTING.md`](CONTRIBUTING.md) — provenance, asset, and dependency rules.
- [`ROADMAP.md`](ROADMAP.md) — shipped, planned, and not planned.
- [`docs/README.md`](docs/README.md) — how to read the specification corpus.
- [`docs/dev/doc-code-parity-gaps.md`](docs/dev/doc-code-parity-gaps.md) — what
  the corpus specifies but the code does not yet implement.
- [`docs/dev/STATE.md`](docs/dev/STATE.md) — the project resume anchor.

## License

Kooka Pictura is free software under the **GNU GPL v3.0 or later**
(see [`LICENSE`](LICENSE)). Third-party component licenses are listed in
[`THIRD-PARTY-LICENSES`](THIRD-PARTY-LICENSES) with texts under
[`LICENSES/`](LICENSES/).
