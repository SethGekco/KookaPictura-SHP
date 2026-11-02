# pictura-shp

Westwood SHP sprites (Tiberian Sun / Red Alert 2) and `.pal` palettes for the
Kooka Pictura SHP fork: sniffing, lenient decoding, and a writer that keeps
every byte of a file the edit did not touch. Standard library only: no Qt, no
other `pictura-*` crate, no third-party dependency.

- `sniff` validates the whole frame table (the format's only magic is a
  leading zero word); `parse` decodes compressions 0/1 (raw), 2 (raw lines
  with a length prefix) and 3 (RLE-Zero), leniently.
- `Source` keeps, per frame, the stored data, the gap before it, the flag
  word, the radar colour and the reserved word, plus the bytes after the
  last frame.
- `build_preserving` writes the original file verbatim when nothing
  changed, and otherwise rebuilds frame by frame: untouched frames keep
  their bytes, edited frames keep their compression family, new frames use
  RLE-Zero only when it is smaller.
- `palette` reads 768-byte 6-bit VGA and JASC-PAL palettes and maps colours
  to indices; `role` names what the game does with an index.
- `sheet` lays the frames out on one RGBA sheet (index `i` drawn as the
  unique colour `U[i]`, index 0 transparent) and maps a sheet back to
  indices exactly, reporting off-palette and gutter pixels instead of
  guessing; `snap`, `clear_gutter` and `rebind` are the explicit fixes.
- `resource` is the private `KookaSHP` PSD image resource (id 4848, in the
  plug-in range) holding the layout, the palette as read and the original
  file bytes, with a small `8BIM` section reader/rewriter that keeps every
  other block.

## Provenance

The code is ported from two GPL-3.0-or-later projects by the owner of this
fork: qshp (`extras/qshp/src/shpcodec.{h,cpp}`: sniffer, decoder, palettes)
and Ochre's C&C addon (`addons/cnc/cncshp.py` and `cncpal.py`: the
byte-preserving writer). Each source file names what it derives from.

The format itself is documented publicly:

- ModdingWiki, "Westwood SHP Format (TS)":
  <https://moddingwiki.shikadi.net/wiki/Westwood_SHP_Format_(TS)>
- ModEnc (modenc.renegadeprojects.com), the "PAL" article on palette files.
- XCC Utilities by Olaf van der Spek, the long-standing reference reader and
  writer; the "RLE-Zero only when smaller" rule follows it.

The real-file rules beyond those documents (flags with a size in their upper
bits, both raw flags in use, unaligned and aligned packing, garbage between
frames, tails, empty frames that point at the end of the data, trailing runs
declared one longer than the row) came from black-box measurement of the
owner's own files. No EA/Westwood asset, sprite or palette is included in
this crate, its tests or the repository: every fixture is generated in code.

## Tests

```bash
cargo test -p pictura-shp
```

The corpus gate runs against a private collection of real files and a
manifest of reference decode hashes; it reads the corpus and writes nothing:

```bash
PICTURA_SHP_CORPUS_LIST=/path/list0 PICTURA_SHP_REFERENCE=/path/hashes.tsv \
  cargo test -p pictura-shp --test corpus -- --ignored --nocapture
```

`list0` holds NUL-separated paths. Each manifest line is
`py<TAB>frames<TAB>md5<TAB>path`, where the md5 covers every frame's full
canvas (index 0 outside the frame's rectangle), or `py-not-shp<TAB>…<TAB>path`
/ `py-error<TAB>…<TAB>path` for a file the reference refused. The gate checks
that the sniffer agrees, that every decode hash matches, that an unchanged
save is byte-identical, and that a one-pixel edit in one frame leaves every
other frame's header fields and data bytes identical and decodes back to the
edited planes.
