## ADDED Requirements

### Requirement: TMP templates from images

The system SHALL make a new TMP terrain template from any image: the image
laid over a template grid of the chosen game's tile size (48x24 or 60x30),
a tile in each chosen cell at the position real templates use, every tile
with the chosen height, land and ramp type, the image's colours mapped to
the chosen palette (nearest, or exact with the rest left for the save
gate), no z-data or flat-ground z-data, and pixels outside the diamonds
made extra blocks only when asked. Saving an image as a TMP SHALL ask for
these options and save the new template, leaving the image unchanged.

#### Scenario: An image becomes tiles [tmp_image_codec]

- **WHEN** an image is laid over a grid with some cells chosen
- **THEN** each chosen cell holds a tile with the image's pixels inside its diamond, and the built file reads back as made

#### Scenario: A template document from an image [tmp_image_document]

- **WHEN** File > New TMP from Image runs on an image
- **THEN** a new template document opens whose unchanged save writes the template as built, with off-palette and stray pixels left for the save gate

#### Scenario: The options dialog and Save As TMP [tmp_image_shell]

- **WHEN** an image is saved as `.tem`, or File > New TMP from Image is chosen
- **THEN** the options dialog shows the cells with pixels checked, the new template is saved or opened, and without dialogs the save is refused with the reason

#### Scenario: Another reader agrees [tmp_image_ochre]

- **WHEN** templates made from images are read by an independent reader
- **THEN** every header, field and block reads as this codec reads it

### Requirement: TMP templates converted to SHP

The system SHALL convert a TMP template into a new SHP document when it is
saved as `.shp`, after asking whether each tile becomes a frame (every
frame the tile's rectangle at the same place, grown for the extra blocks)
or the whole template one frame, and whether the extra blocks are drawn
over the tiles. The SHP SHALL use the template's palette so every index
survives, the system SHALL say that tile headers and z-data are not carried
over, and the template SHALL be unchanged. Without dialogs the save SHALL be
refused with the reason.

#### Scenario: Frames of a template [tmp_to_shp_frames]

- **WHEN** a template is converted per tile, per tile with its extra blocks, or as one frame
- **THEN** each frame holds the tile's diamond (extras drawn over it), or the one frame the whole canvas, with the template's palette indices, and the template is unchanged

#### Scenario: Save As SHP of a template [tmp_to_shp_shell]

- **WHEN** an open template is saved as `.shp`
- **THEN** the dialog says what is not carried over, a new SHP document is made and saved, the template still saves byte-identical, and without dialogs the save is refused

### Requirement: Reshaping TMP templates in the Tiles panel

The system SHALL let the user, in the Tiles panel, move the selected tile to
a free cell of the template grid (its header position following the grid,
its extra block with it, its pixels redrawn there), add a blank tile in a
free cell, remove a tile other than the last, give a tile a new extra block
(transparent, without z-data) and remove a tile's extra block. Each SHALL be
one undoable History step, checked against the size limit when the canvas
grows; Save SHALL keep every untouched tile byte for byte and SHALL say in
the save status when blocks were appended or the file was laid out afresh.

#### Scenario: Reshaping a template document [tmp_reshape_document]

- **WHEN** tiles and extra blocks of a generated template are moved, added and removed, and undone
- **THEN** pixels follow their tiles, new tiles and extras are blank without z-data, outside pixels move with the canvas, refused edits change nothing, the file saves in place or appended, and undo restores the original bytes

#### Scenario: The Tiles panel buttons [tmp_reshape_panel]

- **WHEN** the panel's Move, Add, Remove, Add Extra and Remove Extra are used on an open template, with undo and redo
- **THEN** the buttons follow the selection, the dialogs offer a free cell and a tile-sized block above the tile, the template saves byte-identical after undo, and the status names appended blocks

#### Scenario: Another reader agrees [tmp_reshape_ochre]

- **WHEN** a copy of a real template has a tile moved and an extra block added, and is saved and reopened
- **THEN** it reopens as edited and Ochre's reader sees exactly those changes and nothing else

## MODIFIED Requirements

### Requirement: TMP terrain codec

The system SHALL recognise a Westwood TMP terrain template by its content,
independent of its suffix, and SHALL decode its tiles (diamond, z-data,
extra block and extra z-data, with data offsets relative to the tile record).
Saving a template SHALL patch the original file: a block that kept its size
is written over the original bytes, a block that is new or changed size and
a new tile's record are appended at the end of the file, a moved tile's
header position and offset-table slot are rewritten, and a removed tile's
slot is emptied, so a tile the edit did not touch keeps every byte. Only a
template whose grid differs from the original's, or whose patch would not
read back as the template, SHALL be laid out afresh.

#### Scenario: A template round-trips [tmp_codec_roundtrip]

- **WHEN** a generated template with empty slots, z-data and an extra block is built, parsed and built again
- **THEN** every tile decodes as built and the second build is byte-identical

#### Scenario: Untouched data survives an edit [tmp_codec_preserve]

- **WHEN** one pixel of one tile, or one tile's height, ramp or radar color, changes in a file with garbage header fields, gaps and a tail
- **THEN** only the bytes of that pixel or those fields differ

#### Scenario: New blocks are appended [tmp_codec_append]

- **WHEN** a tile gains z-data or an extra block, a z-less tile's extra block gains z-data, or a tile is added
- **THEN** the new data is appended, only that tile's header fields and slot change before it, an added extra block has no z-data offset, and extra z-data brings a blank z diamond

#### Scenario: Moved and removed tiles and extras [tmp_codec_reshape]

- **WHEN** a tile moves to an empty slot, a tile is removed, or an extra block is removed
- **THEN** only the slots and the header position, the slot, or the flag differ

#### Scenario: A new grid is laid out afresh [tmp_codec_rebuild]

- **WHEN** every edit is made at once, the grid is widened, or a slot points at damaged data
- **THEN** the edits read back as made, the wider grid is laid out afresh, and a damaged slot is kept in place but refused by a rebuild

#### Scenario: The sniffer refuses non-templates [tmp_codec_sniff]

- **WHEN** the bytes are an SHP, have an unknown tile size, an offset inside the table, or no whole tile
- **THEN** the sniffer refuses them

#### Scenario: Real templates [tmp_codec_corpus]

- **WHEN** the owner's real theater files are read
- **THEN** every TMP decodes as an independent reader does, re-encodes byte-identical, and a one-pixel edit changes one byte

### Requirement: Saving TMP terrain documents

The system SHALL save a TMP document to a TMP suffix (the theater suffixes
and `.tmp`) by reading its layers back into the template and writing every
block over the original file, so an untouched tile keeps every byte. Before
saving, the system SHALL check for colour pixels that are not palette
colours, Z pixels that are not depths and pixels outside every tile, offer a
recorded Snap or Clip, Show Me or Cancel, and SHALL refuse with the reason
when no dialog can be shown.

#### Scenario: An unchanged template saves byte-identical [tmp_save_unchanged]

- **WHEN** a template is opened and saved without edits
- **THEN** the file is byte-identical, under the same or another theater suffix

#### Scenario: A one-pixel edit changes one tile [tmp_save_edit]

- **WHEN** one pixel of one tile is painted with a palette colour and saved
- **THEN** exactly one byte, inside that tile's diamond, differs from the original

#### Scenario: Off-palette pixels are caught [tmp_save_gate_palette]

- **WHEN** a template with an off-palette pixel is saved
- **THEN** the gate offers Snap and Save, Show Me (selects the pixel, focuses its tile) and Cancel; Snap is one History step; headless the save is refused with the reason

#### Scenario: Pixels outside the tiles are caught [tmp_save_gate_outside]

- **WHEN** a template with a pixel outside every tile is saved
- **THEN** the gate offers Clip and Save; after clipping the file is the original; headless the save is refused with the reason

#### Scenario: Wrong layers or targets are refused [tmp_save_refused]

- **WHEN** a layer is renamed, or, where no dialog can be shown, a template is saved as `.shp`, an SHP sheet as `.tmp` or an image as a TMP
- **THEN** the save is refused with the reason, and an SHP sheet still saves under its theater suffix byte-identical
