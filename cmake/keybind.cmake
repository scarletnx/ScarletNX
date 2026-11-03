# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# snx_keybind: a runtime hotkey's setting (SDL's key name with modifiers), the event that matches it, and the text a
# captured event takes. The runtime matches the key and the launcher captures it, so both write it alike.
include_guard(GLOBAL)
if(NOT TARGET snx_keybind)
add_library(snx_keybind STATIC "${PROJECT_SOURCE_DIR}/src/common/utilities/keybind.c")
target_link_libraries(snx_keybind PUBLIC scarletnx-common SDL3::SDL3)
target_compile_options(snx_keybind PRIVATE -Wall -Wextra -Werror=missing-prototypes)
endif()
