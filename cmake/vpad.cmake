# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# snx_vpad: the on-screen controls (src/common/utilities/vpad.c): where they are, what a finger on them presses, what
# Auto shows, their settings and their quads, for every console's window, and the quads as rectangles (ui_rects.c);
# the showcase and the stats over the game (vpad_showcase.c, stats.c). The
# YoRHa theme's colours come from the same files as the Switch's menu (cmake/gui_themes.cmake). snx_settings_runtime:
# their tables (g_vpad_ini, g_vpad_shown_ini) and the touch screen's (g_backend_ini), whose set functions are here
include_guard(GLOBAL)
include("${CMAKE_CURRENT_LIST_DIR}/gui_themes.cmake")
add_library(snx_vpad STATIC "${PROJECT_SOURCE_DIR}/src/common/utilities/vpad.c"
                            "${PROJECT_SOURCE_DIR}/src/common/utilities/ui_rects.c"
                            "${PROJECT_SOURCE_DIR}/src/common/utilities/ui_ring.c"
                            "${PROJECT_SOURCE_DIR}/src/common/utilities/vpad_showcase.c"
                            "${PROJECT_SOURCE_DIR}/src/common/utilities/stats.c")
target_include_directories(snx_vpad PRIVATE "${PROJECT_BINARY_DIR}/generated")
target_link_libraries(snx_vpad PUBLIC scarletnx-common snx_settings_runtime)
set_target_properties(snx_vpad PROPERTIES C_STANDARD 23 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_compile_options(snx_vpad PRIVATE -pedantic-errors -Wall -Wextra -Werror=missing-prototypes
                                        -Werror=missing-variable-declarations)
# which controls each console has (src/common/utilities/vpad_consoles.c): tables alone, the launcher's too
add_library(snx_vpad_consoles STATIC "${PROJECT_SOURCE_DIR}/src/common/utilities/vpad_consoles.c")
target_link_libraries(snx_vpad_consoles PUBLIC scarletnx-common snx_settings)
set_target_properties(snx_vpad_consoles PROPERTIES C_STANDARD 23 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_compile_options(snx_vpad_consoles PRIVATE -pedantic-errors -Wall -Wextra -Werror=missing-variable-declarations)
# and in an SDL window (src/common/utilities/vpad_sdl.c), which the Switch's and every other console's share, with the
# runtime's keys (src/common/utilities/hotkeys.c: settings.txt's Shortcuts, a key as SDL names it)
if(TARGET SDL3::SDL3)
    include("${CMAKE_CURRENT_LIST_DIR}/notice.cmake") # the menu's letters' scale
    include("${CMAKE_CURRENT_LIST_DIR}/pad_sdl.cmake") # a key's text (key_text.c)
    include("${CMAKE_CURRENT_LIST_DIR}/window_icon.cmake") # a window to fullscreen and back
    add_library(snx_vpad_sdl STATIC "${PROJECT_SOURCE_DIR}/src/common/utilities/vpad_sdl.c"
                                    "${PROJECT_SOURCE_DIR}/src/common/utilities/hotkeys.c")
    target_include_directories(snx_vpad_sdl PRIVATE "${PROJECT_BINARY_DIR}/generated")
    target_link_libraries(snx_vpad_sdl PUBLIC snx_vpad SDL3::SDL3 snx_pad_sdl snx_console_settings
                                       snx_tracy_client # F10's capture (cmake/tracy.cmake)
                                       PRIVATE snx_notice snx_window_icon)
    set_target_properties(snx_vpad_sdl PROPERTIES C_STANDARD 23 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    target_compile_options(snx_vpad_sdl PRIVATE -pedantic-errors -Wall -Wextra -Werror=missing-prototypes
                                                -Werror=missing-variable-declarations)
    # the game's menu over the game, the Switch's and every console's (src/common/utilities/overlay.c, its runtime's
    # parts through SnxMenuHost), and F1's text (help.c): its dialogs' text is the error's (snx_error's fatal_screen.c)
    include("${CMAKE_CURRENT_LIST_DIR}/error.cmake")
    add_library(snx_menu STATIC "${PROJECT_SOURCE_DIR}/src/common/utilities/overlay.c"
                                "${PROJECT_SOURCE_DIR}/src/common/utilities/help.c"
                                "${PROJECT_SOURCE_DIR}/src/common/utilities/menu_sdl.c"
                                "${PROJECT_SOURCE_DIR}/src/common/utilities/menu_window.c")
    target_include_directories(snx_menu PRIVATE "${PROJECT_BINARY_DIR}/generated")
    target_link_libraries(snx_menu PUBLIC snx_vpad_sdl snx_notice snx_error snx_settings_runtime)
    set_target_properties(snx_menu PROPERTIES C_STANDARD 23 C_STANDARD_REQUIRED ON)
    target_compile_options(snx_menu PRIVATE -Wall -Wextra -Werror=missing-prototypes)
endif()
