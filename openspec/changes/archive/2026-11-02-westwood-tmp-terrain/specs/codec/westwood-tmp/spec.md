## ADDED Requirements

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

- **WHEN** a layer is renamed, a template is saved as `.shp`, or an SHP sheet as `.tmp`
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
