# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
if(WIN32)
    set(gui_process process_win32.c)
elseif(CMAKE_SYSTEM_NAME MATCHES "^(Linux|FreeBSD|NetBSD|Android|Darwin|iOS)$")
    set(gui_process process_posix.c)
else()
    message(FATAL_ERROR "scarletnx-gui currently supports Linux, FreeBSD, NetBSD, macOS, iOS and Windows")
endif()
include("${PROJECT_SOURCE_DIR}/cmake/gui_themes.cmake")
# The menu's languages (i18n.h): snx_tr and the catalogs' reader, strict ISO C99 as the build info; each po/*.po a
# catalog compiled in, checked and made by scripts/build-languages.py against po/scarletnx.pot
add_library(snx_i18n STATIC i18n.c)
set_target_properties(snx_i18n PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_compile_options(snx_i18n PRIVATE -pedantic-errors -Wall -Wextra -Werror=missing-prototypes)
target_link_libraries(snx_i18n PUBLIC scarletnx-common) # the tags' fit (common/utilities/language_tag.h)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
# target, a library of out made from the .po files of folder
function(snx_gui_languages target folder out)
    file(GLOB pos CONFIGURE_DEPENDS "${folder}/*.po")
    add_custom_command(OUTPUT "${out}"
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/scripts/build-languages.py" --out "${out}" ${pos}
        DEPENDS "${PROJECT_SOURCE_DIR}/scripts/build-languages.py" "${PROJECT_SOURCE_DIR}/po/scarletnx.pot" ${pos}
                "${PROJECT_SOURCE_DIR}/scripts/build-glyphs.py" "${CMAKE_CURRENT_SOURCE_DIR}/unifont-latin.hex"
                "${CMAKE_CURRENT_SOURCE_DIR}/unifont-wide.hex" "${CMAKE_CURRENT_SOURCE_DIR}/unifont-titles.hex"
                "${PROJECT_SOURCE_DIR}/src/common/utilities/font.c"
        COMMENT "The menu's languages from ${folder}" VERBATIM)
    add_library(${target} STATIC "${out}")
endfunction()
snx_gui_languages(snx_gui_languages "${PROJECT_SOURCE_DIR}/po" "${PROJECT_BINARY_DIR}/generated/gui_languages.c")
# The letters past the VGA font's CP437: GNU Unifont's, the Latin ones (latin.h) kept in unifont-latin.hex, the CJK and
# Hangul blocks of game titles (titles.h) in unifont-titles.hex and the other wide ones the languages show (wide.h) in
# unifont-wide.hex
add_custom_command(OUTPUT "${PROJECT_BINARY_DIR}/generated/gui_glyphs.c" "${PROJECT_BINARY_DIR}/generated/gui_titles.c"
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/scripts/build-glyphs.py"
            --out "${PROJECT_BINARY_DIR}/generated/gui_glyphs.c"
            --titles-out "${PROJECT_BINARY_DIR}/generated/gui_titles.c"
    DEPENDS "${PROJECT_SOURCE_DIR}/scripts/build-glyphs.py" "${CMAKE_CURRENT_SOURCE_DIR}/unifont-latin.hex"
            "${CMAKE_CURRENT_SOURCE_DIR}/unifont-wide.hex" "${CMAKE_CURRENT_SOURCE_DIR}/unifont-titles.hex"
    COMMENT "The menu's Latin, wide and titles' glyphs" VERBATIM)
# the launcher's scenes, generated from ui.txt by tools/uigen: gui/ui.h and gui/ui.c, whose drawing and callbacks
# main.c defines, so they go into what compiles main.c
include("${PROJECT_SOURCE_DIR}/cmake/ui.cmake")
add_custom_command(OUTPUT "${PROJECT_BINARY_DIR}/generated/gui/ui.h" "${PROJECT_BINARY_DIR}/generated/gui/ui.c"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${PROJECT_BINARY_DIR}/generated/gui"
    COMMAND snx-uigen --out "${PROJECT_BINARY_DIR}/generated" "${CMAKE_CURRENT_SOURCE_DIR}/ui.txt"
    DEPENDS snx-uigen "${CMAKE_CURRENT_SOURCE_DIR}/ui.txt"
    COMMENT "Generating the launcher's scenes from src/gui/ui.txt" VERBATIM)
add_library(snx_gui_ui OBJECT "${PROJECT_BINARY_DIR}/generated/gui/ui.c")
target_include_directories(snx_gui_ui PUBLIC "${PROJECT_BINARY_DIR}/generated")
target_link_libraries(snx_gui_ui PRIVATE SDL3::SDL3) # its platform macros and keycodes
set_target_properties(snx_gui_ui PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
target_compile_options(snx_gui_ui PRIVATE -Wall -Wextra -Werror=missing-prototypes)
add_library(snx_gui_support STATIC aot.c model.c browser.c steps.c theme.c recent.c editor.c settings.c place.c clean.c
            queue.c latin.c wide.c log.c logzip.c gpu_driver.c gpu_driver_fetch.c covers.c probe.c last_run.c title.c
            ${gui_process}
            "${PROJECT_BINARY_DIR}/generated/gui_themes.c" "${PROJECT_BINARY_DIR}/generated/gui_glyphs.c"
            "${PROJECT_BINARY_DIR}/generated/gui_titles.c")
target_link_libraries(snx_gui_support PUBLIC scarletnx-common snx_setup snx_i18n snx_settings snx_miniz PRIVATE snx_libdeflate)
# Download Driver's GitHub lists (gpu_driver_fetch.c), edenprobe's report (probe.c)
target_link_libraries(snx_gui_support PRIVATE snx_yyjson)
# a z3ds game's title (title.c), read from the start of its zstd stream, where the build has zstd
if(TARGET snx_zstd)
    target_link_libraries(snx_gui_support PRIVATE snx_zstd)
    target_compile_definitions(snx_gui_support PRIVATE SNX_GUI_ZSTD)
endif()
target_include_directories(snx_gui_support PRIVATE "${PROJECT_BINARY_DIR}/generated")
set_target_properties(snx_gui_support PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
target_compile_options(snx_gui_support PRIVATE -Wall -Wextra -Werror=missing-prototypes)
if(WIN32)
    target_compile_definitions(snx_gui_support PRIVATE _WIN32_WINNT=0x0602 _CRT_SECURE_NO_WARNINGS)
    target_link_libraries(snx_gui_support PUBLIC psapi)
endif()
include("${PROJECT_SOURCE_DIR}/cmake/window_icon.cmake")
include("${PROJECT_SOURCE_DIR}/cmake/build_info.cmake")
include("${PROJECT_SOURCE_DIR}/cmake/pad_sdl.cmake")
include("${PROJECT_SOURCE_DIR}/cmake/splash.cmake")
# the menu's pads, their presses its keys (pad_menu.h)
add_library(snx_gui_pad STATIC pad_menu.c)
target_link_libraries(snx_gui_pad PUBLIC scarletnx-common snx_pad_sdl SDL3::SDL3)
set_target_properties(snx_gui_pad PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
target_compile_options(snx_gui_pad PRIVATE -Wall -Wextra -Werror=missing-prototypes)
include(GNUInstallDirs)
# The menu: Android's activity loads it as libmain.so and runs its SDL_main (dist/android); scarletnx, the Switch
# runtime, links it and opens it when started with nothing to run (#243); a build without that runtime makes it a
# program of its own, beside the other runtimes
if(ANDROID)
    add_library(scarletnx-gui SHARED main.c android.c)
    set_target_properties(scarletnx-gui PROPERTIES OUTPUT_NAME main)
    # the drivers libscarletnx.so has (src/multicall), which the menu runs, a space apart
    list(JOIN SNX_PROGRAM_DRIVERS " " snx_program_drivers)
    target_compile_definitions(scarletnx-gui PRIVATE "SNX_GUI_PROGRAM_DRIVERS=\"${snx_program_drivers}\"")
    install(TARGETS scarletnx-gui LIBRARY DESTINATION lib/${ANDROID_ABI})
    install(DIRECTORY "${SDL3_SOURCE_DIR}/android-project/app/src/main/java/" DESTINATION java)
elseif(SNX_BUILD_NX)
    add_library(scarletnx-gui STATIC main.c)
    target_compile_definitions(scarletnx-gui PRIVATE SNX_GUI_NO_MAIN)
else()
    add_executable(scarletnx-gui main.c)
    install(TARGETS scarletnx-gui RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    if(NOT WIN32)
        install(FILES "${PROJECT_SOURCE_DIR}/dist/scarletnx-gui.desktop"
                DESTINATION ${CMAKE_INSTALL_DATADIR}/applications COMPONENT desktop)
    endif()
endif()
if(APPLE)
    # the iOS and macOS frontend (docs/hosts/apple.md), as Android's is MainActivity and android.c: apple.m is the menu's
    # side, dist/apple/Frontend.swift the system's (iOS's document picker)
    add_library(snx_apple_frontend STATIC "${PROJECT_SOURCE_DIR}/dist/apple/Frontend.swift")
    # Swift 5's checks: the functions apple.m calls run on the main thread, which Swift 6 would want them to prove
    set_target_properties(snx_apple_frontend PROPERTIES Swift_LANGUAGE_VERSION 5)
    # the Swift runtime is the system's (/usr/lib/swift, from iOS 12.2 and macOS 10.14.4); the objects name its
    # libraries for the linker, which looks for them in the SDK, and for the back-deployment ones in the toolchain
    execute_process(COMMAND xcrun --find swiftc OUTPUT_VARIABLE snx_swiftc OUTPUT_STRIP_TRAILING_WHITESPACE)
    get_filename_component(snx_swift_toolchain "${snx_swiftc}" DIRECTORY)
    if(SNX_IOS_SIMULATOR)
        set(snx_swift_platform iphonesimulator)
    elseif(IOS)
        set(snx_swift_platform iphoneos)
    else()
        set(snx_swift_platform macosx)
    endif()
    target_link_directories(snx_apple_frontend INTERFACE "${SNX_APPLE_SDK}/usr/lib/swift"
                            "${snx_swift_toolchain}/../lib/swift/${snx_swift_platform}")
    target_sources(scarletnx-gui PRIVATE apple.m)
    set_source_files_properties(apple.m PROPERTIES COMPILE_OPTIONS -fobjc-arc)
    # PUBLIC: the link folders above go with it to the program that links the menu
    target_link_libraries(scarletnx-gui PUBLIC snx_apple_frontend "-framework Foundation")
    if(IOS)
        target_link_libraries(scarletnx-gui PUBLIC "-framework Metal") # the resources line (gui_apple_stats)
        target_link_libraries(scarletnx-gui PUBLIC "-framework UIKit") # the battery state notification
        # the reports of the system on the app's runs (apple.m's metrics_start)
        target_link_libraries(scarletnx-gui PUBLIC "-framework MetricKit")
    endif()
    # not Swift's, which linking a Swift library made CMake pick: the archive was made by swiftc, given the C flags.
    # C++: the menu alone (no Switch runtime) is a program, and links the splash's C++ (lunasvg)
    set_target_properties(scarletnx-gui PROPERTIES LINKER_LANGUAGE CXX)
endif()
set_target_properties(scarletnx-gui PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON
    RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}")
target_compile_options(scarletnx-gui PRIVATE -Wall -Wextra -Werror=missing-prototypes)
target_compile_definitions(scarletnx-gui PRIVATE SNX_SOURCE_DIR="${PROJECT_SOURCE_DIR}" SNX_BUILD_DIR="${PROJECT_BINARY_DIR}")
target_link_libraries(scarletnx-gui PUBLIC snx_gui_support PRIVATE snx_gui_ui scarletnx-common SDL3::SDL3
                      snx_window_icon snx_build_info snx_pad_sdl snx_gui_pad snx_splash snx_gui_languages snx_demangle)
# GPU Drivers' Download Driver, where the build has OpenSSL (common/network/https.h)
if(TARGET snx_https)
    target_link_libraries(scarletnx-gui PRIVATE snx_https)
    target_compile_definitions(scarletnx-gui PRIVATE SNX_GUI_HTTPS)
endif()
# GPU Drivers' System driver row asks the system's Vulkan loader for its driver, where the build has Vulkan's headers
if(TARGET Vulkan::Headers)
    target_link_libraries(scarletnx-gui PRIVATE Vulkan::Headers)
    target_compile_definitions(scarletnx-gui PRIVATE SNX_GUI_VULKAN VK_NO_PROTOTYPES)
endif()
if(WIN32 AND NOT SNX_BUILD_NX)
    enable_language(RC)
    target_sources(scarletnx-gui PRIVATE ../common/platform/windows/runtime.rc)
    # where the resource compiler finds the manifest and the icon; C includes from src/ alone
    target_include_directories(scarletnx-gui PRIVATE
                               "$<$<COMPILE_LANGUAGE:RC>:${CMAKE_CURRENT_SOURCE_DIR}/../common/platform/windows>")
    target_link_options(scarletnx-gui PRIVATE -mwindows -static)
endif()
if(SNX_SDL3_FROM_SOURCE AND NOT SNX_BUILD_NX)
    install(FILES "${SDL3_SOURCE_DIR}/LICENSE.txt" DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/scarletnx/SDL3)
endif()
install(FILES FONT-NOTICE.txt MUSL-NOTICE.txt DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/scarletnx)
# The built-in themes as files to copy and edit: beside the GUI in the build, in share/scarletnx installed, where it
# looks for them
file(GLOB_RECURSE gui_themes RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}" CONFIGURE_DEPENDS themes/*/theme.ini)
foreach(theme IN LISTS gui_themes)
    configure_file(${theme} "${PROJECT_BINARY_DIR}/${theme}" COPYONLY)
endforeach()
install(DIRECTORY themes DESTINATION ${CMAKE_INSTALL_DATADIR}/scarletnx)
include("${PROJECT_SOURCE_DIR}/tests/cmake/gui.cmake") # its tests
