# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# A game's copy of the runtime started bare without its <name>_code.so says what is missing and fails; only the
# launcher's own names open the menu (snx_launcher_name). --game= naming a game's folder looks for the code in it. Run by ctest as runtime_game_copy, with no display, so the
# error's box cannot show
set(dir "${CMAKE_CURRENT_BINARY_DIR}/game-copy")
file(REMOVE_RECURSE "${dir}")
file(MAKE_DIRECTORY "${dir}")
file(COPY_FILE "${RUNTIME}" "${dir}/MyGame")
execute_process(COMMAND "${CMAKE_COMMAND}" -E env --unset=DISPLAY --unset=WAYLAND_DISPLAY SDL_VIDEO_DRIVER=dummy
                        "${dir}/MyGame"
                WORKING_DIRECTORY "${dir}" TIMEOUT 20 RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE out)
# and nothing waits for anyone: SDL's dummy video shows no panel, and the run's line says the error did not show
if(result EQUAL 0 OR NOT result MATCHES "^[0-9]+$" OR NOT out MATCHES "Cannot find the recompiled code"
   OR NOT out MATCHES "error: code-missing shown=0")
    file(REMOVE_RECURSE "${dir}")
    message(FATAL_ERROR "a game's copy without its code ended with ${result}, not its error:\n${out}")
endif()
# --game= naming a game's folder runs the game in it, <folder>/<name>: the code it looks for is there
file(MAKE_DIRECTORY "${dir}/games/0100ABCD12345000")
execute_process(COMMAND "${CMAKE_COMMAND}" -E env --unset=DISPLAY --unset=WAYLAND_DISPLAY SDL_VIDEO_DRIVER=dummy
                        "${dir}/MyGame" --game=games/0100ABCD12345000/
                WORKING_DIRECTORY "${dir}" TIMEOUT 20 RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE out)
file(REMOVE_RECURSE "${dir}")
if(result EQUAL 0 OR NOT out MATCHES "Cannot find the recompiled code: games/0100ABCD12345000/0100ABCD12345000_code")
    message(FATAL_ERROR "--game= naming a game's folder ended with ${result}, not looking in it:\n${out}")
endif()
