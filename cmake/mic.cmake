# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# snx_mic: the microphone every console's game hears (src/common/utilities/mic.c): the host's recording device (SDL,
# src/common/platform/mic.c), its noise reduction (src/common/utilities/denoise.c: RNNoise built in, the system's
# libspeexdsp when it is there) and the recording a game's clock takes from
include_guard(GLOBAL)
add_library(snx_mic STATIC "${PROJECT_SOURCE_DIR}/src/common/platform/mic.c"
                           "${PROJECT_SOURCE_DIR}/src/common/utilities/denoise.c"
                           "${PROJECT_SOURCE_DIR}/src/common/utilities/mic.c")
target_link_libraries(snx_mic PUBLIC scarletnx-common
                              PRIVATE SDL3::SDL3 snx_rnnoise snx_tracy_client scarletnx-platform)
set_target_properties(snx_mic PROPERTIES C_STANDARD 23 C_STANDARD_REQUIRED ON)
target_compile_options(snx_mic PRIVATE -Wall -Wextra -Werror=missing-prototypes -Werror=missing-variable-declarations)
if(NOT WIN32)
    target_link_libraries(snx_mic PRIVATE m) # denoise.c's lrintf
endif()
