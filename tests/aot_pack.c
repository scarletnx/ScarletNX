/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The launcher's AOT pack action (src/gui/aot.c): where the runtime left the JIT cache, what its header says, and
   the rule that the game's AOT optimization and the cache's must match. */
#include "gui/aot.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "common/cpu/recompiler/jit/jit_cache.h"
#include "common/utilities/common.h"

static void put32(unsigned char* p, uint32_t v) {
    p[0] = (unsigned char)(v & 0xff);
    p[1] = (unsigned char)((v >> 8) & 0xff);
    p[2] = (unsigned char)((v >> 16) & 0xff);
    p[3] = (unsigned char)((v >> 24) & 0xff);
}
/* gui_aot_cache_path resolves the folder first (realpath on Unix, the final path on Windows), so expect that path */
static char* expected_cache_path(const char* dir, const char* name) {
    char* absolute = snx_absolute(dir);
    char* path = absolute ? snx_format("%s/%s.jitcache", absolute, name) : NULL;
    snx_path_native(path); /* the system's separators, as the launcher's path has */
    free(absolute);
    return path;
}
static void write_cache(const char* path, const char* magic, const char* key) {
    unsigned char head[12];
    size_t n = strlen(key);
    FILE* f;
    assert(n < 8192);
    memcpy(head, magic, 8);
    put32(head + 8, (uint32_t)n);
    f = snx_fopen(path, "wb");
    assert(f);
    assert(fwrite(head, 1, sizeof head, f) == sizeof head);
    assert(fwrite(key, 1, n, f) == n);
    assert(!fclose(f));
}

int main(void) {
    char* root = snx_temp_dir(NULL);
    char* folder;
    char* cache_dir;
    char* folders;
    char* recorded;
    char* path;
    char* expected;
    GuiAotInfo info;
    const char* key = "abi=13 llvm=x rtlib=0 cpu=c opt=2 hook=1 svc=1 features=";
    size_t key_size = strlen(key);

    assert(root);
    folder = snx_format("%s/games/010069401ADB8000", root);
    cache_dir = snx_format("%s/cache/010069401ADB8000", root);
    folders = snx_format("%s/010069401ADB8000.folders", folder);
    assert(folder && cache_dir && folders);
    assert(!snx_mkdirs(folder));
    assert(!snx_mkdirs(cache_dir));

    /* the runtime's .folders cache= wins: that is where its game_dirs put the cache */
    recorded = snx_format("cache=%s\n", cache_dir);
    assert(recorded && !snx_write_file(folders, recorded, strlen(recorded)));
    path = gui_aot_cache_path(folder, "010069401ADB8000");
    expected = expected_cache_path(cache_dir, "010069401ADB8000");
    assert(path && expected && !strcmp(path, expected));

    /* a readable header gives the cache's level and hook, not the whole cache */
    write_cache(path, SNX_JIT_CACHE_MAGIC, key);
    assert(gui_aot_cache_info(path, &info));
    assert(info.opt == '2');
    assert(info.hook == 1);
    assert(info.size == 12 + (uint64_t)key_size);

    /* anything but the cache's own header is refused */
    write_cache(path, "BADMAGIC", key);
    assert(!gui_aot_cache_info(path, &info));
    /* and a key that ends at its level, with none */
    write_cache(path, SNX_JIT_CACHE_MAGIC, "abi=13 llvm=x rtlib=0 cpu=c hook=1 opt=");
    assert(!gui_aot_cache_info(path, &info));

    /* without .folders the runtime's user cache path is used, as kind_dir does */
    assert(!remove(folders));
    free(path);
    free(expected);
    {
        char* moved = snx_user_path(folder, SNX_USER_CACHE);
        char* dir = moved && strcmp(moved, folder) && snx_is_dir(moved) ? moved : folder;
        expected = expected_cache_path(dir, "010069401ADB8000");
        free(moved);
    }
    path = gui_aot_cache_path(folder, "010069401ADB8000");
    assert(path && expected && !strcmp(path, expected));

    /* a portable data folder: snx_user_path moves games/ under .scarletnx/cache/games/ */
    {
        char* data = snx_format("%s/portable/.scarletnx", root);
        char* game = snx_format("%s/games/0100", data);
        char* cache = snx_format("%s/cache/games/0100", data);
        assert(data && game && cache);
        assert(!snx_mkdirs(game));
        assert(!snx_mkdirs(cache));
        free(path);
        free(expected);
        path = gui_aot_cache_path(game, "0100");
        expected = expected_cache_path(cache, "0100");
        assert(path && expected && !strcmp(path, expected));
        free(cache);
        free(game);
        free(data);
    }

    /* the game's level is the launcher's opt setting; no setting means its default, 2 */
    assert(gui_aot_levels_match("2", '2'));
    assert(!gui_aot_levels_match("2", '0'));
    assert(gui_aot_levels_match("z", 'z'));
    assert(gui_aot_levels_match(NULL, '2'));
    assert(!gui_aot_levels_match("", '0'));

    free(expected);
    free(path);
    free(recorded);
    free(folders);
    free(cache_dir);
    free(folder);
    assert(!snx_remove_tree(root));
    free(root);
    return 0;
}
