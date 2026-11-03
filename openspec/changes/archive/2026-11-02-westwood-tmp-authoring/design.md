# Design

See `SHP-PLAN.md` section 7 ("TMP authoring") for every decision and the
measurements behind it.

- **Cell positions from the corpus.** A tile of cell `(col, row)` sits at
  `((col - row) * cx / 2, (col + row) * cy / 2)` when all tiles share a
  height, as every real template measured does horizontally.
- **Built, then opened.** A template made from an image is written with
  the fresh layout and opened from those bytes, so it is a template read
  from a file in every respect.
- **Outside pixels are the user's call.** They become extra blocks only on
  request (nearest tile); otherwise the save gate's Clip applies.
- **TMP to SHP copies pixels only.** The SHP keeps the template's palette,
  so indices survive; tile headers and z-data have no place in an SHP, and
  the dialog and the status say so.
- **Reshaping patches the original file.** Moved tiles keep their records
  (header position and offset-table slot rewritten), removed tiles leave
  their records unreferenced, and new blocks and records are appended, so
  every untouched tile keeps every byte; the fresh layout is used only when
  the grid differs or the patch would not read back as the template, and
  the save status says so.
- **Shapes live in the resource.** The `KookaTMP` block gains a version 2
  with each tile's source slot, position and extra block, so undo, redo and
  a PSD round trip carry the reshaping with the pixels; an unreshaped
  document keeps writing version 1.
- **Moves follow the grid.** A cell step is half a tile right and down per
  column and half a tile left and down per row, the extra block moving
  with its tile; a new tile is placed from the first tile with its height.
