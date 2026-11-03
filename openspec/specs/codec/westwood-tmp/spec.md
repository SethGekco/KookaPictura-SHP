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
