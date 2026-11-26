# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# snx_error: what stops a game as the person playing meets it, the same for every console's runtime, the launcher and
# the error panel's own process (src/common/utilities/error_kind.c): its kinds, their lines and their actions
include_guard(GLOBAL)
add_library(snx_error STATIC "${PROJECT_SOURCE_DIR}/src/common/utilities/error_kind.c")
target_link_libraries(snx_error PUBLIC scarletnx-common)
set_target_properties(snx_error PROPERTIES C_STANDARD 23 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_compile_options(snx_error PRIVATE -pedantic-errors -Wall -Wextra -Werror=missing-prototypes
                                         -Werror=missing-variable-declarations)
