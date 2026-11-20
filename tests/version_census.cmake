# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# Every number in SRC's src/ named as the version's parts are, a #define of a name ending in _ABI or one ending in
# _VERSION with a number, is one cmake/version_parts.cmake lists, as a part of the version or left out of it, and each
# it lists is still there: a new ABI fails here until someone says whether the version counts it.
#   cmake -DSRC=<source tree> -P tests/version_census.cmake
include("${SRC}/cmake/version_parts.cmake")
file(GLOB_RECURSE sources LIST_DIRECTORIES false RELATIVE "${SRC}" "${SRC}/src/*.c" "${SRC}/src/*.h"
     "${SRC}/src/*.inc" "${SRC}/src/*.cpp" "${SRC}/src/*.hpp" "${SRC}/src/*.m")
set(found "")
foreach(path IN LISTS sources)
    file(STRINGS "${SRC}/${path}" lines REGEX
         "^[ \t]*#[ \t]*define[ \t]+[A-Za-z0-9_]*(_ABI([ \t\r].*)?|_VERSION[ \t]+\\(?(0[xX][0-9a-fA-F]+|[0-9]+).*)$")
    foreach(line IN LISTS lines)
        if(line MATCHES "define[ \t]+([A-Za-z0-9_]+)")
            list(APPEND found "${path}:${CMAKE_MATCH_1}")
        endif()
    endforeach()
endforeach()
set(wrong "")
foreach(one IN LISTS found)
    if(NOT one IN_LIST SNX_VERSION_PARTS AND NOT one IN_LIST SNX_VERSION_LEFT_OUT)
        string(APPEND wrong "\n  ${one}: in neither SNX_VERSION_PARTS nor SNX_VERSION_LEFT_OUT")
    endif()
endforeach()
foreach(one IN LISTS SNX_VERSION_LEFT_OUT)
    if(NOT one IN_LIST found)
        string(APPEND wrong "\n  ${one}: left out, and src/ has no such #define there")
    endif()
endforeach()
list(LENGTH sources files)
list(LENGTH found numbers)
if(wrong)
    message(FATAL_ERROR "cmake/version_parts.cmake, against the ${numbers} numbers in ${files} files of src/:${wrong}")
endif()
message(STATUS "the ${numbers} numbers in ${files} files of src/ named as the version's parts are each listed")
