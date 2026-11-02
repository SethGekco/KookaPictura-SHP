# Where the SHP fork touches upstream files

Everything SHP-specific lives in new files. These are the only lines added to
files Kooka Pictura upstream also changes; check them first after
`git merge upstream/master`.

| Upstream file | Hook |
|---|---|
| `Cargo.toml` | `crates/pictura-shp` in `[workspace] members`; its `[profile.test.package.pictura-shp]` opt-level |
| `Cargo.lock` | the `pictura-shp` path package |
| `NOTICE.md` | the ported-code paragraph for `crates/pictura-shp` |

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
