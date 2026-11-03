# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# snx_glslc (glslc.h): GLSL ES 1.00 to 3.20 into SPIR-V, for the backends that run OpenGL ES programs on Vulkan
# (docs/GLSLC.md). C99 and the C library only. No target of the build compiles a shader with it: ScarletNX's own
# shaders go through the host's glslangValidator (cmake/glsl.cmake)
add_library(snx_glslc STATIC glslc.c lexer.c builtins.c parser.c emit.c)
set_target_properties(snx_glslc PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_compile_options(snx_glslc PRIVATE -pedantic-errors -Wall -Wextra -Werror -Werror=missing-prototypes)
