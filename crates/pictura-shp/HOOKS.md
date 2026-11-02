# Where the SHP fork touches upstream files

Everything SHP-specific lives in new files. These are the only lines added to
files Kooka Pictura upstream also changes; check them first after
`git merge upstream/master`.

| Upstream file | Hook |
|---|---|
| `Cargo.toml` | `crates/pictura-shp` in `[workspace] members`; its `[profile.test.package.pictura-shp]` opt-level |
| `Cargo.lock` | the `pictura-shp` path package |
| `NOTICE.md` | the ported-code paragraph for `crates/pictura-shp` |
| `crates/pictura-app/Cargo.toml` | the `pictura-shp` path dependency |
| `crates/pictura-app/build.rs` | `src/cxxqt_object/shp.rs` in the bridge file list |
| `crates/pictura-app/src/cxxqt_object.rs` | `mod shp;` |
| `crates/pictura-app/src/cxxqt_object/state.rs` | the `shp_open_palette` field and its `None` default |
| `crates/pictura-app/src/cxxqt_object/open_jobs/imp.rs` | `decode_image_with_options`: `shp::decode_shp` right after the file is read, before `decode_import` (so it wins over an installed Qt SHP plugin and skips `finalize_import`); `open_image_start`: takes `shp_open_palette` and runs the decode inside `shp::with_open_palette` |
| `crates/pictura-app/src/cxxqt_object/impl_core.rs` | `revert`: the decode runs inside `shp::with_document_palette`, so a reverted SHP keeps its palette |
| `crates/pictura-app/src/cxxqt_object/impl_core/save.rs` | `save`: an `shp` suffix goes to `shp::save_shp` before the native fallback |
| `CMakeLists.txt` | the new shell sources (`shp_palettes`, `shp_support`, `image_view_shp`, `panels/shp_{frames,palette}_panel`) |
| `crates/pictura-app/cpp/tests/CMakeLists.txt` | `tst_shp` in the suite list |
| `crates/pictura-app/cpp/frame_build.cpp` | `registerPanel(ShpSupport::install(this, colorState_)->dock(), Qt::BottomDockWidgetArea)` and its include |
| `crates/pictura-app/cpp/frame_session.cpp` | `retargetDock` calls `ShpSupport::setView`, `refreshDock` calls `ShpSupport::refresh`; one include |
| `crates/pictura-app/cpp/frame_open.cpp` | `startOpenImagePath`: when `shp_sniff_file(path)`, `shp_set_open_palette(*view, resolveShpPalette(path))` before `open_image_start`; two includes |
| `crates/pictura-app/cpp/keyboard_shortcuts_dialog.cpp` | `,` and `.` (frame stepping) added to `workspaceKeys()`, so the shortcut editor never hands them to a command |
| `crates/pictura-app/cpp/image_view.h` | the `ShpGrid` struct, `setShpGrid`/`shpGrid`, `paintShpGrid` and the `shpGrid_` member |
| `crates/pictura-app/cpp/image_view.cpp` | `paintEvent` calls `paintShpGrid(painter)` before the guides |

Everything else for SHP documents lives in new files:
`crates/pictura-app/src/cxxqt_object/shp.rs` (bridge declarations) and
`shp/{imp,tests}.rs`, `crates/pictura-shp/src/{sheet,resource}.rs`, and in
the shell `cpp/shp_palettes.{h,cpp}` (which `.pal` an SHP opens with),
`cpp/shp_support.{h,cpp}` (the dock, `,`/`.` stepping, the save gate),
`cpp/panels/shp_{frames,palette}_panel.{h,cpp}`, `cpp/image_view_shp.cpp`
(the frame grid) and `cpp/tests/tst_shp.cpp`.

The generic save guard (any suffix without a writer is refused instead of
receiving PSD bytes) is kept self-contained so upstream can take it alone:

| Upstream file | Hook |
|---|---|
| `crates/pictura-app/src/cxxqt_object/impl_core/save_jobs/imp.rs` | `save_start_rust` writes only `psd`/`psb` (or suffix-less) paths; test `a_foreign_suffix_never_gets_psd_bytes` |
| `crates/pictura-app/src/cxxqt_object/impl_core.rs`, `impl_core/save.rs` | doc comments only |
| `crates/pictura-app/cpp/dialogs.{h,cpp}` | `isWritableSavePath()`; `savePathNeedsFormatDialog()` returns true for an unwritable path |
| `crates/pictura-app/cpp/frame.cpp` | `saveActiveAs()` refuses an unwritable path |
| `crates/pictura-app/cpp/frame_menus.cpp` | `saveAsWithDialog()` warns and stops on an unwritable suffix |
| `crates/pictura-app/cpp/control_server_documents.cpp` | `save`/`save_as` answer `invalid_param` for an unwritable suffix |
| `crates/pictura-app/cpp/tests/tst_open_save.cpp` | `unknownSuffixNeverGetsPsdBytes` |
