/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The console a game's folder plays (common/utilities/game_ini.c), which the launcher picks the driver by: system= of
   its .ini, recorded and read back; an older version's folder by its layout (a 3DS one's .ini with model=, a Vita one
   with ux0/); the source= a driver wrote in the game's folder; and the game's copy of the runtime, placed when missing
   or another build's */
#define _POSIX_C_SOURCE 200809L /* setenv */
#include "common/utilities/game_ini.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void set_env(const char* name, const char* value) {
#ifdef _WIN32
    assert(!_putenv_s(name, value));
#else
    assert(!setenv(name, value, 1));
#endif
}
static char* folder_of(const char* root, const char* name) {
    char* folder = snx_path_join(root, name);
    assert(folder && !snx_mkdirs(folder));
    return folder;
}

int main(void) {
    char *root = snx_temp_dir(NULL), *folder, *ini, *word, *runtime, *copy, error[256];
    unsigned char* bytes;
    size_t size;
    assert(root);
    /* a folder that says nothing is a Switch game's, as every game an older version made */
    folder = folder_of(root, "plain");
    assert(snx_game_system(folder, "plain") == SNX_SYSTEM_SWITCH);
    free(folder);
    /* system= recorded in [Game] of the .ini in the folder, where cmake/deploy.cmake reads it too, and read back */
    folder = folder_of(root, "agb");
    assert(!snx_record_system(folder, "agb", SNX_SYSTEM_GBA));
    ini = snx_format("%s/agb.ini", folder);
    word = snx_ini_value(ini, "system");
    assert(word && !strcmp(word, "gba") && !strcmp(snx_system_word(SNX_SYSTEM_GBA), "gba"));
    assert(snx_game_system(folder, "agb") == SNX_SYSTEM_GBA);
    assert(!snx_record_system(folder, "agb", SNX_SYSTEM_NDS)); /* one system=, the last */
    assert(snx_game_system(folder, "agb") == SNX_SYSTEM_NDS && !strcmp(snx_system_word(SNX_SYSTEM_NDS), "nds"));
    assert(!snx_record_system(folder, "agb", SNX_SYSTEM_VITA));
    assert(snx_game_system(folder, "agb") == SNX_SYSTEM_VITA);
    free(word);
    free(ini);
    free(folder);
    /* an older 3DS game's .ini, in its folder, names its console (model=) and no system= */
    folder = folder_of(root, "0004000000030700");
    ini = snx_format("%s/0004000000030700.ini", folder);
    assert(!snx_write_file(ini, "source=/x/game.cia\nmodel=new\n", 29));
    assert(snx_game_system(folder, "0004000000030700") == SNX_SYSTEM_3DS);
    assert(!snx_write_file(ini, "source=/x/game.cia\nmodel=new\nsystem=3ds\n", 40));
    assert(snx_game_system(folder, "0004000000030700") == SNX_SYSTEM_3DS);
    free(ini);
    free(folder);
    /* an older Vita game's folder holds its memory card */
    folder = folder_of(root, "PCSE00000");
    ini = snx_path_join(folder, "ux0");
    assert(!snx_mkdirs(ini));
    assert(snx_game_system(folder, "PCSE00000") == SNX_SYSTEM_VITA);
    free(ini);
    free(folder);
    /* a word it does not know is no console's but the Switch's */
    folder = folder_of(root, "odd");
    ini = snx_format("%s/odd.ini", folder);
    assert(!snx_write_file(ini, "system=odd\n", 11));
    assert(snx_game_system(folder, "odd") == SNX_SYSTEM_SWITCH);
    free(ini);
    free(folder);
    /* the copy of the runtime: placed, kept while it is the same build, placed again for another */
    runtime = snx_path_join(root, "runtime");
    copy = snx_path_join(root, "agb/agb");
    assert(!snx_write_file(runtime, "build one", 9));
    assert(snx_place_runtime(runtime, copy, error, sizeof error) == 1);
    assert((bytes = snx_read_file(copy, &size)) && size == 9 && !memcmp(bytes, "build one", 9));
    free(bytes);
    assert(snx_place_runtime(runtime, copy, error, sizeof error) == 0);
    assert(!snx_write_file(runtime, "build two!", 10));
    assert(snx_place_runtime(runtime, copy, error, sizeof error) == 1);
    assert((bytes = snx_read_file(copy, &size)) && size == 10);
    free(bytes);
    assert(snx_place_runtime(copy, copy, error, sizeof error) == 0); /* the copy itself */
    free(copy);
    copy = snx_path_join(root, "no such folder/agb");
    assert(snx_place_runtime(runtime, copy, error, sizeof error) == -1 && strstr(error, copy));
    free(copy);
    free(runtime);
    { /* a game of the user's data folder, whose .ini the launcher keeps in the config folder (snx_user_path): the
         source= a console's driver wrote in the game's own folder is its source all the same, which Recent maps a
         ROM's row to the game by */
#ifdef _WIN32
        static const char* const vars[] = {"APPDATA"};
#elif defined(SNX_XDG)
        static const char* const vars[] = {"XDG_DATA_HOME", "XDG_CONFIG_HOME", "XDG_STATE_HOME", "XDG_CACHE_HOME"};
#else
        static const char* const vars[] = {"HOME"};
#endif
        static const char text[] = "[Game]\nsource=/roms/G.gba\nsystem=gba\n";
        char *data, *made, *own, *launcher, *source;
        int recorded = 0;
        size_t i;
        for (i = 0; i < sizeof vars / sizeof *vars; ++i) {
            char* value = snx_format("%s/user %d", root, (int)i);
            set_env(vars[i], value);
            free(value);
        }
        data = snx_user_data_dir();
        assert(data);
        made = snx_format("%s/recompiled/G", data);
        own = snx_format("%s/G.ini", made);
        launcher = snx_game_ini(made, "G");
        assert(!snx_mkdirs(made) && strcmp(own, launcher)); /* two .ini: the launcher's is elsewhere */
        assert(!snx_write_file(own, text, sizeof text - 1));
        source = snx_game_source(made, "G", &recorded);
        assert(source && !strcmp(source, "/roms/G.gba") && recorded);
        free(source);
        free(launcher);
        free(own);
        free(made);
        free(data);
    }
    assert(!snx_remove_tree(root));
    free(root);
    puts("game_system: ok");
    return 0;
}
