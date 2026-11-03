# Qt Test suites for the C++ shell. Explicit list, no globbing (repo convention).
set(PICTURA_QT_TESTS
    tst_smoke
    tst_gpu_backend
    tst_gpu_fallback
    tst_offthread_jobs
    tst_open_save
    tst_shp
    tst_shp_view
    tst_shp_unit
    tst_tmp
    tst_mix
    tst_multi_file_open
    tst_document_api
    tst_document_session
    tst_selection_api
    tst_paint_api
    tst_transform_api
    tst_fonts
    tst_design_system
    tst_design_system_dialog
    tst_ui_scale
    tst_edit_clipboard
    tst_edit_refresh
    tst_mask_edit
    tst_layers_mask_commands
    tst_last_layer
    tst_client_registration
    tst_control_approval
    tst_control_editing
    tst_control_limits
    tst_control_live
    tst_control_metrics
    tst_control_metrics_off
    tst_control_modal_guard
    tst_control_policy
    tst_control_serial
    tst_preferences_agent
    tst_image_ops
    tst_image_rotate
    tst_crop_straighten
    tst_crop_shield
    tst_crop_settings
    tst_crop_grip
    tst_crop_snap
    tst_crop_session
    tst_crop_options
    tst_crop_preview
    tst_crop_cursor
    tst_session_ports
    tst_perspective_crop
    tst_canvas_pan
    tst_initial_view
    tst_new_document
    tst_image_mode
    tst_hdr_conversion
    tst_layers_panel
    tst_layer_target
    tst_linked_layers
    tst_layers_panel_fixes
    tst_layers_panel_bundle
    tst_layers_row_menu
    tst_layers_preview_coalescing
    tst_layers_snapshot
    tst_canvas_backdrop
    tst_present_cache
    tst_damage_bounds
    tst_fill_layer_editor
    tst_align_distribute
    tst_options_bars_parity
    tst_transform_options
    tst_transform_controls
    tst_transform_skew
    tst_transform_distort
    tst_transform_perspective
    tst_transform_lock
    tst_transform_overlay
    tst_transform_warp
    tst_transform_commands
    tst_command_tree
    tst_command_bus
    tst_merge_commands
    tst_layer_commands
    tst_fill_tools
    tst_eraser_tools
    tst_retouch_tools
    tst_stamp_hold
    tst_pen_tools
    tst_path_selection_tools
    tst_shape_tools
    tst_shape_transform
    tst_rotate_view
    tst_save_for_web
    tst_export_as
    tst_webp
    tst_type_tools
    tst_type_panels
    tst_type_color_preview
    tst_paragraph_type
    tst_type_on_path
    tst_move_feedback
    tst_smart_guides
    tst_tool_options_persistence
    tst_character_paragraph_panels
    tst_paragraph_styles_panel
    tst_character_styles_panel
    tst_type_preferences
    tst_warp_text
    tst_type_placeholder
    tst_text_gamma_fonts
    tst_shared_widgets
    tst_info_panel
    tst_system_info
    tst_histogram_panel
    tst_tool_hint_bar
    tst_swatches_panel
    tst_color_picker
    tst_eyedropper
    tst_color_settings
    tst_profile_dialog
    tst_profile_warnings
    tst_save_options
    tst_save_format_options
    tst_properties_panel
    tst_properties_preview
    tst_adjustments_panel
    tst_actions_panel
    tst_channels_panel
    tst_paths_panel
    tst_icon_assets
    tst_filter_menu
    tst_filter_gallery
    tst_lens_flare_dialog
    tst_picture_frame_dialog
    tst_reduce_noise_dialog
    tst_lighting_effects_dialog
    tst_shear_dialog
    tst_displace
    tst_de_interlace
    tst_custom_filter
    tst_digimarc_watermark
    tst_shape_blur
    tst_smart_blur
    tst_lens_blur_dialog
    tst_blur_gallery_dialog
    tst_tilt_shift
    tst_blur_gallery_effects
    tst_path_blur
    tst_spin_blur
    tst_liquify_dialog
    tst_adaptive_wide_angle
    tst_lens_correction
    tst_lens_profiles
    tst_vanishing_point
    tst_vanishing_point_retouch
    tst_vanishing_point_files
    tst_layer_style
    tst_blend_mode_list
    tst_color_range
    tst_refine_edge
    tst_replace_color
    tst_match_color
    tst_auto_blend
    tst_print
    tst_save_pdf
    tst_import_pdf
    tst_image_adjustments
    tst_image_import
    tst_layers_smart_filters
    tst_smart_filter_apply
    tst_layers_effects
    tst_smart_object_actions
    tst_edit_shortcuts
    tst_workspaces
    tst_floating_workspaces
    tst_floating_workspaces_restart
    tst_zoom_tool
    tst_large_document
    tst_tool_commit
    tst_recovery
    tst_session_store
    tst_rulers_guides
    tst_panels_parity
    tst_selection_alt
    tst_selection_move
    tst_selection_preview
    tst_nested_layer_target
    tst_screen_modes
    tst_dialog_chrome
    tst_keyboard_shortcuts
    tst_search_bar
    tst_native_menu_bar
    tst_loading_tab
    tst_document_windows
    tst_document_float
    tst_document_arrange
    tst_document_navigate
    tst_pictura_raw
    tst_acr_settings
    tst_acr_dialog
    tst_canvas_context_menus
    tst_brush_tip_preview
    tst_untrusted_input
    tst_theme_apply
    tst_test_support
)

file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/qt-test-results)

# The slow suites are registered in several entries, so the layer's wall time
# is set by the number of suites and the parallel level rather than by one
# slow suite. Each entry runs a share of the suite's test functions: the
# share comes from the PICTURA_TEST_SHARD environment variable, and the
# functions are distributed over the shares in declaration order.
#
# Only suites whose test functions are independent of each other belong here.
# A suite may share one window between its test functions and stay here when
# its per-function reset (init() and cleanup()) makes every function
# independent, so each passes run alone by name and in any share; a suite
# whose functions build on each other's state keeps one entry.
set(PICTURA_QT_TEST_SHARDS
    tst_document_arrange
    tst_document_float
    tst_document_windows
    tst_edit_refresh
    tst_floating_workspaces
    tst_import_pdf
    tst_loading_tab
    tst_print
    tst_save_pdf
    tst_ui_scale
    tst_workspaces
)
set(PICTURA_QT_TEST_SHARD_COUNT 4)
# A suite whose slowest functions land in one share of four gets more shares.
set(tst_ui_scale_SHARD_COUNT 8)

# The suites share one precompiled header, built by this object library with
# their own flags: they cannot reuse the shell's, which is compiled position
# independent for the shared library and with full debug info.
if(PICTURA_DEV_PCH)
    add_library(pictura_test_pch OBJECT test_pch.cpp)
    set_target_properties(pictura_test_pch PROPERTIES AUTOMOC OFF)
    target_link_libraries(pictura_test_pch PRIVATE pictura_shell Qt6::Test)
    target_compile_options(pictura_test_pch PRIVATE ${PICTURA_DEV_COMPILE_OPTIONS}
        "$<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-g1>")
    target_precompile_headers(pictura_test_pch PRIVATE "${PICTURA_PCH_HEADER}")
endif()

set(PICTURA_QT_TEST_SOURCES "")
foreach(test_name IN LISTS PICTURA_QT_TESTS)
    list(APPEND PICTURA_QT_TEST_SOURCES ${test_name}.cpp)
    add_executable(${test_name} ${test_name}.cpp)
    if(PICTURA_SHELL_SHARED)
        target_link_libraries(${test_name} PRIVATE pictura_shell Qt6::Test)
    else()
        target_link_libraries(${test_name} PRIVATE pictura_shell Qt6::Test pictura_assets)
    endif()
    # Test debug info only needs line tables; the suites are not debugged through
    # full variable info, and this keeps the test objects and their links small.
    target_compile_options(${test_name} PRIVATE ${PICTURA_DEV_COMPILE_OPTIONS}
        "$<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-g1>")
    if(TARGET pictura_test_pch)
        target_precompile_headers(${test_name} REUSE_FROM pictura_test_pch)
    endif()

    if(test_name IN_LIST PICTURA_QT_TEST_SHARDS)
        set(_pictura_shard_count ${PICTURA_QT_TEST_SHARD_COUNT})
        if(DEFINED ${test_name}_SHARD_COUNT)
            set(_pictura_shard_count ${${test_name}_SHARD_COUNT})
        endif()
        math(EXPR _pictura_last_shard "${_pictura_shard_count} - 1")
        foreach(_shard RANGE 0 ${_pictura_last_shard})
            set(_entry "${test_name}_${_shard}")
            add_test(NAME ${_entry} COMMAND ${test_name}
                -o ${CMAKE_BINARY_DIR}/qt-test-results/${_entry}.xml,junitxml)
            set_tests_properties(${_entry} PROPERTIES
                ENVIRONMENT "QT_QPA_PLATFORM=offscreen;PICTURA_TEST_SHARD=${_shard}/${_pictura_shard_count}"
                TIMEOUT 120
            )
        endforeach()
    else()
        add_test(NAME ${test_name} COMMAND ${test_name}
            -o ${CMAKE_BINARY_DIR}/qt-test-results/${test_name}.xml,junitxml)
        set_tests_properties(${test_name} PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
            TIMEOUT 120
        )
    endif()
endforeach()

# The suites include the generated cxx-qt header, so they follow the bridge
# stamp like the shell does.
pictura_depend_on_bridge_stamp(${PICTURA_QT_TEST_SOURCES})
