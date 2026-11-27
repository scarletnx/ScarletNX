# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# source_paths: every file or folder the tests' CMake names as a whole path under the source tree exists: a test of a
# file that went fails at its run, not where its CMake still names it. ${PROJECT_SOURCE_DIR}/... is checked in every
# file, ${CMAKE_CURRENT_SOURCE_DIR}/... in the files of the tests folder itself (tests/cmake/'s are included from other
# folders too). A path that goes on with a variable is a prefix, not checked.
#   cmake -DROOT=<the source tree> -DTESTS=<the tests folder> -DFILES=<the CMake files, split by |> -P source_paths.cmake
string(REPLACE "|" ";" files "${FILES}")
set(missing 0)
set(count 0)
foreach(file IN LISTS files)
    file(READ "${file}" text)
    get_filename_component(folder "${file}" DIRECTORY)
    set(kinds PROJECT)
    if(folder STREQUAL TESTS)
        set(kinds "PROJECT|CMAKE_CURRENT")
    endif()
    string(REGEX MATCHALL "\\$\\{(${kinds})_SOURCE_DIR\\}/[A-Za-z0-9_./+-]+[^A-Za-z0-9_./+$-]" paths "${text}")
    foreach(path IN LISTS paths)
        string(REGEX REPLACE ".$" "" path "${path}")
        string(REGEX REPLACE "\\.$" "" path "${path}")
        string(REPLACE "\${PROJECT_SOURCE_DIR}" "${ROOT}" whole "${path}")
        string(REPLACE "\${CMAKE_CURRENT_SOURCE_DIR}" "${TESTS}" whole "${whole}")
        math(EXPR count "${count} + 1")
        if(NOT EXISTS "${whole}")
            file(RELATIVE_PATH where "${ROOT}" "${file}")
            message("source_paths: ${where} names ${path}, which is not there")
            set(missing 1)
        endif()
    endforeach()
endforeach()
if(missing)
    message(FATAL_ERROR "source_paths: the tests' CMake names a source that is not there")
endif()
message("source_paths: ${count} paths, each there")
