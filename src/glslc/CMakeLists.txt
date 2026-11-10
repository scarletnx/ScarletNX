# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# snx_glslc (glslc.h): GLSL ES 1.00 to 3.20 into SPIR-V, for the backends that run OpenGL ES programs on Vulkan
# (docs/layers/gpu/glslc.md). C99 and the C library only. No target of the build compiles a shader with it: ScarletNX's own
# shaders go through the host's glslangValidator (cmake/glsl.cmake)
add_library(snx_glslc STATIC glslc.c lexer.c builtins.c keywords.c parser.c emit.c)
set_target_properties(snx_glslc PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_compile_options(snx_glslc PRIVATE -pedantic-errors -Wall -Wextra -Werror -Werror=missing-prototypes)

# glslc_keywords.inc: the keywords' perfect hash, which tools/glslc_kwgen finds for builtins.c's table as glslc is
# built (keywords.c reads it). A cross build builds the tool for the machine doing the build, with that machine's C
# compiler (SNX_HOST_CC), as cmake/decodetree.cmake does decodetree
if(CMAKE_CROSSCOMPILING)
    include("${PROJECT_SOURCE_DIR}/cmake/host_cc.cmake")
    include(ExternalProject)
    set(snx_glslc_kwgen_dir "${PROJECT_BINARY_DIR}/glslc-kwgen-host")
    set(snx_glslc_kwgen_exe "${snx_glslc_kwgen_dir}/bin/glslc_kwgen")
    if(CMAKE_HOST_WIN32)
        string(APPEND snx_glslc_kwgen_exe ".exe")
    endif()
    ExternalProject_Add(snx_glslc_kwgen_host
        SOURCE_DIR "${PROJECT_SOURCE_DIR}/tools/glslc_kwgen"
        BINARY_DIR "${snx_glslc_kwgen_dir}"
        # no -Werror: the host compiler may not be clang (SNX_HOST_CC)
        CMAKE_ARGS -DCMAKE_BUILD_TYPE=Release "-DCMAKE_C_COMPILER=${SNX_HOST_CC}"
                   "-DCMAKE_C_FLAGS=-pedantic-errors -Wall -Wextra"
                   "-DCMAKE_RUNTIME_OUTPUT_DIRECTORY=${snx_glslc_kwgen_dir}/bin"
                   "-DCMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE=${snx_glslc_kwgen_dir}/bin"
        BUILD_COMMAND "${CMAKE_COMMAND}" --build <BINARY_DIR> --config Release
        BUILD_ALWAYS 1
        BUILD_BYPRODUCTS "${snx_glslc_kwgen_exe}"
        INSTALL_COMMAND "")
    add_executable(snx-glslc-kwgen IMPORTED GLOBAL)
    set_target_properties(snx-glslc-kwgen PROPERTIES IMPORTED_LOCATION "${snx_glslc_kwgen_exe}")
    add_dependencies(snx-glslc-kwgen snx_glslc_kwgen_host)
else()
    add_executable(snx-glslc-kwgen "${PROJECT_SOURCE_DIR}/tools/glslc_kwgen/glslc_kwgen.c" builtins.c)
    target_include_directories(snx-glslc-kwgen PRIVATE "${PROJECT_SOURCE_DIR}/src")
    set_target_properties(snx-glslc-kwgen PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    target_compile_options(snx-glslc-kwgen PRIVATE -pedantic-errors -Wall -Wextra -Werror)
endif()
set(snx_glslc_keywords "${CMAKE_CURRENT_BINARY_DIR}/glslc_keywords.inc")
add_custom_command(OUTPUT "${snx_glslc_keywords}"
    COMMAND snx-glslc-kwgen "${snx_glslc_keywords}"
    DEPENDS snx-glslc-kwgen "${CMAKE_CURRENT_SOURCE_DIR}/builtins.c" "${CMAKE_CURRENT_SOURCE_DIR}/internal.h"
    COMMENT "Generating glslc's keyword hash" VERBATIM)
target_sources(snx_glslc PRIVATE "${snx_glslc_keywords}")
target_include_directories(snx_glslc PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
