# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# cmake/gui_icons.cmake on folders of its own: the real icons.h and src/gui/lucide compile, and each folder it refuses
# is refused naming the file and the icon, never compiled with a drawing missing or another's. Run by ctest as
# gui_icons, with SOURCE the checkout and WORK a folder of its own
file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")
set(script "${SOURCE}/cmake/gui_icons.cmake")
# the checkout's own: every icon of icons.h, its SVG in the C under its name
execute_process(COMMAND "${CMAKE_COMMAND}" "-DICONS_H=${SOURCE}/src/gui/icons.h" "-DICONS_DIR=${SOURCE}/src/gui/lucide"
                        "-DOUT=${WORK}/real.c" -P "${script}" RESULT_VARIABLE result ERROR_VARIABLE errors)
if(result)
    message(FATAL_ERROR "src/gui/lucide is refused: ${errors}")
endif()
file(READ "${SOURCE}/src/gui/icons.h" header)
string(REGEX MATCH "GUI_ICON_[A-Z0-9_]+ = 0xE000,[^}]*GUI_ICON_END" block "${header}")
string(REGEX MATCHALL "GUI_ICON_[A-Z0-9_]+( = 0xE000)?," names "${block}")
list(LENGTH names count)
file(READ "${WORK}/real.c" c)
string(REGEX MATCHALL "\n    {\"[a-z0-9-]+\", svg[0-9]+, sizeof svg[0-9]+}," rows "${c}")
list(LENGTH rows listed)
if(NOT listed EQUAL count OR NOT c MATCHES "/\\* GUI_ICON_GAMEPAD_2 \\*/" OR NOT c MATCHES "{\"gamepad-2\", svg0,")
    message(FATAL_ERROR "the C of src/gui/lucide lists ${listed} icons, icons.h ${count}:\n${c}")
endif()

# three icons and their drawings
file(WRITE "${WORK}/icons.h" "enum {\n    GUI_ICON_NONE,\n    GUI_ICON_ONE = 0xE000,\n    GUI_ICON_TWO_2,\n"
                             "    GUI_ICON_THREE,\n    GUI_ICON_END\n};\n")
string(CONCAT svg "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"24\" height=\"24\" viewBox=\"0 0 24 24\"\n"
       "  fill=\"none\" stroke=\"currentColor\">\n  <path d=\"M4 4h16\" />\n</svg>\n")
# folder: a folder of WORK with LICENSE and these files of svg, or of the text after = (one.svg=text)
function(folder name)
    file(REMOVE_RECURSE "${WORK}/${name}")
    file(MAKE_DIRECTORY "${WORK}/${name}")
    foreach(f IN LISTS ARGN)
        if(f MATCHES "^([^=]+)=(.*)$")
            file(WRITE "${WORK}/${name}/${CMAKE_MATCH_1}" "${CMAKE_MATCH_2}")
        elseif(f STREQUAL "LICENSE")
            file(WRITE "${WORK}/${name}/LICENSE" "ISC License\n")
        else()
            file(WRITE "${WORK}/${name}/${f}" "${svg}")
        endif()
    endforeach()
endfunction()
function(refused name expected)
    folder(${name} ${ARGN})
    execute_process(COMMAND "${CMAKE_COMMAND}" "-DICONS_H=${WORK}/icons.h" "-DICONS_DIR=${WORK}/${name}"
                            "-DOUT=${WORK}/${name}.c" -P "${script}" RESULT_VARIABLE result ERROR_VARIABLE errors)
    string(REGEX REPLACE "[ \n]+" " " errors "${errors}")
    if(NOT result OR NOT errors MATCHES "${expected}" OR EXISTS "${WORK}/${name}.c")
        message(FATAL_ERROR "${name}: gui_icons.cmake ended with ${result} and said \"${errors}\", not ${expected}")
    endif()
endfunction()
refused(missing "missing/three.svg: no such file, GUI_ICON_THREE's drawing" LICENSE one.svg two-2.svg)
refused(underscore "underscore/two-2.svg: no such file, GUI_ICON_TWO_2's drawing" LICENSE one.svg two_2.svg three.svg)
refused(license "license: no LICENSE, Lucide's, which goes with its icons" one.svg two-2.svg three.svg)
refused(not_svg "not_svg/two-2.svg: not one <svg>, GUI_ICON_TWO_2's drawing" LICENSE one.svg "two-2.svg=<png/>"
        three.svg)
refused(two_svgs "two_svgs/one.svg: not one <svg>, GUI_ICON_ONE's drawing" LICENSE
        "one.svg=<svg viewBox=\"0 0 24 24\"></svg><svg viewBox=\"0 0 24 24\"></svg>" two-2.svg three.svg)
refused(box "box/three.svg: viewBox=\"0 0 32 32\", where Lucide draws GUI_ICON_THREE on viewBox=\"0 0 24 24\"" LICENSE
        one.svg two-2.svg "three.svg=<svg width=\"32\" viewBox=\"0 0 32 32\"><path d=\"M1 1h9\"/></svg>")
refused(no_box "no_box/one.svg: no viewBox, where Lucide draws GUI_ICON_ONE" LICENSE
        "one.svg=<svg width=\"24\" height=\"24\"><path d=\"M1 1h9\"/></svg>" two-2.svg three.svg)
refused(stray "stray/four.svg: no icon of .*icons.h is drawn from it" LICENSE one.svg two-2.svg three.svg four.svg)
# and the three whole, from files with Windows' line ends, a space each run of white space
folder(crlf LICENSE)
foreach(f one two-2 three)
    string(REPLACE "\n" "\r\n" text "${svg}")
    file(WRITE "${WORK}/crlf/${f}.svg" "${text}")
endforeach()
execute_process(COMMAND "${CMAKE_COMMAND}" "-DICONS_H=${WORK}/icons.h" "-DICONS_DIR=${WORK}/crlf"
                        "-DOUT=${WORK}/crlf.c" -P "${script}" RESULT_VARIABLE result ERROR_VARIABLE errors)
file(READ "${WORK}/crlf.c" c)
if(result OR NOT c MATCHES "{\"one\", svg0, sizeof svg0},\n    {\"two-2\", svg1, sizeof svg1},\n    {\"three\", svg2,"
   OR c MATCHES "0x0d|0x0a|0x3b|0x20,0x20|0x3e,0x20,0x3c")
    message(FATAL_ERROR "three icons with CRLF: ${result} ${errors}\n${c}")
endif()
