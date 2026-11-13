# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
#
#   cmake -DSRC=<source dir> -DWORK=<scratch dir> [-DMESON=<meson>] -P tests/meson_machine_test.cmake
#
# The machine file cmake/meson_machine.cmake gives dav1d's meson (cmake/dav1d.cmake) for each kind of build: the Linux
# cross presets' with the flags cmake/cross.cmake makes for them, Android's, iOS's, a Mac's native file, none for a
# native build without flags, and the reason it gives where it can make none. With MESON, meson reads every file made
# and names the machine in it as the file means, in a project with no language, so no compiler runs. Each case is a
# cmake of its own, since cross.cmake includes once.
if(NOT DEFINED SRC OR NOT DEFINED WORK)
    message(FATAL_ERROR "meson_machine_test.cmake needs -DSRC=... -DWORK=...")
endif()
# WANT_ARGS "" is an empty option, not a missing one
if(POLICY CMP0174)
    cmake_policy(SET CMP0174 NEW)
endif()
if(DEFINED CASE)
    # the variables a configure of that target has by the time cmake/dav1d.cmake runs; cross.cmake writes a cross
    # build's target-cc wrapper into the tree
    if(CROSS_FLAGS)
        set(PROJECT_BINARY_DIR "${WORK}/${CASE}-tree")
        include("${SRC}/cmake/cross.cmake")
    endif()
    include("${SRC}/cmake/meson_machine.cmake")
    snx_meson_machine_file("${WORK}/${CASE}.ini" args error target)
    message("args [${args}] error [${error}] target [${target}] flags [${SNX_TARGET_FLAGS}] ldflags "
            "[${SNX_TARGET_LDFLAGS}]")
    return()
endif()

file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")
set(failed 0)

# a meson array of the items, written independently of the module: ['a', 'b']
function(meson_array out)
    set(items "")
    foreach(item IN LISTS ARGN)
        list(APPEND items "'${item}'")
    endforeach()
    list(JOIN items ", " joined)
    set(${out} "[${joined}]" PARENT_SCOPE)
endfunction()

# Runs the module for the case with the -D definitions given, and checks the meson option it returns, the reason it
# gives, the machine it names and the file it writes. WANT_FILE holds the file's text, WANT_ARGS the option,
# WANT_ERROR a regular expression the reason matches (empty: none); WANT_FILE's pieces are joined. "@FLAGS@" in the
# text stands for the c_args and c_link_args lines of the flags cross.cmake made in that run.
function(check case)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "WANT_ARGS;WANT_ERROR;WANT_TARGET" "DEFINE;WANT_FILE")
    set(definitions "")
    foreach(definition IN LISTS arg_DEFINE)
        list(APPEND definitions "-D${definition}")
    endforeach()
    # a file left by an earlier configure, which a build that needs none removes
    file(WRITE "${WORK}/${case}.ini" "stale\n")
    execute_process(COMMAND "${CMAKE_COMMAND}" "-DSRC=${SRC}" "-DWORK=${WORK}" "-DCASE=${case}" ${definitions}
                            -P "${CMAKE_CURRENT_FUNCTION_LIST_FILE}"
                    OUTPUT_VARIABLE out ERROR_VARIABLE out RESULT_VARIABLE rc)
    string(STRIP "${out}" out)
    if(NOT rc EQUAL 0 OR NOT out MATCHES "^args \\[([^]]*)\\] error \\[(.*)\\] target \\[([^]]*)\\] flags \\[([^]]*)\\] ldflags \\[([^]]*)\\]$")
        message(SEND_ERROR "${case}: meson_machine.cmake exited ${rc}: ${out}")
        set(failed 1 PARENT_SCOPE)
        return()
    endif()
    set(args "${CMAKE_MATCH_1}")
    set(error "${CMAKE_MATCH_2}")
    set(target "${CMAKE_MATCH_3}")
    set(flags "${CMAKE_MATCH_4}")
    set(ldflags "${CMAKE_MATCH_5}")
    set(bad "")
    string(REPLACE "@WORK@" "${WORK}" want_args "${arg_WANT_ARGS}")
    if(NOT args STREQUAL want_args)
        string(APPEND bad "\n  meson's option is [${args}], not [${want_args}]")
    endif()
    if(arg_WANT_ERROR AND NOT error MATCHES "${arg_WANT_ERROR}")
        string(APPEND bad "\n  the reason is [${error}], which does not match [${arg_WANT_ERROR}]")
    elseif(NOT arg_WANT_ERROR AND error)
        string(APPEND bad "\n  it gives the reason [${error}] where it should make the file")
    endif()
    if(NOT target STREQUAL arg_WANT_TARGET)
        string(APPEND bad "\n  the machine is [${target}], not [${arg_WANT_TARGET}]")
    endif()
    string(JOIN "" want_file ${arg_WANT_FILE})
    if(want_file MATCHES "@FLAGS@")
        meson_array(c_args ${flags})
        meson_array(link_args ${flags} ${ldflags})
        string(REPLACE "@FLAGS@" "c_args = ${c_args}\nc_link_args = ${link_args}" want_file "${want_file}")
    endif()
    if(want_file)
        file(READ "${WORK}/${case}.ini" text)
        if(NOT text STREQUAL want_file)
            string(APPEND bad "\n  it wrote\n${text}\n  not\n${want_file}")
        endif()
    elseif(EXISTS "${WORK}/${case}.ini")
        file(READ "${WORK}/${case}.ini" text)
        string(APPEND bad "\n  it left a file where it should make none:\n${text}")
    endif()
    if(bad)
        message(SEND_ERROR "${case}:${bad}")
        set(failed 1 PARENT_SCOPE)
    else()
        message(STATUS "${case}: [${args}] [${error}] [${target}]")
    endif()
endfunction()

set(linux_cross CMAKE_SYSTEM_NAME=Linux CMAKE_CROSSCOMPILING=ON CMAKE_C_COMPILER=/usr/bin/clang
                CMAKE_AR=/usr/bin/llvm-ar CMAKE_SYSROOT=/src/sysroot CROSS_FLAGS=ON)
string(CONCAT cross_text "[binaries]\nc = ['/usr/bin/clang']\nar = ['/usr/bin/llvm-ar']\n\n[built-in options]\n@FLAGS@\n\n"
       "[properties]\nneeds_exe_wrapper = true\n\n[host_machine]\nsystem = 'linux'\n")

# the four cross-built AppImages (build.yml's appimage-cross): ppc64 builds for every 64-bit PowerPC, without AltiVec,
# where dav1d's POWER code builds for the CPU named ppc64le alone
check(linux-ppc64-elfv2-cross DEFINE ${linux_cross} CMAKE_SYSTEM_PROCESSOR=ppc64
      CMAKE_C_COMPILER_TARGET=powerpc64-unknown-linux-gnu CMAKE_C_BYTE_ORDER=BIG_ENDIAN
      WANT_ARGS "--cross-file=@WORK@/linux-ppc64-elfv2-cross.ini" WANT_TARGET "linux ppc64, big-endian"
      WANT_FILE "${cross_text}cpu_family = 'ppc64'\ncpu = 'ppc64'\nendian = 'big'\n")
check(linux-ppc64le-cross DEFINE ${linux_cross} CMAKE_SYSTEM_PROCESSOR=ppc64le
      CMAKE_C_COMPILER_TARGET=powerpc64le-linux-gnu CMAKE_C_BYTE_ORDER=LITTLE_ENDIAN
      WANT_ARGS "--cross-file=@WORK@/linux-ppc64le-cross.ini" WANT_TARGET "linux ppc64le, little-endian"
      WANT_FILE "${cross_text}cpu_family = 'ppc64'\ncpu = 'ppc64le'\nendian = 'little'\n")
check(linux-riscv64-cross DEFINE ${linux_cross} CMAKE_SYSTEM_PROCESSOR=riscv64
      CMAKE_C_COMPILER_TARGET=riscv64-linux-gnu CMAKE_C_BYTE_ORDER=LITTLE_ENDIAN
      WANT_ARGS "--cross-file=@WORK@/linux-riscv64-cross.ini" WANT_TARGET "linux riscv64, little-endian"
      WANT_FILE "${cross_text}cpu_family = 'riscv64'\ncpu = 'riscv64'\nendian = 'little'\n")
check(linux-loongarch64-cross DEFINE ${linux_cross} CMAKE_SYSTEM_PROCESSOR=loongarch64
      CMAKE_C_COMPILER_TARGET=loongarch64-linux-gnu CMAKE_C_BYTE_ORDER=LITTLE_ENDIAN
      WANT_ARGS "--cross-file=@WORK@/linux-loongarch64-cross.ini" WANT_TARGET "linux loongarch64, little-endian"
      WANT_FILE "${cross_text}cpu_family = 'loongarch64'\ncpu = 'loongarch64'\nendian = 'little'\n")
# Debian ports' ELFv1 links with GNU ld, and mips64el is the little-endian MIPS64
check(linux-ppc64-cross DEFINE ${linux_cross} CMAKE_SYSTEM_PROCESSOR=ppc64 SNX_PPC64_ELFV2=OFF
      CMAKE_C_COMPILER_TARGET=powerpc64-linux-gnu CMAKE_C_BYTE_ORDER=BIG_ENDIAN
      WANT_ARGS "--cross-file=@WORK@/linux-ppc64-cross.ini" WANT_TARGET "linux ppc64, big-endian"
      WANT_FILE "${cross_text}cpu_family = 'ppc64'\ncpu = 'ppc64'\nendian = 'big'\n")
check(linux-mips64el-cross DEFINE ${linux_cross} CMAKE_SYSTEM_PROCESSOR=mips64el
      CMAKE_C_COMPILER_TARGET=mips64el-unknown-linux-gnu CMAKE_C_BYTE_ORDER=LITTLE_ENDIAN
      WANT_ARGS "--cross-file=@WORK@/linux-mips64el-cross.ini" WANT_TARGET "linux mips64el, little-endian"
      WANT_FILE "${cross_text}cpu_family = 'mips64'\ncpu = 'mips64el'\nendian = 'little'\n")

# Android: the NDK's triple and sysroot, which cross.cmake leaves to the NDK, and NASM for the x86-64 assembly
check(android-x86_64 DEFINE CMAKE_SYSTEM_NAME=Android ANDROID=ON CMAKE_CROSSCOMPILING=ON
      CMAKE_SYSTEM_PROCESSOR=x86_64 CMAKE_C_COMPILER=/ndk/bin/clang CMAKE_AR=/ndk/bin/llvm-ar
      CMAKE_C_COMPILER_TARGET=x86_64-none-linux-android21 CMAKE_SYSROOT=/ndk/sysroot SNX_NASM=/usr/bin/nasm
      WANT_ARGS "--cross-file=@WORK@/android-x86_64.ini" WANT_TARGET "android x86_64, little-endian"
      WANT_FILE "[binaries]\nc = ['/ndk/bin/clang']\nar = ['/ndk/bin/llvm-ar']\nnasm = ['/usr/bin/nasm']\n\n"
                "[built-in options]\nc_args = ['--target=x86_64-none-linux-android21', '--sysroot=/ndk/sysroot']\n"
                "c_link_args = ['--target=x86_64-none-linux-android21', '--sysroot=/ndk/sysroot']\n\n"
                "[properties]\nneeds_exe_wrapper = true\n\n[host_machine]\nsystem = 'android'\n"
                "cpu_family = 'x86_64'\ncpu = 'x86_64'\nendian = 'little'\n")
check(android-arm64-v8a DEFINE CMAKE_SYSTEM_NAME=Android ANDROID=ON CMAKE_CROSSCOMPILING=ON
      CMAKE_SYSTEM_PROCESSOR=aarch64 CMAKE_C_COMPILER=/ndk/bin/clang CMAKE_AR=/ndk/bin/llvm-ar
      CMAKE_C_COMPILER_TARGET=aarch64-none-linux-android21 CMAKE_SYSROOT=/ndk/sysroot
      WANT_ARGS "--cross-file=@WORK@/android-arm64-v8a.ini" WANT_TARGET "android aarch64, little-endian"
      WANT_FILE "[binaries]\nc = ['/ndk/bin/clang']\nar = ['/ndk/bin/llvm-ar']\n\n"
                "[built-in options]\nc_args = ['--target=aarch64-none-linux-android21', '--sysroot=/ndk/sysroot']\n"
                "c_link_args = ['--target=aarch64-none-linux-android21', '--sysroot=/ndk/sysroot']\n\n"
                "[properties]\nneeds_exe_wrapper = true\n\n[host_machine]\nsystem = 'android'\n"
                "cpu_family = 'aarch64'\ncpu = 'aarch64'\nendian = 'little'\n")

# Apple: cmake/apple.cmake's flags. iOS is a cross build meson would otherwise run programs of on the Mac; a Mac's own
# build is native, and gets them in a native file
set(ios_flags "-arch;arm64;-isysroot;/Xcode/iPhoneOS.sdk;-miphoneos-version-min=16.0")
string(REPLACE ";" "\\;" ios_flags_arg "${ios_flags}")
check(ios DEFINE CMAKE_SYSTEM_NAME=iOS APPLE=ON IOS=ON CMAKE_CROSSCOMPILING=ON CMAKE_SYSTEM_PROCESSOR=arm64
      CMAKE_OSX_ARCHITECTURES=arm64 CMAKE_C_COMPILER=/Xcode/clang CMAKE_AR=/Xcode/ar "SNX_APPLE_FLAGS=${ios_flags_arg}"
      WANT_ARGS "--cross-file=@WORK@/ios.ini" WANT_TARGET "darwin aarch64, little-endian"
      WANT_FILE "[binaries]\nc = ['/Xcode/clang']\nar = ['/Xcode/ar']\n\n[built-in options]\n"
                "c_args = ['-arch', 'arm64', '-isysroot', '/Xcode/iPhoneOS.sdk', '-miphoneos-version-min=16.0']\n"
                "c_link_args = ['-arch', 'arm64', '-isysroot', '/Xcode/iPhoneOS.sdk', '-miphoneos-version-min=16.0']\n\n"
                "[properties]\nneeds_exe_wrapper = true\n\n[host_machine]\nsystem = 'darwin'\nsubsystem = 'ios'\n"
                "cpu_family = 'aarch64'\ncpu = 'aarch64'\nendian = 'little'\n")
# (each piece of a WANT_FILE closes the brackets it opens: a list item with an open [ takes the ; after it in)
set(simulator_flags "['-arch', 'arm64', '-isysroot', '/Xcode/iPhoneSimulator.sdk', '-mios-simulator-version-min=16.0']")
check(ios-simulator DEFINE CMAKE_SYSTEM_NAME=iOS APPLE=ON IOS=ON SNX_IOS_SIMULATOR=ON CMAKE_CROSSCOMPILING=ON
      CMAKE_OSX_ARCHITECTURES=arm64 CMAKE_C_COMPILER=/Xcode/clang
      "SNX_APPLE_FLAGS=-arch\\;arm64\\;-isysroot\\;/Xcode/iPhoneSimulator.sdk\\;-mios-simulator-version-min=16.0"
      WANT_ARGS "--cross-file=@WORK@/ios-simulator.ini" WANT_TARGET "darwin aarch64, little-endian"
      WANT_FILE "[binaries]\nc = ['/Xcode/clang']\n\n[built-in options]\n"
                "c_args = ${simulator_flags}\nc_link_args = ${simulator_flags}\n"
                "\n[properties]\nneeds_exe_wrapper = true\n\n"
                "[host_machine]\nsystem = 'darwin'\nsubsystem = 'ios-simulator'\n"
                "cpu_family = 'aarch64'\ncpu = 'aarch64'\nendian = 'little'\n")
check(macos DEFINE CMAKE_SYSTEM_NAME=Darwin APPLE=ON CMAKE_SYSTEM_PROCESSOR=arm64 CMAKE_OSX_ARCHITECTURES=arm64
      CMAKE_C_COMPILER=/Xcode/clang CMAKE_AR=/Xcode/ar
      "SNX_APPLE_FLAGS=-arch\\;arm64\\;-isysroot\\;/Xcode/MacOSX.sdk\\;-mmacosx-version-min=13.0"
      WANT_ARGS "--native-file=@WORK@/macos.ini" WANT_TARGET "darwin aarch64, little-endian"
      WANT_FILE "[binaries]\nc = ['/Xcode/clang']\nar = ['/Xcode/ar']\n\n[built-in options]\n"
                "c_args = ['-arch', 'arm64', '-isysroot', '/Xcode/MacOSX.sdk', '-mmacosx-version-min=13.0']\n"
                "c_link_args = ['-arch', 'arm64', '-isysroot', '/Xcode/MacOSX.sdk', '-mmacosx-version-min=13.0']\n")

# A native build: none where it has no flags (meson takes CC); a native file where cross.cmake sets a CPU baseline
check(linux-x86_64 DEFINE CMAKE_SYSTEM_NAME=Linux CMAKE_SYSTEM_PROCESSOR=x86_64 CMAKE_C_COMPILER=/usr/bin/clang
      CROSS_FLAGS=ON WANT_ARGS "" WANT_TARGET "linux x86_64, little-endian")
check(linux-riscv64 DEFINE CMAKE_SYSTEM_NAME=Linux CMAKE_SYSTEM_PROCESSOR=riscv64 CMAKE_C_COMPILER=/usr/bin/clang
      CROSS_FLAGS=ON WANT_ARGS "--native-file=@WORK@/linux-riscv64.ini" WANT_TARGET "linux riscv64, little-endian"
      WANT_FILE "[binaries]\nc = ['/usr/bin/clang']\n\n[built-in options]\n@FLAGS@\n")

# What no file can be made for: a cross build's system or processor meson has no name for, a ' in a path. A backslash
# and a space are written as they are, which meson reads as themselves
check(linux-sparc64-cross DEFINE CMAKE_SYSTEM_NAME=Linux CMAKE_CROSSCOMPILING=ON CMAKE_SYSTEM_PROCESSOR=sparc64
      CMAKE_C_COMPILER=/usr/bin/clang
      WANT_ARGS "" WANT_ERROR "^meson has no CPU family for a cross build to sparc64$" WANT_TARGET "Linux sparc64")
check(haiku-cross DEFINE CMAKE_SYSTEM_NAME=Haiku CMAKE_CROSSCOMPILING=ON CMAKE_SYSTEM_PROCESSOR=x86_64
      CMAKE_C_COMPILER=/usr/bin/clang
      WANT_ARGS "" WANT_ERROR "^meson has no system name for a cross build to Haiku$" WANT_TARGET "Haiku x86_64")
check(quote-in-sysroot DEFINE CMAKE_SYSTEM_NAME=Android ANDROID=ON CMAKE_CROSSCOMPILING=ON
      CMAKE_SYSTEM_PROCESSOR=aarch64 CMAKE_C_COMPILER=/ndk/bin/clang CMAKE_C_COMPILER_TARGET=aarch64-none-linux-android21
      "CMAKE_SYSROOT=/home/o'neil/ndk/sysroot" WANT_ARGS ""
      WANT_ERROR "^a meson machine file cannot hold the ' or line break in '--sysroot=/home/o'neil/ndk/sysroot'$"
      WANT_TARGET "android aarch64, little-endian")
check(backslash-in-sysroot DEFINE CMAKE_SYSTEM_NAME=Android ANDROID=ON CMAKE_CROSSCOMPILING=ON
      CMAKE_SYSTEM_PROCESSOR=aarch64 "CMAKE_C_COMPILER=C:\\ndk\\bin\\clang.exe"
      CMAKE_C_COMPILER_TARGET=aarch64-none-linux-android21 "CMAKE_SYSROOT=C:\\Android SDK\\ndk\\sysroot"
      WANT_ARGS "--cross-file=@WORK@/backslash-in-sysroot.ini" WANT_TARGET "android aarch64, little-endian"
      WANT_FILE "[binaries]\nc = ['C:\\ndk\\bin\\clang.exe']\n\n[built-in options]\n"
                "c_args = ['--target=aarch64-none-linux-android21', '--sysroot=C:\\Android SDK\\ndk\\sysroot']\n"
                "c_link_args = ['--target=aarch64-none-linux-android21', '--sysroot=C:\\Android SDK\\ndk\\sysroot']\n\n"
                "[properties]\nneeds_exe_wrapper = true\n\n[host_machine]\nsystem = 'android'\n"
                "cpu_family = 'aarch64'\ncpu = 'aarch64'\nendian = 'little'\n")

# meson's own reading of each file made: the machine it names, and for the native ones that it reads them at all
if(MESON AND NOT MESON MATCHES "-NOTFOUND$")
    file(MAKE_DIRECTORY "${WORK}/project")
    file(WRITE "${WORK}/project/meson.build"
         "project('machine')\n"
         "subsystem = '-'\n"
         "if host_machine.system() == 'darwin' and meson.version().version_compare('>=1.2.0')\n"
         "    subsystem = host_machine.subsystem()\n"
         "endif\n"
         "message('host @0@ @1@ @2@ @3@ @4@'.format(host_machine.system(), subsystem, host_machine.cpu_family(),\n"
         "        host_machine.cpu(), host_machine.endian()))\n")
    execute_process(COMMAND "${MESON}" --version OUTPUT_VARIABLE meson_version OUTPUT_STRIP_TRAILING_WHITESPACE)
    file(GLOB files RELATIVE "${WORK}" "${WORK}/*.ini")
    list(SORT files)
    foreach(name IN LISTS files)
        file(STRINGS "${WORK}/${name}" lines)
        set(want "")
        if(lines MATCHES "\\[host_machine\\]")
            set(option --cross-file)
            set(fields "")
            foreach(key IN ITEMS system subsystem cpu_family cpu endian)
                set(value "-")
                foreach(line IN LISTS lines)
                    if(line MATCHES "^${key} = '([^']*)'$")
                        set(value "${CMAKE_MATCH_1}")
                    endif()
                endforeach()
                # asked of an Apple system alone (meson has none for the others), from meson 1.2
                if(key STREQUAL "subsystem" AND (NOT lines MATCHES "system = 'darwin'" OR
                                                 meson_version VERSION_LESS 1.2.0))
                    set(value "-")
                endif()
                list(APPEND fields "${value}")
            endforeach()
            list(JOIN fields " " want)
            set(want "host ${want}")
        else()
            set(option --native-file)
        endif()
        string(REGEX REPLACE "\\.ini$" "" case "${name}")
        execute_process(COMMAND "${MESON}" setup "${WORK}/build-${case}" "${WORK}/project" "${option}=${WORK}/${name}"
                        OUTPUT_VARIABLE out ERROR_VARIABLE out RESULT_VARIABLE rc)
        if(NOT rc EQUAL 0)
            message(SEND_ERROR "${case}: meson ${meson_version} refuses ${option}=${WORK}/${name}:\n${out}")
            set(failed 1)
        elseif(want AND NOT out MATCHES "Message: ${want}\n")
            message(SEND_ERROR "${case}: meson ${meson_version} does not read [${want}] from ${name}:\n${out}")
            set(failed 1)
        else()
            message(STATUS "${case}: meson ${meson_version} reads ${option} ${want}")
        endif()
    endforeach()
else()
    message(STATUS "meson not found: the files are not read by meson")
endif()

if(failed)
    message(FATAL_ERROR "meson_machine: see the errors above")
endif()
