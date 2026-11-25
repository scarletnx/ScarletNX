# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# snx_vpad: the on-screen controls (src/common/utilities/vpad.c): where they are, what a finger on them presses, what
# Auto shows, their settings and their quads, for every console's window. The YoRHa theme's colours come from the same
# files as the Switch's menu (cmake/gui_themes.cmake). snx_settings_runtime: their tables (g_vpad_ini,
# g_vpad_shown_ini), whose set functions are here
include_guard(GLOBAL)
include("${CMAKE_CURRENT_LIST_DIR}/gui_themes.cmake")
add_library(snx_vpad STATIC "${PROJECT_SOURCE_DIR}/src/common/utilities/vpad.c")
target_include_directories(snx_vpad PRIVATE "${PROJECT_BINARY_DIR}/generated")
target_link_libraries(snx_vpad PUBLIC scarletnx-common snx_settings_runtime)
set_target_properties(snx_vpad PROPERTIES C_STANDARD 23 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_compile_options(snx_vpad PRIVATE -pedantic-errors -Wall -Wextra -Werror=missing-prototypes
                                        -Werror=missing-variable-declarations)
