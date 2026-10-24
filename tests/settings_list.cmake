# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# `scarletnx --settings`: every setting of the game's .ini, by section, a line each of nine fields, the help on one
# line, the menus' name and values (runtime/options.c's ini_list), as the menu's Settings read them. Run by ctest as
# runtime_settings_list
execute_process(COMMAND "${RUNTIME}" --settings RESULT_VARIABLE result OUTPUT_VARIABLE list ERROR_VARIABLE errors)
if(result OR NOT errors STREQUAL "")
    message(FATAL_ERROR "scarletnx --settings ended with ${result}: ${errors}")
endif()
set(field "[^\t\r\n]*")
string(REGEX REPLACE "${field}\t${field}\t${field}\t(int|float|text)\t${field}\t${field}\t${field}\t${field}\t${field}\r?\n"
       "" rest "${list}")
if(NOT rest STREQUAL "" OR list STREQUAL "")
    message(FATAL_ERROR "lines of scarletnx --settings that are not nine fields:\n${rest}")
endif()
# the game's menu's settings with its names and words, as the launcher shows them; a whole number of few values with
# its numbers
foreach(row "Display\tdocked\t1\tint\t0\t1\t1 = TV mode" "Display\tfullscreen\t0\tint\t0\t1\t"
            "Graphics\tresolution\t1\tfloat\t0.25\t4\t" "Performance\tfps_limit\tgame\ttext\t"
            "On-screen controls\tcontrols\t" "\tShow controls\t0=Off|1=On\n"
            "\tScale\t0=50%|10=60%|20=70%|30=80%|40=90%|50=100%|60=110%|70=120%|80=130%|90=140%|100=150%\n"
            "On-screen controls\ttouch\t2\tint\t0\t2\t" "\tTouch screen\t0=Off|1=Fingers|2=Fingers and mouse\n"
            "On-screen controls/Buttons\tcontrols_a_shown\t\tint\t0\t1\t" "\tA\t0=Hidden|1=Shown\n"
            "Controllers\tgamepad_swap_ab\t0\tint\t0\t1\t" "\tSwap A/B\t0=Off|1=On\n"
            "Controllers/Button map\tgamepad_map\t\ttext\t"
            "\tStick dead zone\t0=0%|5=5%|10=10%|" "|90=90%\n" "\tVibration\t0=Off|10=10%|20=20%|"
            "Debug\tlog\t1\tint\t0\t2\t" "\tLog level\t0=0|1=1|2=2\n"
            "Performance\tprepare_shaders\t2\tint\t0\t2\t"
            "\tPrepare shaders\t0=Off|1=While playing|2=Before starting\n"
            ", 2 French, 3 German, 4 Italian, 5 Spanish, 6 Chinese, 7 Korean,")
    string(FIND "${list}" "${row}" at)
    if(at LESS 0)
        message(FATAL_ERROR "scarletnx --settings has no \"${row}\"")
    endif()
endforeach()
# a later player's rows have their own help, its first player's: not the Button map's of the player before
if(NOT list MATCHES "\tgamepad_p2_device\t[^\n]*\tPlayer N's controller" OR list MATCHES "\tgamepad_p2_device\t[^\n]*\tPlayer N's map")
    message(FATAL_ERROR "scarletnx --settings gives gamepad_p2_device another setting's help")
endif()
string(FIND "${list}" "\tcontrols_a\t" at) # a control's place: the game's own, set in its edit mode
if(NOT at LESS 0)
    message(FATAL_ERROR "scarletnx --settings lists controls_a, the game's own")
endif()
if(NOT list MATCHES
   "^Game\t.*\nDisplay\t.*\nGraphics\t.*\nPerformance\t.*\nOn-screen controls\t.*\nOn-screen controls/Buttons\t.*\nControllers\t.*\nControllers/Button map\t.*\nNetwork\t.*\nDebug\t")
    message(FATAL_ERROR "scarletnx --settings lists the sections out of the .ini's order")
endif()
if(list MATCHES "\nOn-screen controls/Buttons\t[^\n]*\nOn-screen controls\t") # a page after its section's others
    message(FATAL_ERROR "scarletnx --settings lists a page's settings before its section's last")
endif()
