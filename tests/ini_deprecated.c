/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The .ini's deprecated keys (src/nx/runtime/ini.c, docs/DEPRECATIONS.md): which keys they are and what became of each,
   the [Deprecated] section as the runtime writes it (the launcher's Clean Old Settings reads it), and the line a run
   says about them */
#include "nx/runtime/ini.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    static const char expected[] =
        "\n[Deprecated]\n; keys an older version used, which do nothing now (docs/DEPRECATIONS.md): delete them, or use "
        "Settings > App >\n;   Clean Old Settings in the launcher\n; renderer: Vulkan is the only renderer\n"
        "renderer=opengl\n; touch_controls: now controls\ntouch_controls=1\n";
    const char* keys[] = {"renderer", "touch_controls"};
    char text[1024], small[40];
    size_t n;
    FILE* f;
    /* the keys: the OpenGL renderer's, the touch controls' old names, the first Spanish ones; a setting is none */
    assert(!strcmp(ini_deprecated("renderer"), "Vulkan is the only renderer"));
    assert(!strcmp(ini_deprecated("lsfg_flow"), "LSFG went with the OpenGL renderer"));
    assert(!strcmp(ini_deprecated("touch_controls"), "now controls") &&
           !strcmp(ini_deprecated("modo_tv"), "now docked"));
    assert(!strcmp(ini_deprecated("user"), "now user_uid, the uid of the user it named") &&
           !ini_deprecated("user_uid"));
    assert(!ini_deprecated("resolution") && !ini_deprecated("controls") && !ini_deprecated(""));
    /* the section, word for word */
    f = tmpfile();
    assert(f);
    ini_deprecated_head(f);
    ini_deprecated_key(f, "renderer", "opengl");
    ini_deprecated_key(f, "touch_controls", "1");
    rewind(f);
    n = fread(text, 1, sizeof text - 1, f);
    text[n] = 0;
    fclose(f);
    assert(!strcmp(text, expected));
    /* the line: none says nothing; one and more, named; cut to the room it has */
    assert(!ini_deprecated_note(text, sizeof text, "Game.ini", keys, 0));
    assert(ini_deprecated_note(text, sizeof text, "Game.ini", keys, 1));
    assert(!strcmp(text, "settings: Game.ini keeps 1 key this version does not use (renderer), under [Deprecated]: "
                         "delete it, or use Settings > App > Clean Old Settings in the launcher"));
    assert(ini_deprecated_note(text, sizeof text, "Game.ini", keys, 2) &&
           strstr(text, "keeps 2 keys this version does not use (renderer, touch_controls)") &&
           strstr(text, "delete them"));
    assert(ini_deprecated_note(small, sizeof small, "Game.ini", keys, 2) && strlen(small) == sizeof small - 1);
    puts("ini_deprecated passed");
    return 0;
}
