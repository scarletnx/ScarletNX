# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
#
# On Linux, a native build takes the third-party libraries from the distribution when it has them. The program then
# links them dynamically, so a distribution update replaces the copy the runtime uses. A library the system does not
# have is fetched and built from the pinned source in cmake/dependencies.cmake, as it is on every other system. The
# option is off elsewhere, so Windows, macOS, FreeBSD, Android and the cross builds keep the pinned libraries they had.
#
# FFmpeg, jemalloc and miniz are not taken from the system: the pinned builds carry what the runtime needs, ScarletNX's
# VP9 NVDEC change (cmake/ffmpeg-vp9-nvdec.patch), jemalloc without background threads (cmake/jemalloc_defs.cmake) and
# miniz without zlib's names, which would interpose on libcurl's zlib.
set(snx_system_deps_default OFF)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT ANDROID AND NOT CMAKE_CROSSCOMPILING)
    set(snx_system_deps_default ON)
endif()
option(SNX_SYSTEM_DEPS "Use the distribution's shared libraries on Linux where they exist"
       ${snx_system_deps_default})

# snx_system_library(<out> <target> [PC <module>] [VERSION <version>] [HEADER <header>] [LIBRARY <name>...])
#
# Looks for the system's copy of a library and, when it finds one, defines <target> as an imported interface target
# and sets <out> to TRUE. A pkg-config module is preferred because it carries the include folders and the libraries
# of the package's dependencies. A package without one is found by its header and library. A module older than VERSION
# is not used: a header and library lookup cannot check its version, so the caller builds the pinned source. A static
# archive is not taken either: the pinned source already builds one, and the point of the system copy is that a package
# update replaces the library the runtime uses.
function(snx_system_library out target)
    cmake_parse_arguments(ARG "" "PC;VERSION;HEADER" "LIBRARY" ${ARGN})
    set(${out} FALSE PARENT_SCOPE)
    if(NOT SNX_SYSTEM_DEPS)
        return()
    endif()
    if(TARGET ${target})
        set(${out} TRUE PARENT_SCOPE)
        return()
    endif()

    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND AND ARG_PC)
        pkg_check_modules(SNX_PC_${target} QUIET IMPORTED_TARGET "${ARG_PC}")
        if(SNX_PC_${target}_FOUND)
            if(ARG_VERSION)
                if(NOT SNX_PC_${target}_VERSION
                   OR SNX_PC_${target}_VERSION VERSION_LESS ARG_VERSION)
                    message(STATUS "The system's ${ARG_PC} ${SNX_PC_${target}_VERSION} is older than ${ARG_VERSION}")
                    return()
                endif()
            endif()
            add_library(${target} INTERFACE IMPORTED GLOBAL)
            target_link_libraries(${target} INTERFACE PkgConfig::SNX_PC_${target})
            set(${target}_LINK_LIBRARIES "${SNX_PC_${target}_LINK_LIBRARIES}" PARENT_SCOPE)
            message(STATUS "Using the system's ${ARG_PC} ${SNX_PC_${target}_VERSION}")
            set(${out} TRUE PARENT_SCOPE)
            return()
        endif()
    endif()

    if(ARG_HEADER)
        find_path(SNX_${target}_INCLUDE_DIR NAMES "${ARG_HEADER}")
        if(ARG_LIBRARY)
            find_library(SNX_${target}_LIBRARY NAMES ${ARG_LIBRARY})
            if(SNX_${target}_LIBRARY MATCHES "\\.a$")
                message(STATUS "The system's ${ARG_LIBRARY} is a static archive; using the pinned source")
                return()
            endif()
        endif()
        if(SNX_${target}_INCLUDE_DIR AND (NOT ARG_LIBRARY OR SNX_${target}_LIBRARY))
            add_library(${target} INTERFACE IMPORTED GLOBAL)
            target_include_directories(${target} INTERFACE "${SNX_${target}_INCLUDE_DIR}")
            if(ARG_LIBRARY)
                target_link_libraries(${target} INTERFACE "${SNX_${target}_LIBRARY}")
                set(${target}_LINK_LIBRARIES "${SNX_${target}_LIBRARY}" PARENT_SCOPE)
            endif()
            message(STATUS "Using the system's ${ARG_HEADER} from ${SNX_${target}_INCLUDE_DIR}")
            set(${out} TRUE PARENT_SCOPE)
            return()
        endif()
    endif()
endfunction()
