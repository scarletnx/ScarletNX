# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# The LLVM backend: the IR of the guest's functions, the fallback made of it (../fallback_llvm.c), and the runtime
# library's bitcode, compiled for the CPU of the module that links it. A folder of the recompiler's (../CMakeLists.txt), whose snx_rtlib_flags, snx_rtlib_gen and
# snx_rtlib_sources the bitcode takes
set(snx_rtlib_dir "${CMAKE_CURRENT_SOURCE_DIR}/../rtlib")
# LLVM reads the bitcode of its own version and older ones
string(REGEX MATCH "^[0-9]+" snx_clang_major "${CMAKE_C_COMPILER_VERSION}")
string(REGEX MATCH "^[0-9]+" snx_llvm_major "${SNX_LLVM_VERSION}")
if(snx_clang_major GREATER snx_llvm_major)
    # A newer Clang writes bitcode the target LLVM cannot read. Its textual IR,
    # emitted without debug metadata (whose field set moves faster than the
    # assembler), still assembles into the target's bitcode with that LLVM's
    # llvm-as. An LLVM found by its CMake package (cmake/llvm.cmake) has no llvm-config to ask: its tools' folder
    if(SNX_LLVM_CONFIG)
        snx_llvm_query(SNX_LLVM_BINDIR --bindir)
    else()
        set(SNX_LLVM_BINDIR "${LLVM_TOOLS_BINARY_DIR}")
    endif()
    find_program(SNX_LLVM_AS NAMES llvm-as llvm-as-${snx_llvm_major} HINTS "${SNX_LLVM_BINDIR}" NO_DEFAULT_PATH)
    if(NOT SNX_LLVM_AS)
        message(FATAL_ERROR "Clang ${CMAKE_C_COMPILER_VERSION} writes the runtime library's bitcode, which LLVM "
                            "${SNX_LLVM_VERSION} (SNX_LLVM_CONFIG) cannot read, and that LLVM ships no llvm-as to "
                            "translate. Use a Clang no newer than LLVM ${snx_llvm_major}, or an LLVM as new as this "
                            "Clang.")
    endif()
    # a cross build's is the target's program (pkgsrc's in a NetBSD sysroot), which this machine may not run, and a
    # newer one writes the same unreadable bitcode: SNX_LLVM_AS then names one of this machine's
    execute_process(COMMAND "${SNX_LLVM_AS}" --version RESULT_VARIABLE snx_llvm_as_result
                    OUTPUT_VARIABLE snx_llvm_as_version ERROR_VARIABLE snx_llvm_as_error)
    string(REGEX MATCH "LLVM version ([0-9]+)" snx_llvm_as_match "${snx_llvm_as_version}")
    if(NOT snx_llvm_as_result STREQUAL "0" OR NOT snx_llvm_as_match)
        string(STRIP "${snx_llvm_as_result} ${snx_llvm_as_error}" snx_llvm_as_error)
        message(FATAL_ERROR "SNX_LLVM_AS=${SNX_LLVM_AS} does not run here (${snx_llvm_as_error}): set SNX_LLVM_AS to "
                            "an llvm-as of LLVM ${snx_llvm_major} this machine runs, or use a Clang no newer than "
                            "LLVM ${snx_llvm_major}.")
    endif()
    if(CMAKE_MATCH_1 GREATER snx_llvm_major)
        message(FATAL_ERROR "SNX_LLVM_AS=${SNX_LLVM_AS} is LLVM ${CMAKE_MATCH_1}, whose bitcode LLVM "
                            "${SNX_LLVM_VERSION} cannot read: set SNX_LLVM_AS to an llvm-as of LLVM ${snx_llvm_major}.")
    endif()
    add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/rtlib_ir_raw.ll"
        COMMAND "${CMAKE_C_COMPILER}" ${snx_rtlib_flags} -g0 -emit-llvm -S -I "${CMAKE_CURRENT_BINARY_DIR}"
            -I "${PROJECT_SOURCE_DIR}/src" "${snx_rtlib_dir}/rtlib.c"
            -o "${CMAKE_CURRENT_BINARY_DIR}/rtlib_ir_raw.ll"
        DEPENDS "${snx_rtlib_dir}/rtlib.c" "${snx_rtlib_dir}/libc.inc" ${snx_rtlib_sources} VERBATIM)
    add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/rtlib_ir.ll"
        COMMAND "${CMAKE_COMMAND}" -DINPUT=${CMAKE_CURRENT_BINARY_DIR}/rtlib_ir_raw.ll
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/rtlib_ir.ll
            -P "${CMAKE_CURRENT_SOURCE_DIR}/rtlib_ir_compat.cmake"
        DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/rtlib_ir_raw.ll" rtlib_ir_compat.cmake VERBATIM)
    add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/rtlib.bc"
        COMMAND "${SNX_LLVM_AS}" "${CMAKE_CURRENT_BINARY_DIR}/rtlib_ir.ll" -o "${CMAKE_CURRENT_BINARY_DIR}/rtlib.bc"
        DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/rtlib_ir.ll" VERBATIM)
else()
    add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/rtlib.bc"
        COMMAND "${CMAKE_C_COMPILER}" ${snx_rtlib_flags} -emit-llvm -I "${CMAKE_CURRENT_BINARY_DIR}"
            -I "${PROJECT_SOURCE_DIR}/src" -c "${snx_rtlib_dir}/rtlib.c"
            -o "${CMAKE_CURRENT_BINARY_DIR}/rtlib.bc"
        DEPENDS "${snx_rtlib_dir}/rtlib.c" "${snx_rtlib_dir}/libc.inc" ${snx_rtlib_sources} VERBATIM)
endif()
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/rtlib_bitcode.c"
    COMMAND "${snx_rtlib_gen}" embed "${CMAKE_CURRENT_BINARY_DIR}/rtlib.bc"
        "${CMAKE_CURRENT_BINARY_DIR}/rtlib_bitcode.c" snx_rtlib_bitcode
    DEPENDS "${snx_rtlib_gen}" "${CMAKE_CURRENT_BINARY_DIR}/rtlib.bc" VERBATIM)

add_library(snx_llvm STATIC ir.c ir_state.c ir_promote.c ir_emit.c ir_profile.c ir_helpers.c ir_int.c ir_sail.c
    ir_ldst.c ir_fpu.c ir_vec.c ir_vec_fp.c ir_vec_crypto.c ir_codegen.c ../fallback_llvm.c
    "${CMAKE_CURRENT_BINARY_DIR}/rtlib_bitcode.c")
set_target_properties(snx_llvm PROPERTIES C_STANDARD 23 C_STANDARD_REQUIRED ON C_EXTENSIONS ON)
target_include_directories(snx_llvm SYSTEM PUBLIC "${SNX_LLVM_INCLUDE_DIR}")
target_compile_options(snx_llvm PRIVATE -Wall -Wextra -Werror=missing-prototypes)
target_compile_definitions(snx_llvm PRIVATE _CRT_SECURE_NO_WARNINGS)
target_link_directories(snx_llvm PUBLIC "${SNX_LLVM_LIBRARY_DIR}")
target_link_libraries(snx_llvm PUBLIC snx_recompiler_core snx_arm_llvm ${SNX_LLVM_LIBS} ${SNX_LLVM_SYSTEM_LIBS}
                      Threads::Threads)
# the A64 emitters sailgen generates from the Sail (ir_sail.c)
include("${PROJECT_SOURCE_DIR}/cmake/sailgen.cmake")
snx_sailgen_a64(snx_llvm)
