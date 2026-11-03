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
| `crates/pictura-app/src/cxxqt_object/state.rs` | the `shp_open` field (`Option<shp::OpenRequest>`: palette and size-limit allowance) and its `None` default; the `shp_on_disk` field (`shp::OnDisk`: the radar fields the last SHP save wrote) and its default |
| `crates/pictura-app/src/cxxqt_object/open_jobs/imp.rs` | `decode_image_with_options`: `shp::decode_shp` right after the file is read, before `decode_import` (so it wins over an installed Qt SHP plugin and skips `finalize_import`); `open_image_start`: takes `shp_open` and runs the decode inside `shp::with_open_request` |
| `crates/pictura-app/src/cxxqt_object/impl_core.rs` | `revert`: the decode runs inside `shp::with_document_palette`, so a reverted SHP keeps its palette |
| `crates/pictura-app/src/cxxqt_object/impl_core/save.rs` | `save`: an `shp` suffix, a theater suffix or `tmp` goes to `shp::save_westwood` (a TMP template to the TMP writer, an SHP sheet to the SHP writer) before the native fallback |
| `CMakeLists.txt` | the new shell sources (`shp_authoring`, `tmp_authoring`, `shp_palettes`, `shp_support`, `shp_support_unit`, `shp_support_tmp`, `shp_undo_focus`, `image_view_shp`, `panels/shp_{frames,palette,playback,unit}_panel`, `panels/tmp_tiles_panel`, and for MIX archives `mix_support` and `panels/mix_browser_panel` with their headers) |
| `crates/pictura-app/cpp/tests/CMakeLists.txt` | `tst_shp`, `tst_shp_view`, `tst_shp_unit`, `tst_tmp` and `tst_mix` in the suite list |
| `crates/pictura-app/cpp/frame_build.cpp` | `registerPanel(ShpSupport::install(this, colorState_)->dock(), Qt::BottomDockWidgetArea)`, `registerPanel(MixSupport::install(this)->dock(), Qt::BottomDockWidgetArea)` (the MIX browser) and their includes |
| `crates/pictura-app/cpp/frame_session.cpp` | `retargetDock` calls `ShpSupport::setView`, `refreshDock` calls `ShpSupport::refresh`; one include |
| `crates/pictura-app/cpp/frame_open.cpp` | `startOpenImagePath`: when `shp_sniff_file(path)`, `ShpSupport::confirmOpen(view, path)` (the size limit, header only) before `beginOpenCursor`, deleting the view and returning 0 when it refuses, and `shp_set_open_palette(*view, resolveShpPalette(path))` before `open_image_start`, else when `tmp_sniff_file(path)` (a TMP terrain template) `shp_set_open_palette(*view, resolveTmpPalette(path))`; for an entry opened from a MIX, `beginLoadingDocument` gets `mixMemberTitle(path)` as the display name and no recent path (its file is temporary); `startOpenDocumentAtPath`: a `.mix` path (`isMixPath`) goes to `MixSupport::openArchive` and returns 0 (no document opens); four includes |
| `crates/pictura-app/cpp/keyboard_shortcuts_dialog.cpp` | `,` and `.` (frame stepping) added to `workspaceKeys()`, so the shortcut editor never hands them to a command |
| `crates/pictura-app/cpp/image_view.h` | the `ShpGrid` struct (grid, active cell, onion skin, off-palette marks, shadow underlay, house-color remap), `setShpGrid`/`shpGrid`, `paintShpGrid`, `paintShpOnion`, `paintShpOffPalette`, `paintShpShadow`, `paintShpHouse`, `shpCellImage`, the `ShpHouseCache` struct and the `shpGrid_` and `shpHouseCache_` members; for TMP templates the `tileOutlines`, `extraOutlines` and `activeTile` fields of `ShpGrid` and `paintTmpOutlines` |
| `crates/pictura-app/cpp/image_view.cpp` | `paintEvent` calls `paintShpGrid(painter)` before the guides |
| `crates/pictura-app/cpp/dialogs.{h,cpp}` | `shp` in `isKnownImageSuffix`; "Westwood SHP (TS/RA2) (*.SHP)" as save filter 8 (before All Formats) and in the open filters, `*.SHP` in both All Formats; `*.MIX` in the open All Formats and "Westwood MIX archive (*.MIX)" before All files; the open filters also list the theater suffixes and `.tmp` (`*.TEM *.SNO *.URB *.UBN *.DES *.LUN *.TMP`) in All Formats and as "Westwood TMP terrain and theater SHP" (the content decides which opens); `defaultSuffixForFilter(8)`; `defaultSaveFilterIndex` returns 8 for an SHP sheet; `savePathNeedsFormatDialog` sends an `.shp` path to Save As unless the document is an SHP sheet (and a TMP path unless it is a TMP template); for TMP: the theater suffixes and `tmp` in `isKnownImageSuffix`, "Westwood TMP terrain" as save filter 9 (All Formats is 10 and lists the suffixes), `defaultSuffixForFilter(9)` (`.tem`), `defaultSaveFilterIndex` returns 9 for a TMP template, `pathForSaveFilter` keeps a typed theater suffix under filters 8 and 9; two includes; doc comments |
| `crates/pictura-app/cpp/frame.cpp` | `saveActiveAs`: a Westwood path (`isShpPath`: `.shp`, the theater suffixes, `.tmp`) runs `ShpSupport::saveGate(view, true, path)` first (for a TMP template, the TMP gate) (the path lets it convert an image that is not an SHP sheet into a new SHP document); `saveActive`: a document opened from a MIX (`isMixMember`) goes to `MixSupport::saveMember` (the loose-file / copy-of-the-MIX prompt); `saveActiveAs` clears the display name ("gi.shp in ra2.mix") when such a document is saved to another path; two includes |
| `crates/pictura-app/cpp/frame_preferences.cpp` | `showPreferences`: `addPage(tr("Advanced"), ShpSupport::advancedPage(...))` (the SHP size-limit settings) and `addPage(tr("MIX Archives"), MixSupport::palettePage(...))` (archives searched for palettes); two includes |
| `crates/pictura-app/cpp/document_window.cpp` | `documentDisplayName`: a document opened from a MIX (`isMixMember(doc.path)`) reads `mixMemberTitle(doc.path)` ("gi.shp in ra2.mix", and the copy's name after Replace in a Copy); one include |
| `crates/pictura-app/cpp/frame_transform_lock.cpp` | the registry's blocked provider also blocks what `ShpSupport::blocks(this, spec.id)` names (Image Size, Canvas Size, Crop, Trim, Reveal All, rotations, flips, mode changes while the active document is an SHP sheet); `installTransformLock` also sets the tool gate (`tools_->setToolGate`, answered by `ShpSupport::allowsTool`) and the Actions step gate (`actionsPanel_->setStepGate`, answered by `ShpSupport::stepRefusal`); two includes (`shp_support.h`, `panels/actions_panel.h`) |
| `crates/pictura-app/cpp/tools.{h,cpp}` | `ToolController::setToolGate` and its `toolGate_` member; `setActiveTool` asks the gate after its no-change check and, refused, re-announces the current tool and returns (the Crop and Perspective Crop tools while an SHP sheet is active) |
| `crates/pictura-app/cpp/panels/actions_panel.h`, `panels/actions_panel_playback.cpp` | the `StepGate` type, `setStepGate`, the `stepGate_` member and `refuseStep` (reports "Step … was not run: <reason>"); `advancePlayback` stops the playback when `refuseStep` refuses a step, before running it; `executeStep` (also Record Again and step edits) returns false for one |
| `crates/pictura-app/cpp/control_server_commands.cpp` | `run_command` answers `refused` with `ShpSupport::blockedReason()` when `ShpSupport::blocks(frame_, id)` (its replay skips the registry's blocked gate); one include |
| `crates/pictura-app/cpp/control_server.cpp` | `dispatch`: a `ShpSupport::Quiet` scope around `route` (no size-limit dialogs), and a size-limit refusal answers `refused` with its reason; one include |
| `crates/pictura-app/cpp/selftest.cpp` | `prefs_pages`: the expected page list ends with "Advanced" and "MIX Archives" |
| `crates/pictura-app/cpp/frame_open.cpp` | `saveRunsOnWorker`: Westwood paths (`isShpPath`) save in place (the worker writes only PSD/PSB) |
| `crates/pictura-app/cpp/control_server_documents.cpp` | `save`/`save_as` of a Westwood path (`isShpPath`) run the gate without dialogs and answer `refused` with its reason, or `io_error` with the writer's message; one include |
| `crates/pictura-app/cpp/tests/tst_open_save.cpp` | `unknownSuffixNeverGetsPsdBytes`: `.shp` is writable now (the PNG-content sprite is still refused) |

Everything else for SHP documents lives in new files:
`crates/pictura-app/src/cxxqt_object/shp.rs` (bridge declarations) and
`shp/{imp,window,frames,authoring,unit,tmp,tmp_image,tmp_shp,tmp_edit,mix}.rs` with their `*_tests.rs`, `crates/pictura-shp/src/{sheet,resource,unit,tmp,tmp_image,terrain}.rs`, the MIX codec `crates/pictura-shp/src/mix/` with its name list `data/mix-names.txt`, and in
the shell `cpp/shp_palettes.{h,cpp}` (which `.pal` an SHP or a TMP template opens with, MIX archives included),
`cpp/mix_support.{h,cpp}` and `cpp/panels/mix_browser_panel.{h,cpp}` (the MIX browser, documents from archives, the save prompt, export, palettes in archives, Preferences > MIX Archives),
`cpp/shp_support.{h,cpp}`, `cpp/shp_support_unit.cpp` and `cpp/shp_support_tmp.cpp` (the TMP save gate) (the dock, `,`/`.` stepping, paging, frame operations, the save gate, the shadow tools, the radar-color save option,
the File > New SHP and Image > Frame Size menu entries, inserted at runtime after
File > New and Image > Canvas Size),
`cpp/shp_authoring.{h,cpp}` (the New SHP, Save As SHP and Frame Size dialogs),
`cpp/tmp_authoring.{h,cpp}` (the New TMP from Image and the template-to-SHP dialogs, and the Tiles panel's cell and extra-block dialogs; `shp_support_tmp.cpp` adds File > New TMP from Image after New SHP, routes Save As TMP of an image through it and Save As SHP of a template through the conversion),
`cpp/shp_undo_focus.{h,cpp}` (Undo/Redo focus the frame they changed),
`cpp/panels/shp_{frames,palette,playback,unit}_panel.{h,cpp}`, `cpp/panels/tmp_tiles_panel.{h,cpp}` (the Tiles panel of a TMP template: fields, and Move, Add, Remove, Add Extra and Remove Extra), `cpp/image_view_shp.cpp`
(the frame grid, tile outlines, house-color preview, onion skin, shadow underlay and off-palette marks) and `cpp/tests/{tst_shp,tst_shp_view,tst_shp_unit,tst_tmp,tst_mix}.cpp` with their
shared generated sprites in `cpp/tests/shp_fixtures.h`.

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

The GPU failure fix (a run-time GPU error, out of video memory for one, turns
the GPU off for the session and the work falls back to the CPU instead of
panicking in a worker thread; a dead open worker still answers the GUI) is
generic too and kept self-contained the same way. It touches no SHP code:

| Upstream file | Hook |
|---|---|
| `crates/pictura-render/src/gpu/failure.rs` (new) | the failed flag and its one-shot notice (`take_gpu_failure_notice`), the uncaptured-error handler, `guard` (a device error or a panic during a GPU job answers `GpuError::Unavailable`), the `inject_gpu_failure` test hook |
| `crates/pictura-render/src/gpu/backend.rs` | `request_device` installs `on_uncaptured_error(failure::uncaptured)` (wgpu's default handler panics) and the device-lost callback calls `failure::disable`; `device_lost()` reads `failure::failed()` (the old `LOST` static is gone) |
| `crates/pictura-render/src/gpu/mod.rs` | `mod failure;`, its two `pub use`s; `composite_gpu_region_raw` runs inside `failure::guard` |
| `crates/pictura-render/src/lib.rs` | `inject_gpu_failure`, `take_gpu_failure_notice` in the `gpu` re-export list |
| `crates/pictura-render/tests/gpu_failure.rs` (new) | `an_injected_device_error_falls_back_to_the_cpu_for_the_session` |
| `crates/pictura-app/src/cxxqt_object/open_jobs/imp.rs` | `run_guarded` (a panicking job answers `error:...`); `start` and `open_pdf_start` run their job inside it; test `a_panicking_open_worker_still_answers` in `open_jobs/imp/tests.rs` |
| `crates/pictura-app/src/gpu_probe_bridge.rs`, `gpu_probe_bridge/imp.rs` | `gpu_take_failure_notice`, `gpu_inject_failure` |
| `crates/pictura-app/cpp/gpu_probe.{h,cpp}` | `takeGpuFailureNotice()` |
| `crates/pictura-app/cpp/frame.cpp` | `updateStatus` shows a pending notice in the status bar (the backend label then reads "CPU (safe mode)") |
| `crates/pictura-app/cpp/tests/CMakeLists.txt`, `tests/tst_gpu_fallback.cpp` (new) | `tst_gpu_fallback` in the suite list (`injectedFailureOpensOnTheCpu`) |

The self-test skip fix is generic too (a run without a GPU adapter skipped
`pp_gpu_commit` and, through an early `return 0`, silently dropped the next
check, `pp_pyramid_per_frame`, which has a CPU path):

| Upstream file | Hook |
|---|---|
| `crates/pictura-app/cpp/selftest_paint_perf.cpp` | `pp_gpu_commit` is a `do { } while (false)` block whose no-adapter skip `break`s instead of returning, so `pp_pyramid_per_frame` still runs |
