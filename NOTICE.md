# Trademark and Attribution Notice

Kooka Pictura is an independent project. It is not affiliated with,
endorsed, sponsored, or approved by Adobe Inc.

**Adobe**, **Photoshop**, **Camera Raw**, **Adobe Camera Raw**, and
**Lightroom** are trademarks or registered trademarks of Adobe Inc. in the
United States and other countries.

Kooka Pictura uses Adobe marks only nominatively: to identify the Adobe Photoshop
CS6 behavior it reimplements, and to name the documented on-disk PSD/PSB
identifiers required for compatibility with Photoshop. Those identifiers are
reproduced byte-for-byte as required by the file format; examples include the
`Adobe Camera Raw Filter` descriptor class ID and the camera-raw smart-filter id
`2683` that Photoshop writes. Our own camera-raw feature is presented to users
under the descriptive name **Pictura Raw**.

The project ships no Adobe source code, binaries, fonts, ICC profiles, or
creative assets. For the full trademark posture, independent-creation method, and asset
policy, see
[`docs/00-overview/licensing-and-provenance.md`](docs/00-overview/licensing-and-provenance.md).

## Licensing

Kooka Pictura is free software, licensed under the **GNU General Public
License v3.0 or later** (see [`LICENSE`](LICENSE)). It links the Qt 6
framework under **LGPL-3.0-or-later** and a set of permissively licensed
Rust crates. The resolved dependency licenses are listed in
[`THIRD-PARTY-LICENSES`](THIRD-PARTY-LICENSES); the required texts are under
[`LICENSES/`](LICENSES/).

## Icon assets

The application icons under `assets/icons/` are derived from the **Lucide**
icon set (<https://lucide.dev>), vendored from `lucide-static` 1.50.0, or are
independently authored in the same 24×24 line style. Lucide is licensed under
the **ISC License**, with a subset derived from the **Feather** project under
the **MIT License**; the full text is in
[`LICENSES/Lucide.txt`](LICENSES/Lucide.txt). Per-icon provenance (Lucide slug
or custom) is recorded in
[`docs/dev/icon-provenance.md`](docs/dev/icon-provenance.md) and
`assets/icons/lucide-map.json`.

## Ported code

Parts of the engine are ported from **PhotoCraft**
(<https://github.com/storytold/photocraft>), Copyright (c) 2026 ArtCraft Team
and the PhotoCraft contributors, licensed under the MIT License or the Apache
License 2.0 at the recipient's option; Kooka Pictura takes it under the
**MIT License**. The ported code is `Filter ▸ Liquify` (the displacement
mesh, brush-dab model and resampling in
`crates/pictura-filters/src/distort/liquify.rs` and its submodules),
`Filter ▸ Artistic ▸ Palette Knife`
(`crates/pictura-filters/src/photorust/artistic/knife_glow_grain.rs`), and
`Edit ▸ Transform ▸ Warp` (the warp styles and Bezier mesh in
`crates/pictura-core/src/warp.rs` and `warp/`, the mesh resampler in
`crates/pictura-render/src/document_ops/layer_ops/warp.rs` and
`warp_raster.rs`, and the Warp-mode session in
`crates/pictura-app/src/cxxqt_object/impl_transform/warp.rs`). The
full license text is in [`LICENSES/photocraft.txt`](LICENSES/photocraft.txt).

The SHP support in `crates/pictura-shp` (a fork addition) is ported from
**qshp** (`src/shpcodec.{h,cpp}`) and the **Ochre** C&C addon
(`addons/cnc/cncshp.py`, `cncpal.py`, and `cnctmp.py` for TMP terrain),
Copyright (C) 2026 Ochre
contributors, licensed under the **GNU General Public License v3 or later**,
the same license as Kooka Pictura. Both are by the owner of this fork. No
EA/Westwood game assets are included. The MIX archive reader and writer
(`crates/pictura-shp/src/mix/`) is new code written from public format
descriptions (see that crate's README); its name list
(`crates/pictura-shp/data/mix-names.txt`) holds only file names, read from
the stock games' own archives.
