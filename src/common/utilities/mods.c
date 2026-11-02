/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "common/utilities/mods.h"
#include "common/utilities/game_ini.h"
#include "common/utilities/patches.h"
#include "common/utilities/ascii.h"
#include "common/utilities/bits.h"
#include "common/utilities/endian.h"
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
size_t snx_mods_list(const char* folder, char*** names) {
    char** list = NULL;
    size_t count = 0, n = 0;
    *names = NULL;
    if (snx_list_dir(folder, &list, &count)) return 0;
    for (size_t i = 0; i < count; ++i) {
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
    qsort(list, n, sizeof *list, snx_compare_strings);
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
    size_t count = snx_mods_list(folder, &names), n = 0;
    *paths = count ? (char**)snx_alloc(count * sizeof **paths) : NULL;
    for (size_t i = 0; i < count; ++i) {
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
    size_t count = snx_mods_played(folder, off, "exefs", &exefs), n = 0, k;
    *names = *paths = NULL;
    for (size_t i = 0; i < count; ++i) {
        char** files = NULL;
        size_t found = 0;
        if (snx_list_dir(exefs[i], &files, &found)) continue;
        for (size_t j = 0; j < found; ++j) {
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
    for (size_t i = 1; i < n; ++i) /* by name: a handful of files */
        for (size_t j = i; j > 0 && strcmp((*names)[j - 1], (*names)[j]) > 0; --j) {
            char *name = (*names)[j], *path = (*paths)[j];
            (*names)[j] = (*names)[j - 1];
            (*paths)[j] = (*paths)[j - 1];
            (*names)[j - 1] = name;
            (*paths)[j - 1] = path;
        }
    return n;
}
/* a file of the stamp, by its name and its bytes */
static SnxU64 hash_file(SnxU64 h, const char* name, const char* path) {
    size_t size = 0;
    unsigned char* data = snx_read_file(path, &size);
    unsigned char size_bytes[8];
    snx_st_le64(size_bytes, (SnxU64)size);
    h = snx_fnv1a(h, name, strlen(name) + 1);
    h = snx_fnv1a(h, size_bytes, sizeof size_bytes);
    if (data) h = snx_fnv1a(h, data, size);
    free(data);
    return h;
}
char* snx_mods_stamp(const char* folder, const char* off) {
    char **names, **paths, **patches;
    size_t count = snx_mods_exefs(folder, off, &names, &paths), npatches = snx_mods_patches(folder, off, &patches);
    SnxU64 h = SNX_FNV_BASIS;
    char* stamp;
    for (size_t i = 0; i < count; ++i) h = hash_file(h, names[i], paths[i]);
    for (size_t i = 0; i < npatches; ++i) { /* after the files, so a stamp with none of them is the one of before */
        const char* name = patches[i] + strlen(patches[i]);
        while (name > patches[i] && !snx_is_any_separator(name[-1])) --name;
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
    char *folder = snx_dirname(base), *off = NULL;
    const char* game = folder + strlen(folder);
    while (game > folder && game[-1] != '/' && game[-1] != '\\') --game; /* the game's folder is named as it is */
    char* mods = snx_game_mods(folder, game, &off);
    char* made = snx_mods_stamp(mods, off);
    free(mods);
    free(off);
    free(folder);
    return made;
}
int snx_game_stamp_is(const char* base, const char* expected) {
    char* stamp = snx_format("%s.abi", base);
    size_t size = 0;
    unsigned char* found = snx_read_file(stamp, &size);
    int same = found && (size = snx_game_stamp_size((const char*)found, size)) == strlen(expected) &&
               !memcmp(found, expected, size);
    free(found);
    free(stamp);
    return same;
}
char* snx_game_made_line(const SnxGameMade* made) {
    return snx_format("\nmade opt=%c aot=%d found=%llu code=%llu%s\n", made->opt ? made->opt : '?', made->aot,
                      made->found, made->code, made->switch2 ? " switch2" : "");
}
size_t snx_game_stamp_size(const char* text, size_t size) {
    for (size_t i = size; i-- > 0;)
        if (text[i] == '\n' && size - i > 6 && !memcmp(text + i + 1, "made ", 5)) return i;
    return size;
}
int snx_game_made_read(const char* base, SnxGameMade* made) {
    char* stamp = snx_format("%s.abi", base);
    size_t size = 0;
    unsigned char* text = snx_read_file(stamp, &size);
    SnxGameMade got = {0, -1, 0, 0, 0};
    char opt = 0;
    int n = 0;
    *made = got;
    free(stamp);
    if (!text) return 0;
    size_t at = snx_game_stamp_size((const char*)text, size);
    int ok = at < size &&
         sscanf((const char*)text + at, "\nmade opt=%c aot=%d found=%llu code=%llu%n", &opt, &got.aot, &got.found,
                &got.code, &n) == 4 &&
         n > 0 && opt && strchr("0123sz", opt) && (got.aot == 0 || got.aot == 1) && got.found <= got.code;
    if (ok) got.switch2 = !strncmp((const char*)text + at + n, " switch2", 8);
    free(text);
    if (!ok) return 0;
    got.opt = opt;
    *made = got;
    return 1;
}
