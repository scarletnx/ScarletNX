# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
#
#   cmake -DSRC=<source dir> -DWORK=<scratch dir> -P tests/ship_walk_test.cmake
#
# The version of the shader walk (cmake/ship_walk.cmake), which keys a game's .vkscan file: every folder of the build
# sees the version that the first one to include the file made, and a checkout with CRLF line ends gives the version
# of one with LF. A build whose version differed from the file's started the file over, and its walk read every file of
# the game again.
foreach(var SRC WORK)
    if(NOT DEFINED ${var})
        message(FATAL_ERROR "ship_walk_test.cmake needs -D${var}=...")
    endif()
endforeach()
function(fail what)
    message(FATAL_ERROR "ship_walk: ${what}")
endfunction()
# the tests' folder, which included the file first in a build with tests
function(first_folder)
    set(PROJECT_SOURCE_DIR "${SRC}")
    include("${SRC}/cmake/ship_walk.cmake")
    snx_ship_walk_version(version)
    set(first "${version}" PARENT_SCOPE)
endfunction()
# src/nx, whose include of the file include_guard skips
function(second_folder)
    include("${SRC}/cmake/ship_walk.cmake")
    snx_ship_walk_version(version)
    set(second "${version}" PARENT_SCOPE)
endfunction()
first_folder()
second_folder()
if(NOT first MATCHES "^[0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f]$")
    fail("the version is \"${first}\", not 16 hex digits")
endif()
if(NOT second STREQUAL first)
    fail("the second folder sees the version \"${second}\", the first \"${first}\"")
endif()

# the same text with LF and with CRLF line ends, and a text one character apart
file(REMOVE_RECURSE "${WORK}")
set(text "/* a source */\nint walk(void) {\n    return 1;\n}\n")
file(CONFIGURE OUTPUT "${WORK}/lf.c" CONTENT "${text}" NEWLINE_STYLE UNIX)
file(CONFIGURE OUTPUT "${WORK}/crlf.c" CONTENT "${text}" NEWLINE_STYLE DOS)
string(REPLACE "return 1" "return 2" other "${text}")
file(CONFIGURE OUTPUT "${WORK}/other.c" CONTENT "${other}" NEWLINE_STYLE UNIX)
file(READ "${WORK}/crlf.c" bytes HEX)
if(NOT bytes MATCHES "0d0a")
    fail("the CRLF fixture has no CRLF")
endif()
snx_ship_walk_hash(lf "${WORK}/lf.c" "${WORK}/lf.c")
snx_ship_walk_hash(crlf "${WORK}/crlf.c" "${WORK}/lf.c")
snx_ship_walk_hash(changed "${WORK}/other.c" "${WORK}/lf.c")
if(NOT lf STREQUAL crlf)
    fail("CRLF line ends give the version ${crlf}, LF ones ${lf}")
endif()
if(lf STREQUAL changed)
    fail("a changed source keeps the version ${lf}")
endif()
message(STATUS "ship_walk: version ${first} in both folders, the same with CRLF line ends")
