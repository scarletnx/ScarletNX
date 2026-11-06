# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# A default-if SNX_IOS line of tools/settingsgen's output where iOS compiles it: settings.c (g_settings) and a table's
# file (its .ini default) preprocessed with __APPLE__ and a TargetConditionals.h of the test's own, whose
# TARGET_OS_IPHONE is all common.h reads to define SNX_IOS. iOS takes the line's value, macOS the default. Run by ctest
# as settingsgen_ios, with SETTINGSGEN the generator, CC the build's C compiler, SRC the source's src/ (common.h, ini.h)
# and WORK a folder of its own
file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}/out" "${WORK}/ios" "${WORK}/macos")
file(WRITE "${WORK}/settings.txt" "
table g_test_ini
setting Phone stack
    key phone_stack
    int 1 64
    default 32
    default-if SNX_IOS 8
")
execute_process(COMMAND "${SETTINGSGEN}" --out "${WORK}/out" "${WORK}/settings.txt" RESULT_VARIABLE rc
                ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "settingsgen failed (${rc}): ${err}")
endif()
file(WRITE "${WORK}/ios/TargetConditionals.h" "#define TARGET_OS_IPHONE 1\n")
file(WRITE "${WORK}/macos/TargetConditionals.h" "#define TARGET_OS_IPHONE 0\n")
file(WRITE "${WORK}/probe.c" "#include \"settings.c\"\n#include \"settings_test.c\"\n"
                             "PROBE SETTING_INIT_phone_stack SETTING_DEF_phone_stack\n")
foreach(system_want IN ITEMS "ios;8" "macos;32")
    list(GET system_want 0 system)
    list(GET system_want 1 want)
    execute_process(COMMAND "${CC}" -E -P -D__APPLE__=1 -I "${WORK}/${system}" -I "${SRC}" -I "${WORK}/out"
                            "${WORK}/probe.c"
                    OUTPUT_VARIABLE out ERROR_VARIABLE err RESULT_VARIABLE rc)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "${system}: the preprocessor failed (${rc}): ${err}")
    endif()
    if(NOT out MATCHES "PROBE ([^ \r\n]+) \"([^\"]*)\"")
        message(FATAL_ERROR "${system}: no PROBE line in the preprocessed settings:\n${out}")
    endif()
    if(NOT CMAKE_MATCH_1 STREQUAL want OR NOT CMAKE_MATCH_2 STREQUAL want)
        message(FATAL_ERROR "${system}: phone_stack is ${CMAKE_MATCH_1} in g_settings and \"${CMAKE_MATCH_2}\" in its "
                            ".ini row, not ${want}")
    endif()
endforeach()
