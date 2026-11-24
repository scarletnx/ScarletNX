# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
#
#   cmake -DSRC=<source dir> -P tests/shader_flags_test.cmake
#
# The stamp of the shader translator's compile flags in the shader files' key (cmake/shader_flags.cmake): nothing for a
# normal build's, with whatever flags it has that cannot change what the translator computes (so its key is what it
# was), and a stamp for every other set, each its own, the same when the configure runs again without the environment's
# CFLAGS
cmake_minimum_required(VERSION 3.28)
if(NOT DEFINED SRC)
    message(FATAL_ERROR "shader_flags_test.cmake needs -DSRC=...")
endif()
include("${SRC}/cmake/shader_flags.cmake")
set(failures 0)
set(stamps "")
macro(fail)
    math(EXPR failures "${failures} + 1")
    message(SEND_ERROR "shader_flags: ${ARGN}")
endmacro()
# the flags as the keywords give them: C, INIT and ENV (none without), CONFIG, OPTIONS
function(flags out said)
    cmake_parse_arguments(PARSE_ARGV 2 arg "" "C;INIT;ENV;CONFIG" "OPTIONS")
    foreach(name IN ITEMS C INIT ENV CONFIG)
        if(NOT DEFINED arg_${name})
            set(arg_${name} "")
        endif()
    endforeach()
    snx_shader_flags(hash words "${arg_C}" "${arg_INIT}" "${arg_ENV}" "${arg_CONFIG}" ${arg_OPTIONS})
    set(${out} "${hash}" PARENT_SCOPE)
    set(${said} "${words}" PARENT_SCOPE)
endfunction()
macro(normal what)
    flags(hash said ${ARGN})
    if(NOT hash STREQUAL "" OR NOT said STREQUAL "")
        fail("${what} is a normal build's, and has the stamp [${hash}] of [${said}]")
    endif()
endmacro()
macro(stamped what)
    flags(hash said ${ARGN})
    if(NOT hash MATCHES "^[0-9a-f]+$" OR said STREQUAL "")
        fail("${what} has no stamp: [${hash}] of [${said}]")
    endif()
    string(LENGTH "${hash}" digits)
    if(NOT digits EQUAL 16)
        fail("${what}: the stamp [${hash}] is not 16 digits")
    endif()
    if("${hash}" IN_LIST stamps)
        fail("${what} has the stamp of another set, ${hash} (${said})")
    endif()
    list(APPEND stamps "${hash}")
endmacro()

# the builds CI makes and a user's default
normal("the default" CONFIG "-O2 -g")
normal("the cross builds' CONFIG" CONFIG "-O2 -g0")
normal("a build without debug information" CONFIG "-O2")
normal("flags that change nothing it computes"
       C "-g3 -gdwarf-4 -Wall -Wno-unused -w -pipe -fno-omit-frame-pointer -fdiagnostics-color=always -flto=thin"
       CONFIG "-O2 -g")
normal("more of them" C "-fPIC -fno-pie -fvisibility=hidden -ffunction-sections -fno-data-sections -mtune=generic
                         -fstack-protector-strong -fstack-clash-protection -fcf-protection=full -ffile-prefix-map=a=b
                         -D_FORTIFY_SOURCE=3 -U_FORTIFY_SOURCE" CONFIG "-O2 -g")
normal("the toolchain's own" C "-march=armv8-a -DANDROID" INIT "-march=armv8-a -DANDROID" CONFIG "-O2 -g")
normal("the toolchain's own, and the environment's that change nothing" C "-pipe -march=rv64gc"
       INIT "-pipe -march=rv64gc" ENV "-pipe" CONFIG "-O2 -g")

# everything else, each its own
stamped("-ffast-math" C "-ffast-math" CONFIG "-O2 -g")
stamped("-ffp-contract=off" C "-ffp-contract=off" CONFIG "-O2 -g")
stamped("-march=native" C "-march=native" CONFIG "-O2 -g")
stamped("a -D" C "-DFOO=1" CONFIG "-O2 -g")
stamped("another -D" C "-DFOO=2" CONFIG "-O2 -g")
stamped("a -D passed to the preprocessor" C "-Wp,-DFOO" CONFIG "-O2 -g")
stamped("two -D" C "-DA -DB" CONFIG "-O2 -g")
stamped("the same two in the other order" C "-DB -DA" CONFIG "-O2 -g")
stamped("an -O in C flags" C "-O0" CONFIG "-O2 -g")
stamped("-O0" CONFIG "-O0 -g")
stamped("-O3" CONFIG "-O3 -g")
stamped("a Release build" CONFIG "-O3 -DNDEBUG")
stamped("a Debug build" CONFIG "-g")
stamped("Tracy" CONFIG "-O2 -g" OPTIONS SNX_TRACY)
stamped("the sanitizers" CONFIG "-O2 -g" OPTIONS SNX_TEST_SANITIZERS)
stamped("both" CONFIG "-O2 -g" OPTIONS SNX_TRACY SNX_TEST_SANITIZERS)
stamped("-fsanitize" C "-fsanitize=undefined" CONFIG "-O2 -g")

# the environment's CFLAGS, on the first configure (INIT begins with them) and on the next, from a shell without them
flags(first said C "-march=native -fPIC" INIT "-march=native -fPIC" ENV "-march=native" CONFIG "-O2 -g")
flags(again said C "-march=native -fPIC" INIT "-fPIC" CONFIG "-O2 -g")
if(first STREQUAL "" OR NOT first STREQUAL again)
    fail("the environment's CFLAGS give [${first}], and [${again}] when the configure runs again without them")
endif()
flags(neutral said C "-march=native -g3 -pipe" CONFIG "-O2 -g0")
flags(plain said C "-march=native" CONFIG "-O2 -g")
if(NOT neutral STREQUAL plain)
    fail("flags that change nothing changed the stamp of -march=native: [${neutral}], not [${plain}]")
endif()
# the same user flags over a toolchain's own are the same stamp: the toolchain's are the machine's, which the key names
flags(over said C "-march=armv8-a -DANDROID -march=native" INIT "-march=armv8-a -DANDROID" CONFIG "-O2 -g")
if(NOT over STREQUAL plain)
    fail("-march=native over the toolchain's flags gives [${over}], not [${plain}]")
endif()
list(LENGTH stamps count)
if(failures)
    message(FATAL_ERROR "shader_flags: ${failures} failed")
endif()
message(STATUS "shader_flags: no stamp for a normal build, and ${count} of other sets, each its own")
