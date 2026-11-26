# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# snx_console_settings: which of the Switch's settings each console's game takes, and why another does not
# (src/common/utilities/console_settings.c), for the consoles' drivers and the launcher. snx_settings_runtime: the rows
# without their set functions (g_console_ini) and the Vulkan extensions' names
include_guard(GLOBAL)
add_library(snx_console_settings STATIC "${PROJECT_SOURCE_DIR}/src/common/utilities/console_settings.c")
target_link_libraries(snx_console_settings PUBLIC snx_settings_runtime)
set_target_properties(snx_console_settings PROPERTIES C_STANDARD 23 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
# a row that leaves a field out names its key: {"key"} would leave why to -Wextra's missing field
target_compile_options(snx_console_settings PRIVATE -pedantic-errors -Wall -Wextra -Werror=missing-prototypes
                                                    -Werror=missing-variable-declarations
                                                    -Werror=missing-field-initializers)
