# Design

See `SHP-PLAN.md` section 5 (Phase 4) for the plan and section 7 for every
deviation recorded while building.

- **In-place writer.** Ochre's `build_tmp` lays records out afresh and keeps
  each tile's 52 header bytes. Measured on the owner's 2,745 real templates,
  that layout already reproduces every file; the port goes one step further
  and writes edited blocks over the original bytes whenever every block keeps
  its size (always, unless a tile gains z-data), so files from other tools
  with other layouts keep their bytes too. The fresh layout remains for a
  tile that gains a block.
- **Sniffing.** TMP has no magic. The sniffer accepts the two known tile
  sizes, 1-256 tiles a side, an offset table that fits, no offset inside the
  table, and at least one whole tile. An SHP's first two bytes are zero, so
  its `blocks_x` would be a multiple of 65,536: the two sniffers can never
  both accept a file (0 of 4,789 theater files did).
