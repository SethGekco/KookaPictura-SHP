# westwood-tmp Specification

## Purpose
Westwood TMP isometric terrain templates (Tiberian Sun / Red Alert 2): the codec and its documents (KookaPictura-SHP fork).

## Requirements

### Requirement: TMP terrain codec

The system SHALL recognise a Westwood TMP terrain template by its content,
independent of its suffix, and SHALL decode its tiles (diamond, z-data,
extra block and extra z-data, with data offsets relative to the tile record).
Saving a template whose blocks kept their sizes SHALL write each block over
the original bytes, so a tile the edit did not touch keeps every byte.

#### Scenario: A template round-trips [tmp_codec_roundtrip]

- **WHEN** a generated template with empty slots, z-data and an extra block is built, parsed and built again
- **THEN** every tile decodes as built and the second build is byte-identical

#### Scenario: Untouched data survives an edit [tmp_codec_preserve]

- **WHEN** one pixel of one tile, or one tile's height, ramp or radar color, changes in a file with garbage header fields, gaps and a tail
- **THEN** only the bytes of that pixel or those fields differ

#### Scenario: A tile that gains z-data is laid out afresh [tmp_codec_rebuild]

- **WHEN** a tile without z-data gets some
- **THEN** the file is rebuilt with that tile's z flag and data, and every tile's unused header bytes are kept

#### Scenario: The sniffer refuses non-templates [tmp_codec_sniff]

- **WHEN** the bytes are an SHP, have an unknown tile size, an offset inside the table, or no whole tile
- **THEN** the sniffer refuses them

#### Scenario: Real templates [tmp_codec_corpus]

- **WHEN** the owner's real theater files are read
- **THEN** every TMP decodes as an independent reader does, re-encodes byte-identical, and a one-pixel edit changes one byte

### Requirement: TMP terrain documents

The system SHALL open a TMP template as a document whose tiles sit at their
template positions on a "Tiles" layer, whose extra blocks sit on "Extras"
layers (one more layer wherever extra blocks overlap), and whose z-data sits
on hidden "Z Tiles" and "Z Extras" layers as greys (depth v as grey 8v, "no
z" transparent), in the terrain palette found for it. The SHP dock SHALL show
a Tiles panel listing every tile and the canvas SHALL outline every tile.

#### Scenario: A template opens as tile, extra and z layers [tmp_open_layers]

- **WHEN** a generated template with z-data and overlapping extra blocks opens
- **THEN** the canvas holds every tile and extra, the layers are Tiles, Extras, Extras 2 and the hidden Z layers, and reading the layers back gives the template unchanged

#### Scenario: The Tiles panel lists the tiles [tmp_open_panel]

- **WHEN** a template opens
- **THEN** the dock shows the Tiles panel with each tile's cell, position, height, land and ramp type, extra block and z-data, the canvas outlines each diamond and extra block, a row click highlights its tile, and hovering names the tile and palette index

#### Scenario: A template opens in its theater palette [tmp_open_palette]

- **WHEN** `clear01.tem` opens with `isotem.pal` beside it
- **THEN** that palette is chosen, its kind is terrain, and Change Palette recolors the color layers by index

#### Scenario: Theater suffixes open by content [tmp_open_sniff]

- **WHEN** a TMP and an SHP both carry a theater suffix
- **THEN** each opens as what its content is, and the Open dialog lists the theater suffixes

#### Scenario: Reshaping is blocked [tmp_open_blocked]

- **WHEN** a template is the active document
- **THEN** Image Size, Canvas Size, Crop and rotations are refused

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

### Requirement: Editing TMP tile fields

The system SHALL let the user change a tile's height, land type, ramp type
and radar colors in the Tiles panel, each change one undoable History step
that Save writes into that tile's header and nowhere else, and SHALL refuse
a value that is not 0-255 or not two colors.

#### Scenario: A field edit is saved and undone [tmp_fields_edit]

- **WHEN** a tile's height, land type and radar colors are changed in the panel, the template is saved, and the changes are undone
- **THEN** the saved file differs only in that tile's header field bytes, and undo restores the fields and the panel

#### Scenario: A bad field value is refused [tmp_fields_refused]

- **WHEN** a height of 300 or a radar color "red" is typed
- **THEN** nothing is recorded and the row shows the old value

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
