# Kooka Pictura — M0 walking skeleton (M0-A).
#
# Rust owns the QObject and the image; CMake builds the Rust crate as a static
# library through Corrosion (via CXX-Qt-CMake) and links it into the C++ shell.

cmake_minimum_required(VERSION 3.24)

project(kooka_pictura VERSION 0.4.0 LANGUAGES CXX) # x-release-please-version

# Route local C++ compilation through sccache when it is installed. An explicit
# -DCMAKE_CXX_COMPILER_LAUNCHER or the same-named environment variable wins: the
# cache variable is seeded from the environment during project(), so this runs
# after a user override is already visible. The nested guard keeps a missing
# sccache from setting the launcher to SCCACHE_EXECUTABLE-NOTFOUND.
if(NOT CMAKE_CXX_COMPILER_LAUNCHER)
    find_program(SCCACHE_EXECUTABLE sccache)
    if(SCCACHE_EXECUTABLE)
        set(CMAKE_CXX_COMPILER_LAUNCHER "${SCCACHE_EXECUTABLE}")
        message(STATUS "C++ compiler launcher: ${SCCACHE_EXECUTABLE}")
    endif()
endif()

# clangd (and therefore Serena's C++ symbol tools) reads compile_commands.json
# from the repo root. CMake writes it into the build dir, so link it back. An
# in-source build (flatpak-builder's cmake buildsystem) has no separate build
# dir, so there is nothing to link.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(_compile_db "${CMAKE_SOURCE_DIR}/compile_commands.json")
if(NOT CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    if(IS_SYMLINK "${_compile_db}")
        file(REMOVE "${_compile_db}")
    endif()
    if(NOT EXISTS "${_compile_db}")
        file(CREATE_LINK "${CMAKE_BINARY_DIR}/compile_commands.json" "${_compile_db}" SYMBOLIC)
    endif()
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
    message(STATUS "No CMAKE_BUILD_TYPE set; defaulting to RelWithDebInfo")
endif()

# Prefer mold on ELF platforms, then lld, then the compiler's default linker.
# check_linker_flag proves the compiler accepts the flag, so an installed mold
# with a GCC older than 12.1 falls through instead of failing at link time. An
# explicit -fuse-ld= in the user's linker flags wins. macOS and Windows keep
# their native linker.
include(CheckLinkerFlag)
if(UNIX AND NOT APPLE AND NOT MSVC
   AND NOT CMAKE_LINKER_TYPE
   AND NOT CMAKE_EXE_LINKER_FLAGS MATCHES "-fuse-ld="
   AND NOT CMAKE_SHARED_LINKER_FLAGS MATCHES "-fuse-ld=")
    find_program(MOLD_EXECUTABLE mold)
    if(MOLD_EXECUTABLE)
        check_linker_flag(CXX "-fuse-ld=mold" PICTURA_LINKER_ACCEPTS_MOLD)
    endif()
    if(PICTURA_LINKER_ACCEPTS_MOLD)
        set(_pictura_linker_flag "-fuse-ld=mold")
    else()
        check_linker_flag(CXX "-fuse-ld=lld" PICTURA_LINKER_ACCEPTS_LLD)
        if(PICTURA_LINKER_ACCEPTS_LLD)
            set(_pictura_linker_flag "-fuse-ld=lld")
        endif()
    endif()
    if(_pictura_linker_flag)
        add_link_options("${_pictura_linker_flag}")
        message(STATUS "C++ linker flag: ${_pictura_linker_flag}")
    endif()
elseif(UNIX AND NOT APPLE AND NOT MSVC AND NOT CMAKE_LINKER_TYPE)
    # The cached -fuse-ld= wins, but a build tree configured before the mold
    # preference keeps an old -fuse-ld=lld without saying so. Say so once per
    # build tree and flag value, so a deliberate choice is not nagged. The
    # first non-mold -fuse-ld= in either the EXE or the SHARED flags counts.
    string(REGEX MATCHALL "-fuse-ld=[^ ]+" _pictura_cached_lds
        "${CMAKE_EXE_LINKER_FLAGS} ${CMAKE_SHARED_LINKER_FLAGS}")
    set(_pictura_cached_ld "")
    foreach(_ld IN LISTS _pictura_cached_lds)
        if(NOT _ld STREQUAL "-fuse-ld=mold")
            set(_pictura_cached_ld "${_ld}")
            break()
        endif()
    endforeach()
    if(_pictura_cached_ld
       AND NOT _pictura_cached_ld STREQUAL "$CACHE{PICTURA_LINKER_MASK_WARNED}")
        find_program(MOLD_EXECUTABLE mold)
        if(MOLD_EXECUTABLE)
            check_linker_flag(CXX "-fuse-ld=mold" PICTURA_LINKER_ACCEPTS_MOLD)
        endif()
        if(PICTURA_LINKER_ACCEPTS_MOLD)
            message(WARNING
                "The cached linker flags carry ${_pictura_cached_ld}, so mold is "
                "not used although it is installed. To let the build pick mold, "
                "reconfigure with: cmake -U CMAKE_EXE_LINKER_FLAGS "
                "-U CMAKE_SHARED_LINKER_FLAGS ${CMAKE_BINARY_DIR}")
            set(PICTURA_LINKER_MASK_WARNED "${_pictura_cached_ld}" CACHE INTERNAL
                "The cached -fuse-ld= flag the mold warning was printed for")
        endif()
    endif()
endif()

# Bound concurrent link steps: a highly parallel compile can otherwise start
# several large links at once and thrash memory. These must be set before the
# first target is created.
if(CMAKE_GENERATOR MATCHES "Ninja")
    set(CMAKE_JOB_POOLS link=4)
    set(CMAKE_JOB_POOL_LINK link)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

# MSVC's <cmath> only exposes M_PI (used by the crop/overlay geometry) when
# _USE_MATH_DEFINES is defined before the include. A command-line define covers
# every translation unit regardless of include order; other compilers ignore it.
add_compile_definitions(_USE_MATH_DEFINES)

# Qt6::GuiPrivate supplies the RHI headers used by interop.cpp. The private
# dependency is deliberate, so mute Qt's configure-time warning at the boundary
# instead of filtering build logs.
set(QT_NO_PRIVATE_MODULE_WARNING ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Svg Network PrintSupport GuiPrivate)

# Enables BUILD_TESTING (default ON); the Qt Test suites live under cpp/tests.
include(CTest)

find_package(CxxQt QUIET)
if(NOT CxxQt_FOUND)
    include(FetchContent)
    FetchContent_Declare(
        CxxQt
        GIT_REPOSITORY https://github.com/kdab/cxx-qt-cmake.git
        GIT_TAG 0.10.0
    )
    FetchContent_MakeAvailable(CxxQt)
endif()

# cxx-qt reaches Qt through qmake. Debian/Ubuntu ship qmake6 for Qt 6; the bare
# `qmake` is Qt 5, so look for qmake6 first. The value the build uses is the
# same in every cargo invocation: no script and no build system exports a
# QMAKE, so the cxx-qt build script finds qmake on the PATH itself. The
# lookup here is the early error for a machine without one.
find_program(QMAKE_EXECUTABLE NAMES qmake6 qmake)
if(NOT QMAKE_EXECUTABLE)
    message(FATAL_ERROR "qmake not found — install qt6-base-dev")
endif()

# An optimized development build compiles the Rust library with the incremental
# `dev-app` profile (see Cargo.toml). Packaging (BUILD_TESTING off), Debug, and
# multi-config generators keep Corrosion's default release/dev mapping.
set(PICTURA_DEV_OPTIMIZED OFF)
if(BUILD_TESTING AND NOT CMAKE_CONFIGURATION_TYPES
   AND CMAKE_BUILD_TYPE MATCHES "^(Release|RelWithDebInfo)$")
    set(PICTURA_DEV_OPTIMIZED ON)
endif()
set(_pictura_cargo_profile "")
if(PICTURA_DEV_OPTIMIZED)
    set(_pictura_cargo_profile PROFILE dev-app)
endif()

# The optimized development build compiles C++ at -Og -g1 behind a precompiled
# header: measured cold and warm, both pay (openspec change
# qt-fixture-and-compile-cost, design section 7). Packaging and Debug configures
# keep their settings. Split DWARF was measured slower, and GCC rejects a
# precompiled header for a unit built with it.
set(PICTURA_DEV_COMPILE_OPTIONS "")
if(PICTURA_DEV_OPTIMIZED)
    set(PICTURA_DEV_COMPILE_OPTIONS "$<$<CXX_COMPILER_ID:GNU,Clang>:-Og;-g1>")
endif()
# The precompiled header was measured with GCC only; it follows the flags'
# compiler scope, so AppleClang and MSVC builds keep compiling without one.
set(PICTURA_DEV_PCH OFF)
if(PICTURA_DEV_OPTIMIZED AND CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang)$")
    set(PICTURA_DEV_PCH ON)
endif()
set(PICTURA_PCH_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/crates/pictura-app/cpp/pictura_pch.h")

cxx_qt_import_crate(
    MANIFEST_PATH crates/pictura-app/Cargo.toml
    ${_pictura_cargo_profile}
    CRATES pictura_app
    QT_MODULES Qt::Core Qt::Gui Qt::Widgets
)

# A generator clean (`ninja -t clean`) deletes the exported initializer object,
# whose real producer is the crate's build script, and cargo still calls that
# script fresh, so the next link fails. The build script watches a stamp in the
# export directory; before each cargo step, the check touches the stamp when
# the object is missing, so cargo reruns the script and writes it again.
get_target_property(_pictura_cxx_qt_export_root pictura_app CXX_QT_EXPORT_DIR)
set(_pictura_cxxqt_regenerate "${_pictura_cxx_qt_export_root}/pictura_app.regenerate")
if(NOT EXISTS "${_pictura_cxxqt_regenerate}")
    file(TOUCH "${_pictura_cxxqt_regenerate}")
endif()
add_custom_target(pictura_cxxqt_export_check
    COMMAND ${CMAKE_COMMAND}
        "-DOBJECT=${_pictura_cxx_qt_export_root}/crates/pictura_app/initializers.o"
        "-DSTAMP=${_pictura_cxxqt_regenerate}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/cxxqt_export_check.cmake"
    VERBATIM)
add_dependencies(cargo-prebuild_pictura_app pictura_cxxqt_export_check)

# The bridge stamp.
#
# The bridge Rust sources define the generated C++ interface. A change to one
# of them has to rebuild the C++ that includes the generated headers in the
# same build, but Ninja decides what to compile before the cargo edge has
# regenerated those headers, so a header rewritten during a build is only seen
# by the next one. This stamp records the bridge content at configure time --
# before the build plans -- and the translation units that include a generated
# header depend on it.
set(PICTURA_BRIDGE_STAMP "${CMAKE_BINARY_DIR}/pictura_bridge.stamp")

# The text of a bridge file that the generated C++ depends on: the
# `#[cxx_qt::bridge]` module, with the outer attributes and doc comments
# directly above it (cxx-gen attaches those to the module too). Everything else
# in the file is implementation, so an edit there leaves the stamp alone. The
# module ends at the first column-0 `}`, which rustfmt guarantees. A string or
# block comment inside the module could hold such a line and end the slice
# early, so the slice must have balanced braces, an even number of quotes, and
# no `/*`; when it does not, or the attribute or the closing brace is missing,
# the whole file is hashed, which rebuilds too much rather than too little. The
# pictura_app test `bridge_modules_end_at_column_zero` pins the same shape from
# the Rust side.
function(pictura_bridge_hash_text path out_var)
    file(READ "${path}" _src)
    set(${out_var} "${_src}" PARENT_SCOPE)
    string(FIND "${_src}" "#[cxx_qt::bridge" _attr)
    if(_attr EQUAL -1)
        return()
    endif()

    set(_start ${_attr})
    while(_start GREATER 0)
        math(EXPR _newline "${_start} - 1")
        string(SUBSTRING "${_src}" ${_newline} 1 _char)
        if(NOT _char STREQUAL "\n")
            break()
        endif()
        string(SUBSTRING "${_src}" 0 ${_newline} _head)
        string(FIND "${_head}" "\n" _previous REVERSE)
        math(EXPR _line_start "${_previous} + 1")
        math(EXPR _line_length "${_newline} - ${_line_start}")
        string(SUBSTRING "${_src}" ${_line_start} ${_line_length} _line)
        if(NOT _line MATCHES "^(#\\[|///)")
            break()
        endif()
        set(_start ${_line_start})
    endwhile()

    string(SUBSTRING "${_src}" ${_attr} -1 _tail)
    string(FIND "${_tail}" "\n}\n" _close)
    if(_close EQUAL -1)
        return()
    endif()
    math(EXPR _length "${_attr} + ${_close} + 2 - ${_start}")
    string(SUBSTRING "${_src}" ${_start} ${_length} _module)

    string(REGEX MATCHALL "{" _opens "${_module}")
    string(REGEX MATCHALL "}" _closes "${_module}")
    list(LENGTH _opens _open_count)
    list(LENGTH _closes _close_count)
    string(REPLACE "\\\\" "" _unescaped "${_module}")
    string(REPLACE "\\\"" "" _unescaped "${_unescaped}")
    string(REGEX MATCHALL "\"" _quotes "${_unescaped}")
    list(LENGTH _quotes _quote_count)
    math(EXPR _odd_quotes "${_quote_count} % 2")
    string(FIND "${_module}" "/*" _block_comment)
    if(_open_count EQUAL _close_count AND _odd_quotes EQUAL 0
       AND _block_comment EQUAL -1)
        set(${out_var} "${_module}" PARENT_SCOPE)
    endif()
endfunction()

# The bridge sources are the Rust files carrying the cxx-qt bridge macro, plus
# every file the build script's `.files([...])` list names. The scan finds a
# bridge wherever it is; the list catches a listed file before it has one, so
# the bridge it gains later is a configure dependency from the start. A listed
# file without a bridge is hashed whole. CONFIGURE_DEPENDS re-runs the configure
# step when a Rust file is added or removed.
file(GLOB_RECURSE _pictura_rs_files RELATIVE "${CMAKE_SOURCE_DIR}" CONFIGURE_DEPENDS
    "crates/pictura-app/src/*.rs")
set(_pictura_bridge_files "")
foreach(_file IN LISTS _pictura_rs_files)
    file(READ "${CMAKE_SOURCE_DIR}/${_file}" _content)
    string(FIND "${_content}" "cxx_qt::bridge" _bridge_at)
    if(NOT _bridge_at EQUAL -1)
        list(APPEND _pictura_bridge_files "${_file}")
    endif()
endforeach()
file(STRINGS "${CMAKE_SOURCE_DIR}/crates/pictura-app/build.rs" _pictura_listed
    REGEX "^[ \t]*\"src/[^\"]+\\.rs\",?[ \t]*$")
foreach(_line IN LISTS _pictura_listed)
    string(REGEX REPLACE "^[ \t]*\"([^\"]+)\".*$" "crates/pictura-app/\\1" _file "${_line}")
    if(EXISTS "${CMAKE_SOURCE_DIR}/${_file}")
        list(APPEND _pictura_bridge_files "${_file}")
    endif()
endforeach()
list(REMOVE_DUPLICATES _pictura_bridge_files)
list(SORT _pictura_bridge_files)
set(_pictura_bridge_digest "")
foreach(_file IN LISTS _pictura_bridge_files)
    pictura_bridge_hash_text("${CMAKE_SOURCE_DIR}/${_file}" _bridge_text)
    string(SHA256 _file_digest "${_bridge_text}")
    string(APPEND _pictura_bridge_digest "${_file_digest} ${_file}\n")
endforeach()
string(SHA256 _pictura_bridge_hash "${_pictura_bridge_digest}")

# Only a bridge file, or build.rs, re-runs the configure step on an edit, so the
# stamp is refreshed when a bridge changes. A file named in build.rs's list is
# a configure dependency even before it carries a bridge.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${_pictura_bridge_files}
    crates/pictura-app/build.rs)

# Rewrite the stamp only when the bridge content changed: a re-configure that
# finds the same bridge must not dirty the whole C++ tree.
if(EXISTS "${PICTURA_BRIDGE_STAMP}")
    file(READ "${PICTURA_BRIDGE_STAMP}" _pictura_stamp_text)
else()
    set(_pictura_stamp_text "")
endif()
if(NOT _pictura_stamp_text STREQUAL _pictura_bridge_hash)
    file(WRITE "${PICTURA_BRIDGE_STAMP}" "${_pictura_bridge_hash}")
endif()

# Make every translation unit that includes a generated cxx-qt header depend on
# the stamp, so a bridge change rebuilds it in the same build.
function(pictura_depend_on_bridge_stamp)
    foreach(_source IN LISTS ARGN)
        if(_source MATCHES "\\.(c|cc|cpp|cxx)$")
            file(READ "${CMAKE_CURRENT_SOURCE_DIR}/${_source}" _content)
            string(FIND "${_content}" "pictura_app/src/" _generated_at)
            if(NOT _generated_at EQUAL -1)
                set_property(SOURCE "${_source}" APPEND PROPERTY
                    OBJECT_DEPENDS "${PICTURA_BRIDGE_STAMP}")
            endif()
        endif()
    endforeach()
endfunction()

# Compile the Qt resource once: the shared shell carries it, and the static
# configuration links this object library into the executable and each test.
# POSITION_INDEPENDENT_CODE: object libraries default to non-PIC, but they are
# linked into PIE executables and the shared shell; lld rejects the qrc's
# references without it.
add_library(pictura_assets OBJECT assets/pictura.qrc)
set_target_properties(pictura_assets PROPERTIES POSITION_INDEPENDENT_CODE ON)

set(PICTURA_SHELL_SOURCES
    crates/pictura-app/cpp/api/document_api.cpp
    crates/pictura-app/cpp/api/document_api.h
    crates/pictura-app/cpp/api/history_api.cpp
    crates/pictura-app/cpp/api/history_api.h
    crates/pictura-app/cpp/api/selection_api.cpp
    crates/pictura-app/cpp/api/selection_api.h
    crates/pictura-app/cpp/api/paint_api.cpp
    crates/pictura-app/cpp/api/paint_api.h
    crates/pictura-app/cpp/api/transform_api.cpp
    crates/pictura-app/cpp/api/transform_api.h
    crates/pictura-app/cpp/api/layers_api.cpp
    crates/pictura-app/cpp/api/layers_api.h
    crates/pictura-app/cpp/api/type_api.cpp
    crates/pictura-app/cpp/api/type_api.h
    crates/pictura-app/cpp/decode_image.cpp
    crates/pictura-app/cpp/decode_image.h
    crates/pictura-app/cpp/encode_image.cpp
    crates/pictura-app/cpp/encode_image.h
    crates/pictura-app/cpp/interop.cpp
    crates/pictura-app/cpp/commands.cpp
    crates/pictura-app/cpp/agent_control.cpp
    crates/pictura-app/cpp/client_registration.cpp
    crates/pictura-app/cpp/client_registration_box.cpp
    crates/pictura-app/cpp/control_approval_bar.cpp
    crates/pictura-app/cpp/control_policy.cpp
    crates/pictura-app/cpp/control_scope.cpp
    crates/pictura-app/cpp/control_server.cpp
    crates/pictura-app/cpp/control_server_actions.cpp
    crates/pictura-app/cpp/control_server_capture.cpp
    crates/pictura-app/cpp/control_server_commands.cpp
    crates/pictura-app/cpp/control_server_documents.cpp
    crates/pictura-app/cpp/control_server_gate.cpp
    crates/pictura-app/cpp/control_server_input.cpp
    crates/pictura-app/cpp/theme.cpp
    crates/pictura-app/cpp/fonts.cpp
    crates/pictura-app/cpp/recovery_store.cpp
    crates/pictura-app/cpp/session.cpp
    crates/pictura-app/cpp/gpu_probe.cpp
    crates/pictura-app/cpp/workspace_store.cpp
    crates/pictura-app/cpp/preferences_agent_page.cpp
    crates/pictura-app/cpp/preferences_dialog.cpp
    crates/pictura-app/cpp/new_guide_dialog.cpp
    crates/pictura-app/cpp/file_info_dialog.cpp
    crates/pictura-app/cpp/profile_dialog.cpp
    crates/pictura-app/cpp/profile_warning_dialog.cpp
    crates/pictura-app/cpp/save_options_dialog.cpp
    crates/pictura-app/cpp/save_format_options.cpp
    crates/pictura-app/cpp/jpeg_options_dialog.cpp
    crates/pictura-app/cpp/png_options_dialog.cpp
    crates/pictura-app/cpp/bmp_options_dialog.cpp
    crates/pictura-app/cpp/tiff_options_dialog.cpp
    crates/pictura-app/cpp/hdr_conversion_dialog.cpp
    crates/pictura-app/cpp/pictura_raw_dialog.cpp
    crates/pictura-app/cpp/acr/acr_settings.cpp
    crates/pictura-app/cpp/acr/acr_controls.cpp
    crates/pictura-app/cpp/acr/acr_preview_pane.cpp
    crates/pictura-app/cpp/acr/acr_histogram.cpp
    crates/pictura-app/cpp/acr/acr_basic_panel.cpp
    crates/pictura-app/cpp/acr/acr_tone_curve_panel.cpp
    crates/pictura-app/cpp/acr/acr_detail_panel.cpp
    crates/pictura-app/cpp/acr/acr_hsl_panel.cpp
    crates/pictura-app/cpp/acr/acr_split_toning_panel.cpp
    crates/pictura-app/cpp/acr/acr_lens_panel.cpp
    crates/pictura-app/cpp/acr/acr_effects_panel.cpp
    crates/pictura-app/cpp/acr/acr_calibration_panel.cpp
    crates/pictura-app/cpp/acr/acr_presets_panel.cpp
    crates/pictura-app/cpp/filter_commands.cpp
    crates/pictura-app/cpp/filter_commands.h
    crates/pictura-app/cpp/digimarc_read_dialog.cpp
    crates/pictura-app/cpp/digimarc_read_dialog.h
    crates/pictura-app/cpp/busy_job.cpp
    crates/pictura-app/cpp/filter_commit.cpp
    crates/pictura-app/cpp/filter_commit.h
    crates/pictura-app/cpp/tab_spinner.cpp
    crates/pictura-app/cpp/tab_spinner.h
    crates/pictura-app/cpp/loading_card.cpp
    crates/pictura-app/cpp/loading_card.h
    crates/pictura-app/cpp/filter_gallery_dialog.cpp
    crates/pictura-app/cpp/filter_gallery_dialog.h
    crates/pictura-app/cpp/filter_param_controls.cpp
    crates/pictura-app/cpp/filter_param_controls.h
    crates/pictura-app/cpp/filter_preview_dialog.cpp
    crates/pictura-app/cpp/filter_preview_dialog.h
    crates/pictura-app/cpp/lens_flare_dialog.cpp
    crates/pictura-app/cpp/lens_flare_dialog.h
    crates/pictura-app/cpp/picture_frame_dialog.cpp
    crates/pictura-app/cpp/picture_frame_dialog.h
    crates/pictura-app/cpp/reduce_noise_dialog.cpp
    crates/pictura-app/cpp/reduce_noise_dialog.h
    crates/pictura-app/cpp/lighting_canvas.cpp
    crates/pictura-app/cpp/lighting_canvas.h
    crates/pictura-app/cpp/lighting_effects_dialog.cpp
    crates/pictura-app/cpp/lighting_effects_dialog.h
    crates/pictura-app/cpp/blur_gallery_canvas.cpp
    crates/pictura-app/cpp/blur_gallery_canvas.h
    crates/pictura-app/cpp/blur_gallery_canvas_path.cpp
    crates/pictura-app/cpp/blur_gallery_dialog.cpp
    crates/pictura-app/cpp/blur_gallery_dialog.h
    crates/pictura-app/cpp/blur_gallery_light_range.h
    crates/pictura-app/cpp/blur_gallery_path.cpp
    crates/pictura-app/cpp/blur_gallery_spin.cpp
    crates/pictura-app/cpp/lighting_rig.cpp
    crates/pictura-app/cpp/lighting_rig.h
    crates/pictura-app/cpp/awa_canvas.cpp
    crates/pictura-app/cpp/awa_canvas.h
    crates/pictura-app/cpp/awa_dialog.cpp
    crates/pictura-app/cpp/awa_dialog.h
    crates/pictura-app/cpp/lens_correction_canvas.cpp
    crates/pictura-app/cpp/lens_correction_canvas.h
    crates/pictura-app/cpp/lens_correction_dialog.cpp
    crates/pictura-app/cpp/lens_correction_dialog.h
    crates/pictura-app/cpp/lens_correction_panels.cpp
    crates/pictura-app/cpp/lens_correction_panels.h
    crates/pictura-app/cpp/vp_canvas.cpp
    crates/pictura-app/cpp/vp_canvas.h
    crates/pictura-app/cpp/vp_dialog.cpp
    crates/pictura-app/cpp/vp_dialog.h
    crates/pictura-app/cpp/vp_options.cpp
    crates/pictura-app/cpp/vp_options.h
    crates/pictura-app/cpp/liquify_canvas.cpp
    crates/pictura-app/cpp/liquify_canvas.h
    crates/pictura-app/cpp/liquify_dialog.cpp
    crates/pictura-app/cpp/liquify_dialog.h
    crates/pictura-app/cpp/blend_modes.cpp
    crates/pictura-app/cpp/blend_modes.h
    crates/pictura-app/cpp/layer_style_dialog.cpp
    crates/pictura-app/cpp/layer_style_dialog.h
    crates/pictura-app/cpp/layer_style_pages.cpp
    crates/pictura-app/cpp/color_settings_dialog.cpp
    crates/pictura-app/cpp/color_models.cpp
    crates/pictura-app/cpp/color_models.h
    crates/pictura-app/cpp/color_picker_dialog.cpp
    crates/pictura-app/cpp/color_picker_math.cpp
    crates/pictura-app/cpp/color_range_dialog.cpp
    crates/pictura-app/cpp/refine_edge_dialog.cpp
    crates/pictura-app/cpp/mask_display_options_dialog.cpp
    crates/pictura-app/cpp/replace_color_dialog.cpp
    crates/pictura-app/cpp/auto_blend_dialog.cpp
    crates/pictura-app/cpp/match_color_dialog.cpp
    crates/pictura-app/cpp/adjustment_dialog.cpp
    crates/pictura-app/cpp/black_white_dialog.cpp
    crates/pictura-app/cpp/channel_mixer_dialog.cpp
    crates/pictura-app/cpp/color_balance_dialog.cpp
    crates/pictura-app/cpp/curves_dialog.cpp
    crates/pictura-app/cpp/canvas_size_dialog.cpp
    crates/pictura-app/cpp/exposure_dialog.cpp
    crates/pictura-app/cpp/fill_layer_editor.cpp
    crates/pictura-app/cpp/gradient_editor_dialog.cpp
    crates/pictura-app/cpp/gradient_fill_dialog.cpp
    crates/pictura-app/cpp/gradient_map_dialog.cpp
    crates/pictura-app/cpp/gradient_noise_page.cpp
    crates/pictura-app/cpp/hue_saturation_dialog.cpp
    crates/pictura-app/cpp/image_size_dialog.cpp
    crates/pictura-app/cpp/levels_dialog.cpp
    crates/pictura-app/cpp/photo_filter_dialog.cpp
    crates/pictura-app/cpp/slider_dialog.cpp
    crates/pictura-app/cpp/hdr_toning_dialog.cpp
    crates/pictura-app/cpp/hdr_toning_controls.cpp
    crates/pictura-app/cpp/print_dialog.cpp
    crates/pictura-app/cpp/pdf_save_dialog.cpp
    crates/pictura-app/cpp/pdf_presets_dialog.cpp
    crates/pictura-app/cpp/import_pdf_dialog.cpp
    crates/pictura-app/cpp/rasterize_eps_dialog.cpp
    crates/pictura-app/cpp/raster_size_fields.cpp
    crates/pictura-app/cpp/export_as_dialog.cpp
    crates/pictura-app/cpp/duplicate_image_dialog.cpp
    crates/pictura-app/cpp/bitmap_mode_dialog.cpp
    crates/pictura-app/cpp/indexed_color_dialog.cpp
    crates/pictura-app/cpp/fill_dialog.cpp
    crates/pictura-app/cpp/defringe_dialog.cpp
    crates/pictura-app/cpp/stroke_dialog.cpp
    crates/pictura-app/cpp/brush_tip_preview.cpp
    crates/pictura-app/cpp/brush_tip_preview.h
    crates/pictura-app/cpp/image_view.cpp
    crates/pictura-app/cpp/image_view_present.cpp
    crates/pictura-app/cpp/image_view_transform_warp.cpp
    crates/pictura-app/cpp/image_view_overlays.cpp
    crates/pictura-app/cpp/image_view_annotations.cpp
    crates/pictura-app/cpp/image_view_guides.cpp
    crates/pictura-app/cpp/image_view_smart_guides.cpp
    crates/pictura-app/cpp/smart_guides.cpp
    crates/pictura-app/cpp/image_view_channels.cpp
    crates/pictura-app/cpp/image_view_document.cpp
    crates/pictura-app/cpp/image_view_paths.cpp
    crates/pictura-app/cpp/image_view_type.cpp
    crates/pictura-app/cpp/image_view_move.cpp
    crates/pictura-app/cpp/canvas_scrollbars.cpp
    crates/pictura-app/cpp/canvas_ruler.cpp
    crates/pictura-app/cpp/icons.cpp
    crates/pictura-app/cpp/svg_icon_engine.cpp
    crates/pictura-app/cpp/file_drop_router.cpp
    crates/pictura-app/cpp/frame.cpp
    crates/pictura-app/cpp/document_window.cpp
    crates/pictura-app/cpp/document_window.h
    crates/pictura-app/cpp/document_float_window.cpp
    crates/pictura-app/cpp/document_float_window.h
    crates/pictura-app/cpp/document_tile_host.cpp
    crates/pictura-app/cpp/document_tile_host.h
    crates/pictura-app/cpp/frame_document_windows.cpp
    crates/pictura-app/cpp/frame_document_menus.cpp
    crates/pictura-app/cpp/frame_document_arrange.cpp
    crates/pictura-app/cpp/frame_color.cpp
    crates/pictura-app/cpp/frame_open.cpp
    crates/pictura-app/cpp/frame_pdf.cpp
    crates/pictura-app/cpp/frame_import.cpp
    crates/pictura-app/cpp/frame_chrome.cpp
    crates/pictura-app/cpp/frame_ui_scale.cpp
    crates/pictura-app/cpp/frame_columns.cpp
    crates/pictura-app/cpp/frame_session.cpp
    crates/pictura-app/cpp/frame_shortcuts.cpp
    crates/pictura-app/cpp/frame_search.cpp
    crates/pictura-app/cpp/search_bar.cpp
    crates/pictura-app/cpp/search_controls.cpp
    crates/pictura-app/cpp/search_index.cpp
    crates/pictura-app/cpp/frame_menus.cpp
    crates/pictura-app/cpp/frame_save_options.cpp
    crates/pictura-app/cpp/frame_system_info.cpp
    crates/pictura-app/cpp/system_info.cpp
    crates/pictura-app/cpp/system_info_dialog.cpp
    crates/pictura-app/cpp/frame_menus_select.cpp
    crates/pictura-app/cpp/frame_menus_edit.cpp
    crates/pictura-app/cpp/frame_menus_transform.cpp
    crates/pictura-app/cpp/frame_menus_filter.cpp
    crates/pictura-app/cpp/frame_menus_align.cpp
    crates/pictura-app/cpp/frame_menus_type.cpp
    crates/pictura-app/cpp/frame_transform_lock.cpp
    crates/pictura-app/cpp/frame_keys.cpp
    crates/pictura-app/cpp/frame_menus_layer_style.cpp
    crates/pictura-app/cpp/frame_menus_adjust.cpp
    crates/pictura-app/cpp/frame_menus_layer_ops.cpp
    crates/pictura-app/cpp/frame_menus_matting.cpp
    crates/pictura-app/cpp/frame_menus_smart_object.cpp
    crates/pictura-app/cpp/frame_mask_overlay.cpp
    crates/pictura-app/cpp/frame_menus_image.cpp
    crates/pictura-app/cpp/frame_menus_image_mode.cpp
    crates/pictura-app/cpp/frame_recent.cpp
    crates/pictura-app/cpp/frame_recovery.cpp
    crates/pictura-app/cpp/frame_guides.cpp
    crates/pictura-app/cpp/frame_preferences.cpp
    crates/pictura-app/cpp/frame_test.cpp
    crates/pictura-app/cpp/frame_build.cpp
    crates/pictura-app/cpp/frame_workspaces.cpp
    crates/pictura-app/cpp/frame_workspace_controller.cpp
    crates/pictura-app/cpp/frame_canvas.cpp
    crates/pictura-app/cpp/frame_canvas.h
    crates/pictura-app/cpp/panels/layers_panel.cpp
    crates/pictura-app/cpp/panels/layers_panel_target.cpp
    crates/pictura-app/cpp/panels/layers_panel_preview.cpp
    crates/pictura-app/cpp/panels/layers_panel_events.cpp
    crates/pictura-app/cpp/panels/paths_panel.cpp
    crates/pictura-app/cpp/panels/layers_filter_proxy.cpp
    crates/pictura-app/cpp/panels/layers_filter_bar.cpp
    crates/pictura-app/cpp/panels/layers_panel_actions.cpp
    crates/pictura-app/cpp/panels/layers_panel_effects.cpp
    crates/pictura-app/cpp/panels/layers_panel_menu.cpp
    crates/pictura-app/cpp/panels/layers_panel_type_menu.cpp
    crates/pictura-app/cpp/panels/layers_panel_mask_menu.cpp
    crates/pictura-app/cpp/panels/layers_panel_test.cpp
    crates/pictura-app/cpp/panels/layers_panel_test_extra.cpp
    crates/pictura-app/cpp/panels/layers_panel_test_solo.cpp
    crates/pictura-app/cpp/panels/percent_field.cpp
    crates/pictura-app/cpp/panels/numeric_field.cpp
    crates/pictura-app/cpp/panels/color_swatch_widget.cpp
    crates/pictura-app/cpp/panels/angle_dial.cpp
    crates/pictura-app/cpp/panels/ramp_slider.cpp
    crates/pictura-app/cpp/panels/spectrum_bar.cpp
    crates/pictura-app/cpp/panels/curve_widget.cpp
    crates/pictura-app/cpp/panels/history_panel.cpp
    crates/pictura-app/cpp/panels/actions_panel.cpp
    crates/pictura-app/cpp/panels/actions_tree_model.cpp
    crates/pictura-app/cpp/panels/actions_button_view.cpp
    crates/pictura-app/cpp/panels/actions_dialogs.cpp
    crates/pictura-app/cpp/panels/actions_panel_playback.cpp
    crates/pictura-app/cpp/panels/actions_engine_bridge.cpp
    crates/pictura-app/cpp/panels/actions_panel_menu.cpp
    crates/pictura-app/cpp/panels/brush_panel.cpp
    crates/pictura-app/cpp/panels/brush_preset_picker.cpp
    crates/pictura-app/cpp/panels/clone_source_panel.cpp
    crates/pictura-app/cpp/panels/adjustment_controls.cpp
    crates/pictura-app/cpp/panels/character_panel.cpp
    crates/pictura-app/cpp/panels/character_panel_icons.cpp
    crates/pictura-app/cpp/panels/character_styles_panel.cpp
    crates/pictura-app/cpp/panels/paragraph_panel.cpp
    crates/pictura-app/cpp/panels/paragraph_styles_panel.cpp
    crates/pictura-app/cpp/justification_dialog.cpp
    crates/pictura-app/cpp/paragraph_style_dialog.cpp
    crates/pictura-app/cpp/paragraph_style_dialog_pages.cpp
    crates/pictura-app/cpp/font_combo.cpp
    crates/pictura-app/cpp/panels/glyphs_panel.cpp
    crates/pictura-app/cpp/panels/navigator_panel.cpp
    crates/pictura-app/cpp/panels/color_panel.cpp
    crates/pictura-app/cpp/panels/swatches_panel.cpp
    crates/pictura-app/cpp/panels/info_panel.cpp
    crates/pictura-app/cpp/panels/notes_panel.cpp
    crates/pictura-app/cpp/panels/histogram_panel.cpp
    crates/pictura-app/cpp/panels/adjustments_panel.cpp
    crates/pictura-app/cpp/panels/placeholder_panel.cpp
    crates/pictura-app/cpp/panels/properties_panel.cpp
    crates/pictura-app/cpp/panels/channels_panel.cpp
    crates/pictura-app/cpp/panels/character_panel.cpp
    crates/pictura-app/cpp/panels/paragraph_panel.cpp
    crates/pictura-app/cpp/panels/panel_group.cpp
    crates/pictura-app/cpp/panels/panel_group_menu.cpp
    crates/pictura-app/cpp/panels/panel_group_test.cpp
    crates/pictura-app/cpp/panels/panel_column.cpp
    crates/pictura-app/cpp/panels/panel_float.cpp
    crates/pictura-app/cpp/panels/panel_column_float.cpp
    crates/pictura-app/cpp/panels/panel_column_iconic.cpp
    crates/pictura-app/cpp/panels/panel_column_menu.cpp
    crates/pictura-app/cpp/panels/panel_column_drag.cpp
    crates/pictura-app/cpp/panels/panel_column_indicator.cpp
    crates/pictura-app/cpp/panels/panel_column_test.cpp
    crates/pictura-app/cpp/tools.cpp
    crates/pictura-app/cpp/tool_options_json.cpp
    crates/pictura-app/cpp/tool_registry.cpp
    crates/pictura-app/cpp/tool_hand_zoom.cpp
    crates/pictura-app/cpp/tool_eyedropper.cpp
    crates/pictura-app/cpp/tool_brush.cpp
    crates/pictura-app/cpp/tool_colorreplacement.cpp
    crates/pictura-app/cpp/tool_mixerbrush.cpp
    crates/pictura-app/cpp/tool_stamps.cpp
    crates/pictura-app/cpp/tool_eraser.cpp
    crates/pictura-app/cpp/tool_fills.cpp
    crates/pictura-app/cpp/tool_retouch.cpp
    crates/pictura-app/cpp/tool_pen.cpp
    crates/pictura-app/cpp/tool_path_selection.cpp
    crates/pictura-app/cpp/tool_shape.cpp
    crates/pictura-app/cpp/tool_rotate_view.cpp
    crates/pictura-app/cpp/save_for_web_dialog.cpp
    crates/pictura-app/cpp/shape_dialogs.cpp
    crates/pictura-app/cpp/paragraph_text_size_dialog.cpp
    crates/pictura-app/cpp/type_frame.cpp
    crates/pictura-app/cpp/warp_text_dialog.cpp
    crates/pictura-app/cpp/warp_text_dialog.h
    crates/pictura-app/cpp/warp_styles.h
    crates/pictura-app/cpp/screen_mode_message.cpp
    crates/pictura-app/cpp/screen_mode_message.h
    crates/pictura-app/cpp/tool_type.cpp
    crates/pictura-app/cpp/type_key_filter.cpp
    crates/pictura-app/cpp/type_fonts.cpp
    crates/pictura-app/cpp/type_text_edit.cpp
    crates/pictura-app/cpp/tool_wand.cpp
    crates/pictura-app/cpp/tool_move.cpp
    crates/pictura-app/cpp/tool_crop.cpp
    crates/pictura-app/cpp/tool_perspectivecrop.cpp
    crates/pictura-app/cpp/tool_slice.cpp
    crates/pictura-app/cpp/tool_annotations.cpp
    crates/pictura-app/cpp/tool_count.cpp
    crates/pictura-app/cpp/tool_ruler.cpp
    crates/pictura-app/cpp/tool_healing.cpp
    crates/pictura-app/cpp/tool_region_drag.cpp
    crates/pictura-app/cpp/tool_patch.cpp
    crates/pictura-app/cpp/tool_redeye.cpp
    crates/pictura-app/cpp/tool_contentawaremove.cpp
    crates/pictura-app/cpp/tool_selection.cpp
    crates/pictura-app/cpp/tool_magneticlasso.cpp
    crates/pictura-app/cpp/tool_catalog.cpp
    crates/pictura-app/cpp/tool_transform.cpp
    crates/pictura-app/cpp/selection_geometry.cpp
    crates/pictura-app/cpp/tools_marquee.cpp
    crates/pictura-app/cpp/tools_guides.cpp
    crates/pictura-app/cpp/tools_selection_move.cpp
    crates/pictura-app/cpp/toolbox.cpp
    crates/pictura-app/cpp/options_bar.cpp
    crates/pictura-app/cpp/options_bar_crop.cpp
    crates/pictura-app/cpp/options_bar_paint.cpp
    crates/pictura-app/cpp/options_bar_stamp.cpp
    crates/pictura-app/cpp/options_bar_erase.cpp
    crates/pictura-app/cpp/options_bar_fill.cpp
    crates/pictura-app/cpp/options_bar_retouch.cpp
    crates/pictura-app/cpp/options_bar_pen.cpp
    crates/pictura-app/cpp/options_bar_shape.cpp
    crates/pictura-app/cpp/options_bar_move.cpp
    crates/pictura-app/cpp/options_bar_transform.cpp
    crates/pictura-app/cpp/options_bar_eyedropper.cpp
    crates/pictura-app/cpp/options_bar_navigate.cpp
    crates/pictura-app/cpp/options_bar_rotate.cpp
    crates/pictura-app/cpp/options_bar_type.cpp
    crates/pictura-app/cpp/tool_hint_bar.cpp
    crates/pictura-app/cpp/new_document_dialog.cpp
    crates/pictura-app/cpp/layer_new_dialog.cpp
    crates/pictura-app/cpp/pattern_fill_dialog.cpp
    crates/pictura-app/cpp/dialog_chrome.cpp
    crates/pictura-app/cpp/dialogs.cpp
    crates/pictura-app/cpp/keyboard_shortcuts_dialog.cpp
    crates/pictura-app/cpp/design_system_dialog.cpp
    crates/pictura-app/cpp/design_system_dialog_pages.cpp
    crates/pictura-app/cpp/design_system_dialog.h
    crates/pictura-app/cpp/commands.h
    crates/pictura-app/cpp/command_ids.h
    crates/pictura-app/cpp/agent_control.h
    crates/pictura-app/cpp/client_registration.h
    crates/pictura-app/cpp/client_registration_box.h
    crates/pictura-app/cpp/control_approval_bar.h
    crates/pictura-app/cpp/control_policy.h
    crates/pictura-app/cpp/control_scope.h
    crates/pictura-app/cpp/control_server.h
    crates/pictura-app/cpp/control_server_support.h
    crates/pictura-app/cpp/theme.h
    crates/pictura-app/cpp/fonts.h
    crates/pictura-app/cpp/session.h
    crates/pictura-app/cpp/workspace_store.h
    crates/pictura-app/cpp/preferences_agent_page.h
    crates/pictura-app/cpp/preferences_dialog.h
    crates/pictura-app/cpp/pdf_save_dialog.h
    crates/pictura-app/cpp/pdf_presets_dialog.h
    crates/pictura-app/cpp/import_pdf_dialog.h
    crates/pictura-app/cpp/rasterize_eps_dialog.h
    crates/pictura-app/cpp/raster_size_fields.h
    crates/pictura-app/cpp/new_guide_dialog.h
    crates/pictura-app/cpp/file_info_dialog.h
    crates/pictura-app/cpp/profile_dialog.h
    crates/pictura-app/cpp/save_format_options.h
    crates/pictura-app/cpp/jpeg_options_dialog.h
    crates/pictura-app/cpp/png_options_dialog.h
    crates/pictura-app/cpp/bmp_options_dialog.h
    crates/pictura-app/cpp/tiff_options_dialog.h
    crates/pictura-app/cpp/hdr_conversion_dialog.h
    crates/pictura-app/cpp/hdr_toning_dialog.h
    crates/pictura-app/cpp/hdr_toning_controls.h
    crates/pictura-app/cpp/black_white_dialog.h
    crates/pictura-app/cpp/channel_mixer_dialog.h
    crates/pictura-app/cpp/color_balance_dialog.h
    crates/pictura-app/cpp/curves_dialog.h
    crates/pictura-app/cpp/canvas_size_dialog.h
    crates/pictura-app/cpp/exposure_dialog.h
    crates/pictura-app/cpp/gradient_editor_dialog.h
    crates/pictura-app/cpp/gradient_map_dialog.h
    crates/pictura-app/cpp/gradient_noise_page.h
    crates/pictura-app/cpp/hue_saturation_dialog.h
    crates/pictura-app/cpp/image_size_dialog.h
    crates/pictura-app/cpp/levels_dialog.h
    crates/pictura-app/cpp/photo_filter_dialog.h
    crates/pictura-app/cpp/slider_dialog.h
    crates/pictura-app/cpp/replace_color_dialog.h
    crates/pictura-app/cpp/auto_blend_dialog.h
    crates/pictura-app/cpp/match_color_dialog.h
    crates/pictura-app/cpp/pictura_raw_dialog.h
    crates/pictura-app/cpp/acr/acr_settings.h
    crates/pictura-app/cpp/acr/acr_controls.h
    crates/pictura-app/cpp/acr/acr_preview_pane.h
    crates/pictura-app/cpp/acr/acr_histogram.h
    crates/pictura-app/cpp/acr/acr_panels.h
    crates/pictura-app/cpp/color_settings_dialog.h
    crates/pictura-app/cpp/color_picker_dialog.h
    crates/pictura-app/cpp/export_as_dialog.h
    crates/pictura-app/cpp/duplicate_image_dialog.h
    crates/pictura-app/cpp/bitmap_mode_dialog.h
    crates/pictura-app/cpp/indexed_color_dialog.h
    crates/pictura-app/cpp/fill_dialog.h
    crates/pictura-app/cpp/defringe_dialog.h
    crates/pictura-app/cpp/stroke_dialog.h
    crates/pictura-app/cpp/image_view.h
    crates/pictura-app/cpp/pictura_debug_timing.h
    crates/pictura-app/cpp/canvas_range.h
    crates/pictura-app/cpp/canvas_scrollbars.h
    crates/pictura-app/cpp/canvas_ruler.h
    crates/pictura-app/cpp/icons.h
    crates/pictura-app/cpp/adjustment_catalogue.cpp
    crates/pictura-app/cpp/adjustment_catalogue.h
    crates/pictura-app/cpp/svg_icon_engine.h
    crates/pictura-app/cpp/file_drop_router.h
    crates/pictura-app/cpp/frame.h
    crates/pictura-app/cpp/system_info.h
    crates/pictura-app/cpp/system_info_dialog.h
    crates/pictura-app/cpp/panels/layers_panel.h
    crates/pictura-app/cpp/panels/layers_panel_model.h
    crates/pictura-app/cpp/panels/layers_filter_proxy.h
    crates/pictura-app/cpp/panels/layers_filter_bar.h
    crates/pictura-app/cpp/panels/percent_field.h
    crates/pictura-app/cpp/panels/numeric_field.h
    crates/pictura-app/cpp/panels/color_swatch_widget.h
    crates/pictura-app/cpp/panels/jump_slider.h
    crates/pictura-app/cpp/panels/angle_dial.h
    crates/pictura-app/cpp/panels/ramp_slider.h
    crates/pictura-app/cpp/panels/spectrum_bar.h
    crates/pictura-app/cpp/panels/curve_widget.h
    crates/pictura-app/cpp/panels/history_panel.h
    crates/pictura-app/cpp/panels/actions_panel.h
    crates/pictura-app/cpp/panels/actions_tree_model.h
    crates/pictura-app/cpp/panels/actions_button_view.h
    crates/pictura-app/cpp/panels/actions_dialogs.h
    crates/pictura-app/cpp/panels/actions_panel_internal.h
    crates/pictura-app/cpp/panels/actions_engine_bridge.h
    crates/pictura-app/cpp/panels/actions_tool_recording_bridge.h
    crates/pictura-app/cpp/panels/navigator_panel.h
    crates/pictura-app/cpp/panels/color_panel.h
    crates/pictura-app/cpp/panels/swatches_panel.h
    crates/pictura-app/cpp/panels/info_panel.h
    crates/pictura-app/cpp/panels/notes_panel.h
    crates/pictura-app/cpp/panels/histogram_panel.h
    crates/pictura-app/cpp/panels/placeholder_panel.h
    crates/pictura-app/cpp/panels/properties_panel.h
    crates/pictura-app/cpp/panels/channels_panel.h
    crates/pictura-app/cpp/panels/character_panel.h
    crates/pictura-app/cpp/panels/character_panel_icons.h
    crates/pictura-app/cpp/panels/character_styles_panel.h
    crates/pictura-app/cpp/panels/paragraph_panel.h
    crates/pictura-app/cpp/panels/paragraph_styles_panel.h
    crates/pictura-app/cpp/justification_dialog.h
    crates/pictura-app/cpp/paragraph_style_dialog.h
    crates/pictura-app/cpp/panels/panel_group.h
    crates/pictura-app/cpp/panels/panel_column.h
    crates/pictura-app/cpp/tools.h
    crates/pictura-app/cpp/tool_options.h
    crates/pictura-app/cpp/tool_services.h
    crates/pictura-app/cpp/tool_handler.h
    crates/pictura-app/cpp/tool_sessions.h
    crates/pictura-app/cpp/tool_registry.h
    crates/pictura-app/cpp/text_input.h
    crates/pictura-app/cpp/selection_geometry.h
    crates/pictura-app/cpp/toolbox.h
    crates/pictura-app/cpp/options_bar.h
    crates/pictura-app/cpp/tool_hint_bar.h
    crates/pictura-app/cpp/search_bar.h
    crates/pictura-app/cpp/search_index.h
    crates/pictura-app/cpp/new_document_dialog.h
    crates/pictura-app/cpp/layer_new_dialog.h
    crates/pictura-app/cpp/dialog_chrome.h
    crates/pictura-app/cpp/dialogs.h
    crates/pictura-app/cpp/keyboard_shortcuts_dialog.h
    crates/pictura-app/cpp/pictura_pch.h
)

# A shared shell lets each Qt Test executable link one small library instead of
# two large static archives. Release, packaging, and Windows keep the static
# shell so the shipped binary stays self-contained and no symbol exports are
# needed.
if(BUILD_TESTING AND NOT WIN32)
    set(PICTURA_SHELL_SHARED ON)
    add_library(pictura_shell SHARED ${PICTURA_SHELL_SOURCES})
else()
    set(PICTURA_SHELL_SHARED OFF)
    add_library(pictura_shell STATIC ${PICTURA_SHELL_SOURCES})
endif()

# The translation units that include a generated cxx-qt header follow the
# bridge stamp.
pictura_depend_on_bridge_stamp(${PICTURA_SHELL_SOURCES})

# Target-level, so the per-source -O1 below still comes after them.
target_compile_options(pictura_shell PRIVATE ${PICTURA_DEV_COMPILE_OPTIONS})
if(PICTURA_DEV_PCH)
    target_precompile_headers(pictura_shell PRIVATE "${PICTURA_PCH_HEADER}")
endif()

target_include_directories(pictura_shell PUBLIC
    crates/pictura-app/cpp
)
target_compile_definitions(pictura_shell PUBLIC
    PICTURA_VERSION="${PROJECT_VERSION}"
    PICTURA_BUILD_TYPE="$<CONFIG>"
)
# In the shared configuration pictura_app and the resource object are private to
# the library, so the generated cxx-qt headers have to be republished for the
# test executables.
if(PICTURA_SHELL_SHARED)
    get_target_property(_pictura_cxx_qt_export_dir pictura_app CXX_QT_EXPORT_DIR)
    if(_pictura_cxx_qt_export_dir)
        target_include_directories(pictura_shell PUBLIC
            "${_pictura_cxx_qt_export_dir}/crates/pictura_app/include")
    endif()
endif()

# The Rust static lib embeds `pictura-color` (Little CMS 2). On Linux the
# `lcms2-sys` crate links the system library dynamically, so the C++ link needs
# `-llcms2`. On Windows pkg-config finds no system lcms2 and `lcms2-sys`'s static
# fallback compiles the vendored copy into `pictura_app`; macOS does the same by
# enabling lcms2-sys's `static` feature (pictura-color), so neither platform
# links anything external.
if(PICTURA_SHELL_SHARED)
    target_link_libraries(pictura_shell PUBLIC Qt6::Widgets Qt6::Svg Qt6::Network Qt6::PrintSupport)
    target_link_libraries(pictura_shell PRIVATE pictura_app pictura_assets)
else()
    target_link_libraries(pictura_shell PUBLIC pictura_app Qt6::Svg Qt6::Network Qt6::PrintSupport)
endif()
# RHI/private headers: interop.cpp includes <rhi/qrhi.h>. The exported private
# target carries the platform-correct include dir (Debian, macOS framework,
# Windows), so no hand-built qmake-derived path is needed.
target_link_libraries(pictura_shell PRIVATE Qt6::GuiPrivate)
if(UNIX AND NOT APPLE)
    find_library(LCMS2_LIBRARY NAMES lcms2 REQUIRED)
    if(PICTURA_SHELL_SHARED)
        target_link_libraries(pictura_shell PRIVATE "${LCMS2_LIBRARY}")
    else()
        target_link_libraries(pictura_shell PUBLIC "${LCMS2_LIBRARY}")
    endif()
endif()

# The shell is a library so the Qt Test suites under cpp/tests can link the
# UI without a second main. Only main.cpp and the self-test sources stay here.
set(PICTURA_SELF_TEST_SOURCES
    crates/pictura-app/cpp/main.cpp
    crates/pictura-app/cpp/selftest_active_layer.cpp
    crates/pictura-app/cpp/selftest_annotations.cpp
    crates/pictura-app/cpp/selftest_canvas.cpp
    crates/pictura-app/cpp/selftest_canvas_view.cpp
    crates/pictura-app/cpp/selftest_click_deselect.cpp
    crates/pictura-app/cpp/selftest_control.cpp
    crates/pictura-app/cpp/selftest.cpp
    crates/pictura-app/cpp/selftest_crop_group.cpp
    crates/pictura-app/cpp/selftest_crop_tool.cpp
    crates/pictura-app/cpp/selftest_erasers.cpp
    crates/pictura-app/cpp/selftest_healing.cpp
    crates/pictura-app/cpp/selftest_large_doc.cpp
    crates/pictura-app/cpp/selftest_layer_locks.cpp
    crates/pictura-app/cpp/selftest_layers_adjustments.cpp
    crates/pictura-app/cpp/selftest_layers_controls.cpp
    crates/pictura-app/cpp/selftest_layers_drag.cpp
    crates/pictura-app/cpp/selftest_layers_filter.cpp
    crates/pictura-app/cpp/selftest_layers_interactions.cpp
    crates/pictura-app/cpp/selftest_layers_round3.cpp
    crates/pictura-app/cpp/selftest_layers_smart_object.cpp
    crates/pictura-app/cpp/selftest_magnetic_lasso.cpp
    crates/pictura-app/cpp/selftest_numeric.cpp
    crates/pictura-app/cpp/selftest_paint_live.cpp
    crates/pictura-app/cpp/selftest_paint_perf.cpp
    crates/pictura-app/cpp/selftest_paint_tools.cpp
    crates/pictura-app/cpp/selftest_ports.cpp
    crates/pictura-app/cpp/selftest_recent_files.cpp
    crates/pictura-app/cpp/selftest_report.cpp
    crates/pictura-app/cpp/selftest_session.cpp
    crates/pictura-app/cpp/selftest_shell_round3.cpp
    crates/pictura-app/cpp/selftest_shell_round4.cpp
    crates/pictura-app/cpp/selftest_shell_round4_float.cpp
    crates/pictura-app/cpp/selftest_tool_canvas.cpp
    crates/pictura-app/cpp/selftest_tools_selection.cpp
    crates/pictura-app/cpp/selftest_ui_persistence.cpp
    crates/pictura-app/cpp/selftest_visibility.cpp
    crates/pictura-app/cpp/selftest_workspace_input.cpp
    crates/pictura-app/cpp/selftest_active_layer.h
    crates/pictura-app/cpp/selftest_annotations.h
    crates/pictura-app/cpp/selftest_canvas.h
    crates/pictura-app/cpp/selftest_canvas_view.h
    crates/pictura-app/cpp/selftest_click_deselect.h
    crates/pictura-app/cpp/selftest_control.h
    crates/pictura-app/cpp/selftest_crop_group.h
    crates/pictura-app/cpp/selftest_crop_tool.h
    crates/pictura-app/cpp/selftest_erasers.h
    crates/pictura-app/cpp/selftest.h
    crates/pictura-app/cpp/selftest_healing.h
    crates/pictura-app/cpp/selftest_large_doc.h
    crates/pictura-app/cpp/selftest_layer_locks.h
    crates/pictura-app/cpp/selftest_layers_adjustments.h
    crates/pictura-app/cpp/selftest_layers_controls.h
    crates/pictura-app/cpp/selftest_layers_drag.h
    crates/pictura-app/cpp/selftest_layers_filter.h
    crates/pictura-app/cpp/selftest_layers_interactions.h
    crates/pictura-app/cpp/selftest_layers_round3.h
    crates/pictura-app/cpp/selftest_layers_smart_object.h
    crates/pictura-app/cpp/selftest_magnetic_lasso.h
    crates/pictura-app/cpp/selftest_numeric.h
    crates/pictura-app/cpp/selftest_paint_fixture.h
    crates/pictura-app/cpp/selftest_paint_live.h
    crates/pictura-app/cpp/selftest_paint_perf.h
    crates/pictura-app/cpp/selftest_paint_tools.h
    crates/pictura-app/cpp/selftest_ports.h
    crates/pictura-app/cpp/selftest_recent_files.h
    crates/pictura-app/cpp/selftest_report.h
    crates/pictura-app/cpp/selftest_session.h
    crates/pictura-app/cpp/selftest_shell_round3.h
    crates/pictura-app/cpp/selftest_shell_round4_float.h
    crates/pictura-app/cpp/selftest_shell_round4.h
    crates/pictura-app/cpp/selftest_tool_canvas.h
    crates/pictura-app/cpp/selftest_tools_selection.h
    crates/pictura-app/cpp/selftest_ui_persistence.h
    crates/pictura-app/cpp/selftest_visibility.h
    crates/pictura-app/cpp/selftest_workspace_input.h
)

add_executable(pictura ${PICTURA_SELF_TEST_SOURCES})
# No precompiled header here: the self-test sources do not see Qt's keyword
# macros (selftest_report.h declares an `emit` function).
target_compile_options(pictura PRIVATE ${PICTURA_DEV_COMPILE_OPTIONS})
# Menu construction is not on a hot path but compiles slowly; a lighter
# optimization level keeps it off the development critical path.
if(PICTURA_DEV_OPTIMIZED)
    # Its -O1 does not match the precompiled header, which GCC would then read
    # as plain text.
    set_source_files_properties(
            crates/pictura-app/cpp/filter_commands.cpp
        PROPERTIES COMPILE_OPTIONS "$<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-O1>"
                   SKIP_PRECOMPILE_HEADERS ON)
endif()

target_link_libraries(pictura PRIVATE pictura_shell)
if(NOT PICTURA_SHELL_SHARED)
    target_link_libraries(pictura PRIVATE pictura_assets)
endif()

# The self-test sources include the generated cxx-qt header too.
pictura_depend_on_bridge_stamp(${PICTURA_SELF_TEST_SOURCES})

# Windows-only presentation: a GUI-subsystem executable (no console window) with
# an embedded icon and manifest. A plain `add_executable` never runs Qt's
# target-finalization hook, so `qt6_finalize_target` is what makes
# QT_WINDOWS_APP_PROJECT_IDENTIFIER — and Qt's generated manifest, which
# declares Windows 10/11 compatibility — actually apply.
if(WIN32)
    enable_language(RC)
    configure_file(packaging/windows/pictura.rc.in
        "${CMAKE_CURRENT_BINARY_DIR}/pictura.rc" @ONLY)
    target_sources(pictura PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/pictura.rc")
    set_target_properties(pictura PROPERTIES
        WIN32_EXECUTABLE ON
        QT_WINDOWS_APP_PROJECT_IDENTIFIER "io.github.KookaPictura.KookaPictura")
    qt6_finalize_target(pictura)
endif()

# macOS presentation: an application bundle, the form macdeployqt and create-dmg
# operate on. The Info.plist template substitutes the project version, so the
# bundle tracks the x-release-please-version marker.
if(APPLE)
    set_target_properties(pictura PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/packaging/macos/Info.plist.in"
        MACOSX_BUNDLE_BUNDLE_NAME "Kooka Pictura"
        MACOSX_BUNDLE_GUI_IDENTIFIER "io.github.KookaPictura.KookaPictura"
        MACOSX_BUNDLE_ICON_FILE app.icns
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}")
    set_source_files_properties(assets/icons/app.icns PROPERTIES
        MACOSX_PACKAGE_LOCATION Resources)
    target_sources(pictura PRIVATE assets/icons/app.icns)
endif()

if(BUILD_TESTING)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    add_subdirectory(crates/pictura-app/cpp/tests)
endif()

# The AppImage job (.github/workflows/appimage.yml) stages this tree into an
# AppDir; the macOS job (.github/workflows/macos.yml) stages the .app bundle.
# The desktop file and icon share the binary's name so Wayland's default app_id
# ("pictura") resolves to them.
include(GNUInstallDirs)
install(TARGETS pictura
    BUNDLE DESTINATION ${CMAKE_INSTALL_BINDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(NOT WIN32)
    install(FILES packaging/pictura.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
    install(FILES assets/icons/app.png
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/512x512/apps
        RENAME pictura.png)
    install(FILES packaging/pictura.metainfo.xml
        DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)
endif()
