# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# GLSL shaders compiled into SPIR-V at build time, with glslang's glslangValidator, so no compiled shader is checked in,
# then optimized by SPIRV-Tools' spirv-opt and validated by its spirv-val, as a step of their own: the optimizations do
# not hang on whether the glslang at hand was built with SPIRV-Tools, which glslang's own -O needs.
#
# The compiler is SNX_GLSLANG when it is set. Otherwise it is KhronosGroup's glslang release for the host, pinned in
# cpmfile.json like the libraries (cmake/packages.cmake), on the hosts it publishes builds for (Linux x86-64, Windows
# x86-64, macOS); elsewhere (FreeBSD, Linux on ARM) the glslangValidator on PATH, from the system's glslang package.
# Cross builds run the host's, since the shader is the same for every target.

set(SNX_GLSLANG "" CACHE FILEPATH "glslangValidator to compile shaders with (empty: the pinned release, or PATH)")
# the suffix of a program of the machine doing the build: CMAKE_HOST_EXECUTABLE_SUFFIX is CMake 3.31's, and the build
# takes 3.28
if(CMAKE_HOST_WIN32)
    set(snx_host_exe ".exe")
else()
    set(snx_host_exe "")
endif()
# A path this file found is found again at each configure (snx_glslang_found keeps it apart from one the user named):
# once the pin moves, the release is in another folder, and the old one's glslangValidator is still a file
if(NOT SNX_GLSLANG OR SNX_GLSLANG STREQUAL "${snx_glslang_found}")
    unset(snx_glslang_path CACHE)
    set(snx_glslang_host "")
    if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
        set(snx_glslang_host linux-x86_64)
    elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows" AND CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
        set(snx_glslang_host windows-x86_64)
    elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
        set(snx_glslang_host macos)
    endif()
    if(snx_glslang_host)
        AddJsonPackage(NAME glslang-tools-${snx_glslang_host} DOWNLOAD_ONLY FORCE_BUNDLED_PACKAGE ON)
        set(snx_glslang_dir "${glslang-tools-${snx_glslang_host}_SOURCE_DIR}")
        set(snx_glslang_path "${snx_glslang_dir}/bin/glslangValidator${snx_host_exe}")
        if (NOT EXISTS "${snx_glslang_path}")
            set(snx_glslang_path "${snx_glslang_dir}/bin/glslang${snx_host_exe}")
        endif()
    else()
        find_program(snx_glslang_path NAMES glslangValidator glslang)
    endif()
    if(NOT EXISTS "${snx_glslang_path}")
        message(FATAL_ERROR "glslangValidator not found: install glslang (its tools), or set SNX_GLSLANG")
    endif()
    set(SNX_GLSLANG "${snx_glslang_path}" CACHE FILEPATH
        "glslangValidator to compile shaders with (empty: the pinned release, or PATH)" FORCE)
    set(snx_glslang_found "${snx_glslang_path}" CACHE INTERNAL "the glslangValidator cmake/glsl.cmake found itself")
endif()

# spirv-opt and spirv-val: SPIRV-Tools' release the build pins, or the ones SNX_SPIRV_OPT and SNX_SPIRV_VAL name
# (cmake/spirv_tools.cmake)
include("${CMAKE_CURRENT_LIST_DIR}/spirv_tools.cmake")

# snx_glsl_spirv(target SOURCE file.comp NAME symbol HEADER include/path.h [TARGET vulkan1.x] [DEFINES MACRO[=value]...]
#                [INCLUDES dir...])
# Compiles SOURCE (its #includes, beside it or from src as a C file's, followed through glslang's depfile) for Vulkan
# 1.1 (or TARGET) without debug information, optimizes it for speed with spirv-opt -O, validates the result with
# spirv-val, and puts it into a C file added to target: `const u32 NAME[]` and `const u64 NAME_size` (bytes). HEADER,
# which the C file includes, declares them: the build writes it too, under generated/include in the build tree (on the
# include path beside src), one header for every NAME given it. DEFINES are predefined macros, for another build of the
# same source, and INCLUDES folders its #includes are looked for in after its own
function(snx_glsl_spirv target)
    cmake_parse_arguments(arg "" "SOURCE;NAME;HEADER;TARGET" "DEFINES;INCLUDES" ${ARGN})
    if(NOT arg_TARGET)
        set(arg_TARGET vulkan1.1)
    endif()
    get_property(headers GLOBAL PROPERTY snx_spirv_headers)
    if(NOT arg_HEADER IN_LIST headers)
        set_property(GLOBAL APPEND PROPERTY snx_spirv_headers "${arg_HEADER}")
    endif()
    set_property(GLOBAL APPEND PROPERTY "snx_spirv_header:${arg_HEADER}" "${arg_NAME}")
    set(defines "")
    foreach(define IN LISTS arg_DEFINES)
        list(APPEND defines "-D${define}")
    endforeach()
    foreach(include IN LISTS arg_INCLUDES)
        list(APPEND defines "-I${include}")
    endforeach()
    get_filename_component(source "${arg_SOURCE}" ABSOLUTE)
    set(dir "${PROJECT_BINARY_DIR}/generated/spirv")
    set(spv "${dir}/${arg_NAME}.spv")
    set(opt "${dir}/${arg_NAME}.opt.spv")
    set(out "${dir}/${arg_NAME}.c")
    file(MAKE_DIRECTORY "${dir}")
    file(RELATIVE_PATH shown "${PROJECT_SOURCE_DIR}" "${source}")
    # two steps, so that glslang's depfile names its command's output (the .spv), as the Makefile generators need it
    # to (they make it a rule for the file it names). DEPENDS rather than MAIN_DEPENDENCY: a source can be built twice.
    # spirv-opt validates what it is given before it optimizes, and spirv-val what it gave
    add_custom_command(OUTPUT "${spv}"
        COMMAND "${SNX_GLSLANG}" -V --target-env ${arg_TARGET} -g0 ${defines} "-I${PROJECT_SOURCE_DIR}/src"
                --depfile "${spv}.d" -o "${spv}" "${source}"
        DEPENDS "${source}"
        DEPFILE "${spv}.d"
        COMMENT "Compiling ${shown} to SPIR-V (${arg_NAME})"
        VERBATIM)
    add_custom_command(OUTPUT "${out}"
        COMMAND snx-spirv-opt -O --target-env=${arg_TARGET} "${spv}" -o "${opt}"
        COMMAND snx-spirv-val --target-env ${arg_TARGET} "${opt}"
        COMMAND "${CMAKE_COMMAND}" -DWORDS=${opt} -DOUT=${out} -DNAME=${arg_NAME} -DHEADER=${arg_HEADER}
                -DSOURCE=${shown} -P "${PROJECT_SOURCE_DIR}/cmake/spirv_source.cmake"
        DEPENDS "${spv}" "${PROJECT_SOURCE_DIR}/cmake/spirv_source.cmake" snx-spirv-opt snx-spirv-val
        COMMENT "Optimizing ${shown}'s SPIR-V (${arg_NAME})"
        VERBATIM)
    target_sources(${target} PRIVATE "${out}")
endfunction()

# Every snx_glsl_spirv HEADER, written once the configure has made all the calls (a header can declare several NAMEs),
# and only where its text changed, so a configure again does not rebuild what includes it
function(snx_glsl_spirv_headers)
    get_property(headers GLOBAL PROPERTY snx_spirv_headers)
    foreach(header IN LISTS headers)
        get_property(names GLOBAL PROPERTY "snx_spirv_header:${header}")
        string(MAKE_C_IDENTIFIER "SNX_${header}" guard)
        string(TOUPPER "${guard}" guard)
        set(text "/* Generated by cmake/glsl.cmake. Do not edit. The words are in each name's .c file, which the build makes\n")
        string(APPEND text "   from its shader with glslang: nothing compiled is checked in. */\n")
        string(APPEND text "#ifndef ${guard}\n#define ${guard}\n#include \"common/cpu/aarch64/cpu.h\"\n\n")
        foreach(name IN LISTS names)
            string(APPEND text "extern const u32 ${name}[];\nextern const u64 ${name}_size; /* in bytes */\n")
        endforeach()
        string(APPEND text "\n#endif\n")
        file(CONFIGURE OUTPUT "${PROJECT_BINARY_DIR}/generated/include/${header}" CONTENT "${text}" @ONLY)
    endforeach()
endfunction()
get_property(snx_spirv_deferred GLOBAL PROPERTY snx_spirv_deferred)
if(NOT snx_spirv_deferred)
    set_property(GLOBAL PROPERTY snx_spirv_deferred 1)
    cmake_language(DEFER DIRECTORY "${PROJECT_SOURCE_DIR}" CALL snx_glsl_spirv_headers)
endif()
