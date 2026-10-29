# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# what the backends share: function discovery over NX images, chunk labels, progress lines, the entry table
add_library(snx_nx_module STATIC ../../loader/module.c ../../loader/codegen_view.c ../../loader/hook_routes.c)
set_target_properties(snx_nx_module PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_compile_options(snx_nx_module PRIVATE -pedantic-errors -Wall -Wextra -Werror=missing-prototypes)
target_link_libraries(snx_nx_module PUBLIC scarletnx-common)
# the route list (loader/hook_routes.c): the symbols the hooks name, whose calls go through their dispatch slots
include("${PROJECT_SOURCE_DIR}/cmake/hookgen.cmake")
snx_hook_routes(snx_nx_module)
add_library(snx_driver STATIC driver.c container.c extract.c game_name.c recompile.c setup.c toolchain.c mcp.c
                              mcp_edenprobe.c mcp_gdb.c ../../loader/loader.c)
set_target_properties(snx_driver PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_compile_definitions(snx_driver PRIVATE
    SNX_SOURCE_DIR="${PROJECT_SOURCE_DIR}" SNX_BUILD_DIR="${PROJECT_BINARY_DIR}" _CRT_SECURE_NO_WARNINGS
    SNX_NX_ABI_INCLUDE="${PROJECT_SOURCE_DIR}/src/nx/loader"
    SNX_COMMON_INCLUDE="${PROJECT_SOURCE_DIR}/src/common")
target_compile_options(snx_driver PRIVATE -pedantic-errors -Wall -Wextra -Werror=missing-prototypes)
# snx_build_info (the top CMakeLists, after CTest): the commit the games' stamps carry
target_link_libraries(snx_driver PUBLIC scarletnx-common snx_setup snx_nx_module snx_lz4 OpenSSL::Crypto
                                        Threads::Threads snx_build_info hactool_lib snx_demangle snx_settings)
if(WIN32)
    target_link_libraries(snx_driver PUBLIC advapi32)
endif()
if(APPLE)
    # setup.c's weak snx_menu_language, the menu's (src/gui): ld64 refuses an undefined symbol, weak or not, unless
    # told it is resolved at run time
    target_link_options(snx_driver INTERFACE "LINKER:-U,_snx_menu_language")
endif()
if(NOT SNX_TOOLS_ONLY)
    target_compile_definitions(snx_driver PRIVATE SNX_WITH_RUNTIME)
endif()
include("${PROJECT_SOURCE_DIR}/cmake/llvm.cmake")
add_subdirectory("${PROJECT_SOURCE_DIR}/src/common/cpu/recompiler/llvm" llvm-backend)
# the NX game module through the LLVM backend; C11 for the LLVM-C headers, the driver itself stays C99
add_library(snx_driver_llvm STATIC ../../loader/module_llvm.c ../../loader/module_variants.c ../../loader/module_arm.c
                                   ../../loader/unit_cache.c llvm_tuning.c disasm.c "${SNX_SETTINGS_LLVM}")
set_target_properties(snx_driver_llvm PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS ON)
target_compile_options(snx_driver_llvm PRIVATE -Wall -Wextra -Werror=missing-prototypes)
# OpenSSL: the unit cache's SHA-256 (unit_cache.c)
target_link_libraries(snx_driver_llvm PUBLIC snx_llvm snx_arm_llvm snx_nx_module snx_settings OpenSSL::Crypto)
# LLVM's revision in the unit cache's key, where its headers name one
include("${PROJECT_SOURCE_DIR}/cmake/llvm_revision.cmake")
snx_llvm_revision(snx_llvm_revision "${SNX_LLVM_INCLUDE_DIR}")
if(snx_llvm_revision)
    target_compile_definitions(snx_driver_llvm PRIVATE "SNX_LLVM_REVISION=\"${snx_llvm_revision}\"")
endif()
# scarletnx --mcp's disassembler (disasm.c), where LLVM has its AArch64 target (cmake/llvm.cmake)
if(SNX_LLVM_A64_DISASM)
    target_compile_definitions(snx_driver_llvm PRIVATE SNX_A64_DISASM)
    target_link_libraries(snx_driver_llvm PUBLIC ${SNX_LLVM_DISASM_LIBS})
endif()
target_link_libraries(snx_driver PUBLIC snx_driver_llvm)
# the recompiler as a library (snx_recompiler.h), over the backends configured above
add_subdirectory("${PROJECT_SOURCE_DIR}/src/common/cpu/recompiler" recompiler-library)
# a build of the host tools: the Switch driver scarletnx runs (src/multicall) is the driver alone, which plays nothing
if(SNX_TOOLS_ONLY)
    add_library(snx_switch_program STATIC main.c)
    set_target_properties(snx_switch_program PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    target_link_libraries(snx_switch_program PUBLIC snx_driver)
endif()
