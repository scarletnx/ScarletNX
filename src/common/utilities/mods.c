/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "common/utilities/mods.h"
#include "common/utilities/game_ini.h"
#include "common/utilities/patches.h"
#include <stdlib.h>
#include <string.h>

char* snx_mods_folder(const char* data) { return snx_path_join(data, "mods"); }
char* snx_game_mods(const char* folder, const char* name, char** off) {
    char *base = snx_path_join(folder, name), *data = snx_format("%s_data", base), *ini = snx_game_ini(folder, name);
    char* mods = snx_mods_folder(data);
    *off = snx_ini_value(ini, "mods_off");
    free(ini);
    free(data);
    free(base);
    return mods;
}
static int by_name(const void* a, const void* b) { return strcmp(*(char* const*)a, *(char* const*)b); }
size_t snx_mods_list(const char* folder, char*** names) {
    char** list = NULL;
    size_t count = 0, n = 0, i;
    *names = NULL;
    if (snx_list_dir(folder, &list, &count)) return 0;
    for (i = 0; i < count; ++i) {
        char* path = snx_path_join(folder, list[i]);
        if (list[i][0] != '.' && snx_is_dir(path) && !snx_is_link(path))
            list[n++] = list[i];
        else
            free(list[i]);
        free(path);
    }
    if (!n) {
        free(list);
        return 0;
    }
    qsort(list, n, sizeof *list, by_name);
    *names = list;
    return n;
}
int snx_mod_off(const char* off, const char* name) {
    size_t n = strlen(name);
    const char* at = off;
    while (at && *at) {
        const char* end = strchr(at, SNX_MODS_SEPARATOR);
        size_t length = end ? (size_t)(end - at) : strlen(at);
        if (length == n && !strncmp(at, name, n)) return 1;
        at = end ? end + 1 : NULL;
    }
    return 0;
}
int snx_mods_exefs_file(const char* name) {
    const char* digit;
    if (!strcmp(name, "rtld") || !strcmp(name, "main") || !strcmp(name, "sdk") || !strcmp(name, "main.npdm")) return 1;
    if (strncmp(name, "subsdk", 6) || !name[6]) return 0;
    for (digit = name + 6; *digit >= '0' && *digit <= '9'; ++digit) continue;
    return !*digit;
}
size_t snx_mods_played(const char* folder, const char* off, const char* part, char*** paths) {
    char** names;
    size_t count = snx_mods_list(folder, &names), n = 0, i;
    *paths = count ? (char**)snx_alloc(count * sizeof **paths) : NULL;
    for (i = 0; i < count; ++i) {
        char *mod = snx_path_join(folder, names[i]), *path = snx_path_join(mod, part);
        if (!snx_mod_off(off, names[i]) && snx_is_dir(path) && !snx_is_link(path))
            (*paths)[n++] = path;
        else
            free(path);
        free(mod);
    }
    snx_free_strings(names, count);
    if (!n) {
        free(*paths);
        *paths = NULL;
    }
    return n;
}
size_t snx_mods_exefs(const char* folder, const char* off, char*** names, char*** paths) {
    char** exefs;
    size_t count = snx_mods_played(folder, off, "exefs", &exefs), n = 0, i, j, k;
    *names = *paths = NULL;
    for (i = 0; i < count; ++i) {
        char** files = NULL;
        size_t found = 0;
        if (snx_list_dir(exefs[i], &files, &found)) continue;
        for (j = 0; j < found; ++j) {
            char* path = snx_path_join(exefs[i], files[j]);
            if (!snx_mods_exefs_file(files[j]) || !snx_is_file(path)) {
                free(path);
                continue;
            }
            for (k = 0; k < n && strcmp((*names)[k], files[j]); ++k) continue;
            if (k < n) { /* a mod before this one has it */
                free((*paths)[k]);
                (*paths)[k] = path;
                continue;
            }
            *names = (char**)snx_realloc(*names, (n + 1) * sizeof **names);
            *paths = (char**)snx_realloc(*paths, (n + 1) * sizeof **paths);
            (*names)[n] = snx_strdup(files[j]);
            (*paths)[n++] = path;
        }
        snx_free_strings(files, found);
    }
    snx_free_strings(exefs, count);
    for (i = 1; i < n; ++i) /* by name: a handful of files */
        for (j = i; j > 0 && strcmp((*names)[j - 1], (*names)[j]) > 0; --j) {
            char *name = (*names)[j], *path = (*paths)[j];
            (*names)[j] = (*names)[j - 1];
            (*paths)[j] = (*paths)[j - 1];
            (*names)[j - 1] = name;
            (*paths)[j - 1] = path;
        }
    return n;
}
/* FNV-1a, 64 bits */
static SnxU64 hash(SnxU64 h, const void* data, size_t size) {
    const unsigned char* p = (const unsigned char*)data;
    size_t i;
    for (i = 0; i < size; ++i) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}
/* a file of the stamp, by its name and its bytes */
static SnxU64 hash_file(SnxU64 h, const char* name, const char* path) {
    size_t size = 0, k;
    unsigned char* data = snx_read_file(path, &size);
    unsigned char size_bytes[8];
    for (k = 0; k < 8; ++k) size_bytes[k] = (unsigned char)((SnxU64)size >> (8 * k));
    h = hash(h, name, strlen(name) + 1);
    h = hash(h, size_bytes, sizeof size_bytes);
    if (data) h = hash(h, data, size);
    free(data);
    return h;
}
char* snx_mods_stamp(const char* folder, const char* off) {
    char **names, **paths, **patches;
    size_t count = snx_mods_exefs(folder, off, &names, &paths), npatches = snx_mods_patches(folder, off, &patches), i;
    SnxU64 h = 0xcbf29ce484222325ull;
    char* stamp;
    for (i = 0; i < count; ++i) h = hash_file(h, names[i], paths[i]);
    for (i = 0; i < npatches; ++i) { /* after the files, so a stamp with none of them is the one of before */
        const char* name = patches[i] + strlen(patches[i]);
        while (name > patches[i] && name[-1] != '/' && name[-1] != '\\') --name;
        h = hash_file(h, name, patches[i]);
    }
    stamp = count || npatches ? snx_format("%016llx", (unsigned long long)h) : snx_strdup("");
    snx_free_strings(names, count);
    snx_free_strings(paths, count);
    snx_free_strings(patches, npatches);
    return stamp;
}
char* snx_game_stamp_for(unsigned abi, const char* mods) {
    return mods[0] ? snx_format("%u mods %s", abi, mods) : snx_format("%u", abi);
}
char* snx_game_mods_stamp(const char* base) {
    char *folder = snx_dirname(base), *off = NULL, *mods, *made;
    const char* game = folder + strlen(folder);
    while (game > folder && game[-1] != '/' && game[-1] != '\\') --game; /* the game's folder is named as it is */
    mods = snx_game_mods(folder, game, &off);
    made = snx_mods_stamp(mods, off);
    free(mods);
    free(off);
    free(folder);
    return made;
}
int snx_game_stamp_is(const char* base, const char* expected) {
    char* stamp = snx_format("%s.abi", base);
    size_t size = 0;
    unsigned char* found = snx_read_file(stamp, &size);
    int same = found && size == strlen(expected) && !memcmp(found, expected, size);
    free(found);
    free(stamp);
    return same;
}
