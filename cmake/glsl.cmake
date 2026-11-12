# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# GLSL shaders compiled into SPIR-V at build time, with glslang's glslangValidator, so no compiled shader is checked in,
# then optimized by SPIRV-Tools' spirv-opt and validated by its spirv-val, as a step of their own: the optimizations do
# not hang on whether the glslang at hand was built with SPIRV-Tools, which glslang's own -O needs.
#
# The compiler is SNX_GLSLANG when it is set. Otherwise it is KhronosGroup's glslang release for the host, a CPM package
# pinned by its SHA-256 like the libraries (cmake/dependencies.cmake), on the hosts it publishes builds for (Linux
# x86-64, Windows x86-64, macOS); elsewhere (FreeBSD, Linux on ARM) the glslangValidator on PATH, from the system's
# glslang package. Cross builds run the host's, since the shader is the same for every target.

set(SNX_GLSLANG "" CACHE FILEPATH "glslangValidator to compile shaders with (empty: the pinned release, or PATH)")
if(NOT SNX_GLSLANG)
    set(snx_glslang_version 16.6.0)
    set(snx_glslang_asset "")
    if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
        set(snx_glslang_asset linux-x86_64-release.tar.gz)
        set(snx_glslang_sha256 2c34071f56ecf39d16233294d3739cf43f981fd39080f2fd9dccb782101191ce)
    elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows" AND CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
        set(snx_glslang_asset windows-x86_64-release.zip)
        set(snx_glslang_sha256 82bf434e69b9bb4829de7e2b4bc2c5e7a7861e53d66cf75e5cc70f5f694a8d9b)
    elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
        set(snx_glslang_asset macos-universal-release.tar.gz)
        set(snx_glslang_sha256 4f8c05f2888a73ed0b7b274bc524af9130e09ae94b43c0f3f9ed83cfae852834)
    endif()
    if(snx_glslang_asset)
        CPMAddPackage(NAME glslang-tools VERSION ${snx_glslang_version}
                      URL https://github.com/KhronosGroup/glslang/releases/download/${snx_glslang_version}/glslang-${snx_glslang_version}-${snx_glslang_asset}
                      URL_HASH SHA256=${snx_glslang_sha256}
                      DOWNLOAD_ONLY YES)
        set(snx_glslang_path "${glslang-tools_SOURCE_DIR}/bin/glslangValidator${CMAKE_HOST_EXECUTABLE_SUFFIX}")
        if (NOT EXISTS "${snx_glslang_path}")
            set(snx_glslang_path "${glslang-tools_SOURCE_DIR}/bin/glslang${CMAKE_HOST_EXECUTABLE_SUFFIX}")
        endif()
    else()
        find_program(snx_glslang_path NAMES glslangValidator glslang)
    endif()
    if(NOT EXISTS "${snx_glslang_path}")
        message(FATAL_ERROR "glslangValidator not found: install glslang (its tools), or set SNX_GLSLANG")
    endif()
    set(SNX_GLSLANG "${snx_glslang_path}" CACHE FILEPATH
        "glslangValidator to compile shaders with (empty: the pinned release, or PATH)" FORCE)
endif()

# spirv-opt and spirv-val: SNX_SPIRV_OPT and SNX_SPIRV_VAL when set, else the ones on PATH, from the system's
# SPIRV-Tools package (spirv-tools), which no glslang release carries. spirv-val is looked for beside spirv-opt first
set(SNX_SPIRV_OPT "" CACHE FILEPATH "spirv-opt to optimize shaders with (empty: the one on PATH)")
set(SNX_SPIRV_VAL "" CACHE FILEPATH "spirv-val to validate shaders with (empty: the one beside spirv-opt, or on PATH)")
if(NOT SNX_SPIRV_OPT)
    find_program(snx_spirv_opt_path NAMES spirv-opt)
    if(NOT snx_spirv_opt_path)
        message(FATAL_ERROR "spirv-opt not found: install SPIRV-Tools (the spirv-tools package), or set SNX_SPIRV_OPT")
    endif()
    set(SNX_SPIRV_OPT "${snx_spirv_opt_path}" CACHE FILEPATH
        "spirv-opt to optimize shaders with (empty: the one on PATH)" FORCE)
endif()
if(NOT SNX_SPIRV_VAL)
    get_filename_component(snx_spirv_tools_dir "${SNX_SPIRV_OPT}" DIRECTORY)
    find_program(snx_spirv_val_path NAMES spirv-val HINTS "${snx_spirv_tools_dir}")
    if(NOT snx_spirv_val_path)
        message(FATAL_ERROR "spirv-val not found: install SPIRV-Tools (the spirv-tools package), or set SNX_SPIRV_VAL")
    endif()
    set(SNX_SPIRV_VAL "${snx_spirv_val_path}" CACHE FILEPATH
        "spirv-val to validate shaders with (empty: the one beside spirv-opt, or on PATH)" FORCE)
endif()

# snx_glsl_spirv(target SOURCE file.comp NAME symbol HEADER include/path.h [TARGET vulkan1.x] [DEFINES MACRO[=value]...])
# Compiles SOURCE (its #includes, beside it or from src as a C file's, followed through glslang's depfile) for Vulkan
# 1.1 (or TARGET) without debug information, optimizes it for speed with spirv-opt -O, validates the result with
# spirv-val, and puts it into a C file added to target: `const u32 NAME[]` and `const u64 NAME_size` (bytes). HEADER,
# which the C file includes, declares them: the build writes it too, under generated/include in the build tree (on the
# include path beside src), one header for every NAME given it. DEFINES are predefined macros, for another build of the
# same source
function(snx_glsl_spirv target)
    cmake_parse_arguments(arg "" "SOURCE;NAME;HEADER;TARGET" "DEFINES" ${ARGN})
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
        COMMAND "${SNX_SPIRV_OPT}" -O --target-env=${arg_TARGET} "${spv}" -o "${opt}"
        COMMAND "${SNX_SPIRV_VAL}" --target-env ${arg_TARGET} "${opt}"
        COMMAND "${CMAKE_COMMAND}" -DWORDS=${opt} -DOUT=${out} -DNAME=${arg_NAME} -DHEADER=${arg_HEADER}
                -DSOURCE=${shown} -P "${PROJECT_SOURCE_DIR}/cmake/spirv_source.cmake"
        DEPENDS "${spv}" "${PROJECT_SOURCE_DIR}/cmake/spirv_source.cmake"
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
