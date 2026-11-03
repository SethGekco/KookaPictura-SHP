# Test Plan

One row per scenario in the spec delta. Coverage names an automated test, or
`manual:` with the reason it cannot be automated.

| Scenario | Steps | Expected result | Coverage |
|---|---|---|---|
| `tmp_codec_roundtrip` | Build 48x24 and 60x30 templates with and without extra z-data, parse, rebuild | Tiles equal, rebuilds identical | `pictura-shp::a_built_template_parses_back_in_both_tile_sizes`, `pictura-shp::diamond_packing_is_exact_and_fills_outside`, `pictura-shp::data_offsets_are_relative_to_the_record` |
| `tmp_codec_preserve` | Edit one pixel, the fields, z and extra of a file with garbage | Only those bytes differ | `pictura-shp::an_in_place_save_keeps_garbage_gaps_and_the_tail`, `pictura-shp::a_one_pixel_edit_changes_one_byte`, `pictura-shp::edited_fields_change_only_their_bytes`, `pictura-shp::edited_z_and_extra_are_written_in_place` |
| `tmp_codec_rebuild` | Give a tile without z some z-data | Rebuilt, flag set, raw bytes kept; a damaged slot refuses the rebuild | `pictura-shp::a_tile_that_gains_z_data_is_laid_out_afresh`, `pictura-shp::a_damaged_slot_is_kept_in_place_and_refused_by_a_rebuild` |
| `tmp_codec_sniff` | Sniff an SHP header, a wrong tile size, an offset in the table, no tiles | All refused | `pictura-shp::sniff_refuses_what_is_not_a_template` |
| `tmp_codec_corpus` | `PICTURA_TMP_CORPUS_LIST`, `PICTURA_TMP_REFERENCE`, ignored test | 2,745 of 2,745 decode as Ochre, identical, one-byte edits | `pictura-shp::every_real_template_decodes_and_saves_unchanged` |
