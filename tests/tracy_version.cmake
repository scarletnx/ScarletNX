# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# The tracy-capture the help names for --tracy-capture is the release the build pins (cmake/dependencies.cmake's
# SNX_TRACY_VERSION): one of another release can't capture the client. Run by ctest as runtime_tracy_version
execute_process(COMMAND "${CMAKE_COMMAND}" -E env --unset=DISPLAY --unset=WAYLAND_DISPLAY SDL_VIDEO_DRIVER=dummy
                        "${RUNTIME}" --help
                TIMEOUT 20 RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE out)
set(expected "from Tracy ${TRACY_VERSION} on PATH")
string(FIND "${out}" "${expected}" at)
if(NOT result EQUAL 0 OR at LESS 0)
    message(FATAL_ERROR "scarletnx --help ended with ${result}, without \"${expected}\":\n${out}")
endif()
