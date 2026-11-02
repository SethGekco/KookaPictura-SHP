use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::time::SystemTime;

use cxx_qt_build::CxxQtBuilder;

/// Locate a Qt 6 `qmake`.
///
/// `qt-build-utils` drives Qt discovery through `qmake`. On Debian/Ubuntu the
/// bare `qmake` may be Qt 5, so prefer `qmake6`.
fn find_qmake6() -> Option<PathBuf> {
    for dir in std::env::split_paths(&std::env::var_os("PATH")?) {
        for name in ["qmake6", "qmake"] {
            let candidate = dir.join(name);
            if !candidate.is_file() {
                continue;
            }
            let output = std::process::Command::new(&candidate)
                .args(["-query", "QT_VERSION"])
                .output();
            if let Ok(output) = output {
                if output.status.success() && output.stdout.starts_with(b"6") {
                    return Some(candidate);
                }
            }
        }
    }
    None
}

/// The hand-written C++ edges the bridges include.
///
/// The `cc` crate emits no change-tracking directive for the sources it
/// compiles, so without these the build script would not rerun when one of
/// them changes and the Rust static library would keep a stale object.
const BRIDGE_EDGES: [&str; 6] = [
    "cpp/decode_image.h",
    "cpp/decode_image.cpp",
    "cpp/encode_image.h",
    "cpp/encode_image.cpp",
    "cpp/panels/actions_tool_recording_bridge.h",
    "cpp/panels/actions_tool_recording_bridge.cpp",
];

/// The directory the C++ build includes the generated cxx-qt headers from.
///
/// `cxx-qt-build` exports them under the directory CMake names, so this is
/// the tree whose timestamps the C++ build tracks.
fn exported_header_root() -> Option<PathBuf> {
    let export = std::env::var_os("CXX_QT_EXPORT_DIR")?;
    Some(
        PathBuf::from(export)
            .join("crates")
            .join("pictura_app")
            .join("include"),
    )
}

/// Record the content and modification time of every file under `root`.
fn snapshot_headers(root: &Path) -> HashMap<PathBuf, (SystemTime, Vec<u8>)> {
    let mut files = HashMap::new();
    let mut directories = vec![root.to_path_buf()];
    while let Some(directory) = directories.pop() {
        let Ok(entries) = std::fs::read_dir(&directory) else {
            continue;
        };
        for entry in entries.flatten() {
            let path = entry.path();
            if path.is_dir() {
                directories.push(path);
                continue;
            }
            let Ok(metadata) = entry.metadata() else {
                continue;
            };
            let Ok(bytes) = std::fs::read(&path) else {
                continue;
            };
            let modified = metadata.modified().unwrap_or(SystemTime::UNIX_EPOCH);
            files.insert(path, (modified, bytes));
        }
    }
    files
}

/// Restore the modification time of every file whose content did not change.
///
/// `cxx-qt-build` rewrites every generated header with `File::create`, so a
/// build-script rerun that produces identical headers still moves their
/// timestamps — and the C++ build reads a timestamp as "the interface
/// changed", which costs a full rebuild of every consumer.
fn restore_unchanged_headers(before: &HashMap<PathBuf, (SystemTime, Vec<u8>)>) {
    for (path, (modified, bytes)) in before {
        let Ok(current) = std::fs::read(path) else {
            continue;
        };
        if &current != bytes {
            continue;
        }
        let Ok(file) = std::fs::File::open(path) else {
            continue;
        };
        let times = std::fs::FileTimes::new().set_modified(*modified);
        let _ = file.set_times(times);
    }
}

fn main() {
    // cxx-qt compiles its exported initializer object with a separate cc::Build
    // that the cc_builder closure does not reach. CXXFLAGS is cc's shared hook;
    // -fPIC keeps that object linkable into the shared pictura_shell library.
    if cfg!(unix) && std::env::var_os("CXXFLAGS").is_none() {
        std::env::set_var("CXXFLAGS", "-fPIC");
    }

    // CMake's cxx_qt_import_crate already exports QMAKE; only fill it in for
    // standalone `cargo build`.
    if std::env::var_os("QMAKE").is_none() {
        if let Some(qmake) = find_qmake6() {
            std::env::set_var("QMAKE", qmake);
        }
    }

    // The hand-written bridge edges are the only C++ files this crate's
    // generated code depends on; track them explicitly.
    for edge in BRIDGE_EDGES {
        println!("cargo::rerun-if-changed={edge}");
    }

    // A generator clean deletes the exported initializer object while cargo
    // still holds this script fresh; CMake touches this stamp when the object is
    // missing so the script reruns and writes it again. Watched only once it
    // exists, since a missing watched file would rerun the script every build.
    if let Some(export) = std::env::var_os("CXX_QT_EXPORT_DIR") {
        let stamp = PathBuf::from(export).join("pictura_app.regenerate");
        if stamp.exists() {
            println!("cargo::rerun-if-changed={}", stamp.display());
        }
    }

    // The headers the C++ build includes are rewritten by every build-script
    // run; remember them so a run that changes nothing leaves their
    // timestamps alone.
    let exported_before = exported_header_root()
        .as_deref()
        .map(snapshot_headers)
        .unwrap_or_default();

    // `cpp/` holds hand-written headers (e.g. `decode_image.h`) included by the
    // cxx bridge, so the generated C++ glue can find them. The `.cpp` files
    // themselves are compiled and linked by CMake, not here.
    unsafe {
        CxxQtBuilder::new()
            .qt_module("Gui")
            .files([
                "src/cxxqt_object.rs",
                "src/decode_bridge.rs",
                "src/mcp_entry.rs",
                "src/gpu_probe_bridge.rs",
                "src/cxxqt_object/actions_engine.rs",
                "src/cxxqt_object/engine_tables/adjustment_catalogue.rs",
                "src/cxxqt_object/adjustment_edit.rs",
                "src/cxxqt_object/align.rs",
                "src/cxxqt_object/auto_blend.rs",
                "src/cxxqt_object/annotations.rs",
                "src/cxxqt_object/engine_tables/blend_modes.rs",
                "src/cxxqt_object/channels.rs",
                "src/cxxqt_object/clipping.rs",
                "src/cxxqt_object/color_management.rs",
                "src/cxxqt_object/command_catalogue.rs",
                "src/cxxqt_object/engine_tables/color_models.rs",
                "src/cxxqt_object/color_range.rs",
                "src/cxxqt_object/clipboard.rs",
                "src/cxxqt_object/crop_group.rs",
                "src/cxxqt_object/export.rs",
                "src/cxxqt_object/eyedropper_sample.rs",
                "src/cxxqt_object/filter_tools.rs",
                "src/cxxqt_object/guides.rs",
                "src/cxxqt_object/filter_tools/jobs.rs",
                "src/cxxqt_object/filter_tools/liquify.rs",
                "src/cxxqt_object/filter_tools/adaptive_wide_angle.rs",
                "src/cxxqt_object/filter_tools/lens_correction.rs",
                "src/cxxqt_object/filter_tools/vanishing_point.rs",
                "src/cxxqt_object/filter_tools/vanishing_point_retouch.rs",
                "src/cxxqt_object/healing.rs",
                "src/cxxqt_object/image_adjust.rs",
                "src/cxxqt_object/impl_selection/bridge.rs",
                "src/cxxqt_object/impl_selection/refine.rs",
                "src/cxxqt_object/impl_transform/bridge.rs",
                "src/cxxqt_object/impl_transform/warp.rs",
                "src/cxxqt_object/image_adjust/gradient_map.rs",
                "src/cxxqt_object/image_adjust/image_mode.rs",
                "src/cxxqt_object/image_adjust/image_ops.rs",
                "src/cxxqt_object/image_adjust/image_size.rs",
                "src/cxxqt_object/image_adjust/jobs.rs",
                "src/cxxqt_object/image_hdr_toning.rs",
                "src/cxxqt_object/hdr_conversion.rs",
                "src/cxxqt_object/impl_core/recovery.rs",
                "src/cxxqt_object/impl_core/save_jobs.rs",
                "src/cxxqt_object/image_replace_color.rs",
                "src/cxxqt_object/image_match_color.rs",
                "src/cxxqt_object/impl_paint/tool_recording.rs",
                "src/cxxqt_object/layer_style.rs",
                "src/cxxqt_object/layer_target_bridge.rs",
                "src/cxxqt_object/layer_path.rs",
                "src/cxxqt_object/layers_smart_filters.rs",
                "src/cxxqt_object/layer_arrange.rs",
                "src/cxxqt_object/impl_layers/auto_select.rs",
                "src/cxxqt_object/impl_layers/fill_content.rs",
                "src/cxxqt_object/impl_layers/layer_masks.rs",
                "src/cxxqt_object/impl_layers/mask_commands.rs",
                "src/cxxqt_object/impl_layers/layers_surface.rs",
                "src/cxxqt_object/impl_layers/mask_refine.rs",
                "src/cxxqt_object/impl_layers/matting.rs",
                "src/cxxqt_object/impl_layers/smart_object_actions.rs",
                "src/cxxqt_object/impl_layers/vector_masks.rs",
                "src/cxxqt_object/magnetic.rs",
                "src/cxxqt_object/open_jobs.rs",
                "src/cxxqt_object/image_adjust/new_document.rs",
                "src/cxxqt_object/paint_tools.rs",
                "src/cxxqt_object/paint_tools/fills.rs",
                "src/cxxqt_object/path_list.rs",
                "src/cxxqt_object/impl_history/display.rs",
                "src/cxxqt_object/impl_history/purge.rs",
                "src/cxxqt_object/paths.rs",
                "src/cxxqt_object/pdf_export.rs",
                "src/cxxqt_object/pdf_export/import.rs",
                "src/cxxqt_object/save_format.rs",
                "src/cxxqt_object/shapes.rs",
                "src/cxxqt_object/shp.rs",
                "src/cxxqt_object/engine_tables/tool_catalogue.rs",
                "src/cxxqt_object/type_tools.rs",
                "src/cxxqt_object/type_tools/warp.rs",
                "src/cxxqt_object/type_tools/fonts.rs",
                "src/cxxqt_object/svg_export.rs",
                "src/cxxqt_object/web_export.rs",
            ])
            .cc_builder(|cc| {
                cc.include("cpp");
                // The bridge objects end up in the shared pictura_shell library in
                // development builds; cc does not add -fPIC on its own. MSVC has
                // no equivalent flag.
                if !cc.get_compiler().is_like_msvc() {
                    cc.flag_if_supported("-fPIC");
                }
                // GCC 16 warns inside Qt's QChar and the generated
                // Vec<LayerRow> constructor; both are cxx-qt/Qt glue, not this
                // project's code.
                cc.flag_if_supported("-Wno-sfinae-incomplete");
                cc.flag_if_supported("-Wno-maybe-uninitialized");
                // The hand-written bridge edges live in `cpp/`; CMake compiles
                // them into the shell, and the Rust test binary needs its own
                // copies (static libraries only pull what they resolve).
                cc.file("cpp/decode_image.cpp");
                cc.file("cpp/encode_image.cpp");
                // This one is not in CMake's shell sources: it must be in the
                // Rust static library so `cargo test -p pictura-app` resolves
                // the same tool-recording hooks the Qt executable uses.
                cc.file("cpp/panels/actions_tool_recording_bridge.cpp");
            })
            // The default crate include root makes cargo watch the whole crate
            // directory, so touching any shell source reran this script. Nothing
            // includes the crate's own headers through that root: the generated
            // headers are namespaced under the crate name, and the hand-written
            // bridge headers are found through the cc include path above.
            .crate_include_root(None)
            .build();
    }

    restore_unchanged_headers(&exported_before);
}
