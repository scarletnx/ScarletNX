# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# cmake/llvm_opt.cmake on opts of its own (shell scripts named opt-<version>, which print their version and, for a
# pipeline, their arguments): the LLVM's exact version prints the pipelines, s and z as O2's; another version, even
# the same with a suffix, is refused with both versions named; a level the opt fails on is said and the others kept;
# an opt that doesn't run is said. Run by ctest as llvm_opt, with LLVM_OPT_CMAKE the module and WORK a folder of its own
include("${LLVM_OPT_CMAKE}")
file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")
foreach(name 22.1.0 23.1.3 22.1.0git 22.1.0-lame)
    file(WRITE "${WORK}/opt-${name}" [=[#!/bin/sh
version=$(basename "$0" | sed -e 's/^opt-//' -e 's/-lame$//')
if [ "$1" = --version ]; then
    echo "  LLVM version $version"
    echo "  Optimized build."
    exit 0
fi
case "$0:$*" in *-lame:*O1*) echo "no O1 here" >&2; exit 1;; esac
echo "passes($*)"
]=])
    file(CHMOD "${WORK}/opt-${name}" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
endforeach()
# the target's triple reaches opt: a target adds passes of its own to the pipelines
set(args "-mtriple=aarch64-unknown-linux-gnu -passes=default<O?> -print-pipeline-passes -disable-output")

snx_llvm_pipelines("${WORK}/opt-22.1.0" 22.1.0 aarch64-unknown-linux-gnu "${WORK}" text why)
if(NOT why STREQUAL "")
    message(FATAL_ERROR "the LLVM's own opt is refused: ${why}")
endif()
set(expected "") # s and z right after 2, whose pipeline they are
foreach(level 0 1 2 s z 3)
    string(REPLACE "?" "${level}" line "${args}")
    if(level MATCHES "[sz]")
        string(REPLACE "?" "2" line "${args}")
    endif()
    string(APPEND expected "${level} passes(${line})\n")
endforeach()
if(NOT text STREQUAL expected)
    message(FATAL_ERROR "the pipelines are not the six expected:\n${text}\nexpected:\n${expected}")
endif()

foreach(other 23.1.3 22.1.0git)
    snx_llvm_pipelines("${WORK}/opt-${other}" 22.1.0 x86_64-pc-linux-gnu "${WORK}" text why)
    if(NOT text STREQUAL "" OR NOT why MATCHES "opt-${other} is LLVM version ${other}, not LLVM version 22\\.1\\.0")
        message(FATAL_ERROR "opt ${other} for LLVM 22.1.0 gave pipelines, or another why: ${text}\n${why}")
    endif()
endforeach()
snx_llvm_pipelines("${WORK}/opt-22.1.0git" 22.1.0git x86_64-pc-linux-gnu "${WORK}" text why)
if(NOT why STREQUAL "" OR NOT text MATCHES "^0 passes")
    message(FATAL_ERROR "the opt of a git LLVM, for that LLVM, is refused: ${why}")
endif()

snx_llvm_pipelines("${WORK}/opt-22.1.0-lame" 22.1.0 x86_64-pc-linux-gnu "${WORK}" text why)
if(NOT why MATCHES "no pipeline for -O1, whose passes LLVM Internals can't turn off: 1 no O1 here" OR why MATCHES "for -O[023]")
    message(FATAL_ERROR "the opt that fails on -O1 is not said so, or more is: ${why}")
endif()
if(text MATCHES "\n1 " OR NOT text MATCHES "^0 passes.*\n2 passes.*\ns passes.*\nz passes.*\n3 passes")
    message(FATAL_ERROR "the opt that fails on -O1 did not keep the other levels: ${text}")
endif()

snx_llvm_pipelines("${WORK}/opt-none" 22.1.0 x86_64-pc-linux-gnu "${WORK}" text why)
if(NOT text STREQUAL "" OR NOT why MATCHES "opt-none does not run")
    message(FATAL_ERROR "an opt that is not there is not said so: ${text}\n${why}")
endif()
file(REMOVE_RECURSE "${WORK}")
