# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# recompiler_symbols (tests/CMakeLists.txt): the recompiler's archive (ARCHIVE, docs/dev/library.md) needs nothing from
# outside it but the system's libraries (the C library, threads, dl, m; advapi32 on Windows). NM lists the
# symbols its objects call that none of them defines, each with the objects that call it. TARGET, a program of BUILD
# that links every object of the archive with those libraries alone, is then built: its link fails on any symbol they
# lack, and the test names those symbols and their objects. For another archive (recompiler_core_symbols,
# tests/cmake/recompiler_core.cmake) LIBRARIES says whose the symbols it may call are, and RULE what keeps it so
cmake_minimum_required(VERSION 3.25)
foreach(name BUILD TARGET ARCHIVE NM)
    if(NOT ${name})
        message(FATAL_ERROR "recompiler_symbols.cmake needs ${name}: BUILD the build tree, TARGET its program that "
                            "links the archive, ARCHIVE the archive's file, NM the nm that reads it")
    endif()
endforeach()

if(NOT LIBRARIES)
    set(LIBRARIES "the system's")
    set(RULE "The recompiler's library holds the core alone (docs/dev/library.md)")
endif()

# -g the global symbols, -P one line each (name, type, value), -A the archive member it is in: "lib.a[ir.c.o]: name U"
# (llvm-nm) or "lib.a:ir.c.o: name U" (GNU nm)
execute_process(COMMAND "${NM}" -g -P -A "${ARCHIVE}" OUTPUT_VARIABLE listing ERROR_VARIABLE nm_error
                RESULT_VARIABLE nm_result)
if(NOT nm_result EQUAL 0)
    message(FATAL_ERROR "${NM} -g -P -A ${ARCHIVE} failed (${nm_result}): ${nm_error}")
endif()
string(REGEX MATCHALL "[^\n]+" lines "${listing}")
set(defined "")
set(called "")
set(read 0)
foreach(line IN LISTS lines)
    if(NOT line MATCHES "^(.*): ([^ ]+) ([A-Za-z?])( |$)")
        continue()
    endif()
    math(EXPR read "${read} + 1")
    set(where "${CMAKE_MATCH_1}")
    set(symbol "${CMAKE_MATCH_2}")
    set(type "${CMAKE_MATCH_3}")
    if(type STREQUAL "U")
        if(where MATCHES "\\[([^]]+)\\]$") # apart: a MATCHES that fails after one that matched empties CMAKE_MATCH_1
            set(where "${CMAKE_MATCH_1}")
        elseif(where MATCHES ":([^:]+)$")
            set(where "${CMAKE_MATCH_1}")
        endif()
        string(MAKE_C_IDENTIFIER "${symbol}" key)
        if(NOT DEFINED callers_${key})
            list(APPEND called "${symbol}")
        endif()
        list(APPEND callers_${key} "${where}")
    else()
        list(APPEND defined "${symbol}")
    endif()
endforeach()
if(read EQUAL 0 OR NOT called)
    string(SUBSTRING "${listing}" 0 2000 start)
    message(FATAL_ERROR "${NM} -g -P -A ${ARCHIVE} gave ${read} lines read as an object, a symbol and its type, and "
                        "no symbol an object calls. Its output begins:\n${start}")
endif()
set(outside "${called}")
if(defined)
    list(REMOVE_DUPLICATES defined)
    list(REMOVE_ITEM outside ${defined})
endif()
list(LENGTH outside outside_count)

set(build_config "")
if(CONFIG)
    set(build_config --config "${CONFIG}")
endif()
# the C locale: the linker's words below are its English ones
execute_process(COMMAND "${CMAKE_COMMAND}" -E env --unset=LANGUAGE LC_ALL=C
                        "${CMAKE_COMMAND}" --build "${BUILD}" --target "${TARGET}" ${build_config}
                OUTPUT_VARIABLE build_output ERROR_VARIABLE build_output RESULT_VARIABLE build_result)
if(build_result EQUAL 0)
    message(STATUS "${ARCHIVE} calls ${outside_count} symbols from outside it, every one ${LIBRARIES}")
    return()
endif()

# the linkers' words for a symbol they found nowhere: lld's and mold's, GNU ld's, Apple's (with Mach-O's leading _, as
# nm prints it); lld for MinGW names an import __declspec(dllimport) NAME, which the object calls as __imp_NAME
string(REGEX MATCHALL "undefined symbol: [^\n]+|undefined reference to [`'][^'\n]+'|\"[^\"\n]+\", referenced from"
       found "${build_output}")
set(missing "")
# one test each: a MATCHES that fails after one that matched empties CMAKE_MATCH_1
foreach(item IN LISTS found)
    if(item MATCHES "^undefined symbol: __declspec\\(dllimport\\) ([^ ]+)")
        list(APPEND missing "__imp_${CMAKE_MATCH_1}")
    elseif(item MATCHES "^undefined symbol: ([^ ]+)")
        list(APPEND missing "${CMAKE_MATCH_1}")
    elseif(item MATCHES "^undefined reference to [`']([^']+)'")
        list(APPEND missing "${CMAKE_MATCH_1}")
    elseif(item MATCHES "^\"([^\"]+)\", referenced from")
        list(APPEND missing "${CMAKE_MATCH_1}")
    endif()
endforeach()
list(REMOVE_DUPLICATES missing)
set(report "")
foreach(symbol IN LISTS missing)
    string(MAKE_C_IDENTIFIER "${symbol}" key)
    if(DEFINED callers_${key})
        list(REMOVE_DUPLICATES callers_${key})
        list(JOIN callers_${key} ", " objects)
        string(APPEND report "  ${symbol}: ${objects}\n")
    else()
        string(APPEND report "  ${symbol}: no object of the archive calls it\n")
    endif()
endforeach()
list(LENGTH missing missing_count)
if(missing_count EQUAL 0)
    message(FATAL_ERROR "${TARGET} did not build (${build_result}), and its output names no symbol its link lacks:\n"
                        "${build_output}")
endif()
message(FATAL_ERROR "${ARCHIVE} calls ${outside_count} symbols from outside it; ${missing_count} of them are not "
                    "${LIBRARIES}, the only libraries ${TARGET}'s link takes. Each, with the objects that call "
                    "it:\n${report}${RULE}: a call into another library of this tree, or a new external library, makes "
                    "it link that too. ${TARGET}'s link:\n${build_output}")
