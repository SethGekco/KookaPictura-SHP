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
- **Z-data on hidden layers.** The plan put z-data in a document channel;
  Kooka's channels are nameless, positional and cannot be the paint
  target, so z-data is on hidden "Z Tiles" / "Z Extras" pixel layers as
  greys (depth v as 8v, "no z" transparent, an invalid stored value as
  (255, 0, v)), every byte reading back.
- **Layers by name at save.** Save reads the layers by their names and
  refuses anything it would have to guess about (a missing, renamed or
  extra layer, a group, mask or adjustment, a resized canvas).
- **Fields as metadata.** Height, land, ramp and radar colors live in the
  resource's field table; editing one is a metadata History step and Save
  writes it into the tile header.
