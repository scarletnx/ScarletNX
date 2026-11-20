# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# cmake/gui_icons.cmake on files of its own: the real icons.h and icons.hex compile, and each .hex it refuses is
# refused naming the icon and the line, never compiled with a glyph missing, blank or another's. Run by ctest as
# gui_icons, with SOURCE the checkout and WORK a folder of its own
file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")
set(script "${SOURCE}/cmake/gui_icons.cmake")
# the checkout's own: every icon of icons.h, its halves in the C
execute_process(COMMAND "${CMAKE_COMMAND}" "-DICONS_H=${SOURCE}/src/gui/icons.h" "-DICONS_HEX=${SOURCE}/src/gui/icons.hex"
                        "-DOUT=${WORK}/real.c" -P "${script}" RESULT_VARIABLE result ERROR_VARIABLE errors)
if(result)
    message(FATAL_ERROR "src/gui/icons.hex is refused: ${errors}")
endif()
file(READ "${SOURCE}/src/gui/icons.h" header)
string(REGEX MATCH "GUI_ICON_[A-Z0-9_]+ = 0xE000,[^}]*GUI_ICON_END" block "${header}")
string(REGEX MATCHALL "GUI_ICON_[A-Z0-9_]+( = 0xE000)?," names "${block}")
list(LENGTH names count)
file(READ "${WORK}/real.c" c)
string(REGEX MATCHALL "/\\* GUI_ICON_[A-Z0-9_]+, left \\*/" lefts "${c}")
list(LENGTH lefts halves)
if(NOT halves EQUAL count OR NOT c MATCHES "GUI_ICON_GAMEPAD_2, right")
    message(FATAL_ERROR "the C of src/gui/icons.hex has ${halves} icons, icons.h ${count}:\n${c}")
endif()

# three icons and their lines
file(WRITE "${WORK}/icons.h" "enum {\n    GUI_ICON_NONE,\n    GUI_ICON_ONE = 0xE000,\n    GUI_ICON_TWO,\n    GUI_ICON_THREE,\n"
                             "    GUI_ICON_END\n};\n")
string(REPEAT "18" 32 ink)
set(one "E000:${ink}\n")
set(two "E001:${ink}\n")
set(three "E002:${ink}\n")
function(refused name hex expected)
    file(WRITE "${WORK}/${name}.hex" "${hex}")
    execute_process(COMMAND "${CMAKE_COMMAND}" "-DICONS_H=${WORK}/icons.h" "-DICONS_HEX=${WORK}/${name}.hex"
                            "-DOUT=${WORK}/${name}.c" -P "${script}" RESULT_VARIABLE result ERROR_VARIABLE errors)
    string(REGEX REPLACE "[ \n]+" " " errors "${errors}")
    if(NOT result OR NOT errors MATCHES "${expected}" OR EXISTS "${WORK}/${name}.c")
        message(FATAL_ERROR "${name}: gui_icons.cmake ended with ${result} and said \"${errors}\", not ${expected}")
    endif()
endfunction()
refused(missing "${one}${two}" "missing.hex: no glyph for GUI_ICON_THREE \\(U\\+E002\\), which line 3 holds")
refused(empty "" "empty.hex: no glyph for GUI_ICON_ONE \\(U\\+E000\\), which line 1 holds")
refused(order "${one}${three}${two}" "order.hex:2: U\\+E002 where GUI_ICON_TWO's U\\+E001 goes")
refused(narrow "${one}E001:00000000183C3C1800000000000000\n${three}"
        "narrow.hex:2: GUI_ICON_TWO \\(U\\+E001\\) has 30 hex digits: an icon is 16x16, 64")
string(REPEAT "0" 64 blank)
refused(blank "${one}${two}E002:${blank}\n" "blank.hex:3: GUI_ICON_THREE \\(U\\+E002\\) is blank")
refused(junk "${one}two\n${three}" "junk.hex:2: \"two\" is not a line of a Unifont .hex, GUI_ICON_TWO's \\(U\\+E001\\)")
refused(lower "${one}${two}e002:${ink}\n" "lower.hex:3: \"e002:")
refused(extra "${one}${two}${three}E003:${ink}\n" "extra.hex:4: \"E003:[0-9A-F]+\" is past the icons of .*icons.h, GUI_ICON_THREE the last")
# and the three whole, in a file with Windows' line ends
file(WRITE "${WORK}/crlf.hex" "E000:${ink}\r\nE001:${ink}\r\nE002:${ink}\r\n")
execute_process(COMMAND "${CMAKE_COMMAND}" "-DICONS_H=${WORK}/icons.h" "-DICONS_HEX=${WORK}/crlf.hex"
                        "-DOUT=${WORK}/crlf.c" -P "${script}" RESULT_VARIABLE result ERROR_VARIABLE errors)
file(READ "${WORK}/crlf.c" c)
if(result OR NOT c MATCHES "0x18, 0x18,[^\n]*/\\* GUI_ICON_THREE, right \\*/")
    message(FATAL_ERROR "three icons with CRLF: ${result} ${errors}\n${c}")
endif()
