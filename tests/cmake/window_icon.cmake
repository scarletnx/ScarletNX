# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# The tests of the windows' icon and fullscreen (cmake/window_icon.cmake), included by cmake/window_icon.cmake where they
# were: they run in its scope, so its variables hold here, and a relative path is the top level's
if(BUILD_TESTING)
    add_executable(snx-test-window-icon "${PROJECT_SOURCE_DIR}/tests/window_icon.c")
    target_link_libraries(snx-test-window-icon PRIVATE snx_window_icon)
    target_compile_definitions(snx-test-window-icon PRIVATE SNX_WINDOW_ICON_FILE="${snx_window_icon_png}"
        SNX_WINDOW_ICON_XMAS_FILE="${snx_window_icon_xmas_png}")
    target_compile_options(snx-test-window-icon PRIVATE -Wall -Wextra -Werror -UNDEBUG)
    add_test(NAME window_icon COMMAND snx-test-window-icon)
    set_tests_properties(window_icon PROPERTIES ENVIRONMENT "SDL_VIDEO_DRIVER=dummy")
    add_executable(snx-test-window-fullscreen "${PROJECT_SOURCE_DIR}/tests/window_fullscreen.c")
    target_link_libraries(snx-test-window-fullscreen PRIVATE snx_window_icon)
    target_compile_options(snx-test-window-fullscreen PRIVATE -Wall -Wextra -Werror)
    add_test(NAME window_fullscreen COMMAND snx-test-window-fullscreen)
    set_tests_properties(window_fullscreen PROPERTIES ENVIRONMENT "SDL_VIDEO_DRIVER=dummy" TIMEOUT 60)
endif()
