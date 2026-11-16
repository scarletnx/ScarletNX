# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
#
#   cmake -DSRC=<source dir> -DWORK=<scratch dir> [-DGENERATOR=<generator> -DMAKE=<its program>]
#         -P tests/ship_walk_test.cmake
#
# The version of the shader walk (cmake/ship_walk.cmake), which keys a game's .vkscan file. A build whose version
# differs from the file's starts the file over, and its walk reads every file of the game again.
#
# 1. Two sibling folders of one configure, as tests/ and src/nx are, each include the file and call snx_ship_walk.
#    Both must get the same 16-digit version. Before, the second folder's include was skipped (include_guard) and it
#    did not see the first folder's variable: the runtime got SNX_SHIP_WALK="" in each build with tests.
# 2. A source with CRLF line ends gives the version of the same source with LF; a changed source gives another.
foreach(var SRC WORK)
    if(NOT ${var})
        message(FATAL_ERROR "ship_walk_test.cmake needs -D${var}=...")
    endif()
endforeach()
function(fail what)
    message(FATAL_ERROR "ship_walk: ${what}")
endfunction()
# a version as snx_ship_walk gives it: 16 hex digits
function(check_version what version)
    string(LENGTH "${version}" n)
    if(NOT n EQUAL 16 OR NOT version MATCHES "^[0-9a-f]+$")
        fail("${what}: the version is \"${version}\", not 16 hex digits")
    endif()
endfunction()
file(REMOVE_RECURSE "${WORK}")

# 1. the project: the walk's sources and cmake file, and two folders that each record the define they got
set(project "${WORK}/project")
foreach(source cmake/ship_walk.cmake src/nx/gpu/shipped_pass.c src/nx/gpu/shipped_pass.h src/nx/gpu/shader/shipped.c
               src/nx/gpu/shader/shipped.h)
    get_filename_component(dir "${source}" DIRECTORY)
    file(COPY "${SRC}/${source}" DESTINATION "${project}/${dir}")
endforeach()
file(WRITE "${project}/CMakeLists.txt" "cmake_minimum_required(VERSION 3.20)
project(ship_walk_test NONE)
add_subdirectory(first)  # tests/, which includes the file first in a build with tests
add_subdirectory(second) # src/nx, the runtime's folder
")
foreach(folder first second)
    file(WRITE "${project}/${folder}/CMakeLists.txt" "include(\"\${PROJECT_SOURCE_DIR}/cmake/ship_walk.cmake\")
snx_ship_walk(walk.c)
get_source_file_property(defs walk.c COMPILE_DEFINITIONS)
file(WRITE \"\${CMAKE_BINARY_DIR}/${folder}.txt\" \"\${defs}\")
")
endforeach()
set(generator "")
if(GENERATOR)
    set(generator -G "${GENERATOR}" "-DCMAKE_MAKE_PROGRAM=${MAKE}")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" -S "${project}" -B "${WORK}/build" ${generator} RESULT_VARIABLE rc
                OUTPUT_VARIABLE out ERROR_VARIABLE out)
if(rc)
    fail("the configure failed:\n${out}")
endif()
foreach(folder first second)
    file(READ "${WORK}/build/${folder}.txt" defs)
    if(NOT defs MATCHES "^SNX_SHIP_WALK=\"([^\"]*)\"$")
        fail("the ${folder} folder's source has the definitions \"${defs}\", not one SNX_SHIP_WALK")
    endif()
    check_version("the ${folder} folder" "${CMAKE_MATCH_1}")
    set(${folder} "${CMAKE_MATCH_1}")
endforeach()
if(NOT second STREQUAL first)
    fail("the second folder got the version ${second}, the first ${first}")
endif()

# 2. the same text with LF and with CRLF line ends, and a text one character apart
set(PROJECT_SOURCE_DIR "${SRC}")
include("${SRC}/cmake/ship_walk.cmake")
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
check_version("LF" "${lf}")
if(NOT lf STREQUAL crlf)
    fail("CRLF line ends give the version ${crlf}, LF ones ${lf}")
endif()
if(lf STREQUAL changed)
    fail("a changed source keeps the version ${lf}")
endif()
message(STATUS "ship_walk: version ${first} in both folders, the same with CRLF line ends")
