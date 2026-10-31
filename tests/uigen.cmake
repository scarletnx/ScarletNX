# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# tools/uigen on a ui.txt of its own: the scenes' items in .rodata, their symbols from their names and English texts,
# the hotkeys' switch, the stubs of the items marked @, the platforms, the lines a compiler's message names; and each
# file it refuses, refused at its line with what is wrong. Run by ctest as uigen, with UIGEN the generator and WORK a
# folder of its own
file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}/out/gui")
# a text longer than any fixed buffer: 1100 letters, a line of generated C over 512
string(REPEAT "Abcdefghij" 110 long)
string(REPEAT "abcdefghij_" 110 long_symbol) # its words split where the case changes
string(REGEX REPLACE "_$" "" long_symbol "${long_symbol}")
set(T "\t")
file(WRITE "${WORK}/ui.txt" "# Liz's example, and what it grew
[scene]Main {
${T}[list]LoadList {
${T}${T}min_row_width = 200px # width of row in pixels
${T}${T}max_row_width = 400px
${T}${T}width = 50%
${T}${T}[button]\"Load Switch NSP/XCI/NRO\" {}
${T}${T}[button]\"Load 3DS game\" {
${T}${T}${T}color = focus
${T}${T}${T}key = alt-f4
${T}${T}}
${T}${T}[button]\"Open Data Folder\" {
${T}${T}${T}for-platform = android
${T}${T}${T}key = d
${T}${T}}
${T}${T}[button]\"Paste #1 \\\"here\\\"\" {
${T}${T}${T}for-platform = desktop
${T}${T}${T}key = ctrl-alt-'
${T}${T}}
${T}${T}[button]\"Drop\" {
${T}${T}${T}key = ctrl-f4
${T}${T}${T}attr = callback
${T}${T}}
${T}${T}@[button]\"Decor\" {
${T}${T}${T}attr = callback
${T}${T}}
${T}${T}[label]\"Fixed words\" {}
${T}}
${T}[list]Games {}
${T}@[list]Spare {}
${T}[label]Status {}
}
[scene]Other {
${T}[button]\"${long}\" {
${T}${T}key = D
${T}}
${T}[list]More {
${T}${T}attr = callback
${T}${T}android.height = 40px
${T}${T}android.row_height = 40px
${T}${T}desktop.max_rows = 4
${T}${T}[button]\"One\" {}
${T}}
}
[scene]Tiles {
${T}[list]Grid {
${T}${T}tile_width = 40px
${T}${T}tile_height = 30px
${T}${T}android.tile_width = 60px
${T}}
}
[scene]Icons {
${T}[list]Rows {
${T}${T}[button]\"Play\" {
${T}${T}${T}icon = gamepad-2
${T}${T}}
${T}}
${T}[button]\"Exit\" {
${T}${T}icon = log-out
${T}}
}
")
execute_process(COMMAND "${UIGEN}" --out "${WORK}/out" "${WORK}/ui.txt"
                RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
if(result OR NOT errors STREQUAL "" OR NOT output STREQUAL "")
    message(FATAL_ERROR "uigen ended with ${result}, or said something: ${output}${errors}")
endif()
file(READ "${WORK}/out/gui/ui.h" header)
file(READ "${WORK}/out/gui/ui.c" c)
foreach(line
        # each scene's items by name, in ui.txt's order, an item of one platform within its #if
        "enum {\n    UI_MAIN_LOAD_LIST,\n    UI_MAIN_LOAD_LIST_LOAD_SWITCH_NSP_XCI_NRO,\n    UI_MAIN_LOAD_LIST_LOAD_3DS_GAME,\n#ifdef SDL_PLATFORM_ANDROID\n    UI_MAIN_LOAD_LIST_OPEN_DATA_FOLDER,\n#endif\n#if !defined(SDL_PLATFORM_ANDROID) && !defined(SDL_PLATFORM_IOS)\n    UI_MAIN_LOAD_LIST_PASTE_1_HERE,\n#endif\n"
        "    UI_MAIN_GAMES,\n    UI_MAIN_SPARE,\n    UI_MAIN_STATUS,\n    UI_MAIN_ITEMS\n};"
        # the callbacks main.c defines: a button's action, a named label's text, a list without items' rows
        "void ui_main_load_list_load_3ds_game_action_cb(App* app, uint32_t hit);\n"
        "void ui_main_status_update_cb(App* app, char* text, size_t size);\n"
        "bool ui_main_games_get_row_info(App* app, size_t row, int width, UiRowInfo* info);\nvoid ui_main_games_row_action_cb(App* app, size_t row, uint32_t hit);\n"
        # and, with attr = callback, a button's attributes as it is drawn
        "void ui_main_load_list_drop_action_cb(App* app, uint32_t hit);\nunsigned ui_main_load_list_drop_attr_cb(App* app);\n"
        # a list's, any row's by its index
        "unsigned ui_other_more_attr_cb(App* app, size_t row);\n"
        # each scene's functions
        "void ui_main_layout(GuiRect at, int scale, GuiRect* rects);\n"
        "void ui_main_draw(App* app, const GuiRect* rects, int scale, const UiList* lists);\n"
        "void ui_main_act(App* app, int item, size_t row, uint32_t hit);\n"
        "void ui_other_${long_symbol}_action_cb(App* app, uint32_t hit);\n"
        # a tile list's scene: its flow, and its rows' callbacks
        "void ui_tiles_flow(const GuiRect* rects, int scale, int item, UiFlow* flow);\n"
        "bool ui_tiles_grid_get_row_info(App* app, size_t row, int width, UiRowInfo* info);\n"
        "bool ui_main_key(App* app, int key, unsigned mods);\n")
    string(FIND "${header}" "${line}" at)
    if(at LESS 0)
        message(FATAL_ERROR "ui.h lacks \"${line}\":\n${header}")
    endif()
endforeach()
foreach(word "ui_main_load_list_fixed_words_" "ui_main_load_list_action_cb")
    string(FIND "${header}" "${word}" at)
    if(NOT at LESS 0)
        message(FATAL_ERROR "ui.h has a callback for ${word}, which takes none:\n${header}")
    endif()
endforeach()
foreach(line
        # the texts, one array, each item at its offset; a # in a text is the text's
        "const char ui_main_texts[] =\n    \"Load Switch NSP/XCI/NRO\" \"\\0\"\n    \"Load 3DS game\" \"\\0\"\n    \"Open Data Folder\" \"\\0\"\n    \"Paste #1 \\\"here\\\"\" \"\\0\"\n"
        # an item at the line it came from: its type, colour, parent, text's offset and lengths
        "#line 3 \"${WORK}/ui.txt\"\n    {UI_LIST, UI_COLOR_NONE, -1, UI_NO_TEXT, 0},\n"
        "#line 8 \"${WORK}/ui.txt\"\n    {UI_BUTTON, UI_COLOR_FOCUS, UI_MAIN_LOAD_LIST, 24, 0},\n"
        # an icon by its name in gui/icons.h, a row's and a button's, drawn beside its text
        "#include \"gui/icons.h\"\n"
        "    {UI_BUTTON, UI_COLOR_NONE, UI_ICONS_ROWS, 0, GUI_ICON_GAMEPAD_2},\n"
        "    {UI_BUTTON, UI_COLOR_NONE, -1, 5, GUI_ICON_LOG_OUT},\n"
        # a static list's rows too drawn up by its shift, scrolled between two rows, inside its area
        "    n += lists[UI_ICONS_ROWS].shift > 0;\n    if (lists[UI_ICONS_ROWS].shift) ui_clip(app, &r[UI_ICONS_ROWS]);\n"
        "ui_draw_row(app, ui_shifted(ui_beside_bar(ui_row(r[UI_ICONS_ROWS], i, 17, 2, 0, 0, s), bw), lists[UI_ICONS_ROWS].shift), ui_tr(ui_icons_texts + ui_icons_items[item].text), ui_icons_items[item].icon, NULL,\n"
        "    ui_draw_row(app, r[UI_ICONS_EXIT], ui_tr(ui_icons_texts + 5), ui_icons_items[UI_ICONS_EXIT].icon, NULL, ui_icons_items[UI_ICONS_EXIT].color, 0, false);\n"
        # the list's rows; its row bounds in px, times the scale
        "static const int16_t ui_main_load_list_rows[] = {\n    UI_MAIN_LOAD_LIST_LOAD_SWITCH_NSP_XCI_NRO,\n"
        "ui_row(r[UI_MAIN_LOAD_LIST], i, 17, 2, 200 * s, 400 * s, s)"
        # the list half the inside wide, under the scene's first row: fractions and pixels in 16.16; its height a third
        # of what the 17 px row leaves, -17/3 px rounded (-371370.67), not cut
        "    r[UI_MAIN_LOAD_LIST] = gui_rect(at.x + UI_AT(0L, at.w, 0L, s), at.y + UI_AT(0L, at.h, 0L, s), UI_AT(32768L, at.w, 0L, s), UI_AT(21845L, at.h, -371371L, s));\n"
        # one case a key, each of its modifiers in it, an item of one platform within its #if
        "        case SDLK_F4:\n#line 10 \"${WORK}/ui.txt\"\n            if (mods == UI_MOD_ALT) {\n                ui_main_load_list_load_3ds_game_action_cb(app, UI_HIT_CLICK);\n                return true;\n            }\n#line 21 \"${WORK}/ui.txt\"\n            if (mods == UI_MOD_CTRL) {\n                ui_main_load_list_drop_action_cb(app, UI_HIT_CLICK);"
        "        case '\\'':\n"
        "            if (mods == (UI_MOD_CTRL | UI_MOD_ALT)) {\n"
        # a letter's key, as SDL names it: the lowercase one, shift or not
        "        case 'd':\n"
        # the stubs: a decorative button does nothing, a stubbed list has no rows
        "void ui_main_load_list_decor_action_cb(App* app, uint32_t hit) {\n    (void)app;\n    (void)hit;\n}\nunsigned ui_main_load_list_decor_attr_cb(App* app) {\n    (void)app;\n    return 0;\n}\n"
        # a static list's rows get their attributes by item, and the arrows of the rows hidden above and below while it
        # has no scroll bar, which main.c draws beside the rows shown while its rows are more, the rows beside it
        "static unsigned ui_main_attr(App* app, int item) {\n    (void)app;\n    switch (item) {\n"
        "        case UI_MAIN_LOAD_LIST_DROP: return ui_main_load_list_drop_attr_cb(app);\n"
        "    bw = ui_bar(app, ui_rows_span(ui_row(r[UI_MAIN_LOAD_LIST], 0, 17, 2, 200 * s, 400 * s, s), ui_row(r[UI_MAIN_LOAD_LIST], n ? n - 1 : 0, 17, 2, 200 * s, 400 * s, s)), &lists[UI_MAIN_LOAD_LIST], sizeof ui_main_load_list_rows / sizeof *ui_main_load_list_rows, n);\n"
        "        unsigned attr = ui_main_attr(app, item);\n        if (i == 0 && row > 0 && !bw) attr |= UI_ATTR_MORE_UP; /* else the bar says it */\n        if (i + 1 == n && row + 1 < sizeof ui_main_load_list_rows / sizeof *ui_main_load_list_rows && !bw) attr |= UI_ATTR_MORE_DOWN;\n"
        # a list without items asks for the row after the last one shown first: the row's own label may be a buffer the
        # next call writes; each asked with the row's width, for a label cut to fit, and drawn in that box
        # its scroll bar from the code's count of its rows; scrolled between two rows (shift), one more row shows under
        # them, the drawing kept in the list's area
        "    n = ui_main_shown(r, scale, UI_MAIN_GAMES);\n    bw = ui_bar(app, ui_rows_span(ui_row(r[UI_MAIN_GAMES], 0, 17, 2, 0, 0, s), ui_row(r[UI_MAIN_GAMES], n ? n - 1 : 0, 17, 2, 0, 0, s)), &lists[UI_MAIN_GAMES], lists[UI_MAIN_GAMES].count, n);\n"
        "    n += lists[UI_MAIN_GAMES].shift > 0;\n    if (lists[UI_MAIN_GAMES].shift) ui_clip(app, &r[UI_MAIN_GAMES]);\n"
        "        GuiRect box = ui_beside_bar(ui_row(r[UI_MAIN_GAMES], i, 17, 2, 0, 0, s), bw);\n        UiRowInfo next;\n        box.y -= lists[UI_MAIN_GAMES].shift;\n        bool more = i + 1 == n && ui_main_games_get_row_info(app, row + 1, box.w, &next);\n        info.label = NULL;\n        info.icon = 0;\n        info.image = NULL;\n        info.attr = 0;\n        if (!ui_main_games_get_row_info(app, row, box.w, &info)) break;\n        if (i == 0 && row > 0 && !bw) info.attr |= UI_ATTR_MORE_UP; /* else the bar says it */\n        if (more && !bw) info.attr |= UI_ATTR_MORE_DOWN;\n        ui_draw_row(app, box, info.label ? info.label : \"(null)\", info.icon, info.image, "
        # the hit test: a row of a list without items is there while get_row_info says so, hit where it is drawn
        "    n = ui_main_shown(r, scale, UI_MAIN_GAMES);\n    bw = ui_bar_width(app, lists[UI_MAIN_GAMES].count, n);\n    n += lists[UI_MAIN_GAMES].shift > 0;\n    for (i = 0; i < n && ui_in(r[UI_MAIN_GAMES], x, y); i++) {\n"
        "        GuiRect box = ui_beside_bar(ui_row(r[UI_MAIN_GAMES], i, 17, 2, 0, 0, s), bw);\n        box.y -= lists[UI_MAIN_GAMES].shift;\n        *row = lists[UI_MAIN_GAMES].first + i;\n        if (!ui_main_games_get_row_info(app, *row, box.w, &info)) break;\n        if (ui_in(box, x, y)) {\n            *item = UI_MAIN_GAMES;\n"
        # ui.txt's texts as shown, through main.c's ui_tr
        "ui_tr(ui_main_texts + ui_main_items[item].text)"
        "        unsigned attr = ui_other_more_attr_cb(app, row);\n"
        # a platform's own metric: a macro each platform defines, the plain value or the default where it sets none;
        # android's on iOS too, the phones'
        "#if defined(SDL_PLATFORM_ANDROID) || defined(SDL_PLATFORM_IOS)\n#define UI_OTHER_MORE__ROW_HEIGHT 40\n#else\n#define UI_OTHER_MORE__ROW_HEIGHT 17\n#endif\n"
        "#if defined(SDL_PLATFORM_ANDROID) || defined(SDL_PLATFORM_IOS)\n#define UI_OTHER_MORE__MAX_ROWS 8\n#else\n#define UI_OTHER_MORE__MAX_ROWS 4\n#endif\n"
        "case UI_OTHER_MORE: return ui_shown(r[UI_OTHER_MORE].h, UI_OTHER_MORE__ROW_HEIGHT, 2, UI_OTHER_MORE__MAX_ROWS, scale);\n"
        # a platform's own height: the scene laid out for each platform, the phones' More 40px, the others' growing
        "void ui_other_layout(GuiRect at, int scale, GuiRect* r) {\n    int s = scale;\n#if defined(SDL_PLATFORM_ANDROID) || defined(SDL_PLATFORM_IOS)\n"
        "    r[UI_OTHER_MORE] = gui_rect(at.x + UI_AT(0L, at.w, 0L, s), at.y + UI_AT(0L, at.h, 1114112L, s), UI_AT(65536L, at.w, 0L, s), UI_AT(0L, at.h, 2621440L, s));\n"
        "    r[UI_OTHER_MORE] = gui_rect(at.x + UI_AT(0L, at.w, 0L, s), at.y + UI_AT(0L, at.h, 1114112L, s), UI_AT(65536L, at.w, 0L, s), UI_AT(65536L, at.h, -1114112L, s));\n"
        "bool ui_main_spare_get_row_info(App* app, size_t row, int width, UiRowInfo* info) {\n    (void)app;\n    (void)row;\n    (void)width;\n    (void)info;\n    return false;\n}\n"
        # a tile list's metrics, a platform's own tile_width a macro as a row's metric, its rows placed by its flow
        "#if defined(SDL_PLATFORM_ANDROID) || defined(SDL_PLATFORM_IOS)\n#define UI_TILES_GRID__TILE_WIDTH 60\n#else\n#define UI_TILES_GRID__TILE_WIDTH 40\n#endif\n"
        "        case UI_TILES_GRID: ui_flow_start(flow, r[UI_TILES_GRID], UI_TILES_GRID__TILE_WIDTH, 30, 2, 8, 17, scale); return;\n"
        "        case UI_TILES_GRID: return ui_tiles_shown(r[UI_TILES_GRID], UI_TILES_GRID__TILE_WIDTH, 30, 2, 8, scale);\n"
        "            if (!ui_tiles_grid_get_row_info(app, row, flow.tile_w, &info)) break;\n            if (!ui_flow_next(&flow, (info.attr & UI_ATTR_LABEL) != 0, &box)) break;\n"
        # shifted, its lines drawn up and hit where they are drawn, inside its area
        "        flow.area.y -= lists[UI_TILES_GRID].shift;\n        flow.area.h += lists[UI_TILES_GRID].shift;\n        if (lists[UI_TILES_GRID].shift) ui_clip(app, &r[UI_TILES_GRID]);\n"
        "        for (i = 0; ui_in(r[UI_TILES_GRID], x, y); i++) {\n")
    string(FIND "${c}" "${line}" at)
    if(at LESS 0)
        message(FATAL_ERROR "ui.c lacks \"${line}\":\n${c}")
    endif()
endforeach()
# the hotkeys of scene Other: D, its own scene's, not Main's d
string(FIND "${c}" "bool ui_other_key(App* app, int key, unsigned mods) {" other)
string(SUBSTRING "${c}" ${other} -1 other_key)
string(FIND "${other_key}" "case 'd':" at)
if(at LESS 0)
    message(FATAL_ERROR "ui_other_key has no case 'd' for D:\n${other_key}")
endif()
# after a line of ui.txt, the next generated line is ui.c's own again: its number, counted
string(REGEX MATCH "\n#line ([0-9]+) \"[^\"\n]*/out/gui/ui\\.c\"\n" back "${c}")
set(number "${CMAKE_MATCH_1}")
if(NOT back)
    message(FATAL_ERROR "ui.c never goes back to its own lines:\n${c}")
endif()
string(FIND "${c}" "${back}" at)
string(SUBSTRING "${c}" 0 ${at} before)
string(REGEX MATCHALL "\n" newlines "${before}")
list(LENGTH newlines n)
math(EXPR expected "${n} + 3") # the match opens on line n + 1's newline: the #line is n + 2, the line after it n + 3
if(NOT number EQUAL expected)
    message(FATAL_ERROR "ui.c goes back to line ${number}, not ${expected}:\n${c}")
endif()

# what it refuses: the file's line and what is wrong
function(refused name text expected)
    file(WRITE "${WORK}/${name}.txt" "${text}")
    execute_process(COMMAND "${UIGEN}" --out "${WORK}/out" "${WORK}/${name}.txt"
                    RESULT_VARIABLE result ERROR_VARIABLE errors)
    if(NOT result OR NOT errors MATCHES "^[^\n]*${name}\\.txt:${expected}\n$")
        message(FATAL_ERROR "${name}: uigen ended with ${result} and said \"${errors}\", not ${name}.txt:${expected}")
    endif()
endfunction()
set(S "[scene]S {\n")
refused(spaces "${S}    [button]\"A\" {}\n}\n" "2: indented with spaces: ui.txt indents with tabs")
refused(item_tabs "${S}${T}${T}[button]\"A\" {}\n}\n" "2: an item at 2 tab\\(s\\): inside its parent, 1")
refused(property_tabs "${S}${T}[list]L {\n${T}width = 1px\n${T}}\n}\n" "3: a property at 1 tab\\(s\\): its item's are at 2")
refused(stray_close "}\n" "1: } with no block open")
refused(close_tabs "${S}${T}}\n" "2: } at 1 tab\\(s\\): the block it closes opens at 0")
refused(type "${S}${T}[slider]V {}\n}\n" "2: \\[slider\\] is not a type: scene, list, button, label, box, slot")
refused(outside "[button]\"A\" {}\n" "1: a button outside a scene: the file holds scenes")
refused(holds "${S}${T}[button]\"A\" {\n${T}${T}[label]\"B\" {}\n${T}}\n}\n" "3: a button cannot hold a label")
refused(scene_text "[scene]\"Main\" {}\n" "1: a scene has a Name, not a text")
refused(button_name "${S}${T}[button]Load {}\n}\n" "2: a button has its \"Text\": its symbol is the text's, and a label is what shows a changing one")
refused(property "${S}${T}[button]\"A\" {\n${T}${T}some_prop = 131313\n${T}}\n}\n" "3: some_prop is not a property of a button: it takes width, height, grow, attr, color, key, for-platform, icon")
refused(scene_property "${S}${T}color = ink\n}\n" "2: color is not a property of a scene: it takes gap, padding, narrow_below, direction, align, justify")
refused(twice "${S}${T}[label]\"A\" {\n${T}${T}color = ink\n${T}${T}color = soft\n${T}}\n}\n" "4: color is set twice for one label")
refused(percent "${S}${T}[list]L {\n${T}${T}width = 150%\n${T}}\n}\n" "3: width = 150%: a % of its parent is 0 to 100")
refused(unit "${S}${T}[list]L {\n${T}${T}width = 12em\n${T}}\n}\n" "3: width = 12em: a length ends in px, cols, rows or %")
refused(negative "${S}${T}[list]L {\n${T}${T}width = -3px\n${T}}\n}\n" "3: width = -3px: a length is whole numbers of px, cols or rows, or a % of its parent, added with \\+")
refused(color "${S}${T}[label]\"A\" {\n${T}${T}color = focsu\n${T}}\n}\n" "3: color = focsu: the theme's colours are page, ink, soft, focus, active, failure, dim, warning, added")
refused(platform "${S}${T}[label]\"A\" {\n${T}${T}for-platform = ios\n${T}}\n}\n" "3: for-platform = ios: the platforms are android and desktop")
refused(enter "${S}${T}[button]\"A\" {\n${T}${T}key = enter\n${T}}\n}\n" "3: key = enter: one ASCII character or f1 to f12, after ctrl- or alt- \\(Enter is the click's, not a hotkey\\)")
refused(f13 "${S}${T}[button]\"A\" {\n${T}${T}key = f13\n${T}}\n}\n" "3: key = f13: the function keys are f1 to f12")
refused(ctrl_ctrl "${S}${T}[button]\"A\" {\n${T}${T}key = ctrl-ctrl-a\n${T}}\n}\n" "3: key = ctrl-ctrl-a: ctrl twice")
refused(same_key "${S}${T}[button]\"A\" {\n${T}${T}key = r\n${T}}\n${T}[button]\"B\" {\n${T}${T}key = R\n${T}}\n}\n" "6: its key is line 3's too, in the same scene")
refused(stub_label "${S}${T}@[label]\"A\" {}\n}\n" "2: @ stubs an item's callbacks, and this label has none")
refused(stub_list "${S}${T}@[list]L {\n${T}${T}[button]\"A\" {}\n${T}}\n}\n" "2: @ stubs a list's rows, and this one has items of its own")
refused(symbol "${S}${T}[button]\"Load: CIA\" {}\n${T}[button]\"Load CIA\" {}\n}\n" "3: ui_s_load_cia is the symbol of line 2's item too: their names or texts differ only in what is not a letter or a digit")
refused(scene_names "${S}${T}[list]Items {}\n}\n" "2: ui_s_items is the scene's own table or function: name it otherwise")
refused(unclosed "${S}${T}[list]L {\n${T}}\n" "1: \\[scene\\]S is never closed")
refused(orphan "x = 1px\n" "1: x = 1px outside any item")
refused(no_equals "${S}${T}[list]L {\n${T}${T}width\n${T}}\n}\n" "3: width: a property is word = value")
refused(unterminated "${S}${T}[button]\"A {}\n}\n" "2: a text without its closing \"")
refused(empty_text "${S}${T}[button]\"\" {}\n}\n" "2: an empty text: an item shows its text, or a label has a name instead")
refused(junk "${S}${T}[button]\"A\" {} x\n}\n" "2: after the item, \\{ or \\{\\}, not \"\\{\\} x\"")
refused(scene_platform "${S}${T}[button]\"A\" {\n${T}${T}for-platform = android\n${T}}\n}\n" "2: for-platform is a row's: an item of a scene keeps its place on every platform")
refused(row_width "${S}${T}[list]L {\n${T}${T}[button]\"A\" {\n${T}${T}${T}width = 10px\n${T}${T}}\n${T}}\n}\n" "4: width is not a property of a row's button: it takes attr, color, key, for-platform, icon")
refused(row_label "${S}${T}[list]L {\n${T}${T}[label]Status {}\n${T}}\n}\n" "3: a row of a list shows its \"Text\": a list whose rows change has no items, its rows come from its get_row_info")
refused(grow_and_height "${S}${T}[list]L {\n${T}${T}height = 40px\n${T}${T}grow = 2\n${T}}\n}\n" "2: height in px and grow 2: one of them, a length or a share of what is left")
refused(max_rows "${S}${T}[list]L {\n${T}${T}max_rows = 65\n${T}}\n}\n" "3: max_rows = 65: a whole number from 1 to 64")
refused(scroll "${S}${T}[list]L {\n${T}${T}scroll = middle\n${T}}\n}\n" "3: scroll = middle: it is center or edge")
refused(direction "[scene]S {\n${T}direction = diagonal\n}\n" "2: direction = diagonal: it is column or row")
refused(pixels "[scene]S {\n${T}gap = 5%\n}\n" "2: gap = 5%: pixels end in px, cols or rows")
refused(row_bounds "${S}${T}[list]L {\n${T}${T}min_row_width = 300px\n${T}${T}max_row_width = 200px\n${T}}\n}\n" "2: min_row_width 300px is over max_row_width 200px")
refused(row_bounds_pct "${S}${T}[list]L {\n${T}${T}min_row_width = 60%\n${T}${T}max_row_width = 50%\n${T}}\n}\n" "2: min_row_width 60% is over max_row_width 50%")
refused(icon_case "${S}${T}[button]\"A\" {\n${T}${T}icon = Gamepad\n${T}}\n}\n" "3: icon = Gamepad: an icon's name is lower case letters, digits and -, as gui/icons.h's list has it")
refused(icon_dash "${S}${T}[button]\"A\" {\n${T}${T}icon = folder-\n${T}}\n}\n" "3: icon = folder-: an icon's name is lower case letters, digits and -, as gui/icons.h's list has it")
refused(icon_digit "${S}${T}[button]\"A\" {\n${T}${T}icon = 2d\n${T}}\n}\n" "3: icon = 2d: an icon's name is lower case letters, digits and -, as gui/icons.h's list has it")
refused(icon_list "${S}${T}[list]L {\n${T}${T}icon = folder\n${T}}\n}\n" "3: icon is not a property of a list: it takes .*")
refused(label_attr "${S}${T}[label]\"A\" {\n${T}${T}attr = callback\n${T}}\n}\n" "3: attr is not a property of a label: it takes width, height, grow, color, for-platform")
refused(attr "${S}${T}[button]\"A\" {\n${T}${T}attr = always\n${T}}\n}\n" "3: attr = always: it is callback")
refused(rows_attr "${S}${T}[list]L {\n${T}${T}attr = callback\n${T}}\n}\n" "2: attr = callback on a list without items: its get_row_info gives each row's attr")
refused(platform_gap "[scene]S {\n${T}android.gap = 10px\n}\n" "2: android.gap: a platform's own value is a width, a height, or a list's row_height, row_gap, max_rows, tile_width or tile_height")
refused(platform_button "${S}${T}[button]\"A\" {\n${T}${T}desktop.row_gap = 4px\n${T}}\n}\n" "3: desktop.row_gap: a platform's own value is a width, a height, or a list's row_height, row_gap, max_rows, tile_width or tile_height")
# a tile list: both its tiles' lengths, in pixels, a pixel at least; its rows from callbacks, its tiles' own width
refused(tile_alone "${S}${T}[list]L {\n${T}${T}tile_width = 10cols\n${T}}\n}\n" "2: tile_width without tile_height: a tile list has both, its tiles' width and height")
refused(tile_height_alone "${S}${T}[list]L {\n${T}${T}tile_height = 4rows\n${T}}\n}\n" "2: tile_height without tile_width: a tile list has both, its tiles' width and height")
refused(tile_items "${S}${T}[list]L {\n${T}${T}tile_width = 10cols\n${T}${T}tile_height = 4rows\n${T}${T}[button]\"A\" {}\n${T}}\n}\n" "2: tile_width on a list with items: a tile list's rows come from its get_row_info")
refused(tile_row_width "${S}${T}[list]L {\n${T}${T}tile_width = 10cols\n${T}${T}tile_height = 4rows\n${T}${T}max_row_width = 50%\n${T}}\n}\n" "2: max_row_width on a tile list: its tiles are tile_width wide, a label its whole width")
refused(tile_zero "${S}${T}[list]L {\n${T}${T}tile_width = 0px\n${T}${T}tile_height = 4rows\n${T}}\n}\n" "2: tile_width = 0px: a tile is a pixel at least")
refused(tile_percent "${S}${T}[list]L {\n${T}${T}tile_width = 10%\n${T}}\n}\n" "3: tile_width = 10%: pixels end in px, cols or rows")
refused(tile_platform "${S}${T}[list]L {\n${T}${T}android.tile_width = 60px\n${T}}\n}\n" "2: a platform's tile_width or tile_height on a list that is no tile list: it has them on every platform, its own where a platform sets them")
refused(platform_value "${S}${T}[list]L {\n${T}${T}android.row_height = 5%\n${T}}\n}\n" "3: row_height = 5%: pixels end in px, cols or rows")
refused(platform_twice "${S}${T}[list]L {\n${T}${T}android.row_gap = 4px\n${T}${T}android.row_gap = 6px\n${T}}\n}\n" "4: android.row_gap is set twice for one list")
refused(table_name "${S}${T}[list]Layout {}\n}\n" "2: ui_s_layout is the scene's own table or function: name it otherwise")

# a frame: a scene of boxes and slots, laid out at run time
set(F "[scene]F {\n${T}direction = column\n")
refused(frame_list "${F}${T}[slot]A {\n${T}${T}grow = 1\n${T}}\n${T}[list]L {}\n}\n" "6: a list in scene F, whose boxes and slots are areas the code draws in: no list, button or label beside them")
refused(frame_holds "${F}${T}[slot]A {\n${T}${T}grow = 1\n${T}${T}[slot]B {}\n${T}}\n}\n" "5: a slot cannot hold a slot")
refused(slot_along "${F}${T}[slot]A {\n${T}${T}width = 10px\n${T}}\n}\n" "3: slot A has no height along its column: a height, a grow or measure = callback")
refused(slot_along_row "[scene]F {\n${T}direction = row\n${T}[slot]A {\n${T}${T}measure = callback\n${T}}\n}\n" "3: slot A has no width along its row: a width, a grow or \\(in a column\\) measure = callback")
refused(measure_height "${F}${T}[slot]A {\n${T}${T}measure = callback\n${T}${T}height = 2rows\n${T}}\n}\n" "3: measure and height: a measured slot's height is its measure_cb's")
refused(frame_grow "${F}${T}[slot]A {\n${T}${T}height = 2rows\n${T}${T}grow = 1\n${T}}\n}\n" "3: height and grow: one of them, a length or a share of what is left")
refused(box_measure "${F}${T}[box]B {\n${T}${T}measure = callback\n${T}}\n}\n" "4: measure is not a property of a box: it takes width, height, min_width, max_width, min_height, max_height, gap, padding, grow, direction, align, justify, shown, narrow")
refused(slot_color "${F}${T}[slot]A {\n${T}${T}color = ink\n${T}}\n}\n" "4: color is not a property of a slot: it takes width, height, min_width, max_width, min_height, max_height, grow, measure, shown, narrow")
refused(shown_word "${F}${T}[slot]A {\n${T}${T}grow = 1\n${T}${T}shown = always\n${T}}\n}\n" "5: shown = always: it is callback")
refused(narrow_word "${F}${T}narrow_below = 45cols\n${T}[slot]A {\n${T}${T}grow = 1\n${T}${T}narrow = shrink\n${T}}\n}\n" "6: narrow = shrink: it is hide")
refused(narrow_alone "${F}${T}[slot]A {\n${T}${T}grow = 1\n${T}${T}narrow = hide\n${T}}\n}\n" "3: narrow and narrow.\\* need the scene's narrow_below, the width they start below")
refused(narrow_color "${F}${T}narrow_below = 45cols\n${T}[slot]A {\n${T}${T}grow = 1\n${T}${T}narrow.color = ink\n${T}}\n}\n" "6: narrow.color: below narrow_below a box or a slot may have its own width, height, grow, bounds or a box its direction")
refused(narrow_twice "${F}${T}narrow_below = 45cols\n${T}[slot]A {\n${T}${T}grow = 1\n${T}${T}narrow.grow = 2\n${T}${T}narrow.grow = 3\n${T}}\n}\n" "7: narrow.grow is set twice for one slot")
refused(narrow_along "${F}${T}narrow_below = 45cols\n${T}[box]B {\n${T}${T}direction = row\n${T}${T}grow = 1\n${T}${T}narrow.direction = column\n${T}${T}[slot]A {\n${T}${T}${T}width = 10cols\n${T}${T}}\n${T}}\n}\n" "8: slot A has no height along its column: a height, a grow or measure = callback")
refused(narrow_below_list "${S}${T}narrow_below = 45cols\n${T}[list]L {}\n}\n" "1: narrow_below on scene S, which has no box or slot to lay out narrow")
refused(content_pct "${F}${T}[box]B {\n${T}${T}direction = column\n${T}${T}[slot]A {\n${T}${T}${T}height = 50%\n${T}${T}}\n${T}}\n}\n" "5: height 50% in box B, which takes its height from its content: a % of it is of nothing")
refused(frame_stub "${F}${T}@[slot]A {\n${T}${T}grow = 1\n${T}}\n}\n" "3: @ on a slot: a frame's callbacks are the code's that draws in it")
refused(narrow_bound "${F}${T}narrow_below = 45cols\n${T}[slot]A {\n${T}${T}grow = 1\n${T}${T}narrow.min_width = callback\n${T}}\n}\n" "6: narrow.min_width = callback: a bound's callback is the plain one's, called below narrow_below too")
refused(bound_word "${F}${T}[slot]A {\n${T}${T}grow = 1\n${T}${T}min_width = always\n${T}}\n}\n" "5: min_width = always: a length is whole numbers of px, cols or rows, or a % of its parent, added with \\+")
refused(sum_unit "${F}${T}[slot]A {\n${T}${T}height = 1rows + 4em\n${T}}\n}\n" "4: height = 1rows \\+ 4em: a length ends in px, cols, rows or %")
# the same key on two platforms is two keys
file(WRITE "${WORK}/platforms.txt" "${S}${T}[list]L {\n${T}${T}[button]\"A\" {\n${T}${T}${T}key = r\n${T}${T}${T}for-platform = android\n${T}${T}}\n${T}${T}[button]\"B\" {\n${T}${T}${T}key = r\n${T}${T}${T}for-platform = desktop\n${T}${T}}\n${T}}\n}\n")
execute_process(COMMAND "${UIGEN}" --out "${WORK}/out" "${WORK}/platforms.txt" RESULT_VARIABLE result ERROR_VARIABLE errors)
if(result)
    message(FATAL_ERROR "uigen refused one key on two platforms: ${errors}")
endif()
# the command line, and a file with no scene
foreach(args "--out;${WORK}/out;--colour;${WORK}/ui.txt" "--out;${WORK}/out" "--out;${WORK}/out;${WORK}/ui.txt;${WORK}/ui.txt")
    execute_process(COMMAND "${UIGEN}" ${args} RESULT_VARIABLE result ERROR_VARIABLE errors)
    if(NOT result OR NOT errors MATCHES "^uigen: (--colour is not an option: --out DIR, then ui.txt|usage: uigen --out DIR FILE|[^\n]*ui.txt after [^\n]*ui.txt: one ui.txt holds every scene)\n$")
        message(FATAL_ERROR "uigen ${args}: ended with ${result} and said \"${errors}\"")
    endif()
endforeach()
file(WRITE "${WORK}/none.txt" "# no scene\n")
execute_process(COMMAND "${UIGEN}" --out "${WORK}/out" "${WORK}/none.txt" RESULT_VARIABLE result ERROR_VARIABLE errors)
if(NOT result OR NOT errors MATCHES "^uigen: no scene in [^\n]*none.txt\n$")
    message(FATAL_ERROR "uigen on no scene: ended with ${result} and said \"${errors}\"")
endif()
