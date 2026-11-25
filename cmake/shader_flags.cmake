# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# The compile flags of the shader translator (src/nx/gpu/shader) as the shader files' key names them (.vkshaders and
# .vkshipped, gpu/vk/pipeline.c): "" for the flags a normal build has, so its key is the commit's alone, else 16 hex
# digits of the flags it has besides, so that two builds of one commit whose flags differ don't read each other's
# SPIR-V.
#
# A normal build is RelWithDebInfo's -O2 (SNX_OPT_FLAGS' -g or -g0 is no difference) with the toolchain's own flags (the
# NDK's, cmake/cross.cmake's CPU baseline: neither is in the cached CMAKE_C_FLAGS) and none of the user's. What cannot
# change what the translator computes is none either: debug information, warnings, the frame pointer, prefix maps,
# position independence, sections, stack protection, _FORTIFY_SOURCE, LTO and -mtune. Every other flag is one, such as
# -ffast-math, -march (a fused multiply-add), a -D and another -O level.
include_guard(GLOBAL)

# the flags of ARGN that can change what the compiled code computes, in their order
function(snx_shader_flags_kept out)
    # those that cannot, as regexes of a whole flag (a -W that passes one on to the preprocessor is none of them)
    set(neutral
        "-g.*" "-W.*" "-w" "-pipe" "-f(no-)?color-diagnostics" "-fdiagnostics-.*" "-f(no-)?omit-frame-pointer"
        "-f(debug|file|macro)-prefix-map=.*" "-f(no-)?(pic|PIC|pie|PIE)" "-fvisibility.*"
        "-f(no-)?(function|data)-sections" "-f(no-)?stack-(protector|clash-protection).*" "-fcf-protection.*"
        "-[DU]_FORTIFY_SOURCE.*" "-f(no-)?lto.*" "-mtune=.*")
    list(JOIN neutral "|" neutral)
    set(kept "")
    foreach(flag IN LISTS ARGN)
        if(flag MATCHES "^-W[pal]," OR NOT flag MATCHES "^(${neutral})$")
            list(APPEND kept "${flag}")
        endif()
    endforeach()
    set(${out} "${kept}" PARENT_SCOPE)
endfunction()

# The stamp: c_flags the cached CMAKE_C_FLAGS, init and env what CMake made it of (CMAKE_C_FLAGS_INIT, which begins with
# the environment's CFLAGS, and that), config_flags the configuration's (CMAKE_C_FLAGS_<CONFIG>), and after them the
# build's options that reach the translator. said gives the flags in words, "" for a normal build
function(snx_shader_flags out said c_flags init env config_flags)
    separate_arguments(user UNIX_COMMAND "${c_flags}")
    separate_arguments(base UNIX_COMMAND "${init}")
    separate_arguments(env UNIX_COMMAND "${env}")
    # the toolchain's own: INIT without the environment's flags, which are the user's
    foreach(flag IN LISTS env)
        list(FIND base "${flag}" at)
        if(NOT at LESS 0)
            list(REMOVE_AT base ${at})
        endif()
    endforeach()
    foreach(flag IN LISTS base)
        list(FIND user "${flag}" at)
        if(NOT at LESS 0)
            list(REMOVE_AT user ${at})
        endif()
    endforeach()
    separate_arguments(config UNIX_COMMAND "${config_flags}")
    snx_shader_flags_kept(user ${user})
    snx_shader_flags_kept(config ${config})
    set(text "")
    if(user)
        list(JOIN user " " user)
        string(APPEND text "C flags ${user}; ")
    endif()
    list(JOIN config " " config)
    if(NOT config STREQUAL "-O2")
        if(config STREQUAL "")
            set(config "none")
        endif()
        string(APPEND text "configuration flags ${config}; ")
    endif()
    if(ARGN)
        list(JOIN ARGN " " options)
        string(APPEND text "options ${options}; ")
    endif()
    set(hash "")
    if(text)
        string(SHA256 hash "${text}")
        string(SUBSTRING "${hash}" 0 16 hash)
    endif()
    set(${out} "${hash}" PARENT_SCOPE)
    set(${said} "${text}" PARENT_SCOPE)
endfunction()

# SNX_SHADER_FLAGS for the source (of target snx_vk) that writes the key, for the configurations this build tree has
function(snx_shader_flags_of source)
    set(options "")
    foreach(option IN ITEMS SNX_TRACY SNX_TRAPV SNX_TEST_SANITIZERS)
        if(${option})
            list(APPEND options ${option})
        endif()
    endforeach()
    set(configs ${CMAKE_CONFIGURATION_TYPES})
    if(NOT configs)
        set(configs ${CMAKE_BUILD_TYPE})
    endif()
    foreach(config IN LISTS configs)
        string(TOUPPER "${config}" upper)
        snx_shader_flags(hash said "$CACHE{CMAKE_C_FLAGS}" "${CMAKE_C_FLAGS_INIT}" "$ENV{CFLAGS}"
                         "${CMAKE_C_FLAGS_${upper}}" ${options})
        if(said)
            message(STATUS "The shader files' key has the flags of this ${config} build: ${said}as flags=${hash}")
        endif()
        set_property(SOURCE "${source}" APPEND PROPERTY COMPILE_DEFINITIONS
                     "$<$<CONFIG:${config}>:SNX_SHADER_FLAGS=\"${hash}\">")
    endforeach()
endfunction()
