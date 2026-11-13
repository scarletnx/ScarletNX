# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# SNX_SYSTEM_SDL3 takes a system SDL3 from 3.4.12 (cmake/system_sdl3.cmake): a small project finds a prefix's SDL3
# package at 3.4.12 and at 3.4.18, and refuses one at 3.4.11 naming its version, none at all, and one without
# SDL3::SDL3. Run by ctest as system_sdl3, with -DSRC, -DWORK, -DGENERATOR and -DMAKE (the build's, which the
# project is configured with: the system's paths, where CMake would find one, are left out)
file(REMOVE_RECURSE "${WORK}")
set(prefix "${WORK}/prefix")
set(package "${prefix}/lib/cmake/SDL3")
file(WRITE "${WORK}/project/CMakeLists.txt" "cmake_minimum_required(VERSION 3.25)
project(system_sdl3_test LANGUAGES NONE)
include(\"${SRC}/cmake/system_sdl3.cmake\")
snx_system_sdl3()
message(STATUS \"RESULT \${SDL3_VERSION}\")
")
# a prefix with SDL3 at version, with SDL3::SDL3 unless target is empty
function(sdl3 version target)
    file(WRITE "${package}/SDL3Config.cmake" "${target}\n")
    file(WRITE "${package}/SDL3ConfigVersion.cmake"
         "set(PACKAGE_VERSION ${version})\nset(PACKAGE_VERSION_COMPATIBLE TRUE)\n")
endfunction()
# configures the project, which has to say expected: the prefix is the one place it looks, as a machine with SDL3
# installed (CI's Arch containers) would answer the cases without one
function(check label expected)
    execute_process(COMMAND "${CMAKE_COMMAND}" -E env --unset=SDL3_DIR --unset=CMAKE_PREFIX_PATH "${CMAKE_COMMAND}"
                            -S "${WORK}/project" -B "${WORK}/${label}" -G "${GENERATOR}" "-DCMAKE_MAKE_PROGRAM=${MAKE}"
                            "-DCMAKE_PREFIX_PATH=${prefix}"
                            -DCMAKE_FIND_USE_CMAKE_SYSTEM_PATH=OFF -DCMAKE_FIND_USE_SYSTEM_ENVIRONMENT_PATH=OFF
                            -DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF -DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF
                    RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE out)
    string(REGEX REPLACE "\n *" " " flat "${out}") # CMake wraps an error's text
    string(FIND "${flat}" "${expected}" at)
    if(at LESS 0)
        message(FATAL_ERROR "${label}: the configure (exit ${result}) did not say \"${expected}\":\n${out}")
    endif()
endfunction()

set(target "add_library(SDL3::SDL3 INTERFACE IMPORTED)")
sdl3(3.4.12 "${target}") # Arch POWER's
check(oldest "Using the system's SDL3 3.4.12 -- RESULT 3.4.12")
sdl3(3.4.18 "${target}")
check(pinned "RESULT 3.4.18")
sdl3(3.4.11 "${target}")
check(older "SNX_SYSTEM_SDL3: the system's SDL3 is 3.4.11, older than 3.4.12.")
sdl3(3.4.12 "")
check(no-target "SNX_SYSTEM_SDL3: the system's SDL3 3.4.12 has no SDL3::SDL3 target")
file(REMOVE_RECURSE "${package}")
check(none "SNX_SYSTEM_SDL3: no SDL3 package found, 3.4.12 or newer wanted")
file(REMOVE_RECURSE "${WORK}")
