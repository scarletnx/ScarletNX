# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# snx_error: what stops a game as the person playing meets it, the same for every console's runtime, the launcher and
# the error panel's own process (src/common/utilities/error_kind.c): its kinds, their lines and their actions, and the
# text an error shows (fatal_screen.c: rows, the font's bytes, Share Log's file, the homes as ~)
include_guard(GLOBAL)
add_library(snx_error STATIC "${PROJECT_SOURCE_DIR}/src/common/utilities/error_kind.c"
                             "${PROJECT_SOURCE_DIR}/src/common/utilities/fatal_screen.c")
target_link_libraries(snx_error PUBLIC scarletnx-common)
set_target_properties(snx_error PROPERTIES C_STANDARD 23 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_compile_options(snx_error PRIVATE -pedantic-errors -Wall -Wextra -Werror=missing-prototypes
                                         -Werror=missing-variable-declarations)
# snx_error_panel: the error's panel, laid out as the game's menu lays out its dialogs (error_panel.c), in quads of fills
# and letters; the menu's scale and width are the notices' (snx_notice, which a build with SDL3 has)
if(TARGET snx_notice)
    include("${CMAKE_CURRENT_LIST_DIR}/gui_themes.cmake")
    add_library(snx_error_panel STATIC "${PROJECT_SOURCE_DIR}/src/common/utilities/error_panel.c")
    target_include_directories(snx_error_panel PRIVATE "${PROJECT_BINARY_DIR}/generated") # the YoRHa colours
    target_link_libraries(snx_error_panel PUBLIC snx_error snx_notice scarletnx-common)
    set_target_properties(snx_error_panel PROPERTIES C_STANDARD 23 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    target_compile_options(snx_error_panel PRIVATE -pedantic-errors -Wall -Wextra -Werror=missing-prototypes
                                                   -Werror=missing-variable-declarations)
endif()
