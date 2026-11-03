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
- `tmp` reads and writes TMP isometric terrain templates (the theater
  suffixes `.tem`, `.sno`, `.urb`, `.ubn`, `.des`, `.lun`, which theater
  SHPs share; the content decides): the diamond, its z-data, the extra block
  and its z-data. The writer patches the original file in place while every
  block keeps its size, so untouched tiles, uninitialised header bytes, gaps
  and tails stay byte for byte; a tile that gains a block is laid out afresh
  as Ochre's `build_tmp` does.
- `mix` reads MIX archives: the Tiberian Dawn / Red Alert 1 layout and the
  Red Alert / Tiberian Sun / Red Alert 2 layout with flags, the encrypted
  index (Blowfish, its key recovered from the archive's 80-byte key source
  with Westwood's public RSA key) and the SHA-1 checksum of the body; both
  ID hashes; nested archives; entry kinds sniffed from their first bytes;
  member paths (`ra2.mix/cache.mix/unittem.pal`). Reading streams: an index
  and the head of each entry, never a whole archive. `Plan` writes an
  archive to a new file with the index rebuilt (sorted by signed ID, never
  encrypted), a checksum recomputed when the source had one, and every
  untouched entry copied byte for byte; replacing a nested entry rebuilds
  only the archives on its chain. `names` holds the name lists:
  `data/mix-names.txt` (the stock archives' names, built by
  `tests/mix_names.rs`), local mix databases, and names harvested from game
  INI files, each used only when its ID and content kind agree.

## Provenance

The code is ported from two GPL-3.0-or-later projects by the owner of this
fork: qshp (`extras/qshp/src/shpcodec.{h,cpp}`: sniffer, decoder, palettes)
and Ochre's C&C addon (`addons/cnc/cncshp.py` and `cncpal.py`: the
byte-preserving writer; `cnctmp.py`: the TMP reader and writer). Each
source file names what it derives from.

The format itself is documented publicly:

- ModdingWiki, "Westwood SHP Format (TS)":
  <https://moddingwiki.shikadi.net/wiki/Westwood_SHP_Format_(TS)>
- ModEnc (modenc.renegadeprojects.com), the "PAL" article on palette files.
- XCC Utilities by Olaf van der Spek, the long-standing reference reader and
  writer; the "RLE-Zero only when smaller" rule follows it.
- MIX archives: ModdingWiki, "MIX Format (TD)" and "MIX Format (TS)"
  (layouts, ID hashes); XCC's `mix_decode.cpp` and OpenRA's
  `BlowfishKeyProvider.cs` describe the key recovery (Westwood's public
  key, the 40-byte blocks, the 56-byte key). The code here is written from
  those descriptions and from the published definitions of Blowfish
  (Schneier, 1993; its tables are the hexadecimal digits of pi, computed
  here, see `src/mix/blowfish_tables.rs`), SHA-1 (FIPS 180-4) and
  CRC-32 (IEEE 802.3); nothing is copied from those projects.

The real-file rules beyond those documents (flags with a size in their upper
bits, both raw flags in use, unaligned and aligned packing, garbage between
frames, tails, empty frames that point at the end of the data, trailing runs
declared one longer than the row) came from black-box measurement of the
owner's own files. No EA/Westwood asset, sprite or palette is included in
this crate, its tests or the repository: every fixture is generated in code.
The one thing taken from the games is `data/mix-names.txt`, a list of 10,111
file names (no file content): names read from the stock Red Alert 2 and
Yuri's Revenge archives' INI files and the stock executables' strings, each
kept only because its ID is an entry of a stock archive of the kind its
suffix says. Names from mods stay in a local list under
`$XDG_STATE_HOME/kooka-pictura/`, never committed.

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

The TMP gate works the same way over a list of theater files (SHPs among
them), against digests from Ochre's `cnctmp.py` (the digest is described in
`tests/tmp_corpus.rs`):

```bash
PICTURA_TMP_CORPUS_LIST=/path/list0 PICTURA_TMP_REFERENCE=/path/ref.tsv \
  cargo test -p pictura-shp --test tmp_corpus -- --ignored --nocapture
```

It checks that the sniffer agrees with the reference, that every decode
matches, that an unchanged save is byte-identical both in place and laid out
afresh, and that a one-pixel edit changes exactly one byte, inside that
tile's diamond.

The MIX corpus gate reads every archive of a NUL-separated list (with
nesting), checks the indexes, the entry bounds and the checksums, and
decodes and re-encodes every SHP and TMP entry; the name-list builder
writes `data/mix-names.txt` from the stock archives and the local list from
the rest (see `SHP-PLAN.md` section 7 for the recipe):

```bash
PICTURA_MIX_CORPUS_LIST=/path/mixlist0 \
  cargo test -p pictura-shp --release --test mix_corpus -- --ignored --nocapture
```
