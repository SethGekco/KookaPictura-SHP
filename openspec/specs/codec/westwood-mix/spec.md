# westwood-mix Specification

## Purpose
Westwood MIX archives (Tiberian Dawn to Red Alert 2): the codec, its name lists, and their use in the shell (KookaPictura-SHP fork).

## Requirements

### Requirement: MIX archive codec

The system SHALL read Westwood MIX archives in the Tiberian Dawn / Red Alert
1 layout (no flags word) and in the Red Alert / Tiberian Sun / Red Alert 2
layout (a flags word), including an index encrypted with Blowfish under a key
recovered from the archive's key source with Westwood's public RSA key, and
SHALL verify a SHA-1 checksum of the body when the flags announce one. It
SHALL compute entry IDs with the rotate-add hash of the older games and the
CRC-32 hash of the newer ones, read archives nested inside archives, and
treat an entry outside its archive as broken rather than reading past it.

#### Scenario: Both layouts read [mix_codec_layouts]

- **WHEN** a generated archive in the old layout, and one in the new layout with and without a checksum, is read
- **THEN** every entry's ID, offset and size come back as written, and the checksum verifies

#### Scenario: An encrypted index reads [mix_codec_encrypted]

- **WHEN** an archive whose index is encrypted under a test RSA key pair is read with that public key
- **THEN** the Blowfish key recovered from its key source decrypts the index, and the key recovery matches an independent implementation

#### Scenario: Primitives match published values [mix_codec_primitives]

- **WHEN** Blowfish, SHA-1, CRC-32 and both ID hashes run on published test vectors and known name/ID pairs
- **THEN** each gives the published value

#### Scenario: Nested archives and member paths [mix_codec_nested]

- **WHEN** an archive holds archives holding sprites, templates and palettes
- **THEN** a scan lists every level with each entry's kind told from its content, and a member path such as `game.mix/cache.mix/unittem.pal` reads the entry

#### Scenario: Damaged archives are refused safely [mix_codec_damaged]

- **WHEN** an entry lies outside its archive, or a tiny sprite's first bytes look like a flags word
- **THEN** the entry is reported broken and refuses a rewrite, and the sprite is not taken for an archive

#### Scenario: Real archives [mix_codec_corpus]

- **WHEN** the owner's real game and mod archives are read
- **THEN** every archive parses, every entry lies inside its archive or is reported broken, nested archives parse, and every SHP and TMP entry decodes and re-encodes byte-identical

### Requirement: MIX archive writing

The system SHALL write an archive to a new file with its index rebuilt
(sorted by ID as a signed number, never encrypted), every untouched entry
copied byte for byte, and a SHA-1 checksum recomputed when the source had
one. Replacing an entry inside nested archives SHALL rebuild only the
archives that hold it.

#### Scenario: A rewrite round-trips [mix_write_roundtrip]

- **WHEN** an unencrypted archive is read and written unchanged, and an encrypted one is read and written
- **THEN** the first is byte-identical, and the second reads back unencrypted with the same entries

#### Scenario: Replacing a nested entry [mix_write_replace]

- **WHEN** an entry two archives deep is replaced
- **THEN** the new archive holds the new bytes there and every other entry unchanged

### Requirement: MIX entry names

The system SHALL name an entry only from a provable source: the list shipped
with the codec (built from the stock game archives only), the local mix
databases and game INI files inside the archives read, and a local list
built from the user's own files that is never shipped. A name SHALL be used
only when its ID is an entry of the archive and the entry's content is the
kind its suffix says. An unnamed entry SHALL show by its ID with a kind
sniffed from its content.

#### Scenario: Names come from databases, INI files and lists [mix_names_sources]

- **WHEN** an archive holds a local mix database and an art INI naming images, and a name list is loaded
- **THEN** those names apply to their IDs, a name whose suffix does not fit the entry's content is refused, and an entry no source names keeps its ID

#### Scenario: The shipped list is built from stock archives [mix_names_build]

- **WHEN** the name-list builder runs over the stock game archives and the owner's corpus
- **THEN** the shipped list holds only names proven against stock archives, and every other proven name goes to the local list

### Requirement: MIX archives in the shell

The system SHALL open a `.mix` from File > Open in a MIX browser that lists
the archive and its nested archives as a tree with each entry's name or ID,
kind, size and ID, narrowed by a filter. An SHP or TMP entry SHALL open as a
document titled "<entry> in <archive>". Saving such a document SHALL ask
whether to save a loose file (the default) or replace the entry in a copy of
the archive; writing over the archive itself SHALL need a confirmation that
names the file, and without dialogs the save SHALL be refused with the
reason. Selected entries SHALL export to a folder without overwriting files.

#### Scenario: The browser lists an archive [mix_shell_browser]

- **WHEN** a generated archive with named, unnamed and nested entries is opened and filtered
- **THEN** no document opens, the browser shows every entry with its kind and ID, the filter narrows the rows by name, ID or kind, and a damaged file reports why

#### Scenario: An entry opens as a document [mix_shell_open]

- **WHEN** an SHP, an unnamed SHP and a TMP entry are opened from the browser, and a palette entry is asked to open
- **THEN** the three open titled "gi.shp in game.mix", "12345678.shp in game.mix" and "clear01.tem in game.mix", and the palette is refused with the reason

#### Scenario: Saving asks where [mix_shell_save]

- **WHEN** such a document is saved, choosing Cancel, the loose file and Replace in a Copy
- **THEN** the prompt names the archive and defaults to the loose file; the loose file is written and the title drops the archive; the copy holds the entry and the title names the copy; the source archive never changes

#### Scenario: Writing over the archive asks first [mix_shell_in_place]

- **WHEN** the copy path is the archive itself, with and without dialogs
- **THEN** a confirmation names the archive's path and defaults to Cancel, Cancel leaves the file unchanged, Replace writes it through a partial file, and without dialogs it is refused

#### Scenario: Entries export [mix_shell_export]

- **WHEN** entries and a nested archive are exported twice to a folder
- **THEN** each is written byte for byte, an unnamed one under its ID, and the second export does not overwrite the first

### Requirement: Palettes from MIX archives

The system SHALL look for a sprite's or template's palette, when no loose
palette fits, inside the archive the document came from, the archives open
in the browser and the archives listed in Preferences > MIX Archives (a
folder standing for the archives in it, in the games' override order), and
SHALL load a palette from such an archive.

#### Scenario: A document from an archive finds its palette there [mix_palette_from_archive]

- **WHEN** an SHP and a TMP entry open from an archive holding `cache.mix/unittem.pal` and `isotem.pal`
- **THEN** the sprite opens with `unittem.pal` and the template with `isotem.pal` from inside the archive

#### Scenario: Loose files find palettes in listed archives [mix_palette_preferences]

- **WHEN** a loose sprite and template with no palette near them resolve their palettes with a folder of archives in Preferences, after it is removed, and with the archive open in the browser
- **THEN** they find `unittem.pal` and `isotem.pal` inside the archive, nothing once it is removed, and the sprite's again while the archive is open
