# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# A --key=value that no table of runtime/options.c knows is refused before anything runs; a known one reaches the
# input's check, its key's _ each a - or none (a game's, the recompile's, a timed run's), the key itself said once as
# the older spelling; some _ left is no key. Run by ctest as runtime_unknown_option
set(dir "${CMAKE_CURRENT_BINARY_DIR}/unknown-option")
file(REMOVE_RECURSE "${dir}")
file(MAKE_DIRECTORY "${dir}")
foreach(option --resoltion=2 --resolution=2 --seconds=5 --state-slots=5 --state_slots=5 --aot-threads=2
               --shot-every=5 --aot-threads_most=2)
    execute_process(COMMAND "${CMAKE_COMMAND}" -E env --unset=DISPLAY --unset=WAYLAND_DISPLAY SDL_VIDEO_DRIVER=dummy
                            "${RUNTIME}" ${option} missing.nro
                    WORKING_DIRECTORY "${dir}" TIMEOUT 20 RESULT_VARIABLE result OUTPUT_VARIABLE out
                    ERROR_VARIABLE out)
    if(option STREQUAL "--resoltion=2" OR option STREQUAL "--aot-threads_most=2")
        set(expected "unknown option: ${option}")
    else()
        set(expected "missing.nro is not a file")
    endif()
    string(FIND "${out}" "${expected}" at)
    if(result EQUAL 0 OR at LESS 0)
        file(REMOVE_RECURSE "${dir}")
        message(FATAL_ERROR "scarletnx ${option} missing.nro ended with ${result}, not \"${expected}\":\n${out}")
    endif()
endforeach()
# a key as the .ini writes it, the older spelling, is still read and said once a run with the name now, however many
# times it is given; the help gives the settings with a - for each _ of their keys
execute_process(COMMAND "${CMAKE_COMMAND}" -E env --unset=DISPLAY --unset=WAYLAND_DISPLAY SDL_VIDEO_DRIVER=dummy
                        "${RUNTIME}" --state_slots=5 --state_slots=6 missing.nro
                WORKING_DIRECTORY "${dir}" TIMEOUT 20 RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE out)
set(said "settings: --state_slots is the old spelling of --state-slots: it is read as that; write --state-slots")
string(FIND "${out}" "${said}" first)
string(FIND "${out}" "${said}" last REVERSE)
string(FIND "${out}" "missing.nro is not a file" reached)
if(first LESS 0 OR NOT first EQUAL last OR reached LESS 0)
    file(REMOVE_RECURSE "${dir}")
    message(FATAL_ERROR "scarletnx --state_slots=5 --state_slots=6 missing.nro did not say \"${said}\" once, or did not "
                        "go on to the input:\n${out}")
endif()
foreach(case "--help;--state-slots=5" "-r;-h;--aot-threads")
    list(POP_BACK case expected)
    execute_process(COMMAND "${CMAKE_COMMAND}" -E env --unset=DISPLAY --unset=WAYLAND_DISPLAY SDL_VIDEO_DRIVER=dummy
                            "${RUNTIME}" ${case}
                    WORKING_DIRECTORY "${dir}" TIMEOUT 20 RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE out)
    string(FIND "${out}" "${expected}" at)
    if(at LESS 0)
        file(REMOVE_RECURSE "${dir}")
        message(FATAL_ERROR "scarletnx ${case}'s help does not give ${expected}:\n${out}")
    endif()
endforeach()
# everything after the "--" is the game's as given, a runtime option's name too, so the run goes on to the input's check
set(given --log=1 --seconds=5 --game=foo --foo=bar x)
execute_process(COMMAND "${CMAKE_COMMAND}" -E env --unset=DISPLAY --unset=WAYLAND_DISPLAY SDL_VIDEO_DRIVER=dummy
                        "${RUNTIME}" missing.nro -- ${given}
                WORKING_DIRECTORY "${dir}" TIMEOUT 20 RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE out)
set(expected "missing.nro is not a file")
string(FIND "${out}" "${expected}" at)
if(result EQUAL 0 OR at LESS 0)
    file(REMOVE_RECURSE "${dir}")
    list(JOIN given " " given)
    message(FATAL_ERROR "scarletnx missing.nro -- ${given} ended with ${result}, not \"${expected}\":\n${out}")
endif()
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
