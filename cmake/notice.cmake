# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# snx_notice: a notice over a running game where the console's window has no menu of its own (the 3DS's, the Vita's,
# the GBA's), drawn as the Switch's game draws its own (src/common/utilities/notice.c), and a key's text as a setting
# holds it (key_text.c), which the Switch's runtime keys and the 3DS's and the Vita's shortcuts read alike. The YoRHa
# theme's colours come from the same files as the Switch's menu (cmake/gui_themes.cmake)
include_guard(GLOBAL)
include("${CMAKE_CURRENT_LIST_DIR}/gui_themes.cmake")
add_library(snx_notice STATIC "${PROJECT_SOURCE_DIR}/src/common/utilities/notice.c"
                              "${PROJECT_SOURCE_DIR}/src/common/utilities/key_text.c")
target_include_directories(snx_notice PRIVATE "${PROJECT_BINARY_DIR}/generated")
target_link_libraries(snx_notice PUBLIC scarletnx-common SDL3::SDL3)
target_compile_options(snx_notice PRIVATE -Wall -Wextra -Werror=missing-prototypes)
