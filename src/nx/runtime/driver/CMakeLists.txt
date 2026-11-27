# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# what the backends share: function discovery over NX images, chunk labels, progress lines, the entry table
add_library(snx_nx_module STATIC ../../loader/module.c ../../loader/codegen_view.c ../../loader/hook_routes.c ../../loader/hook_libs.c
                                  ../nce/nce_patch.c)
set_target_properties(snx_nx_module PROPERTIES C_STANDARD 23 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_compile_options(snx_nx_module PRIVATE -pedantic-errors -Wall -Wextra -Werror=missing-prototypes)
target_link_libraries(snx_nx_module PUBLIC scarletnx-common)
# the route list (loader/hook_routes.c): the symbols the hooks name, whose calls go through their dispatch slots
include("${PROJECT_SOURCE_DIR}/cmake/hookgen.cmake")
snx_hook_routes(snx_nx_module)
add_library(snx_driver STATIC driver.c container.c extract.c game_name.c fabricate.c nros.c pack.c recompile.c setup.c toolchain.c mcp.c
                              mcp_amberlith.c mcp_gdb.c disasm.c ../../loader/loader.c ../../loader/match.c ../../loader/relocate.c)
set_target_properties(snx_driver PROPERTIES C_STANDARD 23 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_compile_definitions(snx_driver PRIVATE
    SNX_SOURCE_DIR="${PROJECT_SOURCE_DIR}" SNX_BUILD_DIR="${PROJECT_BINARY_DIR}" _CRT_SECURE_NO_WARNINGS
    SNX_NX_ABI_INCLUDE="${PROJECT_SOURCE_DIR}/src/nx/loader"
    SNX_COMMON_INCLUDE="${PROJECT_SOURCE_DIR}/src/common" SNX_TRACY_VERSION="${SNX_TRACY_VERSION}")
target_compile_options(snx_driver PRIVATE -pedantic-errors -Wall -Wextra -Werror=missing-prototypes)
# snx_build_info (the top CMakeLists, after CTest): the commit the games' stamps carry
target_link_libraries(snx_driver PUBLIC scarletnx-common snx_setup snx_nx_module snx_lz4 OpenSSL::Crypto
                                        Threads::Threads snx_build_info hactool_lib snx_yyjson snx_demangle snx_settings)
if(WIN32)
    target_link_libraries(snx_driver PUBLIC advapi32)
endif()
if(NOT SNX_TOOLS_ONLY)
    target_compile_definitions(snx_driver PRIVATE SNX_WITH_RUNTIME)
endif()
# the recompiler, and the library of it (snx_recompiler.h)
add_subdirectory("${PROJECT_SOURCE_DIR}/src/common/cpu/recompiler" recompiler)
# the descriptor of a module of the native back end's code (loader/module_x64.c), and the A64 Sail's encoders: the
# arm64 host's descriptor functions
add_library(snx_nx_descriptor STATIC ../../loader/module_x64.c)
set_target_properties(snx_nx_descriptor PROPERTIES C_STANDARD 23 C_STANDARD_REQUIRED ON C_EXTENSIONS ON)
target_compile_options(snx_nx_descriptor PRIVATE -Wall -Wextra -Werror=missing-prototypes)
target_link_libraries(snx_nx_descriptor PUBLIC snx_recompiler_core snx_nx_module)
include("${PROJECT_SOURCE_DIR}/cmake/sailgen.cmake")
snx_sailgen_a64_encoders(snx_nx_descriptor)
# the NX game module ahead of time (loader/module_a64.c, module_arm.c): the native back end's code
add_library(snx_nx_aot STATIC ../../loader/module_a64.c ../../loader/module_arm.c)
set_target_properties(snx_nx_aot PROPERTIES C_STANDARD 23 C_STANDARD_REQUIRED ON C_EXTENSIONS ON)
target_compile_options(snx_nx_aot PRIVATE -Wall -Wextra -Werror=missing-prototypes)
target_link_libraries(snx_nx_aot PUBLIC snx_nx_descriptor snx_recompiler_jit snx_nx_module snx_settings)
target_link_libraries(snx_driver PUBLIC snx_nx_aot)
# a build of the host tools: the Switch driver scarletnx runs (src/multicall) is the driver alone, which plays nothing
if(SNX_TOOLS_ONLY)
    add_library(snx_switch_program STATIC main.c)
    set_target_properties(snx_switch_program PROPERTIES C_STANDARD 23 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    target_link_libraries(snx_switch_program PUBLIC snx_driver)
endif()
