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

## MODIFIED Requirements

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
