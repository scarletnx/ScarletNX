# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# decodetree (tools/decodetree/) generates the instruction decoders from QEMU's .decode files: the A64 decoder from
# a64.decode and ScarletNX's profile of it (--a64), and QEMU-style decoders, as QEMU's scripts/decodetree.py makes
# them, for A32, T32 and T16, and for the GPU shaders of the Vita (USSE, -w 64) and the 3DS (PICA200). It runs during
# the build, so a cross build also builds it for the machine doing the build, with that machine's C compiler
# (SNX_HOST_CC), as shaderdec.cmake does for shaderdec
include_guard(GLOBAL)
set(snx_decodetree_sources "${PROJECT_SOURCE_DIR}/tools/decodetree/decodetree.c"
                           "${PROJECT_SOURCE_DIR}/tools/decodetree/a64.c")
if(CMAKE_CROSSCOMPILING)
    include("${CMAKE_CURRENT_LIST_DIR}/host_cc.cmake")
    include(ExternalProject)
    set(snx_decodetree_dir "${PROJECT_BINARY_DIR}/decodetree-host")
    set(snx_decodetree_exe "${snx_decodetree_dir}/bin/decodetree")
    if(CMAKE_HOST_WIN32)
        string(APPEND snx_decodetree_exe ".exe")
    endif()
    ExternalProject_Add(snx_decodetree_host
        SOURCE_DIR "${PROJECT_SOURCE_DIR}/tools/decodetree"
        BINARY_DIR "${snx_decodetree_dir}"
        # no -Werror: the host compiler may not be clang (SNX_HOST_CC)
        CMAKE_ARGS -DCMAKE_BUILD_TYPE=Release "-DCMAKE_C_COMPILER=${SNX_HOST_CC}"
                   "-DCMAKE_C_FLAGS=-pedantic-errors -Wall -Wextra"
                   "-DCMAKE_RUNTIME_OUTPUT_DIRECTORY=${snx_decodetree_dir}/bin"
                   "-DCMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE=${snx_decodetree_dir}/bin"
        BUILD_COMMAND "${CMAKE_COMMAND}" --build <BINARY_DIR> --config Release
        BUILD_ALWAYS 1
        BUILD_BYPRODUCTS "${snx_decodetree_exe}"
        INSTALL_COMMAND "")
    add_executable(snx-decodetree IMPORTED GLOBAL)
    set_target_properties(snx-decodetree PROPERTIES IMPORTED_LOCATION "${snx_decodetree_exe}")
    add_dependencies(snx-decodetree snx_decodetree_host)
else()
    add_executable(snx-decodetree ${snx_decodetree_sources})
    set_target_properties(snx-decodetree PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON
                                                    C_EXTENSIONS OFF RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}")
    target_compile_options(snx-decodetree PRIVATE -pedantic-errors -Wall -Wextra -Werror)
endif()

# snx_decodetree_a64(<target> [PROFILE <file.profile> DECODE <file.decode>...]): generates a64_decode.h (the types) and
# a64_decode.inc (the decoder) into the build folder and gives <target> their include root. Without PROFILE, from
# a64.decode and a64.profile; sve.decode and sme.decode join a64.decode in the list when they are vendored. With
# PROFILE and DECODE, from those files, into a folder of <target>'s own: a test's decoder of a few rows
function(snx_decodetree_a64 target)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "PROFILE" "DECODE")
    set(dir "${PROJECT_SOURCE_DIR}/src/common/cpu/aarch64/decode")
    set(profile "${dir}/a64.profile")
    set(decode "${dir}/a64.decode")
    set(out "${CMAKE_CURRENT_BINARY_DIR}/decodetree/a64")
    if(arg_PROFILE OR arg_DECODE)
        if(NOT arg_PROFILE OR NOT arg_DECODE)
            message(FATAL_ERROR "snx_decodetree_a64(${target}) needs PROFILE and DECODE together")
        endif()
        set(profile "${arg_PROFILE}")
        set(decode ${arg_DECODE})
        set(out "${CMAKE_CURRENT_BINARY_DIR}/decodetree/${target}")
    endif()
    add_custom_command(OUTPUT "${out}/a64_decode.h" "${out}/a64_decode.inc"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${out}"
        COMMAND snx-decodetree --a64-profile "${profile}" --a64-header "${out}/a64_decode.h"
                --a64-decoder "${out}/a64_decode.inc" ${decode}
        DEPENDS snx-decodetree "${profile}" ${decode}
        COMMENT "Generating the A64 instruction decoder" VERBATIM)
    target_sources(${target} PRIVATE "${out}/a64_decode.h" "${out}/a64_decode.inc")
    target_include_directories(${target} PUBLIC "${out}")
endfunction()

# snx_decodetree(<target> OUTPUT <name.inc> DECODE <file.decode>... [OPTIONS <decodetree options>...]): generates a
# QEMU-style decoder (arg_ structs, trans_ declarations, the decode function) into the build folder and gives <target>
# its include root. The C file that includes it defines DisasContext and the trans_ functions
function(snx_decodetree target)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "OUTPUT" "DECODE;OPTIONS")
    if(NOT arg_OUTPUT OR NOT arg_DECODE)
        message(FATAL_ERROR "snx_decodetree(${target}) needs OUTPUT and DECODE")
    endif()
    set(out "${CMAKE_CURRENT_BINARY_DIR}/decodetree/${target}")
    add_custom_command(OUTPUT "${out}/${arg_OUTPUT}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${out}"
        COMMAND snx-decodetree ${arg_OPTIONS} -o "${out}/${arg_OUTPUT}" ${arg_DECODE}
        DEPENDS snx-decodetree ${arg_DECODE}
        COMMENT "Generating the decoder ${arg_OUTPUT}" VERBATIM)
    target_sources(${target} PRIVATE "${out}/${arg_OUTPUT}")
    target_include_directories(${target} PRIVATE "${out}")
endfunction()
