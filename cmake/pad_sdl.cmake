# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# snx_pad_sdl: what an SDL gamepad's buttons are as pad_map.h's controls, held or not, and the labels on its face. The
# runtime maps a game's buttons with it and the launcher draws the same map, so both read a pad alike
include_guard(GLOBAL)
add_library(snx_pad_sdl STATIC "${PROJECT_SOURCE_DIR}/src/common/utilities/pad_sdl.c"
                               "${PROJECT_SOURCE_DIR}/src/common/utilities/key_text.c") # a key as a setting names it
target_link_libraries(snx_pad_sdl PUBLIC scarletnx-common SDL3::SDL3)
target_compile_options(snx_pad_sdl PRIVATE -Wall -Wextra -Werror=missing-prototypes)
