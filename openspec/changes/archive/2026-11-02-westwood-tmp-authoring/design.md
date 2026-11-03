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
