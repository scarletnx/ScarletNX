# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# Builds the ScarletNX runtime for Linux or FreeBSD, for Windows with llvm-mingw (natively, or cross-built), or for
# macOS and iOS with Xcode's Clang (the macos and ios presets, docs/APPLE.md). The
# game module (<Game>_code.so or .dll) comes from `scarletnx -r`. The
# third-party libraries are fetched and built with the runtime, see cmake/dependencies.cmake.
#
#   cmake --preset default && cmake --build --preset default        (or windows-cross; see CMakePresets.json)
#   cmake -B build && cmake --build build                           (without the presets)
#
# After each build the runtime is copied next to every recompiled game in the data folder's games/ (SNX_DEPLOY), so a
# runtime fix reaches the games without recompiling them.
cmake_minimum_required(VERSION 3.25)

# Clang only: the runtime, the generated game code and the third-party libraries are built and tested with one
# compiler. It is the default here, so a plain `cmake -B build` picks it over the system's cc
if(NOT DEFINED CMAKE_C_COMPILER AND NOT DEFINED ENV{CC})
    set(CMAKE_C_COMPILER clang)
endif()
if(NOT DEFINED CMAKE_CXX_COMPILER AND NOT DEFINED ENV{CXX})
    set(CMAKE_CXX_COMPILER clang++)
endif()
project(ScarletNX LANGUAGES C CXX)
# Xcode's Clang names itself AppleClang: the same compiler, with Apple's version numbers
if(NOT CMAKE_C_COMPILER_ID MATCHES "^(Apple)?Clang$" OR NOT CMAKE_CXX_COMPILER_ID MATCHES "^(Apple)?Clang$")
    message(FATAL_ERROR "ScarletNX builds with Clang only, and this is ${CMAKE_C_COMPILER_ID} "
                        "(${CMAKE_C_COMPILER}). Use a preset, or -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ "
                        "in a new build folder.")
endif()
# The Apple apps are what `cmake --install` makes (cmake/apple_package.cmake): on macOS the prefix is ScarletNX.app's
# Contents, the programs in its MacOS/ and share/ beside them; on iOS share/ goes in the app the build makes. Over
# GNUInstallDirs' defaults, which a build folder configured before keeps in its cache
if(IOS AND NOT CMAKE_INSTALL_DATADIR)
    set(CMAKE_INSTALL_DATADIR scarletnx.app/share)
elseif(APPLE AND NOT IOS AND (NOT CMAKE_INSTALL_BINDIR OR CMAKE_INSTALL_BINDIR STREQUAL "bin"))
    set(CMAKE_INSTALL_BINDIR MacOS)
endif()
# the CPU baseline (POWER4 on PowerPC64) and a Linux cross build's target and sysroot, for FFmpeg, OpenSSL and the
# recompiler's runtime library
include(cmake/cross.cmake)
# the iOS and macOS apps' frontend (src/gui/apple.m and dist/apple's Swift, docs/APPLE.md), which the runtime links
# with the menu: the languages are enabled where every target that links them is below
if(APPLE AND (NOT DEFINED SNX_BUILD_GUI OR SNX_BUILD_GUI))
    enable_language(OBJC)
    enable_language(Swift)
endif()

# GNU dialects: the runtime uses GNU extensions (statement expressions, __atomic builtins)
set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)
# The strict ISO C (RFC #142: the driver, swipc, the build info and their tests) builds as C99 with -pedantic-errors
# and without extensions (C_STANDARD 99, C_EXTENSIONS OFF on those targets)

option(SNX_BUILD_NX "Build the Switch Horizon/Maxwell runtime" ON)
option(SNX_BUILD_GNUABI "Build the CPU-only Linux AArch64 runtime" ON)
option(SNX_BUILD_3DS "Build the 3DS driver: extraction, the ARM11 AOT compiler and the runtime" ON)
option(SNX_BUILD_VITA "Build the PS Vita driver: VPK extraction, the AArch32 AOT compiler and the runtime" ON)
option(SNX_BUILD_AGB "Build the Game Boy Advance AGB board runtime" ON)
# Reject stale backend selections instead of silently changing an existing build's behavior.
if(SNX_BUILD_NX AND DEFINED SNX_BACKEND AND NOT SNX_BACKEND STREQUAL "sdl3")
    message(FATAL_ERROR "SNX_BACKEND=${SNX_BACKEND}: SDL3 is now mandatory for the NX runtime. "
                        "Configure a new build folder, or remove SNX_BACKEND with -USNX_BACKEND. "
                        "Use -H/--headless at runtime to run without a window.")
endif()
option(SNX_DEPLOY "Copy the runtime next to every recompiled game in SNX_RECOMPILED_DIR after building" ON)
set(SNX_RECOMPILED_DIR "" CACHE PATH
    "Folder holding the recompiled games (empty: recompiled/ of the drivers' data folder)")
# SNX_GAMES_DIR, its name before (docs/DEPRECATIONS.md): taken while SNX_RECOMPILED_DIR is empty, and said
if(DEFINED SNX_GAMES_DIR)
    if(NOT SNX_GAMES_DIR STREQUAL "" AND SNX_RECOMPILED_DIR STREQUAL "")
        message(STATUS "SNX_GAMES_DIR=${SNX_GAMES_DIR} is now SNX_RECOMPILED_DIR=${SNX_GAMES_DIR}")
        set_property(CACHE SNX_RECOMPILED_DIR PROPERTY VALUE "${SNX_GAMES_DIR}")
    elseif(NOT SNX_GAMES_DIR STREQUAL "")
        message(STATUS "SNX_GAMES_DIR=${SNX_GAMES_DIR} is not used: SNX_RECOMPILED_DIR=${SNX_RECOMPILED_DIR} is set")
    endif()
    unset(SNX_GAMES_DIR CACHE)
endif()
# the checkout's games/ was the default while the drivers of a build tree worked in it (docs/DEPRECATIONS.md)
if(SNX_RECOMPILED_DIR STREQUAL "${PROJECT_SOURCE_DIR}/games")
    message(STATUS "SNX_RECOMPILED_DIR=${SNX_RECOMPILED_DIR} was the checkout's games/, an older version's default: "
                   "the data folder's recompiled/ from now on")
    set_property(CACHE SNX_RECOMPILED_DIR PROPERTY VALUE "")
endif()

# RelWithDebInfo builds with -O2 and adds symbols for crash reports. It leaves out -DNDEBUG, which keeps the runtime's
# few assert()s (hle/kernel/ipc.c's full pools) and VMA's assertions on.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()
# CI's cross builds, which no one debugs, set -O2 -g0: compiles, links and the compiler cache are smaller without
# the symbols
set(SNX_OPT_FLAGS "-O2 -g" CACHE STRING "Optimization and debug flags of a RelWithDebInfo build")
set(CMAKE_C_FLAGS_RELWITHDEBINFO "${SNX_OPT_FLAGS}")
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "${SNX_OPT_FLAGS}")

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)
option(SNX_TOOLS_ONLY "Build the C99 extraction/recompilation driver without the runtime" OFF)
set(snx_gui_default OFF)
if(SNX_BUILD_NX AND NOT SNX_TOOLS_ONLY)
    set(snx_gui_default ON)
endif()
option(SNX_BUILD_GUI "Build the keyboard-driven SDL3 launcher" ${snx_gui_default})
# The program, scarletnx, is every driver the build has (src/multicall): each driver is a library, and the program
# runs the one its name says. scarletnx-switch, scarletnx-3ds, scarletnx-vita, scarletnx-gnuabi and scarletnx-agb are
# links to it on Linux and macOS
if(DEFINED CACHE{SNX_MULTICALL}) # an option before, said and taken out of the cache
    message(STATUS "SNX_MULTICALL is no longer an option: scarletnx is every driver the build has")
    unset(SNX_MULTICALL CACHE)
endif()
if(SNX_BUILD_GUI AND SNX_TOOLS_ONLY)
    message(FATAL_ERROR "SNX_BUILD_GUI requires a runtime build, not a tools-only build")
endif()
option(SNX_TRACY "Build the runtime with the Tracy profiler client (development builds)" OFF)
if(SNX_TRACY AND NOT SNX_BUILD_NX)
    message(FATAL_ERROR "SNX_TRACY requires SNX_BUILD_NX")
endif()
if(SNX_TRACY AND SNX_TOOLS_ONLY)
    message(FATAL_ERROR "SNX_TRACY requires the runtime, not a tools-only build")
endif()
if(SNX_TRACY AND IOS)
    message(FATAL_ERROR "SNX_TRACY is not available on iOS")
endif()
if(SNX_TRACY AND (CMAKE_C_COMPILER_ID MATCHES "GNU|Clang" OR CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang"))
    # The sampler's stacks are walked by the frame pointers, which -O2 drops.
    add_compile_options(-fno-omit-frame-pointer)
endif()
# No post-build copy of the profiled runtime over every game. The driver still hands a game the runtime that plays it,
# so the first run through a profiled build puts it beside that game, and a later run through a normal build puts that
# one back (cmake/deploy.cmake, driver.c's refresh_runtime).
if(SNX_TRACY AND SNX_DEPLOY)
    set(SNX_DEPLOY OFF CACHE BOOL "Copy the runtime next to every recompiled game in SNX_GAMES_DIR after building" FORCE)
    message(STATUS "SNX_TRACY: no post-build deploy of the profiling runtime (a run of the profiled driver still puts "
                   "it beside its game)")
endif()
# A project that adds ScarletNX (CPM, add_subdirectory) builds none of its tests unless it asks: a plain variable here
# rather than BUILD_TESTING in the cache, which the including project's own tests read
option(SNX_BUILD_TESTING "Build ScarletNX's tests (off when another project includes it)" ${PROJECT_IS_TOP_LEVEL})
if(NOT SNX_BUILD_TESTING)
    set(BUILD_TESTING OFF)
endif()
# Before the dependencies: the tests' network fixtures link ENet and zstd, whatever the build
include(CTest)
# Before the sources: the C99 driver of a tools-only build links LZ4 too
include(cmake/dependencies.cmake)
# every local include is a path from src/ ("nx/hle/kernel/ipc.h"); generated and third-party headers come with their
# targets
include_directories("${PROJECT_SOURCE_DIR}/src")
# every setting, generated from src/nx/runtime/settings.txt (struct Settings, g_settings, their tables)
include(cmake/settings.cmake)
add_subdirectory(src/common)
if(SNX_BUILD_NX)
    add_subdirectory(src/nx/runtime/driver host-tools)
endif()
# The recompiler library (docs/LIBRARY.md) is configured by the NX driver's tree or the GNU ABI runtime's. Without
# either, as a project that only wants the library builds ScarletNX, it is configured here, with LLVM alone
if(NOT SNX_BUILD_NX AND NOT SNX_BUILD_GNUABI)
    include(cmake/llvm.cmake)
    add_subdirectory(src/common/cpu/recompiler/llvm llvm-backend)
    add_subdirectory(src/common/cpu/recompiler recompiler-library)
endif()
include(cmake/build_info.cmake) # the window titles' line; the driver stamps games with its commit
if(SNX_BUILD_3DS)
    # the window's icon, as the Switch runtime's: made here, as the folder including it first owns snx_window_icon,
    # and a target of src/3ds compiles only src/3ds (tests/architecture.py)
    include(cmake/window_icon.cmake)
    add_subdirectory(src/3ds)
endif()
if(SNX_BUILD_VITA AND NOT SNX_TOOLS_ONLY)
    add_subdirectory(src/vita)
endif()
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
# the program, wherever the build has a driver: it links the drivers' libraries by name, the ones configured below too,
# by the conditions their folders are configured by here (a build of the host tools has no Vita driver, and with the
# Switch's tools it stops here)
if(SNX_BUILD_NX OR SNX_BUILD_3DS OR SNX_BUILD_GNUABI OR SNX_BUILD_AGB OR (SNX_BUILD_VITA AND NOT SNX_TOOLS_ONLY))
    add_subdirectory(src/multicall)
endif()
if(SNX_TOOLS_ONLY AND SNX_BUILD_NX)
    include(GNUInstallDirs)
    install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/scarletnx)
    return()
endif()
if(SNX_BUILD_GNUABI)
    add_subdirectory(src/gnuabi)
endif()
if(SNX_BUILD_AGB)
    # the runner's window icon, made here as the 3DS's is: a target of src/agb compiles only src/agb
    include(cmake/window_icon.cmake)
    add_subdirectory(src/agb)
endif()
if(SNX_BUILD_GUI)
    add_subdirectory(src/gui)
endif()
# The icon both programs' desktop entries name, in every size (scripts/make-icons.py); Windows has it in the executables
if(NOT WIN32)
    include(GNUInstallDirs)
    install(FILES dist/wimsy.svg DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps COMPONENT desktop)
    install(DIRECTORY dist/icons/hicolor DESTINATION ${CMAKE_INSTALL_DATADIR}/icons COMPONENT desktop)
endif()
# jemalloc's, which the program links whatever runtimes it holds (SNX_JEMALLOC)
if(SNX_JEMALLOC)
    include(GNUInstallDirs)
    install(FILES "${jemalloc_SOURCE_DIR}/COPYING" DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/scarletnx/jemalloc)
endif()
if(NOT SNX_BUILD_NX)
    include(GNUInstallDirs)
    install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/scarletnx)
    # the Linux runtime alone (a big-endian host's build): its desktop entry, for the AppImage made of it
    # (scripts/make-appimage.sh), and the license of OpenSSL, which it links
    if(SNX_BUILD_GNUABI AND NOT SNX_BUILD_3DS)
        if(NOT WIN32)
            install(FILES dist/scarletnx-gnuabi.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications
                    COMPONENT desktop)
        endif()
        install(FILES "${OpenSSL_SOURCE_DIR}/LICENSE.txt" DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/scarletnx/OpenSSL)
    endif()
    return()
endif()
if(BUILD_TESTING AND TARGET hactool_lib)
    add_executable(snx-test-hactool-decrypted tests/hactool_decrypted.c)
    snx_test_target(snx-test-hactool-decrypted 11)
    target_link_libraries(snx-test-hactool-decrypted PRIVATE hactool_internals)
    add_test(NAME hactool_decrypted COMMAND snx-test-hactool-decrypted)
    # the storages ported from LibHac (src/nx/runtime/driver/hactool/storage.c), and NCAs read in place in a container
    add_executable(snx-test-hactool-layers tests/hactool_layers.c)
    snx_test_target(snx-test-hactool-layers 11)
    target_link_libraries(snx-test-hactool-layers PRIVATE hactool_internals)
    add_test(NAME hactool_layers COMMAND snx-test-hactool-layers)
    snx_sanitizer_path() # tests/CMakeLists.txt
endif()
add_subdirectory(src/nx)
add_subdirectory(tools)

# `cmake --install` puts the runtime, the desktop entry and the licenses under CMAKE_INSTALL_PREFIX, which is
# what the AppImage and the CI artifacts are made from (.github/workflows/build.yml)
include(GNUInstallDirs) # the program itself is src/multicall's
if(NOT WIN32)
    install(FILES dist/scarletnx.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications COMPONENT desktop)
    install(FILES dist/io.github.scarletnx.scarletnx.metainfo.xml DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo COMPONENT desktop)
endif()
if(ANDROID) # the app's launcher icon (dist/android)
    install(FILES dist/icons/hicolor/256x256/apps/wimsy.png DESTINATION res/mipmap-xxxhdpi RENAME ic_launcher.png)
endif()

# The licenses go with the binaries: ScarletNX's (GPL-3.0), and those of the libraries and code built into the runtime
# and the driver (hactool among them), each in a folder named after it
set(licenses "${CMAKE_INSTALL_DATADIR}/licenses/scarletnx")
install(FILES LICENSE DESTINATION ${licenses})
# FFmpeg's with the change made to it (cmake/ffmpeg-vp9-nvdec.patch), which its LGPL wants carried with the binaries
install(FILES "${FFmpeg_SOURCE_DIR}/COPYING.LGPLv2.1" "${FFmpeg_SOURCE_DIR}/LICENSE.md" cmake/ffmpeg-vp9-nvdec.patch
        DESTINATION ${licenses}/FFmpeg)
install(FILES dist/licenses/Noto/OFL.txt DESTINATION ${licenses}/Noto) # the shared fonts' stand-ins (pl.c)
install(FILES dist/licenses/Montserrat/OFL.txt DESTINATION ${licenses}/Montserrat) # the 3DS shared font's stand-in
install(FILES dist/licenses/Ryujinx/LICENSE.txt DESTINATION ${licenses}/Ryujinx) # the default Miis (misc.c)
install(FILES "${Vulkan-Headers_SOURCE_DIR}/LICENSE.md" DESTINATION ${licenses}/Vulkan-Headers)
install(DIRECTORY "${Vulkan-Headers_SOURCE_DIR}/LICENSES" DESTINATION ${licenses}/Vulkan-Headers)
install(FILES "${volk_SOURCE_DIR}/LICENSE.md" DESTINATION ${licenses}/volk)
install(FILES "${VulkanMemoryAllocator_SOURCE_DIR}/LICENSE.txt" DESTINATION ${licenses}/VulkanMemoryAllocator)
# renderdoc_app.h's license is the comment at the top of the header
file(READ "${renderdoc_app_SOURCE_DIR}/renderdoc_app.h" renderdoc_app)
string(FIND "${renderdoc_app}" "*/" renderdoc_app_end)
string(SUBSTRING "${renderdoc_app}" 0 ${renderdoc_app_end} renderdoc_app)
file(WRITE "${PROJECT_BINARY_DIR}/renderdoc_app-LICENSE.txt" "${renderdoc_app}*/\n")
install(FILES "${PROJECT_BINARY_DIR}/renderdoc_app-LICENSE.txt" DESTINATION ${licenses}/renderdoc_app RENAME LICENSE.txt)
install(FILES "${OpenSSL_SOURCE_DIR}/LICENSE.txt" DESTINATION ${licenses}/OpenSSL)
# lib/LICENSE is the library's BSD-2-Clause. The top-level LICENSE covers the rest of LZ4 (GPL-2.0-or-later), none
# of which is built here
install(FILES "${lz4_SOURCE_DIR}/lib/LICENSE" DESTINATION ${licenses}/lz4)
if(TARGET snx_libdeflate)
    install(FILES "${libdeflate_SOURCE_DIR}/COPYING" DESTINATION ${licenses}/libdeflate RENAME LICENSE)
endif()
if(TARGET snx_enet)
    install(FILES "${enet_SOURCE_DIR}/LICENSE" DESTINATION ${licenses}/enet)
    install(FILES "${zstd_SOURCE_DIR}/LICENSE" DESTINATION ${licenses}/zstd)
endif()
if(SNX_SDL3_FROM_SOURCE)
    install(FILES "${SDL3_SOURCE_DIR}/LICENSE.txt" DESTINATION ${licenses}/SDL3)
endif()
if(TARGET Tracy::TracyClient)
    install(FILES "${tracy_SOURCE_DIR}/LICENSE" DESTINATION ${licenses}/Tracy)
endif()
if(TARGET hactool_lib)
    install(FILES "${hactool_src}/LICENSE" DESTINATION ${licenses}/hactool)
endif()
if(TARGET snx_cjson)
    install(FILES "${cjson_SOURCE_DIR}/LICENSE" DESTINATION ${licenses}/cJSON)
endif()
if(APPLE) # the app's own files, its signature and CPack's package of it
    include(cmake/apple_package.cmake)
endif()

# No -fno-strict-aliasing or -fwrapv in ScarletNX's own code, once every target is defined (cmake/banned_flags.cmake)
include(cmake/banned_flags.cmake)
snx_check_project_flags()
