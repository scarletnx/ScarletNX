/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* gui/covers.c: a ROM's CRC-32, the name a No-Intro list in clrmamepro's format gives it (a game's first name, not the
   list's own nor its rom's; a crc of eight digits exactly; CRLF lines), none for a CRC no game has or a name past the
   room, and the link of a name's box as libretro-thumbnails names its files */
#include "gui/covers.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static const char list[] = "clrmamepro (\n"
                           "\tname \"Test console - Test list\"\n"
                           "\tversion \"2026.01.01\"\n"
                           ")\n"
                           "\n"
                           "game (\n"
                           "\tname \"Test Cartridge: One & Two (World)\"\n"
                           "\tregion \"World\"\n"
                           "\trom ( name \"Test Cartridge One.gba\" size 9 crc 1234ABCD md5 00 sha1 00 )\n"
                           ")\n"
                           "game (\n"
                           "\tname \"No Rom Line (World)\"\n"
                           ")\n"
                           "game (\r\n"
                           "\tname \"Second Test (Europe) (En,Fr)\"\r\n"
                           "\trom ( name \"Other Name.gba\" size 9 crc cbf43926 md5 00 )\r\n"
                           ")\r\n"
                           "game (\n"
                           "\tname \"Longer Digits (World)\"\n"
                           "\trom ( name \"x.gba\" size 9 crc 0BADF00D1 )\n"
                           ")\n";

int main(void) {
    char name[64], url[400], small[8];
    /* the CRC-32 of the usual check string */
    assert(gui_cover_crc("123456789", 9) == 0xCBF43926u && gui_cover_crc("", 0) == 0);
    /* the game's name, not its rom's nor the list's; lower-case digits and CRLF lines read as well */
    assert(gui_cover_name(list, sizeof list - 1, 0x1234ABCDu, name, sizeof name) &&
           !strcmp(name, "Test Cartridge: One & Two (World)"));
    assert(gui_cover_name(list, sizeof list - 1, 0xCBF43926u, name, sizeof name) &&
           !strcmp(name, "Second Test (Europe) (En,Fr)"));
    /* none: a CRC no game has, one whose crc field has more digits, a name past the room */
    assert(!gui_cover_name(list, sizeof list - 1, 0x0BADF00Du, name, sizeof name) && !name[0]);
    assert(!gui_cover_name(list, sizeof list - 1, 0xDEADBEEFu, name, sizeof name) && !name[0]);
    assert(!gui_cover_name(list, sizeof list - 1, 0x1234ABCDu, small, sizeof small) && !small[0]);
    assert(!gui_cover_name(list, 0, 0x1234ABCDu, name, sizeof name) && !name[0]);
    /* the box's link: each of &*\/:`<>?\| an _, the rest percent-encoded but letters, digits and -._~ */
    assert(gui_cover_url("Test Cartridge: One & Two (World)", url, sizeof url) &&
           !strcmp(url, GUI_COVERS_BOX_URL "Test%20Cartridge_%20One%20_%20Two%20%28World%29.png"));
    assert(gui_cover_url("a*b/c`d<e>f?g\\h|i~j-k.l", url, sizeof url) &&
           !strcmp(url, GUI_COVERS_BOX_URL "a_b_c_d_e_f_g_h_i~j-k.l.png"));
    assert(gui_cover_url("Caf\xc3\xa9, 100%!", url, sizeof url) &&
           !strcmp(url, GUI_COVERS_BOX_URL "Caf%C3%A9%2C%20100%25%21.png"));
    /* a link past the room: none, rather than one cut */
    assert(!gui_cover_url("Test Cartridge", url, strlen(GUI_COVERS_BOX_URL) + 10) && !url[0]);
    assert(gui_cover_url("Test", url, strlen(GUI_COVERS_BOX_URL) + 9) && !strcmp(url, GUI_COVERS_BOX_URL "Test.png"));
    assert(!gui_cover_url("Test", url, strlen(GUI_COVERS_BOX_URL) + 8) && !url[0]);
    assert(gui_cover_png("\x89PNG\r\n\x1a\n\0\0", 10) && !gui_cover_png("\x89PNG\r\n\x1a", 7) &&
           !gui_cover_png("<html>404</html>", 16));
    puts("gui_covers: ok");
    return 0;
}
