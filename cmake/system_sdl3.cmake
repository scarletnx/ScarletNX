# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# The system's SDL3 package (SNX_SYSTEM_SDL3, cmake/dependencies.cmake), from 3.4.12: Arch POWER's, the one the
# PowerPC64 AppImages bundle. Every SDL name the tree uses is in it; 3.4.13 to 3.4.18 add none the tree uses. A missing
# package, an older one or one without SDL3::SDL3 stops the configure, saying which
include_guard(GLOBAL)
set(SNX_SDL3_OLDEST 3.4.12)
# a macro: find_package's variables and SDL3::SDL3 are the caller's
macro(snx_system_sdl3)
    find_package(SDL3 CONFIG QUIET)
    if(NOT SDL3_FOUND)
        message(FATAL_ERROR "SNX_SYSTEM_SDL3: no SDL3 package found, ${SNX_SDL3_OLDEST} or newer wanted (FreeBSD: "
                            "pkg install sdl3; NetBSD: pkgin install SDL3; Arch: pacman -S sdl3). "
                            "Use -DSNX_SYSTEM_SDL3=OFF for CPM's pinned source build.")
    endif()
    if(SDL3_VERSION VERSION_LESS SNX_SDL3_OLDEST)
        message(FATAL_ERROR "SNX_SYSTEM_SDL3: the system's SDL3 is ${SDL3_VERSION}, older than ${SNX_SDL3_OLDEST}. "
                            "Use -DSNX_SYSTEM_SDL3=OFF for CPM's pinned source build.")
    endif()
    if(NOT TARGET SDL3::SDL3)
        message(FATAL_ERROR "SNX_SYSTEM_SDL3: the system's SDL3 ${SDL3_VERSION} has no SDL3::SDL3 target")
    endif()
    message(STATUS "Using the system's SDL3 ${SDL3_VERSION}")
endmacro()
