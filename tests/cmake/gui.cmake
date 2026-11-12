# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# The tests of the GUI (src/gui), included by src/gui/CMakeLists.txt where they were: they run in its scope, so its
# variables hold here, and a relative path is from src/gui
if(BUILD_TESTING)
    add_executable(snx-test-gui-model "${PROJECT_SOURCE_DIR}/tests/gui_model.c")
    target_link_libraries(snx-test-gui-model PRIVATE snx_gui_support)
    add_test(NAME gui_model COMMAND snx-test-gui-model)
    # what tools/uigen makes of tests/ui_test.txt, built and run: layouts, drawing, hit tests, actions, hotkeys
    set(gui_ui_test "${CMAKE_CURRENT_BINARY_DIR}/uitest")
    add_custom_command(OUTPUT "${gui_ui_test}/gui/ui.h" "${gui_ui_test}/gui/ui.c"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${gui_ui_test}/gui"
        COMMAND snx-uigen --out "${gui_ui_test}" "${PROJECT_SOURCE_DIR}/tests/ui_test.txt"
        DEPENDS snx-uigen "${PROJECT_SOURCE_DIR}/tests/ui_test.txt"
        COMMENT "Generating tests/ui_test.txt's scenes" VERBATIM)
    add_executable(snx-test-gui-ui "${PROJECT_SOURCE_DIR}/tests/gui_ui.c" "${gui_ui_test}/gui/ui.c")
    target_include_directories(snx-test-gui-ui PRIVATE "${gui_ui_test}")
    target_link_libraries(snx-test-gui-ui PRIVATE snx_gui_support SDL3::SDL3) # SDL3: its keycodes' header
    add_test(NAME gui_ui COMMAND snx-test-gui-ui)
    add_executable(snx-test-gui-cia-browser "${PROJECT_SOURCE_DIR}/tests/gui_cia_browser.c")
    target_link_libraries(snx-test-gui-cia-browser PRIVATE snx_gui_support scarletnx-common)
    add_test(NAME gui_cia_browser COMMAND snx-test-gui-cia-browser)
    # a GPU driver package's zip added, refused, listed and removed, and the driver a game's run is passed
    add_executable(snx-test-gui-gpu-driver "${PROJECT_SOURCE_DIR}/tests/gui_gpu_driver.c")
    target_link_libraries(snx-test-gui-gpu-driver PRIVATE snx_gui_support scarletnx-common)
    add_test(NAME gui_gpu_driver COMMAND snx-test-gui-gpu-driver)
    # Download Driver's lists: GitHub's releases of Eden's repositories, the marks, the recommendation by model
    add_executable(snx-test-gui-gpu-driver-fetch "${PROJECT_SOURCE_DIR}/tests/gui_gpu_driver_fetch.c")
    target_link_libraries(snx-test-gui-gpu-driver-fetch PRIVATE snx_gui_support scarletnx-common)
    add_test(NAME gui_gpu_driver_fetch COMMAND snx-test-gui-gpu-driver-fetch)
    add_executable(snx-test-gui-recent "${PROJECT_SOURCE_DIR}/tests/gui_recent.c")
    target_link_libraries(snx-test-gui-recent PRIVATE snx_gui_support scarletnx-common)
    add_test(NAME gui_recent COMMAND snx-test-gui-recent)
    add_executable(snx-test-gui-queue "${PROJECT_SOURCE_DIR}/tests/gui_queue.c")
    target_link_libraries(snx-test-gui-queue PRIVATE snx_gui_support)
    add_test(NAME gui_queue COMMAND snx-test-gui-queue)
    add_executable(snx-test-gui-pad "${PROJECT_SOURCE_DIR}/tests/gui_pad.c")
    target_link_libraries(snx-test-gui-pad PRIVATE snx_gui_pad)
    add_test(NAME gui_pad COMMAND snx-test-gui-pad)
    add_executable(snx-test-gui-steps "${PROJECT_SOURCE_DIR}/tests/gui_steps.c")
    target_link_libraries(snx-test-gui-steps PRIVATE snx_gui_support)
    add_test(NAME gui_steps COMMAND snx-test-gui-steps)
    add_executable(snx-test-gui-theme "${PROJECT_SOURCE_DIR}/tests/gui_theme.c")
    target_link_libraries(snx-test-gui-theme PRIVATE snx_gui_support scarletnx-common)
    target_compile_definitions(snx-test-gui-theme PRIVATE SNX_GUI_THEMES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/themes")
    target_compile_definitions(snx-test-gui-theme PRIVATE SNX_GUI_DOCS="${PROJECT_SOURCE_DIR}/docs/user/gui.md")
    target_include_directories(snx-test-gui-theme PRIVATE "${PROJECT_BINARY_DIR}/generated")
    add_test(NAME gui_theme COMMAND snx-test-gui-theme)
    add_executable(snx-test-gui-theme-write "${PROJECT_SOURCE_DIR}/tests/gui_theme_write.c")
    target_link_libraries(snx-test-gui-theme-write PRIVATE snx_gui_support scarletnx-common)
    add_test(NAME gui_theme_write COMMAND snx-test-gui-theme-write)
    add_executable(snx-test-gui-editor "${PROJECT_SOURCE_DIR}/tests/gui_editor.c")
    target_link_libraries(snx-test-gui-editor PRIVATE snx_gui_support scarletnx-common)
    add_test(NAME gui_editor COMMAND snx-test-gui-editor)
    add_executable(snx-test-gui-clean "${PROJECT_SOURCE_DIR}/tests/gui_clean.c")
    target_link_libraries(snx-test-gui-clean PRIVATE snx_gui_support scarletnx-common)
    add_test(NAME gui_clean COMMAND snx-test-gui-clean)
    add_executable(snx-test-gui-settings "${PROJECT_SOURCE_DIR}/tests/gui_settings.c")
    target_link_libraries(snx-test-gui-settings PRIVATE snx_gui_support scarletnx-common)
    if(SNX_BUILD_NX) # and the runtime's own list
        add_test(NAME gui_settings COMMAND snx-test-gui-settings $<TARGET_FILE:scarletnx>)
    else()
        add_test(NAME gui_settings COMMAND snx-test-gui-settings)
    endif()
    add_executable(snx-test-gui-i18n "${PROJECT_SOURCE_DIR}/tests/gui_i18n.c")
    target_link_libraries(snx-test-gui-i18n PRIVATE snx_i18n snx_gui_languages_test)
    add_test(NAME gui_i18n COMMAND snx-test-gui-i18n)
    # po/scarletnx.pot is the code's phrases, and every po/*.po compiles; the script refuses what it should
    add_test(NAME gui_languages_pot COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/scripts/build-languages.py"
             --check)
    add_test(NAME gui_languages_script COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/languages_test.py")
    if(NOT CMAKE_CROSSCOMPILING)
        # tools/uigen on a ui.txt of its own: what it generates, and each file it refuses, at its line
        add_test(NAME uigen COMMAND "${CMAKE_COMMAND}" "-DUIGEN=$<TARGET_FILE:snx-uigen>"
                 "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/uigen-test" -P "${PROJECT_SOURCE_DIR}/tests/uigen.cmake")
    endif()
    # unifont-latin.hex holds the Latin letters CP437 lacks, unifont-titles.hex every code point of the titles' blocks,
    # unifont-wide.hex the other wide ones the languages show, each once, in order
    add_test(NAME gui_glyphs_kept COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/scripts/build-glyphs.py" --check)
    add_test(NAME gui_glyphs_script COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/glyphs_test.py")
    add_executable(snx-test-gui-log "${PROJECT_SOURCE_DIR}/tests/gui_log.c")
    target_link_libraries(snx-test-gui-log PRIVATE snx_gui_support scarletnx-common)
    add_test(NAME gui_log COMMAND snx-test-gui-log)
    add_executable(snx-test-gui-logzip "${PROJECT_SOURCE_DIR}/tests/gui_logzip.c")
    target_link_libraries(snx-test-gui-logzip PRIVATE snx_gui_support scarletnx-common)
    add_test(NAME gui_logzip COMMAND snx-test-gui-logzip)
    # cmake/gui_icons.cmake compiles icons.hex, and refuses a glyph missing, blank or out of its place by its icon
    add_test(NAME gui_icons COMMAND "${CMAKE_COMMAND}" "-DSOURCE=${PROJECT_SOURCE_DIR}"
             "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/gui-icons-test" -P "${PROJECT_SOURCE_DIR}/tests/gui_icons.cmake")
    add_executable(snx-test-gui-process "${PROJECT_SOURCE_DIR}/tests/gui_process.c")
    target_link_libraries(snx-test-gui-process PRIVATE snx_gui_support scarletnx-common)
    add_test(NAME gui_process_tree COMMAND snx-test-gui-process)
    # qemu-user answers /proc/<pid>/stat for its own process with no resident size, which the test measures
    set_tests_properties(gui_process_tree PROPERTIES TIMEOUT 30)
    if(CMAKE_CROSSCOMPILING_EMULATOR)
        set_tests_properties(gui_process_tree PROPERTIES DISABLED TRUE)
    endif()
    add_executable(snx-test-gui-sdl "${PROJECT_SOURCE_DIR}/tests/gui_sdl.c")
    if(WIN32) # a crash there says where it was, not only ctest's SegFault
        target_sources(snx-test-gui-sdl PRIVATE "${PROJECT_SOURCE_DIR}/tests/win32_crash.c")
    endif()
    # a language of its own (tests/po), not the menu's: the screens' checks hold whatever po/ has
    snx_gui_languages(snx_gui_languages_test "${PROJECT_SOURCE_DIR}/tests/po"
                      "${PROJECT_BINARY_DIR}/generated/gui_languages_test.c")
    # ui.txt read back: its phones' rows a finger's, as main.c's GUI_FINGER_ROW
    target_compile_definitions(snx-test-gui-sdl PRIVATE SNX_GUI_UI_TXT="${PROJECT_SOURCE_DIR}/src/gui/ui.txt")
    target_link_libraries(snx-test-gui-sdl PRIVATE snx_gui_support snx_gui_ui scarletnx-common SDL3::SDL3
                          snx_window_icon snx_build_info snx_pad_sdl snx_gui_pad snx_splash snx_gui_languages_test snx_demangle)
    if(APPLE) # it compiles main.c itself, which calls the frontend (apple.m and its Swift), as scarletnx-gui links it
        target_sources(snx-test-gui-sdl PRIVATE apple.m)
        target_link_libraries(snx-test-gui-sdl PRIVATE snx_apple_frontend "-framework Foundation")
        if(IOS)
            target_link_libraries(snx-test-gui-sdl PRIVATE "-framework UIKit")
        endif()
        set_target_properties(snx-test-gui-sdl PROPERTIES LINKER_LANGUAGE CXX)
    endif()
    # the runtime's sections and pages, each of which has an icon in the Settings; the key sentences, each key of which
    # has an icon in F1's box
    target_compile_definitions(snx-test-gui-sdl PRIVATE
                               SNX_GUI_SETTINGS_TXT="${PROJECT_SOURCE_DIR}/src/nx/runtime/settings.txt"
                               SNX_GUI_POT="${PROJECT_SOURCE_DIR}/po/scarletnx.pot")
    # the screens' tests: no file picker of the desktop's, as the portal would open one on the screen, DBus being there
    # without a display. A Mac's TMPDIR, /var/folders/..., is long enough to wrap the paths the screens show, and /var
    # and /tmp are links into /private, which the checks compare as text: /private/tmp is short and real
    set(gui_screen_env SDL_VIDEO_DRIVER=dummy SDL_RENDER_DRIVER=software SDL_FILE_DIALOG_DRIVER=none
        SDL_PREFERRED_LOCALES=en_US)
    if(APPLE)
        list(APPEND gui_screen_env "TMPDIR=/private/tmp")
    endif()
    if(NOT IOS)
        add_test(NAME gui_sdl_events COMMAND snx-test-gui-sdl)
        # 20 to 27 s under the sanitizers in CI, where 30 stopped it (#1638)
        set_tests_properties(gui_sdl_events PROPERTIES TIMEOUT 60 ENVIRONMENT "${gui_screen_env}")
    endif()
    # the setup's pages (gui_setup.c), as gui_sdl's: the real renderer, a language of the tests' own, no picker
    add_executable(snx-test-gui-setup "${PROJECT_SOURCE_DIR}/tests/gui_setup.c")
    target_link_libraries(snx-test-gui-setup PRIVATE snx_gui_support snx_gui_ui scarletnx-common SDL3::SDL3
                          snx_window_icon snx_build_info snx_pad_sdl snx_gui_pad snx_splash snx_gui_languages_test snx_demangle)
    if(APPLE)
        target_sources(snx-test-gui-setup PRIVATE apple.m)
        target_link_libraries(snx-test-gui-setup PRIVATE snx_apple_frontend "-framework Foundation")
        if(IOS)
            target_link_libraries(snx-test-gui-setup PRIVATE "-framework UIKit")
        endif()
        set_target_properties(snx-test-gui-setup PROPERTIES LINKER_LANGUAGE CXX)
    endif()
    if(NOT IOS)
        add_test(NAME gui_setup COMMAND snx-test-gui-setup)
        set_tests_properties(gui_setup PROPERTIES TIMEOUT 30 ENVIRONMENT "${gui_screen_env}")
    endif()
    # every test here, as snx_test_target does: warnings are errors, the asserts stay in any build type, and the
    # sanitizers come with SNX_TEST_SANITIZERS
    get_directory_property(gui_tests BUILDSYSTEM_TARGETS)
    list(FILTER gui_tests INCLUDE REGEX "^snx-test-")
    foreach(test IN LISTS gui_tests)
        target_compile_options(${test} PRIVATE -Wall -Wextra -Werror -UNDEBUG)
    endforeach()
    snx_test_sanitize(${gui_tests})
endif()
if(COMMAND snx_sanitizer_path) # tests/CMakeLists.txt: this folder's tests
    snx_sanitizer_path()
endif()
