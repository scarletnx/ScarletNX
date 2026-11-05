# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# A --key=value that no table of runtime/options.c knows is refused before anything runs; a known one reaches the
# input's check. Run by ctest as runtime_unknown_option
set(dir "${CMAKE_CURRENT_BINARY_DIR}/unknown-option")
file(REMOVE_RECURSE "${dir}")
file(MAKE_DIRECTORY "${dir}")
foreach(option --resoltion=2 --resolution=2 --seconds=5)
    execute_process(COMMAND "${CMAKE_COMMAND}" -E env --unset=DISPLAY --unset=WAYLAND_DISPLAY SDL_VIDEO_DRIVER=dummy
                            "${RUNTIME}" ${option} missing.nro
                    WORKING_DIRECTORY "${dir}" TIMEOUT 20 RESULT_VARIABLE result OUTPUT_VARIABLE out
                    ERROR_VARIABLE out)
    if(option STREQUAL "--resoltion=2")
        set(expected "unknown option: --resoltion=2")
    else()
        set(expected "missing.nro is not a file")
    endif()
    string(FIND "${out}" "${expected}" at)
    if(result EQUAL 0 OR at LESS 0)
        file(REMOVE_RECURSE "${dir}")
        message(FATAL_ERROR "scarletnx ${option} missing.nro ended with ${result}, not \"${expected}\":\n${out}")
    endif()
endforeach()
# a runtime option after the "--" is a game argument that nothing reads, so a timed run given there never ended: it is
# refused as it was given, and the message says where it belongs; a game's own --key=value still reaches the game
foreach(case "--seconds=5;--seconds=5 comes after the \"--\"" "--game=foo;--game=foo comes after the \"--\""
             "--foo=bar;missing.nro is not a file")
    list(POP_BACK case expected)
    execute_process(COMMAND "${CMAKE_COMMAND}" -E env --unset=DISPLAY --unset=WAYLAND_DISPLAY SDL_VIDEO_DRIVER=dummy
                            "${RUNTIME}" missing.nro -- ${case}
                    WORKING_DIRECTORY "${dir}" TIMEOUT 20 RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE out)
    string(FIND "${out}" "${expected}" at)
    if(result EQUAL 0 OR at LESS 0)
        file(REMOVE_RECURSE "${dir}")
        message(FATAL_ERROR "scarletnx missing.nro -- ${case} ended with ${result}, not \"${expected}\":\n${out}")
    endif()
endforeach()
# an option nothing takes is refused by its name, with no game given too; an option's older name is said once, with
# its name now, and taken as that
foreach(case "--bogus;unknown option: --bogus (scarletnx --help lists them)"
             "--settings=bogus;--settings=bogus: not --settings (the game's) or --settings=recompile"
             "--recomp;--recomp;--help;--recomp is --recompile now: this run takes it as --recompile")
    list(POP_BACK case expected)
    execute_process(COMMAND "${CMAKE_COMMAND}" -E env --unset=DISPLAY --unset=WAYLAND_DISPLAY SDL_VIDEO_DRIVER=dummy
                            "${RUNTIME}" ${case}
                    WORKING_DIRECTORY "${dir}" TIMEOUT 20 RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE out)
    string(FIND "${out}" "${expected}" at)
    string(REPLACE "${expected}" "" rest "${out}")
    string(FIND "${rest}" "is --recompile now" again)
    if(at LESS 0 OR NOT again LESS 0)
        file(REMOVE_RECURSE "${dir}")
        message(FATAL_ERROR "scarletnx ${case} did not say \"${expected}\" once:\n${out}")
    endif()
endforeach()
file(REMOVE_RECURSE "${dir}")
