# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# ScarletNX's version: the sum of the numbers cmake/version_parts.cmake names, read from SRC's files. Writes OUT, a
# header defining SNX_VERSION, only when that changed, so a bump relinks the programs that show it and nothing else;
# without OUT, prints the version. With CHECK set, compares OUT with SRC instead, as the version_current test. A part
# that is not one "#define <name> <number>" line in its file fails, naming both
#   cmake -DSRC=<source tree> [-DOUT=<header> [-DCHECK=1]] -P cmake/version.cmake
include("${CMAKE_CURRENT_LIST_DIR}/version_parts.cmake")
set(sum 0)
set(said "")
foreach(part IN LISTS SNX_VERSION_PARTS)
    if(NOT part MATCHES "^(.+):([A-Za-z0-9_]+)$")
        message(FATAL_ERROR "cmake/version_parts.cmake: ${part} is not <file>:<name>")
    endif()
    set(file "${CMAKE_MATCH_1}")
    set(name "${CMAKE_MATCH_2}")
    if(NOT EXISTS "${SRC}/${file}")
        message(FATAL_ERROR "cmake/version_parts.cmake: ${file}, where ${name} is, is not in ${SRC}")
    endif()
    file(STRINGS "${SRC}/${file}" lines REGEX "^#define ${name}[ \t]")
    list(LENGTH lines found)
    if(NOT found EQUAL 1 OR NOT lines MATCHES "^#define ${name}[ \t]+([0-9]+)u?([ \t\r].*)?$")
        message(FATAL_ERROR "cmake/version_parts.cmake: ${file} has ${found} line(s) \"#define ${name} ...\", not one "
                            "with a number: ${lines}")
    endif()
    math(EXPR sum "${sum} + ${CMAKE_MATCH_1}")
    string(APPEND said "   ${name} ${CMAKE_MATCH_1} (${file})\n")
endforeach()
set(header "/* ScarletNX's version, the sum of cmake/version_parts.cmake's numbers:\n${said}*/\n")
string(APPEND header "#define SNX_VERSION \"${sum}\"\n")
if(CHECK)
    file(READ "${OUT}" made)
    if(NOT made STREQUAL header)
        message(FATAL_ERROR "${OUT} says\n${made}where the tree's numbers make\n${header}")
    endif()
elseif(OUT)
    file(CONFIGURE OUTPUT "${OUT}" CONTENT "${header}" @ONLY)
else()
    execute_process(COMMAND "${CMAKE_COMMAND}" -E echo "${sum}")
endif()
