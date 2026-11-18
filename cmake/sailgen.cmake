# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# sailgen (tools/sailgen/) generates the A64 instructions' LLVM emitters from the Sail of the Arm architecture in
# src/common/cpu/aarch64/sail/ (sail-arm's, with the NX's fixups over it). It runs during the build, so a cross build
# also builds it for the machine doing the build, with that machine's C compiler (SNX_HOST_CC), as swipc.cmake does
# for swipc
include_guard(GLOBAL)
set(snx_sailgen_sources "${PROJECT_SOURCE_DIR}/tools/sailgen/main.c" "${PROJECT_SOURCE_DIR}/tools/sailgen/parse.c"
                        "${PROJECT_SOURCE_DIR}/tools/sailgen/extract.c" "${PROJECT_SOURCE_DIR}/tools/sailgen/gen.c")
if(CMAKE_CROSSCOMPILING)
    include("${CMAKE_CURRENT_LIST_DIR}/host_cc.cmake")
    include(ExternalProject)
    set(snx_sailgen_dir "${PROJECT_BINARY_DIR}/sailgen-host")
    set(snx_sailgen_exe "${snx_sailgen_dir}/bin/sailgen")
    if(CMAKE_HOST_WIN32)
        string(APPEND snx_sailgen_exe ".exe")
    endif()
    ExternalProject_Add(snx_sailgen_host
        SOURCE_DIR "${PROJECT_SOURCE_DIR}/tools/sailgen"
        BINARY_DIR "${snx_sailgen_dir}"
        # no -Werror: the host compiler may not be clang (SNX_HOST_CC)
        CMAKE_ARGS -DCMAKE_BUILD_TYPE=Release "-DCMAKE_C_COMPILER=${SNX_HOST_CC}"
                   "-DCMAKE_C_FLAGS=-pedantic-errors -Wall -Wextra"
                   "-DCMAKE_RUNTIME_OUTPUT_DIRECTORY=${snx_sailgen_dir}/bin"
                   "-DCMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE=${snx_sailgen_dir}/bin"
        BUILD_COMMAND "${CMAKE_COMMAND}" --build <BINARY_DIR> --config Release
        BUILD_ALWAYS 1
        BUILD_BYPRODUCTS "${snx_sailgen_exe}"
        INSTALL_COMMAND "")
    add_executable(snx-sailgen IMPORTED GLOBAL)
    set_target_properties(snx-sailgen PROPERTIES IMPORTED_LOCATION "${snx_sailgen_exe}")
    add_dependencies(snx-sailgen snx_sailgen_host)
else()
    add_executable(snx-sailgen ${snx_sailgen_sources})
    set_target_properties(snx-sailgen PROPERTIES C_STANDARD 23 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF
                                                 RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}")
    target_compile_options(snx-sailgen PRIVATE -pedantic-errors -Wall -Wextra -Werror)
endif()

# snx_sailgen_a64(<target>): generates a64_sail.inc (the A64 subsets' emitters, sailgen's back end at its top) into
# the build folder from a64.roots, a64.sail and nx.sail, with the features the decoder's profile names, and gives
# <target> its include root
function(snx_sailgen_a64 target)
    set(dir "${PROJECT_SOURCE_DIR}/src/common/cpu/aarch64/sail")
    set(backend "${PROJECT_SOURCE_DIR}/tools/sailgen/llvm.h")
    set(profile "${PROJECT_SOURCE_DIR}/src/common/cpu/aarch64/decode/a64.profile")
    set(out "${CMAKE_CURRENT_BINARY_DIR}/sailgen")
    add_custom_command(OUTPUT "${out}/a64_sail.inc"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${out}"
        COMMAND snx-sailgen --generate "${dir}/a64.roots" --backend "${backend}" --profile "${profile}"
                -o "${out}/a64_sail.inc" "${dir}/a64.sail" --overlay "${dir}/nx.sail"
        DEPENDS snx-sailgen "${dir}/a64.roots" "${dir}/a64.sail" "${dir}/nx.sail" "${backend}" "${profile}"
        COMMENT "Generating the A64 emitters from Sail" VERBATIM)
    target_sources(${target} PRIVATE "${out}/a64_sail.inc")
    target_include_directories(${target} PRIVATE "${out}")
endfunction()
