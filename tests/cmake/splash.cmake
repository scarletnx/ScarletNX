# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# The tests of the splash pictures (cmake/splash.cmake), included by cmake/splash.cmake where they were: they run in its
# scope, so its variables hold here, and a relative path is the top level's
if(BUILD_TESTING)
    add_executable(snx-test-splash "${PROJECT_SOURCE_DIR}/tests/splash.c")
    target_link_libraries(snx-test-splash PRIVATE snx_splash SDL3::SDL3)
    # plutovg, built into snx_splash, makes the JPEGs the pictures are drawn from
    target_include_directories(snx-test-splash SYSTEM PRIVATE "${plutovg_SOURCE_DIR}/include")
    target_compile_definitions(snx-test-splash PRIVATE PLUTOVG_BUILD_STATIC)
    target_compile_definitions(snx-test-splash PRIVATE
                               SNX_SPLASH_ICON="${PROJECT_SOURCE_DIR}/dist/icons/hicolor/256x256/apps/wimsy.png")
    target_compile_options(snx-test-splash PRIVATE -Wall -Wextra -Werror -UNDEBUG)
    add_test(NAME splash COMMAND snx-test-splash)
    set_tests_properties(splash PROPERTIES ENVIRONMENT "SDL_VIDEO_DRIVER=dummy")
    # an SVG with two <metadata> blocks is refused, not built with its drawing gone. Written only when it changes:
    # runtime_architecture reconfigures the tree while this test may be reading it
    file(CONFIGURE OUTPUT "${PROJECT_BINARY_DIR}/splash-two-metadata.svg" @ONLY CONTENT
         "<svg viewBox=\"0 0 8 8\"><metadata>a</metadata><rect width=\"8\" height=\"8\"/><metadata>b</metadata></svg>\n")
    add_test(NAME splash_two_metadata COMMAND "${CMAKE_COMMAND}" "-DSVG=${PROJECT_BINARY_DIR}/splash-two-metadata.svg" -P
             "${PROJECT_SOURCE_DIR}/cmake/splash.cmake")
    set_tests_properties(splash_two_metadata PROPERTIES PASS_REGULAR_EXPRESSION "2 <metadata> blocks")
endif()
