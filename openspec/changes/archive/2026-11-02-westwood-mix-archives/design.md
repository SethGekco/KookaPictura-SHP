# Design

See `SHP-PLAN.md` section 7 (MIX archives) for the measurements behind each
decision.

- **Where.** A `mix` module of `crates/pictura-shp` rather than a new crate:
  it shares the SHP and TMP sniffers for entry kinds and needs no new
  dependency. Blowfish, SHA-1, CRC-32 and the RSA arithmetic (a small
  big-integer `pow_mod`) are written here from their published definitions;
  no code is vendored. The algorithms are cited in the module comments
  (XCC's `mix_decode.cpp`, OpenRA's `BlowfishKeyProvider`, the ModdingWiki
  MIX pages).
- **Encrypted index.** The 80-byte key source is two 40-byte blocks, each a
  little-endian integer raised to Westwood's public exponent modulo its
  public modulus; the first 56 of the 78 output bytes are the Blowfish key.
  Tests use a key pair of their own (and a Python known-answer vector), since
  Westwood's private key is not public; the real key is proven by the stock
  archives, whose decrypted indexes account for every byte and whose SHA-1
  checksums match.
- **Writer.** Untouched entries are copied byte for byte in their original
  body order; the index is sorted by ID as a signed number (the order every
  stock archive uses, and the games binary-search); the header is written
  unencrypted (encrypting needs the private key; the games read both); a
  checksum is recomputed when the source had one. Replacing a nested entry
  rebuilds only the archives on its chain. Output always goes to a new file
  (`<out>.kooka-partial`, synced, then renamed); the shell asks before `out`
  is the source archive.
- **Streaming.** A listing reads the index and the first bytes of each entry
  in offset order through one buffered reader (nested archives the same
  way); no entry is read whole except INI files and local mix databases
  (8 MB cap) and the entry being extracted. Listings run on a worker thread.
- **Names.** Only names that are proven: an ID match in the archive and a
  kind that fits the suffix (`.shp` must be an SHP, `.pal` a palette, and so
  on). The shipped list comes from the stock game archives only; names from
  mods go to `$XDG_STATE_HOME/kooka-pictura/mix-names.txt`, which is never
  committed.
- **Documents from archives.** An opened entry is extracted to a temporary
  folder and opened like a loose file; a registry maps that file to its
  archive and ID chain, which the title, Save and palette resolution read.
