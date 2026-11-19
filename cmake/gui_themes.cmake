# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# The built-in themes are their files, src/gui/themes/<id>/theme.ini, and nothing else: the GUI has each file compiled
# in (generated/gui_themes.c) and reads it as it reads any theme.ini (src/gui/theme.c). The runtime's game menu takes
# YoRHa's colours from generated/gui_theme_colors.h, made here from the same files, and so do its shader loading
# screen (with YoRHa's name, loading title and [run] layout, texture and progress) and its notification's fades when it
# can't read the launcher's theme. tests/gui_theme.c checks both against the GUI's own reading of the files
include_guard(GLOBAL)
get_filename_component(snx_gui_themes_dir "${CMAKE_CURRENT_LIST_DIR}/../src/gui/themes" ABSOLUTE)
file(GLOB snx_gui_themes_all RELATIVE "${snx_gui_themes_dir}" LIST_DIRECTORIES true CONFIGURE_DEPENDS
     "${snx_gui_themes_dir}/*")
list(SORT snx_gui_themes_all)
# default first: a theme that names no base, or one that cannot be read, is the default
set(snx_gui_themes default)
foreach(id IN LISTS snx_gui_themes_all)
    if(NOT id STREQUAL "default" AND EXISTS "${snx_gui_themes_dir}/${id}/theme.ini")
        list(APPEND snx_gui_themes "${id}")
    endif()
endforeach()
list(LENGTH snx_gui_themes snx_gui_themes_count)

set(snx_gui_themes_texts "")
set(snx_gui_themes_names "")
set(snx_gui_themes_colors "")
set(snx_gui_themes_index 0)
set(snx_gui_pictures none square round) # [head] picture's words, GuiPicture's order
set(snx_gui_progresses amount percent time) # [run] progress's words, GuiProgress's order
set(snx_gui_hex "[0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f]")
foreach(id IN LISTS snx_gui_themes)
    set(file "${snx_gui_themes_dir}/${id}/theme.ini")
    set_property(DIRECTORY "${PROJECT_SOURCE_DIR}" APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${file}")
    # the file's bytes, then a NUL
    file(READ "${file}" hex HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
    string(APPEND snx_gui_themes_texts "static const char text_${snx_gui_themes_index}[] = {\n    ${bytes}0x00};\n")
    string(APPEND snx_gui_themes_names "\"${id}\", ")
    # Its colours, as theme.c reads them: a section's name up to its "]", a key's #RRGGBB bare or quoted, the last
    # one of a key winning. Comment lines go first, then the semicolons CMake would split the lines at
    file(READ "${file}" text)
    string(REGEX REPLACE "\n[ \t]*[;#][^\n]*" "\n" text "\n${text}")
    string(REPLACE ";" " " text "${text}")
    string(REPLACE "\n" ";" lines "${text}")
    string(MAKE_C_IDENTIFIER "${id}" name)
    string(TOUPPER "${name}" name)
    set(section "")
    foreach(line IN LISTS lines)
        if(line MATCHES "^[ \t]*\\[([^]]*)\\]")
            set(section "${CMAKE_MATCH_1}")
        elseif(section STREQUAL "head" AND line MATCHES "^[ \t]*picture[ \t]*=[ \t]*(none|square|round)[ \t\r]*$")
            list(FIND snx_gui_pictures "${CMAKE_MATCH_1}" picture_${name})
        elseif(section STREQUAL "theme" AND line MATCHES "^[ \t]*name[ \t]*=[ \t]*([A-Za-z0-9 ._-]+)[ \t\r]*$")
            string(STRIP "${CMAKE_MATCH_1}" text_${name}_NAME)
        elseif(section STREQUAL "head" AND line MATCHES "^[ \t]*loading[ \t]*=[ \t]*([A-Za-z0-9 ._-]+)[ \t\r]*$")
            string(STRIP "${CMAKE_MATCH_1}" text_${name}_LOADING)
        elseif(section STREQUAL "run" AND line MATCHES "^[ \t]*layout[ \t]*=[ \t]*(steps|console)[ \t\r]*$")
            if(CMAKE_MATCH_1 STREQUAL "steps")
                set(word_${name}_RUN_STEPS 1)
            else()
                set(word_${name}_RUN_STEPS 0)
            endif()
        elseif(section STREQUAL "run" AND line MATCHES "^[ \t]*texture[ \t]*=[ \t]*(true|false)[ \t\r]*$")
            if(CMAKE_MATCH_1 STREQUAL "true")
                set(word_${name}_RUN_TEXTURE 1)
            else()
                set(word_${name}_RUN_TEXTURE 0)
            endif()
        elseif(section STREQUAL "run" AND line MATCHES "^[ \t]*progress[ \t]*=[ \t]*(amount|percent|time)[ \t\r]*$")
            list(FIND snx_gui_progresses "${CMAKE_MATCH_1}" word_${name}_RUN_PROGRESS)
        elseif(section STREQUAL "transitions" AND line MATCHES "^[ \t]*fade_(in|out)_ms[ \t]*=[ \t]*([0-9]+)[ \t\r]*$")
            # a number past 10000 is none to theme.c, which keeps the base's
            string(TOUPPER "${CMAKE_MATCH_1}" key)
            if(NOT CMAKE_MATCH_2 GREATER 10000)
                math(EXPR value "${CMAKE_MATCH_2}")
                set(word_${name}_FADE_${key}_MS "${value}u")
            endif()
        elseif(line MATCHES "^[ \t]*([a-z]+)[ \t]*=[ \t]*(#${snx_gui_hex}|\"#${snx_gui_hex}\")[ \t\r]*$")
            set(key "${CMAKE_MATCH_1}")
            string(REGEX REPLACE "[\"#]" "" value "${CMAKE_MATCH_2}")
            string(TOUPPER "${value}" value)
            string(TOUPPER "${key}" key)
            if(section STREQUAL "colors" AND key MATCHES "^(PAGE|INK|SOFT|FOCUS|ACTIVE|FAILURE|DIM|WARNING)$")
                set(color_${name}_${key} "0x${value}u")
            elseif(section STREQUAL "run" AND key MATCHES "^(PAGE|INK|SOFT|DIM|FAILURE|WARNING)$")
                set(color_${name}_RUN_${key} "0x${value}u")
            elseif(section STREQUAL "splash" AND key STREQUAL "PAGE")
                set(color_${name}_SPLASH_PAGE "0x${value}u")
            endif()
        endif()
    endforeach()
    string(APPEND snx_gui_themes_colors "/* ${id} */\n")
    foreach(key PAGE INK SOFT FOCUS ACTIVE FAILURE DIM WARNING RUN_PAGE RUN_INK RUN_SOFT RUN_DIM RUN_FAILURE RUN_WARNING
                SPLASH_PAGE)
        if(DEFINED color_${name}_${key})
            string(APPEND snx_gui_themes_colors "#define SNX_GUI_THEME_${name}_${key} ${color_${name}_${key}}\n")
        endif()
    endforeach()
    if(DEFINED picture_${name})
        string(APPEND snx_gui_themes_colors "#define SNX_GUI_THEME_${name}_PICTURE ${picture_${name}}\n")
    endif()
    foreach(key NAME LOADING)
        if(DEFINED text_${name}_${key})
            string(APPEND snx_gui_themes_colors "#define SNX_GUI_THEME_${name}_${key} \"${text_${name}_${key}}\"\n")
        endif()
    endforeach()
    foreach(key RUN_STEPS RUN_TEXTURE RUN_PROGRESS FADE_IN_MS FADE_OUT_MS)
        if(DEFINED word_${name}_${key})
            string(APPEND snx_gui_themes_colors "#define SNX_GUI_THEME_${name}_${key} ${word_${name}_${key}}\n")
        endif()
    endforeach()
    math(EXPR snx_gui_themes_index "${snx_gui_themes_index} + 1")
endforeach()
set(snx_gui_themes_list "")
foreach(i RANGE 1 ${snx_gui_themes_count})
    math(EXPR i "${i} - 1")
    string(APPEND snx_gui_themes_list "text_${i}, ")
endforeach()

# file(CONFIGURE) writes only when the content changed, so a configure rebuilds nothing on its own
file(CONFIGURE OUTPUT "${PROJECT_BINARY_DIR}/generated/gui_themes.h" @ONLY CONTENT
"/* Generated by cmake/gui_themes.cmake from src/gui/themes. Do not edit. */
#ifndef SNX_GUI_THEMES_H
#define SNX_GUI_THEMES_H
/* the folders of src/gui/themes, default first, and each one's theme.ini */
#define SNX_GUI_THEME_COUNT @snx_gui_themes_count@
extern const char* const snx_gui_theme_ids[SNX_GUI_THEME_COUNT];
extern const char* const snx_gui_theme_texts[SNX_GUI_THEME_COUNT];
#endif
")
file(CONFIGURE OUTPUT "${PROJECT_BINARY_DIR}/generated/gui_themes.c" @ONLY CONTENT
"/* Generated by cmake/gui_themes.cmake from src/gui/themes. Do not edit. */
#include \"gui_themes.h\"
@snx_gui_themes_texts@const char* const snx_gui_theme_ids[SNX_GUI_THEME_COUNT] = {@snx_gui_themes_names@};
const char* const snx_gui_theme_texts[SNX_GUI_THEME_COUNT] = {@snx_gui_themes_list@};
")
file(CONFIGURE OUTPUT "${PROJECT_BINARY_DIR}/generated/gui_theme_colors.h" @ONLY CONTENT
"/* Generated by cmake/gui_themes.cmake from src/gui/themes/<id>/theme.ini. Do not edit. */
#ifndef SNX_GUI_THEME_COLORS_H
#define SNX_GUI_THEME_COLORS_H
/* each built-in's colours, 0xRRGGBB: [colors], [run] as RUN_, [splash] page as SPLASH_PAGE; and [head] picture as
   PICTURE, 0 none, 1 square, 2 round. As its file says them (a key it leaves to its base isn't here): [theme] name
   as NAME, [head] loading as LOADING, [run] layout as RUN_STEPS (1 steps, 0 console), texture as RUN_TEXTURE
   and progress as RUN_PROGRESS (GuiProgress's numbers), and [transitions] fade_in_ms and fade_out_ms as FADE_IN_MS
   and FADE_OUT_MS */
@snx_gui_themes_colors@#endif
")
