# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# The version of the walk over a game's files for the shader programs it ships (src/nx/gpu/shipped_pass.c): its
# sources' hash. The renderer keeps the files the walk found nothing in under it across builds (SHIP_NONE, its
# .vkscan file), so a change to what the walk finds must change it, and nothing else may: a build whose version differs
# from the file's starts the file over, and its walk reads every file of the game again (51,000 of them for one game,
# minutes). A change to any of them configures again, and snx_ship_walk(source) hands the hash to that source, in the
# calling folder, as SNX_SHIP_WALK.
#
# The hash is a global property, not a variable: only the first folder that includes this file runs it (include_guard),
# and a variable set there is not seen in a sibling folder. The tests' folder includes it before src/nx, so the runtime
# got an empty version in a build with tests and the hash in a build without them, and each switch between those
# builds started the game's .vkscan over.
include_guard(GLOBAL)
set(snx_ship_walk_sources src/nx/gpu/shipped_pass.c src/nx/gpu/shipped_pass.h src/nx/gpu/shader/shipped.c
                          src/nx/gpu/shader/shipped.h)
# out: the first 16 hex digits of the hash of the files' texts, each with its line ends made LF. A checkout with CRLF
# line ends (git's core.autocrlf on Windows) gives the version of one with LF
function(snx_ship_walk_hash out)
    set(all "")
    foreach(path IN LISTS ARGN)
        file(READ "${path}" text)
        string(REPLACE "\r\n" "\n" text "${text}")
        string(SHA256 hash "${text}")
        string(APPEND all "${hash}")
    endforeach()
    string(SHA256 all "${all}")
    string(SUBSTRING "${all}" 0 16 all)
    set(${out} "${all}" PARENT_SCOPE)
endfunction()
# out: the version, in any folder
function(snx_ship_walk_version out)
    get_property(version GLOBAL PROPERTY snx_ship_walk)
    set(${out} "${version}" PARENT_SCOPE)
endfunction()
function(snx_ship_walk_init)
    set(paths "")
    foreach(source IN LISTS snx_ship_walk_sources)
        list(APPEND paths "${PROJECT_SOURCE_DIR}/${source}")
        if(NOT CMAKE_SCRIPT_MODE_FILE)
            set_property(DIRECTORY "${PROJECT_SOURCE_DIR}" APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                         "${PROJECT_SOURCE_DIR}/${source}")
        endif()
    endforeach()
    snx_ship_walk_hash(version ${paths})
    set_property(GLOBAL PROPERTY snx_ship_walk "${version}")
endfunction()
snx_ship_walk_init()
function(snx_ship_walk source)
    snx_ship_walk_version(version)
    string(LENGTH "${version}" n)
    if(NOT n EQUAL 16 OR NOT version MATCHES "^[0-9a-f]+$")
        message(FATAL_ERROR "ship_walk.cmake: the walk's version is \"${version}\", not 16 hex digits")
    endif()
    set_property(SOURCE "${source}" APPEND PROPERTY COMPILE_DEFINITIONS "SNX_SHIP_WALK=\"${version}\"")
endfunction()
