# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# The tests of the window icon (cmake/window_icon.cmake), included by cmake/window_icon.cmake where they were: they run
# in its scope, so its variables hold here, and a relative path is the top level's
if(BUILD_TESTING)
    add_executable(snx-test-window-icon "${PROJECT_SOURCE_DIR}/tests/window_icon.c")
    target_link_libraries(snx-test-window-icon PRIVATE snx_window_icon)
    target_compile_definitions(snx-test-window-icon PRIVATE SNX_WINDOW_ICON_FILE="${snx_window_icon_png}")
    target_compile_options(snx-test-window-icon PRIVATE -Wall -Wextra -Werror -UNDEBUG)
    add_test(NAME window_icon COMMAND snx-test-window-icon)
    set_tests_properties(window_icon PROPERTIES ENVIRONMENT "SDL_VIDEO_DRIVER=dummy")
endif()
