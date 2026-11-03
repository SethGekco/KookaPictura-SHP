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
