/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "gui/model.h"
#include "gui/browser.h"
#include "gui/latin.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    GuiMenu menu = {0};
    GuiConfirm confirm = {0};
    /* the items alone make the ring: the games are the Games tab's */
    menu.selected = GUI_SWITCH;
    gui_menu_key(&menu, GUI_LEFT);
    assert(menu.selected == GUI_EXIT);
    gui_menu_key(&menu, GUI_RIGHT);
    assert(menu.selected == GUI_SWITCH && !menu.activated);
    /* a load opens its screen at once; ESC goes back to the menu, on it */
    gui_menu_key(&menu, GUI_SELECT);
    assert(menu.view == GUI_LOAD_DROP && menu.activated && menu.active == GUI_SWITCH && !menu.running);
    gui_menu_key(&menu, GUI_CANCEL);
    assert(menu.view == GUI_LOAD_NONE && menu.selected == GUI_SWITCH);
    /* the Games tab's browser takes the keys */
    menu.view = GUI_LOAD_GAMES;
    assert(gui_menu_key(&menu, GUI_DOWN) == GUI_MENU_STAY && menu.selected == GUI_SWITCH);
    menu.view = GUI_LOAD_NONE;
    gui_menu_key(&menu, GUI_DOWN);
    assert(menu.selected == GUI_LINUX);
    gui_menu_key(&menu, GUI_DOWN);
    assert(menu.selected == GUI_3DS);
    gui_menu_key(&menu, GUI_SELECT);
    assert(menu.active == GUI_3DS && menu.view == GUI_LOAD_DROP);
    gui_menu_key(&menu, GUI_CANCEL);
    gui_menu_key(&menu, GUI_RIGHT);
    assert(menu.selected == GUI_GBA);
    gui_menu_key(&menu, GUI_SELECT);
    assert(menu.active == GUI_GBA && menu.view == GUI_LOAD_DROP);
    gui_menu_key(&menu, GUI_CANCEL);
    gui_menu_key(&menu, GUI_DOWN);
    assert(menu.selected == GUI_KEYS);
    gui_menu_key(&menu, GUI_SELECT);
    assert(menu.view == GUI_LOAD_KEYS && !menu.running);
    gui_menu_key(&menu, GUI_DOWN);
    gui_menu_key(&menu, GUI_SELECT);
    assert(menu.selected == GUI_KEYS && menu.view == GUI_LOAD_KEYS);
    gui_menu_key(&menu, GUI_CANCEL);
    assert(menu.view == GUI_LOAD_NONE && !menu.quit);
    gui_menu_key(&menu, GUI_RIGHT);
    assert(menu.selected == GUI_SETTINGS);
    /* Settings opens the screens of the game's settings, the launcher's and its App (settings.h, gui_settings.c) */
    assert(gui_menu_key(&menu, GUI_SELECT) == GUI_MENU_SETTINGS && menu.active == GUI_SETTINGS);
    assert(menu.view == GUI_LOAD_NONE && strstr(gui_setting_description(&menu), "its theme"));
    menu.running = true;
    assert(gui_menu_key(&menu, GUI_DOWN) == GUI_MENU_STAY && menu.selected == GUI_SETTINGS);
    assert(gui_menu_key(&menu, GUI_SELECT) == GUI_MENU_STAY);
    menu.running = false;
    gui_menu_key(&menu, GUI_DOWN);
    assert(gui_menu_key(&menu, GUI_SELECT) == GUI_MENU_LOG && menu.selected == GUI_LOG && !menu.quit);
    gui_menu_key(&menu, GUI_DOWN);
    gui_menu_key(&menu, GUI_SELECT);
    assert(menu.quit);

    /* a load's screen keeps the load a picked file goes to while the focus moves (a phone's menu shows under it) */
    menu = (GuiMenu){0};
    gui_menu_key(&menu, GUI_SELECT); /* Games hidden: the first load */
    assert(menu.active == GUI_SWITCH && menu.view == GUI_LOAD_DROP);
    menu.selected = GUI_LINUX;
    gui_menu_key(&menu, GUI_DOWN);
    assert(menu.selected == GUI_3DS && menu.active == GUI_SWITCH && menu.view == GUI_LOAD_DROP);
    gui_menu_key(&menu, GUI_CANCEL);
    assert(menu.view == GUI_LOAD_NONE && menu.selected == GUI_SWITCH);

    /* the footer's use, as a player reads it: MB under a GB, the CPU a share of every core, never over 100% */
    {
        char use[128];
        gui_usage_text(use, sizeof use, true, 812ull << 20, 52, 4);
        assert(!strcmp(use, "GUI + children: RAM 812 MB  CPU 13% (0.5 cores)"));
        gui_usage_text(use, sizeof use, true, 1404ull << 20, 216, 8); /* a game compiling on 8 cores */
        assert(!strcmp(use, "GUI + children: RAM 1.4 GB  CPU 27% (2.2 cores)"));
        gui_usage_text(use, sizeof use, true, 1, 830, 8);
        assert(!strcmp(use, "GUI + children: RAM 1 KB  CPU 100% (8.0 cores)"));
        gui_usage_text(use, sizeof use, true, 0, 150, 0); /* SDL knows no cores: one */
        assert(!strcmp(use, "GUI + children: RAM 0 MB  CPU 100% (1.0 cores)"));
        gui_usage_text(use, sizeof use, false, 1, 1, 1);
        assert(!strcmp(use, "GUI + children: resource usage unavailable"));
        /* sizes, as the footer and the Settings' disk line write them: KB under a MB, rounded up, MB under a GB */
        gui_size_text(use, sizeof use, 0);
        assert(!strcmp(use, "0 MB"));
        gui_size_text(use, sizeof use, 1);
        assert(!strcmp(use, "1 KB"));
        gui_size_text(use, sizeof use, 1025);
        assert(!strcmp(use, "2 KB"));
        gui_size_text(use, sizeof use, 1023ull << 10);
        assert(!strcmp(use, "1023 KB"));
        gui_size_text(use, sizeof use, (1ull << 20) - 1);
        assert(!strcmp(use, "1 MB"));
        gui_size_text(use, sizeof use, 1023ull << 20);
        assert(!strcmp(use, "1023 MB"));
        gui_size_text(use, sizeof use, 1ull << 30);
        assert(!strcmp(use, "1.0 GB"));
        gui_size_text(use, sizeof use, 26ull << 30 | 400ull << 20);
        assert(!strcmp(use, "26.4 GB"));
    }
    /* a run's room: the extraction and a Switch game's RomFS copy each about the dump's size, both from one disk's free
       space, or each from its own; a 3DS game has no copy */
    {
        uint64_t need, left;
        assert(gui_room(10, true, 20, 0, true, &need, &left) && need == 20 && left == 20);
        assert(!gui_room(10, true, 19, 99, true, &need, &left) && need == 20 && left == 19);
        assert(gui_room(10, true, 10, 10, false, &need, &left));
        assert(!gui_room(10, true, 9, 99, false, &need, &left) && need == 10 && left == 9);
        assert(!gui_room(10, true, 99, 9, false, &need, &left) && need == 10 && left == 9); /* the games' disk */
        assert(gui_room(10, false, 10, 0, true, &need, &left) && need == 10);
        assert(!gui_room(10, false, 9, 99, true, &need, &left) && need == 10 && left == 9);
    }
    /* a line 5 columns too long, a column every 100 ms: rests 8 steps, slides, rests at its end, slides back */
    assert(!gui_slide(0, 900, 100) && !gui_slide(5, 900, 0) && !gui_slide(-3, 900, 100));
    assert(!gui_slide(5, 0, 100) && !gui_slide(5, 799, 100) && gui_slide(5, 900, 100) == 1);
    assert(gui_slide(5, 1300, 100) == 5 && gui_slide(5, 2000, 100) == 5 && gui_slide(5, 2200, 100) == 4);
    assert(gui_slide(5, 2500, 100) == 1 && !gui_slide(5, 2600, 100) && gui_slide(5, 2600 + 900, 100) == 1);

    /* a row of a wrapped line: whole when it fits, else after its last slash, else its last space, else cut */
    assert(gui_wrap("abc", 5) == 3 && gui_wrap("abcde", 5) == 5 && gui_wrap("", 5) == 0);
    assert(gui_wrap("/home/me/games/Some Long Game Name.ini", 20) == 15); /* "/home/me/games/" */
    assert(gui_wrap("C:\\Users\\me\\Some Game 2", 16) == 12);             /* up to the last backslash */
    assert(gui_wrap("An older version left", 10) == 9);                   /* "An older " */
    assert(gui_wrap("abc/", 4) == 4 && gui_wrap("abc/def", 4) == 4 && gui_wrap("abcdefghij", 4) == 4);
    assert(gui_wrap("ab", 0) == 1 && gui_wrap("ab", -3) == 1);
    assert(gui_wrap("\xc3\xa9\xc3\xa9\xc3\xa9", 2) == 4); /* characters, not bytes */

    /* the head (Add a folder and two recent games) over each page of the games, one ring with the page */
    GuiBrowser browser = {.count = 23, .head = 3};
    gui_browser_resize(&browser, 10);
    assert(gui_browser_pages(&browser) == 3 && gui_browser_items(&browser) == 10);
    assert(gui_browser_key(&browser, GUI_SELECT) == GUI_BROWSER_HEAD && !browser.selected);
    gui_browser_key(&browser, GUI_DOWN);
    gui_browser_key(&browser, GUI_DOWN);
    assert(gui_browser_key(&browser, GUI_SELECT) == GUI_BROWSER_HEAD && browser.selected == 2);
    gui_browser_key(&browser, GUI_DOWN); /* the page's first game */
    assert(browser.selected == 3 && !browser.activated);
    gui_browser_key(&browser, GUI_UP);
    gui_browser_key(&browser, GUI_UP);
    gui_browser_key(&browser, GUI_UP);
    gui_browser_key(&browser, GUI_UP); /* round to the page's last */
    assert(browser.selected == 12 && !browser.activated);
    gui_browser_key(&browser, GUI_RIGHT);
    assert(browser.page == 1 && browser.selected == 12);
    gui_browser_key(&browser, GUI_PAGE_DOWN);
    assert(browser.page == 2 && browser.selected == 5 && gui_browser_items(&browser) == 3);
    gui_browser_key(&browser, GUI_RIGHT); /* no wrapping past the last page */
    assert(browser.page == 2);
    assert(gui_browser_key(&browser, GUI_SELECT) == GUI_BROWSER_OPEN);
    assert(browser.activated && browser.active == 22);
    gui_browser_resize(&browser, 7);
    assert(browser.page * browser.page_size + browser.selected - browser.head == 22);
    gui_browser_key(&browser, GUI_PAGE_UP);
    gui_browser_key(&browser, GUI_LEFT);
    assert(browser.page == 1);
    browser.selected = 1; /* a head row keeps the focus as the page changes */
    gui_browser_key(&browser, GUI_RIGHT);
    gui_browser_resize(&browser, 5);
    assert(browser.selected == 1);
    gui_browser_key(&browser, GUI_DOWN);
    gui_browser_key(&browser, GUI_DOWN);
    assert(browser.selected == 3);
    assert(gui_browser_key(&browser, GUI_CANCEL) == GUI_BROWSER_BACK);
    browser = (GuiBrowser){.head = 1}; /* no games: Add a folder alone */
    gui_browser_resize(&browser, 0);
    gui_browser_key(&browser, GUI_DOWN);
    gui_browser_key(&browser, GUI_PAGE_DOWN);
    assert(browser.page == 0 && browser.selected == 0 && gui_browser_pages(&browser) == 1);
    assert(gui_browser_key(&browser, GUI_SELECT) == GUI_BROWSER_HEAD);
    browser = (GuiBrowser){0};
    assert(gui_browser_key(&browser, GUI_SELECT) == GUI_BROWSER_NONE && !browser.selected);

    GuiConsole screen;
    gui_console_init(&screen, 5, 3);
    gui_console_feed(&screen, "abcdefghijklmnoP", 16);
    assert(!memcmp(screen.cells[0], "fghij", 5));
    assert(!memcmp(screen.cells[1], "klmno", 5));
    assert(screen.cells[2][0] == 'P');
    gui_console_feed(&screen, "\rX\n", 3);
    assert(screen.cells[1][0] == 'X' && screen.col == 0);
    gui_console_init(&screen, 10, 2);
    gui_console_feed(&screen, "\033[3", 3);
    gui_console_feed(&screen, "1mred\033[0m\033]0;title\007!", strlen("1mred\033[0m\033]0;title\007!"));
    assert(!memcmp(screen.cells[0], "red!", 4));
    gui_console_feed(&screen, "\xc3", 1);
    gui_console_feed(&screen, "\xa9", 1);
    assert(screen.cells[0][4] == gui_glyph(0xe9));
    /* the font's symbols under 32 and its house at 127 come from their Unicode, as a theme's cursor or a key's arrow;
       the control bytes stay themselves */
    assert(gui_glyph(0x25ba) == 0x10 && gui_glyph(0x2022) == 0x07 && gui_glyph(0x25ac) == 0x16);
    assert(gui_glyph(0x2191) == 0x18 && gui_glyph(0x2193) == 0x19 && gui_glyph(0x2192) == 0x1a);
    assert(gui_glyph(0x2190) == 0x1b && gui_glyph(0x263a) == 0x01 && gui_glyph(0x25bc) == 0x1f);
    assert(gui_glyph(0x2302) == 0x7f && gui_glyph(0x00a7) == 0x15 && gui_glyph(0x10) == 0x10);
    /* the menu's letters: CP437's as they are, the Latin ones past it from Unifont, the rest '?' */
    assert(gui_menu_glyph('a') == 'a' && gui_menu_glyph(0xe9) == 0x82 && gui_menu_glyph(0xa0) == 0xff);
    assert(gui_menu_glyph(0xe7) == 0x87 && gui_menu_glyph(0x2302) == 0x7f);
    {
        static const uint32_t latin[] = {0xa4, 0xc3, 0xe3, 0xf5, 0xd5, 0x152, 0x219, 0x24f};
        size_t i;
        for (i = 0; i < sizeof latin / sizeof *latin; ++i) {
            int c = gui_menu_glyph(latin[i]);
            assert(c >= 256 && c < 256 + gui_latin_count && gui_latin_points[c - 256] == latin[i]);
        }
        for (i = 1; i < (size_t)gui_latin_count; ++i) assert(gui_latin_points[i - 1] < gui_latin_points[i]);
        assert(gui_latin_count > 300 && gui_latin_points[0] >= 0xa0 && gui_latin_points[gui_latin_count - 1] <= 0x24f);
        for (i = 0; i < (size_t)gui_latin_count; ++i) assert(gui_glyph(gui_latin_points[i]) == '?'); /* CP437's own */
    }
    assert(gui_menu_glyph('?') == '?' && gui_menu_glyph(0x9f) == '?' && gui_menu_glyph(0xad) == '?');
    assert(gui_menu_glyph(0x250) == '?' && gui_menu_glyph(0x2026) == '?' && gui_menu_glyph(0x4e00) == '?');
    assert(gui_menu_glyph(0x1f600) == '?');
    for (int i = 0; i < 1000000; ++i) gui_console_feed(&screen, "z", 1);
    assert(screen.row < screen.rows && screen.col <= screen.cols);
    gui_console_resize(&screen, 3, 1);
    assert(screen.row == 0 && screen.cols == 3 && screen.col <= 3);
    gui_console_resize(&screen, 9999, 9999);
    assert(screen.rows == GUI_MAX_ROWS && screen.cols == GUI_MAX_COLS);

    char* path = gui_drop_path("  \"/tmp/a b;$(not-a-shell).nro\"\r\n", true);
    assert(path && !strcmp(path, "/tmp/a b;$(not-a-shell).nro"));
    free(path);
    path = gui_drop_path("file://localhost/tmp/a%20b.nro", true);
    assert(path && !strcmp(path, "/tmp/a b.nro"));
    free(path);
    path = gui_drop_path(" /a real trailing space ", false);
    assert(path && !strcmp(path, " /a real trailing space "));
    free(path);
    assert(!gui_drop_path("file://remote/tmp/a", true));
    assert(!gui_drop_path("file:///tmp/%00.nro", true));
    assert(!gui_drop_path("file:///tmp/%zz", true));
    assert(!gui_drop_path("one\ntwo", true));
    assert(!gui_drop_path("", true));
    /* what a picker gives: the Android one's documents of the device's storage and its cards, a download by path; no
       other of its documents has a path */
    path =
        gui_document_path("content://com.android.externalstorage.documents/document/primary%3ADownload%2FGame%20X.xci");
    assert(path && !strcmp(path, "/storage/emulated/0/Download/Game X.xci"));
    free(path);
    path = gui_document_path("content://com.android.externalstorage.documents/document/1A2B-3C4D%3Agames%2Fa.nsp");
    assert(path && !strcmp(path, "/storage/1A2B-3C4D/games/a.nsp"));
    free(path);
    path = gui_document_path("content://com.android.providers.downloads.documents/document/"
                             "raw%3A%2Fstorage%2Femulated%2F0%2FDownload%2Fprod.keys");
    assert(path && !strcmp(path, "/storage/emulated/0/Download/prod.keys"));
    free(path);
    assert(!gui_document_path("content://com.android.providers.downloads.documents/document/msf%3A1234"));
    /* what a prod.keys is when it comes with no name: "name = hex" lines, the others let be; never binary */
    {
        static const char keys[] = "header_key = 00112233\nkey_area_key_application_00 = AABBCCDD  \n";
        static const char crlf[] = "; Lockpick\r\n\r\nmaster_key_00 = 0011\r\n";
        static const char nul[] = "header_key = 0011\n\0";
        assert(gui_keys_text(keys, sizeof keys - 1) && gui_keys_text(crlf, sizeof crlf - 1));
        assert(gui_keys_text("header_key=0011", 15));
        assert(!gui_keys_text(nul, sizeof nul - 1) && !gui_keys_text("", 0) && !gui_keys_text(NULL, 0));
        assert(!gui_keys_text("a game, not keys\n", 17) && !gui_keys_text("header_key = 0\n", 15));
        assert(!gui_keys_text("header_key = 00zz\n", 18) && !gui_keys_text(" = 0011\n", 8));
    }
    assert(!gui_document_path("content://com.android.providers.media.documents/document/document%3A12"));
    assert(!gui_document_path("content://com.android.externalstorage.documents/document/primary%3A%zz"));
    assert(!gui_document_path("content://com.android.externalstorage.documents/document/primary%3A%00.xci"));
    assert(!gui_document_path("content://com.android.externalstorage.documents/document/primary"));
    /* a folder its folder picker gave: a tree of the device's storage, its root too, or a card's; Downloads' has none */
    path = gui_document_path("content://com.android.externalstorage.documents/tree/primary%3AGames%2FSwitch");
    assert(path && !strcmp(path, "/storage/emulated/0/Games/Switch"));
    free(path);
    path = gui_document_path("content://com.android.externalstorage.documents/tree/primary%3A");
    assert(path && !strcmp(path, "/storage/emulated/0/"));
    free(path);
    path = gui_document_path("content://com.android.externalstorage.documents/tree/1A2B-3C4D%3Agames");
    assert(path && !strcmp(path, "/storage/1A2B-3C4D/games"));
    free(path);
    assert(!gui_document_path("content://com.android.providers.downloads.documents/tree/downloads"));
    path = gui_document_path("file:///tmp/a%20b.nro");
    assert(path && !strcmp(path, "/tmp/a b.nro"));
    free(path);
    path = gui_document_path("/tmp/ a.nro ");
    assert(path && !strcmp(path, "/tmp/ a.nro "));
    free(path);
    /* the dumps the Android games/ list offers, to prepare first */
    assert(gui_is_dump("a.NRO") && gui_is_dump("b.xci") && gui_is_dump("c.nsp") && gui_is_dump("d.dxci"));
    assert(!gui_is_dump("e_code.so") && !gui_is_dump("nro") && !gui_is_dump("f.nro.ini"));
    path = gui_document_path("C:\\Games\\a.nro"); /* Windows' */
    assert(path && !strcmp(path, "C:\\Games\\a.nro"));
    free(path);

    /* the question before an abort: keys do nothing while it is not shown; the arrows move between its choices, Select
       takes the one in focus and Cancel keeps going, both closing it */
    assert(!gui_confirm_key(&confirm, GUI_SELECT) && !gui_confirm_key(&confirm, GUI_DOWN));
    assert(!confirm.shown && confirm.selected == GUI_CONFIRM_KEEP);
    confirm.shown = true;
    assert(!gui_confirm_key(&confirm, GUI_SELECT) && !confirm.shown); /* Keep going is the first choice */
    confirm.shown = true;
    assert(!gui_confirm_key(&confirm, GUI_DOWN) && confirm.selected == GUI_CONFIRM_ABORT && confirm.shown);
    assert(!gui_confirm_key(&confirm, GUI_PAGE_DOWN) && confirm.selected == GUI_CONFIRM_ABORT && confirm.shown);
    assert(!gui_confirm_key(&confirm, GUI_CANCEL) && !confirm.shown); /* Back again, on Abort too, keeps going */
    confirm.shown = true;
    assert(!gui_confirm_key(&confirm, GUI_UP) && confirm.selected == GUI_CONFIRM_KEEP);
    assert(!gui_confirm_key(&confirm, GUI_RIGHT) && confirm.selected == GUI_CONFIRM_ABORT);
    assert(!gui_confirm_key(&confirm, GUI_LEFT) && confirm.selected == GUI_CONFIRM_KEEP);
    assert(!gui_confirm_key(&confirm, GUI_UP) && confirm.selected == GUI_CONFIRM_ABORT);
    assert(gui_confirm_key(&confirm, GUI_SELECT) && !confirm.shown);
    assert(!gui_confirm_key(&confirm, GUI_SELECT)); /* answered: nothing more to abort */
    /* opened: the first choice in focus, nothing armed; a question asked once takes its second choice at once */
    confirm.selected = GUI_CONFIRM_ABORT;
    confirm.armed = true;
    gui_confirm_open(&confirm, GUI_ASK_EXIT, false);
    assert(confirm.shown && confirm.ask == GUI_ASK_EXIT && confirm.selected == GUI_CONFIRM_KEEP && !confirm.armed);
    assert(!gui_confirm_key(&confirm, GUI_DOWN) && gui_confirm_key(&confirm, GUI_SELECT) && !confirm.shown);
    /* asked twice: the first Select on the second choice arms it, the second takes it */
    gui_confirm_open(&confirm, GUI_ASK_DEEP, true);
    assert(!gui_confirm_key(&confirm, GUI_SELECT) && !confirm.shown); /* the first choice at one press */
    gui_confirm_open(&confirm, GUI_ASK_DEEP, true);
    assert(!gui_confirm_key(&confirm, GUI_DOWN) && !gui_confirm_key(&confirm, GUI_SELECT));
    assert(confirm.shown && confirm.armed);
    assert(gui_confirm_key(&confirm, GUI_SELECT) && !confirm.shown && confirm.ask == GUI_ASK_DEEP);
    /* a move between choices, by key or pointer, undoes the first press; the pointer staying on it does not */
    gui_confirm_open(&confirm, GUI_ASK_DEEP, true);
    assert(!gui_confirm_key(&confirm, GUI_RIGHT) && !gui_confirm_key(&confirm, GUI_SELECT) && confirm.armed);
    assert(!gui_confirm_key(&confirm, GUI_LEFT) && !confirm.armed && confirm.selected == GUI_CONFIRM_KEEP);
    assert(!gui_confirm_key(&confirm, GUI_RIGHT) && !gui_confirm_key(&confirm, GUI_SELECT) && confirm.armed);
    gui_confirm_focus(&confirm, GUI_CONFIRM_ABORT);
    assert(confirm.armed);
    gui_confirm_focus(&confirm, GUI_CONFIRM_KEEP);
    assert(!confirm.armed && confirm.shown);
    gui_confirm_focus(&confirm, GUI_CONFIRM_ABORT);
    assert(!gui_confirm_key(&confirm, GUI_SELECT) && confirm.armed);
    assert(!gui_confirm_key(&confirm, GUI_CANCEL) && !confirm.shown); /* Back while armed takes nothing */
    puts("GUI menu, bounded console, UTF-8 and file drops passed");
    return 0;
}
