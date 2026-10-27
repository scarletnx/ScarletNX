/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The MCP server's game tools (mcp_int.h): what this machine has made, how to build one, and an unattended run with
   the update, DLCs, mods and cheats the caller picks for that run. The game's .ini is never rewritten: every choice
   is an argument, and the run passes the runtime the same choices the recompile was stamped with. Recompiling is its
   own call (game_recompile), as a run of a game that is not built or is stamped for other mods says. */
#include "nx/runtime/driver/mcp_int.h"
#include "common/utilities/build_info.h"
#include "common/utilities/cheat_list.h"
#include "common/utilities/common.h"
#include "common/utilities/game_ini.h"
#include "common/utilities/mods.h"
#include "nx/loader/loader.h"
#include "nx/runtime/driver/container.h"
#include "nx/runtime/driver/game_name.h"
#include "nx/runtime/driver/driver.h"
#include "nx/runtime/driver/hactool/cJSON.h"
#include "nx/runtime/driver/recompile.h"
#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define GAME_RUNS_MAX     20
#define GAME_OUTPUT_LINES 240

#ifdef _WIN32
#define GAME_PROGRAM_SUFFIX ".exe"
#define GAME_CODE_SUFFIX    "_code.dll"
#elif defined(SNX_IOS)
#define GAME_PROGRAM_SUFFIX ""
#define GAME_CODE_SUFFIX    "_code.snxobj"
#else
#define GAME_PROGRAM_SUFFIX ""
#define GAME_CODE_SUFFIX    "_code.so"
#endif

/* ============================================================ paths and names */
static int game_separator(char c) { return c == '/' || c == '\\'; }
static const char* game_leaf(const char* path) {
    const char* leaf = path;
    const char* p;
    for (p = path; *p; ++p)
        if (game_separator(*p)) leaf = p + 1;
    return leaf;
}
static int game_ends_with(const char* text, const char* end) {
    size_t n = strlen(text), m = strlen(end);
    return n >= m && !strcmp(text + n - m, end);
}
static void game_copy(char* out, size_t size, const char* text) {
    size_t n = strlen(text);
    if (n >= size) n = size - 1;
    memcpy(out, text, n);
    out[n] = 0;
}
/* a path without its extension, owned */
static char* game_without_suffix(const char* path, const char* suffix) {
    size_t n = strlen(path), m = strlen(suffix);
    char* out;
    if (n < m || strcmp(path + n - m, suffix)) return snx_strdup(path);
    out = (char*)snx_alloc(n - m + 1);
    memcpy(out, path, n - m);
    out[n - m] = 0;
    return out;
}
/* sixteen hexadecimal digits, any case, into id */
static int game_title_parse(const char* text, SnxU64* id) {
    char word[17];
    size_t i;
    if (!text || strlen(text) != 16) return 0;
    for (i = 0; i < 16; ++i) {
        if (!isxdigit((unsigned char)text[i])) return 0;
        word[i] = (char)toupper((unsigned char)text[i]);
    }
    word[16] = 0;
    {
        SnxU64 value;
        return snx_title_parse(word, id ? id : &value);
    }
}
static void game_title_text(SnxU64 id, char out[17]) { snx_title_text(id, out); }
static char* game_nacp_version(const char* data) {
    char *path = snx_path_join(data, "control.nacp"), version[64];
    unsigned char* nacp;
    size_t size = 0;
    if (!path) return NULL;
    nacp = snx_read_file(path, &size);
    free(path);
    if (!nacp) return NULL;
    if (!snx_nacp_version(nacp, size, version, sizeof version)) version[0] = 0;
    free(nacp);
    return version[0] ? snx_strdup(version) : NULL;
}

/* the folder the game keeps files of kind in: its <name>.folders record's when that is there (what the runtime's
   game_dirs reads), else snx_user_path, else beside the game */
static char* game_kind_dir(const char* folder, const char* name, const char* key, int kind) {
    char* record = snx_format("%s/%s.folders", folder, name);
    char* picked = record ? snx_ini_value(record, key) : NULL;
    if (picked && snx_is_dir(picked)) {
        free(record);
        return picked;
    }
    free(picked);
    free(record);
    return snx_user_path(folder, kind);
}
/* A game as this machine made it: its folder and the program and data folder there. A game extracted in work/ has
   no program: its data is the extraction itself, and the recompiled game's data is <folder>/<name>_data */
typedef struct {
    char* folder;
    char* root; /* the data folder or its cache the game is in: what these tools rewrite under */
    char* name;
    char* data;
    char* program; /* base game's program, <folder>/<name> */
    char* addon;   /* the update program the caller named, its entry, or NULL */
    char* ini;
    char* log;
    SnxU64 id;
    int has_id;
    int has_data; /* <folder>/<name>_data is a folder */
    int built;    /* the base program is a file */
} Game;
static char* game_mods_folder(const Game* g);
static void game_close(Game* g) {
    free(g->folder);
    free(g->root);
    free(g->name);
    free(g->data);
    free(g->program);
    free(g->addon);
    free(g->ini);
    free(g->log);
    memset(g, 0, sizeof *g);
}
static char* game_program_path(const char* folder, const char* name) {
    char* base = snx_path_join(folder, name);
    char* out = snx_format("%s%s", base, GAME_PROGRAM_SUFFIX);
    free(base);
    return out;
}
static char* game_code_path(const char* base) { return snx_format("%s%s", base, GAME_CODE_SUFFIX); }
static char* game_program_base(const char* program) { return game_without_suffix(program, GAME_PROGRAM_SUFFIX); }
static char* game_addon_name(const Game* g, const char* entry) { return snx_format("%s_%s", g->name, entry); }
static char* game_addon_base(const Game* g, const char* entry) {
    char* name = game_addon_name(g, entry);
    char* base = snx_path_join(g->folder, name);
    free(name);
    return base;
}

/* an extraction in work/ (the cache's, then an older one in the data folder), by its folder name or its title ID */
static char* game_work_find(const char* data, const char* selector, int by_id, SnxU64 id) {
    char *old, *cache, *dirs[2], *found = NULL;
    int w;
    old = snx_path_join(data, "work");
    cache = old ? snx_user_path(old, SNX_USER_CACHE) : NULL;
    dirs[0] = cache;
    dirs[1] = old;
    for (w = 0; w < 2 && !found; ++w) {
        char** names = NULL;
        size_t count = 0, i;
        if (!dirs[w] || !snx_is_dir(dirs[w])) continue;
        if (w && dirs[0] && !strcmp(dirs[0], dirs[1])) continue;
        if (!by_id) {
            char* path = snx_path_join(dirs[w], selector);
            if (path && snx_is_dir(path))
                found = path;
            else
                free(path);
            continue;
        }
        if (snx_list_dir(dirs[w], &names, &count)) continue;
        for (i = 0; i < count && !found; ++i) {
            char* path = snx_path_join(dirs[w], names[i]);
            SnxNpdm npdm;
            char error[256];
            if (snx_is_dir(path) && !snx_load_npdm(path, &npdm, error, sizeof error) && npdm.title_id == id)
                found = path;
            else
                free(path);
        }
        snx_free_strings(names, count);
    }
    free(cache);
    free(old);
    return found;
}

/* data names a game's title ID, its folder, a program in it, or the name it was made by. Only under the data folder or
   its cache: these tools run and rewrite what ScarletNX keeps there, and never another folder's programs. */
static int game_open(const char* data, const char* selector, Game* g, Text* err) {
    char *folder = NULL, *root = NULL, *cache_root = NULL, *cache_work = NULL, *file = NULL, *data_path = NULL,
         *state = NULL;
    const char* leaf;
    memset(g, 0, sizeof *g);
    if (!selector || !*selector) {
        mcp_put(err, "game: a title ID, a recompiled game's folder, a program in it, or the name it was made by");
        return -1;
    }
    if (snx_is_dir(selector) || snx_is_file(selector)) {
        char* absolute = snx_absolute(selector);
        if (!absolute) {
            mcp_put(err, "the game's path cannot be resolved");
            return -1;
        }
        if (snx_is_file(absolute)) {
            if (snx_is_dump(absolute)) {
                mcp_put(err, "a dump is prepared with scarletnx <dump> first, which makes its game folder; these "
                             "tools then name that folder, its program or its title ID");
                free(absolute);
                return -1;
            }
            file = absolute;
            folder = snx_dirname(absolute);
        } else if (snx_is_dir(absolute)) {
            folder = absolute;
        } else {
            mcp_put(err, "the game's path is not a folder or a program");
            free(absolute);
            return -1;
        }
    } else {
        SnxU64 id;
        char* name;
        int by_id = game_title_parse(selector, &id);
        name = snx_strdup(selector);
        if (!name) {
            mcp_put(err, "out of memory");
            return -1;
        }
        if (by_id) {
            size_t i;
            for (i = 0; name[i]; ++i) name[i] = (char)toupper((unsigned char)name[i]);
        }
        folder = snx_recompiled_game(data, name);
        if (!folder || !snx_is_dir(folder)) {
            char* found = game_work_find(data, name, by_id, by_id ? id : 0);
            free(folder);
            folder = found;
        }
        free(name);
        if (!folder || !snx_is_dir(folder)) {
            mcp_put(err, "no game of that title ID, folder or name under this data folder (find_game lists them)");
            free(folder);
            return -1;
        }
    }
    root = snx_absolute(data);
    {
        char* work = snx_path_join(data, "work");
        char* cache = snx_user_path(data, SNX_USER_CACHE);
        char* cache_work_path = work ? snx_user_path(work, SNX_USER_CACHE) : NULL;
        cache_root = cache ? snx_absolute(cache) : NULL;
        cache_work = cache_work_path ? snx_absolute(cache_work_path) : NULL;
        free(cache);
        free(cache_work_path);
        free(work);
    }
    if (root && snx_path_within(folder, root))
        g->root = snx_strdup(root);
    else if (cache_root && snx_path_within(folder, cache_root))
        g->root = snx_strdup(cache_root);
    else if (cache_work && snx_path_within(folder, cache_work))
        g->root = snx_strdup(cache_work);
    if (!root || !g->root) {
        mcp_put(err, "the game is outside the data folder and its cache, where these tools read and run");
        free(root);
        free(cache_root);
        free(cache_work);
        free(folder);
        free(file);
        game_close(g);
        return -1;
    }
    free(root);
    free(cache_root);
    free(cache_work);
    leaf = game_leaf(folder);
    {
        char* fbase = file ? game_program_base(game_leaf(file)) : NULL;
        SnxU64 folder_id, base_id;
        if (fbase && game_title_parse(leaf, &folder_id) && strlen(fbase) > 17 && fbase[16] == '_' &&
            game_title_parse(fbase, &base_id) && base_id == folder_id) {
            /* a program in the game's folder: <id>_<update entry> names the update too */
            g->name = snx_strdup(leaf);
            g->addon = snx_strdup(fbase + 17);
        } else if (file && !game_title_parse(leaf, NULL)) {
            g->name = fbase;
            fbase = NULL;
        } else
            g->name = snx_strdup(leaf);
        free(fbase);
    }
    if (!g->name || !*g->name || !strcmp(g->name, ".") || !strcmp(g->name, "..")) {
        mcp_put(err, "the game's folder has no usable name");
        free(folder);
        free(file);
        game_close(g);
        return -1;
    }
    g->folder = folder;
    g->has_id = game_title_parse(g->name, &g->id);
    data_path = snx_format("%s/%s_data", g->folder, g->name);
    if (data_path && snx_is_dir(data_path)) {
        g->data = data_path;
        g->has_data = 1;
    } else {
        free(data_path);
        g->data = snx_strdup(g->folder);
    }
    g->program = game_program_path(g->folder, g->name);
    g->built = g->program && snx_is_file(g->program);
    {
        char* config = game_kind_dir(g->folder, g->name, "config", SNX_USER_CONFIG);
        g->ini = config ? snx_format("%s/%s.ini", config, g->name) : NULL;
        free(config);
    }
    state = game_kind_dir(g->folder, g->name, "state", SNX_USER_STATE);
    g->log = state ? snx_format("%s/%s.log", state, g->name) : NULL;
    free(state);
    free(file);
    if (!g->has_id) { /* a game an older version named by its title, rather than its title ID */
        SnxNpdm npdm;
        char error[256];
        if (g->data && !snx_load_npdm(g->data, &npdm, error, sizeof error) && npdm.title_id) {
            g->id = npdm.title_id;
            g->has_id = 1;
        } else if (g->folder && !snx_load_npdm(g->folder, &npdm, error, sizeof error) && npdm.title_id) {
            g->id = npdm.title_id;
            g->has_id = 1;
        }
    }
    if (!g->data || !g->ini || !g->program || !g->log) {
        mcp_put(err, "out of memory");
        game_close(g);
        return -1;
    }
    return 0;
}

/* ============================================================ add-ons, mods, cheats */
typedef struct {
    char entry[32];
    SnxU64 id;
    SnxU64 version;
    int update;
    int current;
    int installed;
    int built;
    char* program;
    char* data;
    char* source;
} Addon;
typedef struct {
    Addon* v;
    size_t n, cap;
} Addons;
static void addons_close(Addons* a) {
    size_t i;
    for (i = 0; i < a->n; ++i) {
        free(a->v[i].program);
        free(a->v[i].data);
        free(a->v[i].source);
    }
    free(a->v);
    memset(a, 0, sizeof *a);
}
static Addon* addon_add(Addons* a, const char* entry, int current) {
    size_t i;
    for (i = 0; i < a->n; ++i)
        if (!strcmp(a->v[i].entry, entry)) {
            if (current) a->v[i].current = 1;
            return &a->v[i];
        }
    if (a->n == a->cap) {
        a->cap = a->cap ? a->cap * 2 : 8;
        a->v = (Addon*)snx_realloc(a->v, a->cap * sizeof *a->v);
    }
    memset(&a->v[a->n], 0, sizeof a->v[a->n]);
    memcpy(a->v[a->n].entry, entry, sizeof a->v[a->n].entry);
    a->v[a->n].entry[sizeof a->v[a->n].entry - 1] = 0;
    a->v[a->n].current = current;
    return &a->v[a->n++];
}
/* the game's .ini's addons=, or none */
static size_t game_addons_current(const Game* g, SnxAddon* list, size_t max) {
    char id[17], *text;
    size_t count;
    if (!g->has_id) return 0;
    text = snx_ini_value(g->ini, "addons");
    if (!text) return 0;
    game_title_text(g->id, id);
    count = snx_addons_parse(text, id, list, max);
    free(text);
    return count;
}
static void addons_scan(const Game* g, Addons* a) {
    SnxAddon current[SNX_ADDONS_MAX];
    size_t ncurrent = game_addons_current(g, current, SNX_ADDONS_MAX), i;
    for (i = 0; i < ncurrent; ++i)
        if (snx_addon_entry_id(g->id, current[i].entry, NULL)) addon_add(a, current[i].entry, 1);
    if (g->has_id && snx_is_dir(g->folder)) {
        char** names = NULL;
        size_t count = 0;
        if (!snx_list_dir(g->folder, &names, &count)) {
            size_t prefix = strlen(g->name) + 1;
            for (i = 0; i < count; ++i) {
                const char* name = names[i];
                char entry[32], stripped[32];
                size_t n;
                if (strlen(name) < prefix || strncmp(name, g->name, prefix - 1) || name[prefix - 1] != '_') continue;
                name += prefix;
                if (game_ends_with(name, "_data")) {
                    n = strlen(name) - 5;
                    if (!n || n >= sizeof stripped) continue;
                    memcpy(stripped, name, n);
                    stripped[n] = 0;
                } else {
                    char* base = game_without_suffix(name, GAME_PROGRAM_SUFFIX);
                    game_copy(stripped, sizeof stripped, base);
                    free(base);
                }
                game_copy(entry, sizeof entry, stripped);
                if (snx_addon_entry_id(g->id, entry, NULL)) addon_add(a, entry, 0);
            }
            snx_free_strings(names, count);
        }
    }
    for (i = 0; i < a->n; ++i) {
        Addon* addon = &a->v[i];
        char* name = game_addon_name(g, addon->entry);
        char* program = snx_path_join(g->folder, name);
        addon->data = snx_format("%s_data", program);
        addon->program = snx_format("%s%s", program, GAME_PROGRAM_SUFFIX);
        addon->installed = snx_is_dir(addon->data) || snx_is_file(addon->program);
        addon->built = snx_is_file(addon->program);
        addon->id = 0;
        snx_addon_entry_id(g->id, addon->entry, &addon->id);
        addon->update = snx_addon_kind(g->id, addon->id) == SNX_ADDON_UPDATE;
        addon->version = snx_addon_version(addon->entry);
        addon->source = snx_game_source(g->folder, name, NULL);
        free(program);
        free(name);
    }
}
static Addon* addon_find_entry(Addons* a, const char* entry) {
    size_t i;
    for (i = 0; i < a->n; ++i)
        if (!strcmp(a->v[i].entry, entry)) return &a->v[i];
    return NULL;
}
static Addon* addon_find_id(Addons* a, SnxU64 id, int update) {
    Addon* found = NULL;
    size_t i;
    for (i = 0; i < a->n; ++i)
        if (a->v[i].id == id && a->v[i].update == update &&
            (!found || (a->v[i].installed != found->installed ? a->v[i].installed : a->v[i].version > found->version)))
            found = &a->v[i];
    return found;
}
static Addon* addon_current_update(Addons* a) {
    size_t i;
    for (i = 0; i < a->n; ++i)
        if (a->v[i].update && a->v[i].current) return &a->v[i];
    return NULL;
}
static Addon* addon_latest_update(Addons* a) {
    Addon* found = NULL;
    size_t i;
    for (i = 0; i < a->n; ++i)
        if (a->v[i].update && a->v[i].installed && (!found || a->v[i].version > found->version)) found = &a->v[i];
    return found;
}

/* ============================================================ the inventory as JSON */
static void game_json_add_string(cJSON* object, const char* name, const char* value) {
    if (value)
        cJSON_AddStringToObject(object, name, value);
    else
        cJSON_AddNullToObject(object, name);
}
static cJSON* addons_json(Addons* addons, int updates) {
    cJSON* list = cJSON_CreateArray();
    size_t i;
    for (i = 0; i < addons->n; ++i) {
        const Addon* a = &addons->v[i];
        cJSON* item;
        char id[17];
        if (a->update != updates) continue;
        item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "entry", a->entry);
        game_title_text(a->id, id);
        cJSON_AddStringToObject(item, "title_id", id);
        cJSON_AddStringToObject(item, "kind", a->update ? "update" : "dlc");
        if (a->version != SNX_NO_VERSION)
            cJSON_AddNumberToObject(item, "version", (double)a->version);
        else
            cJSON_AddNullToObject(item, "version");
        cJSON_AddBoolToObject(item, "installed", a->installed);
        cJSON_AddBoolToObject(item, "built", a->built);
        cJSON_AddBoolToObject(item, "current", a->current);
        game_json_add_string(item, "program", a->update && a->built ? a->program : NULL);
        game_json_add_string(item, "data", a->installed ? a->data : NULL);
        game_json_add_string(item, "source", a->source);
        cJSON_AddItemToArray(list, item);
    }
    return list;
}
static cJSON* mods_json(const char* folder, const char* off) {
    char** names = NULL;
    size_t count = snx_mods_list(folder, &names), i;
    cJSON* list = cJSON_CreateArray();
    for (i = 0; i < count; ++i) {
        char *mod = snx_path_join(folder, names[i]), *exefs = snx_path_join(mod, "exefs"),
             *romfs = snx_path_join(mod, "romfs");
        cJSON* item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "name", names[i]);
        cJSON_AddBoolToObject(item, "enabled", !snx_mod_off(off, names[i]));
        cJSON_AddBoolToObject(item, "exefs", snx_is_dir(exefs));
        cJSON_AddBoolToObject(item, "romfs", snx_is_dir(romfs));
        cJSON_AddItemToArray(list, item);
        free(exefs);
        free(romfs);
        free(mod);
    }
    snx_free_strings(names, count);
    return list;
}
static cJSON* cheats_json(const char* folder, const char* off) {
    char** names = NULL;
    size_t count = snx_cheats_list(folder, &names), i;
    cJSON* list = cJSON_CreateArray();
    for (i = 0; i < count; ++i) {
        cJSON* item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "name", names[i]);
        cJSON_AddBoolToObject(item, "enabled", !snx_cheat_off(off, names[i]));
        cJSON_AddItemToArray(list, item);
    }
    snx_free_strings(names, count);
    return list;
}
static void current_addons_json(cJSON* current, Addons* addons) {
    Addon* update = addon_current_update(addons);
    cJSON* dlcs = cJSON_CreateArray();
    size_t i;
    game_json_add_string(current, "update", update ? update->entry : NULL);
    for (i = 0; i < addons->n; ++i)
        if (addons->v[i].current && !addons->v[i].update)
            cJSON_AddItemToArray(dlcs, cJSON_CreateString(addons->v[i].entry));
    cJSON_AddItemToObject(current, "dlcs", dlcs);
}
/* why a program cannot be run: NULL when it can. base is the program without its suffix (game_program_base) */
static char* game_build_ready_why(const char* program, const char* data, int has_data, const char* mods,
                                  const char* mods_off) {
    char *stamp, *base, *code;
    char why[1024];
    if (!has_data)
        return snx_format("%s is not a recompiled game's data folder: call game_recompile (a work/ extraction is "
                          "built from its dump first)",
                          data ? data : "the game's data folder");
    if (!program) return snx_strdup("the game's program is unknown: call game_recompile");
    if (!snx_is_file(program)) return snx_format("%s does not exist: call game_recompile", program);
    base = game_program_base(program);
    code = game_code_path(base);
    if (!code || !snx_is_file(code)) {
        char* message = snx_format("%s is missing: call game_recompile", code ? code : "the game's code");
        free(code);
        free(base);
        return message;
    }
    free(code);
    stamp = snx_game_stamp_for(snx_game_abi(), snx_mods_stamp(mods, mods_off));
    if (!snx_game_stamp_is(base, stamp)) {
        snprintf(why, sizeof why,
                 "%s is not stamped for this ABI and these mods: call game_recompile with the same mods", base);
        free(base);
        free(stamp);
        return snx_strdup(why);
    }
    free(base);
    free(stamp);
    return NULL;
}
/* why the game's current build cannot be run: the update the .ini names, else the base game */
static char* game_ready_why(const Game* g, Addons* addons, const char* mods_off) {
    Addon* update = addon_current_update(addons);
    char* mods = game_mods_folder(g);
    char* why;
    if (update)
        why = game_build_ready_why(update->program, update->data, snx_is_dir(update->data), mods, mods_off);
    else
        why = game_build_ready_why(g->program, g->data, g->has_data, mods, mods_off);
    free(mods);
    return why;
}
static int game_capability_tracy(void) { return snx_setting_known && snx_setting_known("tracy_profiler"); }
static void game_capabilities_json(cJSON* root) {
    cJSON* caps = cJSON_CreateObject();
    cJSON_AddBoolToObject(caps, "tracy", game_capability_tracy());
#ifdef _WIN32
    cJSON_AddBoolToObject(caps, "gdb", 0);
#else
    cJSON_AddBoolToObject(caps, "gdb", 1);
#endif
    cJSON_AddItemToObject(root, "capabilities", caps);
}
char* mcp_build_info(void) {
    cJSON* root = cJSON_CreateObject();
    char* out;
    cJSON_AddStringToObject(root, "build", snx_build_info());
    cJSON_AddStringToObject(root, "commit", snx_build_commit());
    cJSON_AddNumberToObject(root, "abi", (double)snx_game_abi());
    game_capabilities_json(root);
    out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out;
}
char* mcp_game_info(const char* data, const char* game, Text* err) {
    Game g;
    Addons addons = {0};
    char *mods = NULL, *mods_off = NULL, *cheats = NULL, *cheats_off = NULL, *version = NULL, *why = NULL;
    char* out;
    cJSON *root, *what, *current, *paths;
    if (game_open(data, game, &g, err)) return NULL;
    addons_scan(&g, &addons);
    mods = game_mods_folder(&g);
    mods_off = snx_ini_value(g.ini, "mods_off");
    if (!mods_off) mods_off = snx_strdup("");
    cheats = snx_cheats_folder(g.data);
    cheats_off = snx_ini_value(g.ini, "cheats_off");
    if (!cheats_off) cheats_off = snx_strdup("");
    version = game_nacp_version(g.data);
    why = game_ready_why(&g, &addons, mods_off);
    root = cJSON_CreateObject();
    what = cJSON_CreateObject();
    cJSON_AddStringToObject(what, "path", g.folder);
    cJSON_AddStringToObject(what, "name", g.name);
    if (g.has_id) {
        char id[17];
        game_title_text(g.id, id);
        cJSON_AddStringToObject(what, "title_id", id);
    } else
        cJSON_AddNullToObject(what, "title_id");
    game_json_add_string(what, "version", version);
    cJSON_AddStringToObject(what, "data", g.data);
    cJSON_AddStringToObject(what, "program", g.program);
    cJSON_AddStringToObject(what, "ini", g.ini);
    cJSON_AddStringToObject(what, "log", g.log);
    cJSON_AddBoolToObject(what, "built", g.built);
    cJSON_AddBoolToObject(what, "ready", !why);
    game_json_add_string(what, "not_ready", why);
    cJSON_AddItemToObject(root, "game", what);
    current = cJSON_CreateObject();
    current_addons_json(current, &addons);
    cJSON_AddStringToObject(current, "mods_off", mods_off);
    cJSON_AddStringToObject(current, "cheats_off", cheats_off);
    cJSON_AddItemToObject(root, "current", current);
    cJSON_AddItemToObject(root, "updates", addons_json(&addons, 1));
    cJSON_AddItemToObject(root, "dlcs", addons_json(&addons, 0));
    cJSON_AddItemToObject(root, "mods", mods_json(mods, mods_off));
    cJSON_AddItemToObject(root, "cheats", cheats_json(cheats, cheats_off));
    paths = cJSON_CreateObject();
    game_json_add_string(paths, "mods", mods);
    game_json_add_string(paths, "cheats", cheats);
    cJSON_AddItemToObject(root, "paths", paths);
    game_capabilities_json(root);
    out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    free(version);
    free(why);
    free(mods);
    free(mods_off);
    free(cheats);
    free(cheats_off);
    addons_close(&addons);
    game_close(&g);
    return out;
}

/* ============================================================ the caller's choices */
static const char* game_arg_string(const cJSON* args, const char* key) {
    const cJSON* value = cJSON_GetObjectItemCaseSensitive(args, key);
    return cJSON_IsString(value) ? value->valuestring : NULL;
}
static long game_arg_number(const cJSON* args, const char* key, long otherwise) {
    const cJSON* value = cJSON_GetObjectItemCaseSensitive(args, key);
    if (!cJSON_IsNumber(value) || value->valuedouble < (double)LONG_MIN || value->valuedouble > (double)LONG_MAX)
        return otherwise;
    return (long)value->valuedouble;
}
static int game_arg_bool(const cJSON* args, const char* key, int otherwise) {
    const cJSON* value = cJSON_GetObjectItemCaseSensitive(args, key);
    if (cJSON_IsBool(value)) return cJSON_IsTrue(value);
    return otherwise;
}
static void game_list_close(char** list, size_t count) { snx_free_strings(list, count); }
static int game_list_has(char** list, size_t count, const char* name) {
    size_t i;
    for (i = 0; i < count; ++i)
        if (!strcmp(list[i], name)) return 1;
    return 0;
}
/* a string's comma-separated words, or an array's strings, owned; -1 when it is neither */
static int game_choice_list(const cJSON* value, char*** out, size_t* count, Text* err) {
    *out = NULL;
    *count = 0;
    if (!value || cJSON_IsNull(value)) return 0;
    if (cJSON_IsString(value)) {
        const char* text = value->valuestring;
        char** list = NULL;
        size_t n = 0, at = 0, length = strlen(text);
        while (at < length) {
            size_t start, end;
            while (at < length && (text[at] == ',' || isspace((unsigned char)text[at]))) ++at;
            start = at;
            while (at < length && text[at] != ',') ++at;
            end = at;
            while (end > start && isspace((unsigned char)text[end - 1])) --end;
            if (end > start) {
                size_t size = end - start;
                char* word = (char*)snx_alloc(size + 1);
                memcpy(word, text + start, size);
                word[size] = 0;
                list = (char**)snx_realloc(list, (n + 1) * sizeof *list);
                list[n++] = word;
            }
        }
        *out = list;
        *count = n;
        return 0;
    }
    if (cJSON_IsArray(value)) {
        int size = cJSON_GetArraySize(value), i;
        char** list = size > 0 ? (char**)snx_alloc((size_t)size * sizeof *list) : NULL;
        size_t n = 0;
        for (i = 0; i < size; ++i) {
            const cJSON* item = cJSON_GetArrayItem(value, i);
            if (!cJSON_IsString(item)) {
                game_list_close(list, n);
                mcp_put(err, "a list of choices holds strings");
                return -1;
            }
            list[n++] = snx_strdup(item->valuestring);
        }
        *out = list;
        *count = n;
        return 0;
    }
    mcp_put(err, "a choice is a string or a list of strings");
    return -1;
}
static char* game_mods_folder(const Game* g) { return snx_mods_folder(g->data); }
static char* game_current_cheats_off(const Game* g) {
    char* off = snx_ini_value(g->ini, "cheats_off");
    return off ? off : snx_strdup("");
}
/* the mods this run leaves out: current (the .ini's), all, none, or the list of mods it plays */
static char* game_mods_off(const Game* g, const cJSON* choice, const char* direct, Text* err) {
    char *folder = NULL, *current, *off = NULL;
    char** all = NULL;
    size_t count;
    if (direct) return snx_strdup(direct);
    folder = game_mods_folder(g);
    current = snx_ini_value(g->ini, "mods_off");
    if (!current) current = snx_strdup("");
    if (!choice || cJSON_IsNull(choice) || (cJSON_IsString(choice) && !strcmp(choice->valuestring, "current"))) {
        free(folder);
        return current;
    }
    if (cJSON_IsString(choice) && !strcmp(choice->valuestring, "all")) {
        free(folder);
        free(current);
        return snx_strdup("");
    }
    count = snx_mods_list(folder, &all);
    if (cJSON_IsString(choice) && (!strcmp(choice->valuestring, "none") || !strcmp(choice->valuestring, "off"))) {
        Text t = {0};
        size_t i;
        for (i = 0; i < count; ++i) {
            if (t.n) mcp_put_n(&t, "/", 1);
            mcp_put(&t, all[i]);
        }
        off = t.p ? t.p : snx_strdup("");
    } else {
        char** wanted = NULL;
        size_t wanted_count = 0, i;
        int kept;
        if (game_choice_list(choice, &wanted, &wanted_count, err)) {
            free(folder);
            free(current);
            snx_free_strings(all, count);
            return NULL;
        }
        for (i = 0; i < wanted_count; ++i)
            if (!game_list_has(all, count, wanted[i])) {
                mcp_put(err, "no mod named ");
                mcp_put(err, wanted[i]);
                mcp_put(err, " (game_info lists them)");
                game_list_close(wanted, wanted_count);
                free(folder);
                free(current);
                snx_free_strings(all, count);
                return NULL;
            }
        {
            Text t = {0};
            for (i = 0; i < count; ++i) {
                kept = game_list_has(wanted, wanted_count, all[i]);
                if (!kept) {
                    if (t.n) mcp_put_n(&t, "/", 1);
                    mcp_put(&t, all[i]);
                }
            }
            off = t.p ? t.p : snx_strdup("");
        }
        game_list_close(wanted, wanted_count);
    }
    free(folder);
    free(current);
    snx_free_strings(all, count);
    return off;
}
/* the cheats this run leaves out, as game_mods_off does for mods */
static char* game_cheats_off(const Game* g, const cJSON* choice, const char* direct, Text* err) {
    char *folder = snx_cheats_folder(g->data), *current = game_current_cheats_off(g), *off = NULL;
    char** all = NULL;
    size_t count;
    if (direct) {
        free(folder);
        free(current);
        return snx_strdup(direct);
    }
    if (!choice || cJSON_IsNull(choice) || (cJSON_IsString(choice) && !strcmp(choice->valuestring, "current"))) {
        free(folder);
        return current;
    }
    if (cJSON_IsString(choice) && !strcmp(choice->valuestring, "all")) {
        free(folder);
        free(current);
        return snx_strdup("");
    }
    count = snx_cheats_list(folder, &all);
    if (cJSON_IsString(choice) && (!strcmp(choice->valuestring, "none") || !strcmp(choice->valuestring, "off"))) {
        Text t = {0};
        size_t i;
        for (i = 0; i < count; ++i) {
            if (t.n) mcp_put_n(&t, "|", 1);
            mcp_put(&t, all[i]);
        }
        off = t.p ? t.p : snx_strdup("");
    } else {
        char** wanted = NULL;
        size_t wanted_count = 0, i;
        if (game_choice_list(choice, &wanted, &wanted_count, err)) {
            free(folder);
            free(current);
            snx_free_strings(all, count);
            return NULL;
        }
        for (i = 0; i < wanted_count; ++i)
            if (!game_list_has(all, count, wanted[i])) {
                mcp_put(err, "no cheat named ");
                mcp_put(err, wanted[i]);
                mcp_put(err, " (game_info lists them)");
                game_list_close(wanted, wanted_count);
                free(folder);
                free(current);
                snx_free_strings(all, count);
                return NULL;
            }
        {
            Text t = {0};
            for (i = 0; i < count; ++i) {
                if (game_list_has(wanted, wanted_count, all[i])) continue;
                if (t.n) mcp_put_n(&t, "|", 1);
                mcp_put(&t, all[i]);
            }
            off = t.p ? t.p : snx_strdup("");
        }
        game_list_close(wanted, wanted_count);
    }
    free(folder);
    free(current);
    snx_free_strings(all, count);
    return off;
}
/* the update the caller named: current (the .ini's), latest, none/base/off, an entry, or a title ID */
static int game_select_update(Addons* addons, const char* selector, Addon** out, Text* err) {
    SnxU64 id;
    *out = NULL;
    if (!selector || !*selector || !strcmp(selector, "current")) {
        *out = addon_current_update(addons);
        return 0;
    }
    if (!strcmp(selector, "none") || !strcmp(selector, "base") || !strcmp(selector, "off")) return 0;
    if (!strcmp(selector, "latest")) {
        *out = addon_latest_update(addons);
        if (!*out) mcp_put(err, "no update is installed");
        return *out ? 0 : -1;
    }
    *out = addon_find_entry(addons, selector);
    if (!*out && game_title_parse(selector, &id)) *out = addon_find_id(addons, id, 1);
    if (*out && !(*out)->update) *out = NULL;
    if (!*out) {
        mcp_put(err, "no update ");
        mcp_put(err, selector);
        mcp_put(err, " is installed (game_info lists current, latest and the entries)");
        return -1;
    }
    return 0;
}
/* the DLCs the caller named: current (the .ini's), all, none, entries or title IDs */
static int game_select_dlcs(Addons* addons, const cJSON* choice, char*** out, size_t* count, Text* err) {
    size_t i, n = 0;
    *out = NULL;
    *count = 0;
    if (!choice || cJSON_IsNull(choice) || (cJSON_IsString(choice) && !strcmp(choice->valuestring, "current"))) {
        for (i = 0; i < addons->n; ++i)
            if (addons->v[i].current && !addons->v[i].update) ++n;
        *out = n ? (char**)snx_alloc(n * sizeof **out) : NULL;
        for (i = 0; i < addons->n && *count < n; ++i) {
            if (addons->v[i].current && !addons->v[i].update) {
                if (!addons->v[i].installed) {
                    mcp_put(err, "no DLC ");
                    mcp_put(err, addons->v[i].entry);
                    mcp_put(err, " is installed (game_info lists them)");
                    game_list_close(*out, *count);
                    *out = NULL;
                    *count = 0;
                    return -1;
                }
                (*out)[(*count)++] = snx_strdup(addons->v[i].entry);
            }
        }
        return 0;
    }
    if (cJSON_IsString(choice) && (!strcmp(choice->valuestring, "none") || !strcmp(choice->valuestring, "off")))
        return 0;
    if (cJSON_IsString(choice) && !strcmp(choice->valuestring, "all")) {
        for (i = 0; i < addons->n; ++i)
            if (!addons->v[i].update && addons->v[i].installed) ++n;
        *out = n ? (char**)snx_alloc(n * sizeof **out) : NULL;
        for (i = 0; i < addons->n; ++i)
            if (!addons->v[i].update && addons->v[i].installed) (*out)[(*count)++] = snx_strdup(addons->v[i].entry);
        return 0;
    }
    {
        char** wanted = NULL;
        size_t wanted_count = 0;
        if (game_choice_list(choice, &wanted, &wanted_count, err)) return -1;
        *out = wanted_count ? (char**)snx_alloc(wanted_count * sizeof **out) : NULL;
        for (i = 0; i < wanted_count; ++i) {
            Addon* addon = addon_find_entry(addons, wanted[i]);
            SnxU64 id;
            if (!addon && game_title_parse(wanted[i], &id)) addon = addon_find_id(addons, id, 0);
            if (!addon || addon->update || !addon->installed) {
                mcp_put(err, "no DLC ");
                mcp_put(err, wanted[i]);
                mcp_put(err, " is installed (game_info lists them)");
                game_list_close(wanted, wanted_count);
                game_list_close(*out, *count);
                *out = NULL;
                *count = 0;
                return -1;
            }
            (*out)[(*count)++] = snx_strdup(addon->entry);
        }
        game_list_close(wanted, wanted_count);
    }
    return 0;
}
static char* game_dlc_arg(char** entries, size_t count) {
    Text t = {0};
    size_t i;
    for (i = 0; i < count; ++i) {
        if (t.n) mcp_put_n(&t, ",", 1);
        mcp_put(&t, entries[i]);
    }
    return t.p ? t.p : snx_strdup("");
}

/* what the recompiler is given for a game made already, as scarletnx -r gives it: the NRO its _data keeps, else that
   folder, else the dump its .ini names to read it in place from */
static char* game_recompile_source(const Game* g) {
    char *base = snx_path_join(g->folder, g->name), *source = NULL;
    char* nro = snx_format("%s_data/exefs/main.nro", base);
    char* exefs = snx_format("%s_data/exefs", base);
    if (snx_is_file(nro))
        source = nro;
    else if (!snx_is_dir(exefs)) {
        char* from = snx_game_source(g->folder, g->name, NULL);
        source = from && snx_is_file(from) ? from : snx_format("%s_data", base);
        free(from);
    } else
        source = snx_format("%s_data", base);
    free(nro);
    free(exefs);
    free(base);
    return source;
}

/* ============================================================ recompiling */
static void game_command_add(cJSON* command, const char* text) {
    if (text) cJSON_AddItemToArray(command, cJSON_CreateString(text));
}
static char* game_output_tail(const char* text, int lines) {
    const char *at, *end;
    int seen = 0;
    if (!text) return snx_strdup("");
    end = text + strlen(text);
    if (end > text && end[-1] == '\n') --end; /* the line break after the last line, not a line */
    at = end;
    while (at > text && seen < lines) {
        --at;
        if (*at == '\n') ++seen;
    }
    if (at > text && *at == '\n') ++at;
    return snx_strdup(at);
}
char* mcp_game_recompile(const char* self, const char* data, const char* game, const struct cJSON* args, Text* err) {
    const cJSON* cj = (const cJSON*)args;
    Game g;
    Addons addons = {0};
    Addon* update = NULL;
    char *mods = NULL, *mods_off = NULL, *base = NULL, *program = NULL, *code = NULL, *stamp = NULL, *source = NULL;
    char *outdir = NULL, *name = NULL, *output = NULL, *tail = NULL, *out = NULL;
    const char* update_choice = game_arg_string(cj, "update");
    const char* direct_off = game_arg_string(cj, "mods_off");
    const char* optimization = game_arg_string(cj, "optimization");
    const char* jit_cache = game_arg_string(cj, "jit_cache");
    const cJSON* mods_choice = cJSON_GetObjectItemCaseSensitive(cj, "mods");
    long jobs = game_arg_number(cj, "jobs", 0);
    int force = game_arg_bool(cj, "force", 0);
    int dry_run = game_arg_bool(cj, "dry_run", 0);
    int keep_ir = game_arg_bool(cj, "keep_ir", 0);
    int hook = game_arg_bool(cj, "hook_direct_calls", 0);
    int rebuild_options = optimization || jobs || keep_ir || hook || jit_cache;
    const char* argv[32];
    size_t argc = 0, i;
    int rc = 1, up_to_date = 0;
    cJSON *root, *command;
    if (game_open(data, game, &g, err)) return NULL;
    if (!self) {
        mcp_put(err, "this program's path is unknown: it cannot recompile the game");
        game_close(&g);
        return NULL;
    }
    if (!g.has_data) {
        mcp_put(err, "the game has no <name>_data folder: recompile it from its dump first (scarletnx <dump>)");
        game_close(&g);
        return NULL;
    }
    addons_scan(&g, &addons);
    if (!update_choice && g.addon) update_choice = g.addon;
    if (game_select_update(&addons, update_choice, &update, err)) goto done;
    mods_off = game_mods_off(&g, mods_choice, direct_off, err);
    if (!mods_off) goto done;
    mods = game_mods_folder(&g);
    /* the build goes in its game's folder's parent, under the root game_open checked, and --name keeps it there even
       when that is the name it is called by rather than its title ID */
    outdir = g.folder ? snx_dirname(g.folder) : NULL;
    if (!outdir || !g.root || !snx_path_within(outdir, g.root)) {
        mcp_put(err, "the game's parent is outside the data folder and its cache, where these tools rewrite");
        goto done;
    }
    if (update) {
        char id[17];
        char* game_folder;
        game_title_text(g.id, id);
        game_folder = snx_path_join(outdir, id);
        name = snx_format("%s_%s", id, update->entry);
        base = snx_path_join(game_folder, name);
        free(game_folder);
        program = snx_format("%s%s", base, GAME_PROGRAM_SUFFIX);
        /* an update is only built from its dump, which the recompiler reads in place over its game's files */
        if (update->source && snx_is_file(update->source) && snx_is_dump(update->source)) {
            source = snx_absolute(update->source);
            if (!source) goto done;
        } else {
            char* ini = game_addon_name(&g, update->entry);
            mcp_put(err, "the update was not made from a dump this tool can read: name it as source= in ");
            mcp_put(err, ini ? ini : "the update's .ini beside the game's");
            free(ini);
            goto done;
        }
    } else {
        name = snx_game_title(g.name, NULL, g.folder);
        base = snx_path_join(outdir, name ? name : g.name);
        program = snx_format("%s%s", base, GAME_PROGRAM_SUFFIX);
        source = game_recompile_source(&g);
    }
    if (!source) {
        mcp_put(err, "cannot resolve the game's source");
        goto done;
    }
    code = game_code_path(base);
    stamp = snx_game_stamp_for(snx_game_abi(), snx_mods_stamp(mods, mods_off));
    up_to_date = snx_is_file(program) && code && snx_is_file(code) && snx_game_stamp_is(base, stamp) && !force &&
                 !rebuild_options;
    command = cJSON_CreateArray();
    game_command_add(command, self);
    game_command_add(command, "-r");
    game_command_add(command, "--data");
    game_command_add(command, data);
    game_command_add(command, "--outdir");
    game_command_add(command, outdir);
    if (!update) { /* an update is named after its game and itself: --name names a game alone */
        char* arg = snx_format("--name=%s", name ? name : g.name);
        game_command_add(command, arg);
        free(arg);
    }
    {
        char* arg = snx_format("--mods_off=%s", mods_off);
        game_command_add(command, arg);
        free(arg);
    }
    if (optimization) {
        game_command_add(command, "-O");
        game_command_add(command, optimization);
    }
    if (jobs) {
        char* arg = snx_format("--jobs=%ld", jobs);
        game_command_add(command, arg);
        free(arg);
    }
    if (keep_ir) game_command_add(command, "--keep-ir");
    if (hook) game_command_add(command, "--hook-direct-calls");
    if (jit_cache) {
        char* arg = snx_format("--jit-cache=%s", jit_cache);
        game_command_add(command, arg);
        free(arg);
    }
    game_command_add(command, "--");
    game_command_add(command, source);
    root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "game", g.folder);
    if (g.has_id) {
        char id[17];
        game_title_text(g.id, id);
        cJSON_AddStringToObject(root, "title_id", id);
    }
    game_json_add_string(root, "update", update ? update->entry : NULL);
    cJSON_AddStringToObject(root, "mods_off", mods_off);
    cJSON_AddStringToObject(root, "source", source);
    cJSON_AddStringToObject(root, "program", program);
    cJSON_AddStringToObject(root, "stamp", stamp);
    cJSON_AddItemToObject(root, "command", command);
    if (dry_run) {
        cJSON_AddStringToObject(root, "status", "dry_run");
        cJSON_AddBoolToObject(root, "ok", 1);
        out = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
        goto done;
    }
    if (up_to_date) {
        cJSON_AddStringToObject(root, "status", "up_to_date");
        cJSON_AddBoolToObject(root, "ok", 1);
        out = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
        goto done;
    }
    /* argv borrows the command's strings, which live until the run has returned */
    argc = (size_t)cJSON_GetArraySize(command);
    if (argc >= sizeof argv / sizeof *argv) {
        cJSON_Delete(root);
        mcp_put(err, "the recompile command is too long");
        goto done;
    }
    for (i = 0; i < argc; ++i) argv[i] = cJSON_GetArrayItem(command, (int)i)->valuestring;
    argv[argc] = NULL;
    rc = snx_run(argv, g.folder, NULL, &output);
    tail = game_output_tail(output, GAME_OUTPUT_LINES);
    cJSON_AddNumberToObject(root, "exit", rc);
    cJSON_AddBoolToObject(root, "ok", rc == 0 && snx_is_file(program) && snx_game_stamp_is(base, stamp));
    cJSON_AddStringToObject(root, "status",
                            rc == 0 && snx_is_file(program) && snx_game_stamp_is(base, stamp) ? "built" : "failed");
    cJSON_AddStringToObject(root, "output", tail);
    out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
done:
    free(output);
    free(tail);
    free(mods);
    free(mods_off);
    free(base);
    free(program);
    free(code);
    free(stamp);
    free(source);
    free(outdir);
    free(name);
    addons_close(&addons);
    game_close(&g);
    return out;
}

/* ============================================================ a run, and what it did */
typedef struct {
    int crash;
    int exception;
    unsigned long exception_code;
    int signal;
    SnxU64 pc, lr;
    int has_regs;
    int brk;
    unsigned long long reason;
    int has_frames;
    unsigned long long frames;
    char hash[65];
} RunSummary;
static void run_summary_parse(const char* text, RunSummary* s) {
    const char *crash, *brk, *at, *regs = NULL, *last = NULL;
    memset(s, 0, sizeof *s);
    if (!text) return;
    crash = strstr(text, "*** Crash (signal ");
    if (crash) {
        char* end;
        long signal;
        crash += strlen("*** Crash (signal ");
        signal = strtol(crash, &end, 10);
        if (end != crash) {
            s->crash = 1;
            s->signal = (int)signal;
            regs = end;
        }
    } else {
        crash = strstr(text, "*** Crash (exception 0x");
        if (crash) {
            char* end;
            unsigned long code;
            crash += strlen("*** Crash (exception 0x");
            code = strtoul(crash, &end, 16);
            if (end != crash) {
                s->crash = 1;
                s->exception = 1;
                s->exception_code = code;
                regs = end;
            }
        }
    }
    if (regs) {
        for (last = regs; (at = strstr(last, "pc=")) != NULL; last = at + 3) {
            unsigned long long pc, lr;
            char tail;
            if (sscanf(at, "pc=%llx lr=%llx%c", &pc, &lr, &tail) >= 2) {
                s->pc = (SnxU64)pc;
                s->lr = (SnxU64)lr;
                s->has_regs = 1;
            }
        }
    }
    brk = strstr(text, "*** The game called svcBreak (reason ");
    if (brk) {
        brk += strlen("*** The game called svcBreak (reason ");
        s->reason = strtoull(brk, NULL, 16);
        s->brk = 1;
    }
    at = text;
    while ((last = strstr(at, "frames=")) != NULL) {
        unsigned long long frames;
        char hash[65];
        if (sscanf(last, "frames=%llu hash=%64s", &frames, hash) == 2) {
            s->has_frames = 1;
            s->frames = frames;
            game_copy(s->hash, sizeof s->hash, hash);
        }
        at = last + 7;
    }
}
static cJSON* run_summary_json(const RunSummary* s, long seconds, int exit_code, int timed_out, char** line, int* bad) {
    cJSON* item = cJSON_CreateObject();
    Text t = {0};
    *bad = exit_code != 0 || timed_out || s->crash || s->brk;
    if (s->crash) {
        if (s->exception) {
            mcp_put(&t, "CRASH exception 0x");
            mcp_put_hex(&t, s->exception_code);
            cJSON_AddNullToObject(item, "signal");
            cJSON_AddNumberToObject(item, "exception", (double)s->exception_code);
        } else {
            mcp_put(&t, "CRASH signal ");
            mcp_put_num(&t, (unsigned long)s->signal);
            cJSON_AddNumberToObject(item, "signal", s->signal);
            cJSON_AddNullToObject(item, "exception");
        }
        if (s->has_regs) {
            mcp_put(&t, " at pc ");
            mcp_put_hex(&t, s->pc);
            mcp_put(&t, " (lr ");
            mcp_put_hex(&t, s->lr);
            mcp_put(&t, ")");
        } else
            mcp_put(&t, " (no game registers in the output)");
        if (s->has_regs) {
            char pc[17], lr[17];
            snprintf(pc, sizeof pc, "%016llx", (unsigned long long)s->pc);
            snprintf(lr, sizeof lr, "%016llx", (unsigned long long)s->lr);
            cJSON_AddStringToObject(item, "pc", pc);
            cJSON_AddStringToObject(item, "lr", lr);
        } else {
            cJSON_AddNullToObject(item, "pc");
            cJSON_AddNullToObject(item, "lr");
        }
    } else {
        cJSON_AddNullToObject(item, "signal");
        cJSON_AddNullToObject(item, "exception");
        cJSON_AddNullToObject(item, "pc");
        cJSON_AddNullToObject(item, "lr");
    }
    if (s->brk) {
        char reason[17];
        if (t.n) mcp_put(&t, "; ");
        snprintf(reason, sizeof reason, "%016llx", s->reason);
        mcp_put(&t, "svcBreak reason ");
        mcp_put(&t, reason);
        cJSON_AddStringToObject(item, "svcbreak", reason);
    } else
        cJSON_AddNullToObject(item, "svcbreak");
    if (timed_out) {
        if (t.n) mcp_put(&t, "; ");
        mcp_put(&t, "timeout");
    }
    if (!t.n) {
        if (exit_code) {
            mcp_put(&t, "exit ");
            mcp_put_num(&t, (unsigned long)exit_code);
        } else
            mcp_put(&t, "ok");
    }
    if (s->has_frames) {
        char frames[64];
        if (seconds > 0)
            snprintf(frames, sizeof frames, "%llu frames (%.1f FPS on average)", s->frames,
                     (double)s->frames / (double)seconds);
        else
            snprintf(frames, sizeof frames, "%llu frames", s->frames);
        mcp_put(&t, ", ");
        mcp_put(&t, frames);
        cJSON_AddNumberToObject(item, "frames", (double)s->frames);
        if (seconds > 0)
            cJSON_AddNumberToObject(item, "fps", (double)s->frames / (double)seconds);
        else
            cJSON_AddNullToObject(item, "fps");
        if (s->hash[0]) cJSON_AddStringToObject(item, "hash", s->hash);
    } else {
        cJSON_AddNullToObject(item, "frames");
        cJSON_AddNullToObject(item, "fps");
    }
    cJSON_AddNumberToObject(item, "exit", exit_code);
    cJSON_AddBoolToObject(item, "ok", !*bad);
    cJSON_AddStringToObject(item, "summary", t.p ? t.p : "ok");
    *line = t.p ? t.p : snx_strdup("ok");
    return item;
}
/* the newest Tracy capture in the game's captures/ folder, owned, or NULL */
static char* run_newest_tracy(const char* folder) {
    char* dir = snx_path_join(folder, "captures");
    char** names = NULL;
    size_t count = 0, i;
    char* found = NULL;
    if (!dir || snx_list_dir(dir, &names, &count)) {
        free(dir);
        return NULL;
    }
    for (i = 0; i < count; ++i)
        if (game_ends_with(names[i], ".tracy")) {
            free(found);
            found = snx_path_join(dir, names[i]);
        }
    snx_free_strings(names, count);
    free(dir);
    return found;
}
/* where a run's logs and shots go: the caller's folder under data or cache, or a new one in the cache */
static char* run_out_dir(const char* data, const char* cache, const Game* g, const char* requested, Text* err) {
    static unsigned long serial;
    char* out = NULL;
    if (requested && *requested) {
        char *root = snx_absolute(data), *croot = cache ? snx_absolute(cache) : NULL;
        out = snx_absolute(requested);
        if (!out || !root || (!snx_path_within(out, root) && (!croot || !snx_path_within(out, croot)))) {
            mcp_put(err, "out: a folder under the data folder or its cache folder");
            free(out);
            out = NULL;
        }
        free(root);
        free(croot);
        return out;
    }
    if (!cache) {
        mcp_put(err, "the cache folder is unknown: name out");
        return NULL;
    }
    do {
        free(out);
        out = snx_format("%s/mcp-runs/%s-%ld-%lu", cache, g->name, (long)time(NULL), ++serial);
    } while (out && snx_path_exists(out));
    return out;
}
/* a settings object's values as the runtime's --KEY=VALUE, in command; reserved keys are the tool's arguments */
static int run_settings(cJSON* command, const cJSON* settings, Text* err) {
    const cJSON* item;
    if (!settings || cJSON_IsNull(settings)) return 0;
    if (!cJSON_IsObject(settings)) {
        mcp_put(err, "settings: an object of setting names and values");
        return -1;
    }
    for (item = settings->child; item; item = item->next) {
        const char* key = item->string;
        const char* value = NULL;
        char number[64], *arg;
        if (!key || !*key) continue;
        if (!strcmp(key, "addons") || !strcmp(key, "mods_off") || !strcmp(key, "cheats_off") ||
            !strcmp(key, "seconds") || !strcmp(key, "frames") || !strcmp(key, "runs") || !strcmp(key, "runtime") ||
            !strcmp(key, "headless") || !strcmp(key, "shot") || !strcmp(key, "script") || !strcmp(key, "tracy") ||
            !strcmp(key, "state_save") || !strcmp(key, "state_load") || !strcmp(key, "timeout") ||
            !strcmp(key, "out")) {
            mcp_put(err, "settings: ");
            mcp_put(err, key);
            mcp_put(err, " has its own argument in game_run");
            return -1;
        }
        if (snx_setting_known && !snx_setting_known(key)) {
            mcp_put(err, "settings: no setting named ");
            mcp_put(err, key);
            return -1;
        }
        if (cJSON_IsString(item))
            value = item->valuestring;
        else if (cJSON_IsBool(item))
            value = cJSON_IsTrue(item) ? "1" : "0";
        else if (cJSON_IsNumber(item)) {
            snprintf(number, sizeof number, "%.17g", item->valuedouble);
            value = number;
        } else {
            mcp_put(err, "settings: ");
            mcp_put(err, key);
            mcp_put(err, " is not a string, number or boolean");
            return -1;
        }
        arg = snx_format("--%s=%s", key, value);
        game_command_add(command, arg);
        free(arg);
    }
    return 0;
}
/* the command for one run: the shared arguments and that run's screenshot */
static cJSON* run_command(cJSON* base, const char* out, long run, int shot) {
    cJSON* command = cJSON_Duplicate(base, 1);
    if (shot) {
        char* path = snx_format("%s/run%ld", out, run);
        char* arg = snx_format("--shot=%s", path);
        game_command_add(command, arg);
        free(arg);
        free(path);
    }
    return command;
}
/* the argv of a command, malloc'd only for the array: its strings are the command's */
static const char** run_argv(const cJSON* command) {
    size_t count = (size_t)cJSON_GetArraySize(command), i;
    const char** argv = (const char**)snx_alloc((count + 1) * sizeof *argv);
    for (i = 0; i < count; ++i) argv[i] = cJSON_GetArrayItem(command, (int)i)->valuestring;
    argv[count] = NULL;
    return argv;
}
char* mcp_game_run(const char* self, const char* data, const char* cache, const char* game, const struct cJSON* args,
                   Text* err) {
    const cJSON* cj = (const cJSON*)args;
    Game g;
    Addons addons = {0};
    Addon* update = NULL;
    char *mods = NULL, *mods_off = NULL, *cheats_off = NULL, *base = NULL, *program = NULL, *code = NULL, *stamp = NULL;
    char *out = NULL, *dlc_arg = NULL, *script = NULL, *tracy_path = NULL;
    char** dlcs = NULL;
    size_t ndlcs = 0, i;
    const char* update_choice = game_arg_string(cj, "update");
    const char* runtime = game_arg_string(cj, "runtime");
    const char* script_arg = game_arg_string(cj, "script");
    const char* tracy = game_arg_string(cj, "tracy");
    const char* requested_out = game_arg_string(cj, "out");
    const char* state_save = game_arg_string(cj, "state_save");
    const char* state_load = game_arg_string(cj, "state_load");
    const cJSON* dlcs_choice = cJSON_GetObjectItemCaseSensitive(cj, "dlcs");
    const cJSON* mods_choice = cJSON_GetObjectItemCaseSensitive(cj, "mods");
    const cJSON* cheats_choice = cJSON_GetObjectItemCaseSensitive(cj, "cheats");
    const cJSON* settings = cJSON_GetObjectItemCaseSensitive(cj, "settings");
    long runs = game_arg_number(cj, "runs", 1), seconds = game_arg_number(cj, "seconds", 60);
    long frames = game_arg_number(cj, "frames", 0), timeout = game_arg_number(cj, "timeout", 0);
    int headless = game_arg_bool(cj, "headless", 1);
    int shot = game_arg_bool(cj, "shot", 1);
    int dry_run = game_arg_bool(cj, "dry_run", 0);
    int stop = game_arg_bool(cj, "stop_on_failure", 0);
    int tracy_capture = 0, tracy_client = 0;
    int failed = 0;
    cJSON *root = NULL, *runs_json = NULL, *base_command = NULL, *first_command = NULL;
    char* result = NULL;
    memset(&g, 0, sizeof g);
    if (!runtime || !*runtime || !strcmp(runtime, "current") || !strcmp(runtime, "self")) {
        if (!self) {
            mcp_put(err, "this program's path is unknown: name runtime \"game\" to run the game's own copy");
            return NULL;
        }
    } else if (strcmp(runtime, "game") && strcmp(runtime, "deployed")) {
        mcp_put(err, "runtime: current (this build) or game (the copy in the game's folder)");
        return NULL;
    }
    if (runs < 1 || runs > GAME_RUNS_MAX) {
        mcp_put(err, "runs: 1 to 20");
        return NULL;
    }
    if (seconds < 1 || seconds > 86400) {
        mcp_put(err, "seconds: 1 to 86400");
        return NULL;
    }
    if (frames < 0 || frames > 100000000) {
        mcp_put(err, "frames: a whole number from 0");
        return NULL;
    }
    if (timeout < 0 || timeout > 86400) {
        mcp_put(err, "timeout: 0 (2*seconds+60) or 1 to 86400 seconds");
        return NULL;
    }
    if (tracy && *tracy && strcmp(tracy, "off") && strcmp(tracy, "capture") && strcmp(tracy, "client")) {
        mcp_put(err, "tracy: capture, client or off");
        return NULL;
    }
    tracy_capture = tracy && !strcmp(tracy, "capture");
    tracy_client = tracy && !strcmp(tracy, "client");
    if ((tracy_capture || tracy_client) && !game_capability_tracy()) {
        mcp_put(err, "this build has no Tracy: capture and client need a build configured with SNX_TRACY");
        return NULL;
    }
    if (game_open(data, game, &g, err)) return NULL;
    addons_scan(&g, &addons);
    if (!update_choice && g.addon) update_choice = g.addon;
    if (game_select_update(&addons, update_choice, &update, err)) goto done;
    if (game_select_dlcs(&addons, dlcs_choice, &dlcs, &ndlcs, err)) goto done;
    mods_off = game_mods_off(&g, mods_choice, game_arg_string(cj, "mods_off"), err);
    if (!mods_off) goto done;
    cheats_off = game_cheats_off(&g, cheats_choice, game_arg_string(cj, "cheats_off"), err);
    if (!cheats_off) goto done;
    if (update) {
        base = game_addon_base(&g, update->entry);
        program = snx_format("%s%s", base, GAME_PROGRAM_SUFFIX);
        if (!update->built || !snx_is_dir(update->data)) {
            if (!dry_run) {
                mcp_put(err, "the update is not built: call game_recompile with game=");
                mcp_put(err, g.folder);
                mcp_put(err, " and update=");
                mcp_put(err, update->entry);
                goto done;
            }
        }
    } else {
        base = game_program_base(g.program);
        program = snx_strdup(g.program);
        if ((!g.built || !g.has_data) && !dry_run) {
            if (!g.has_data) {
                mcp_put(err, "the game's data folder is missing: a work/ extraction is built from its dump first, "
                             "then game_recompile with game=");
            } else
                mcp_put(err, "the game is not built: call game_recompile with game=");
            mcp_put(err, g.folder);
            goto done;
        }
    }
    code = game_code_path(base);
    mods = game_mods_folder(&g);
    stamp = snx_game_stamp_for(snx_game_abi(), snx_mods_stamp(mods, mods_off));
    if (!dry_run &&
        (!program || !snx_is_file(program) || !code || !snx_is_file(code) || !snx_game_stamp_is(base, stamp))) {
        mcp_put(err, "the game is not built for this ABI and these mods: call game_recompile with game=");
        mcp_put(err, g.folder);
        if (update) {
            mcp_put(err, ", update=");
            mcp_put(err, update->entry);
        }
        mcp_put(err, " (and the same mods) before game_run");
        goto done;
    }
    out = run_out_dir(data, cache, &g, requested_out, err);
    if (!out) goto done;
    script = snx_strdup(script_arg ? script_arg : "");
    dlc_arg = game_dlc_arg(dlcs, ndlcs);
    base_command = cJSON_CreateArray();
    if (runtime && (!strcmp(runtime, "game") || !strcmp(runtime, "deployed")))
        game_command_add(base_command, program);
    else {
        game_command_add(base_command, self);
        {
            char* arg = snx_format("--game=%s", program);
            game_command_add(base_command, arg);
            free(arg);
        }
    }
    {
        char* arg = snx_format("--seconds=%ld", seconds);
        game_command_add(base_command, arg);
        free(arg);
    }
    if (frames > 0) {
        char* arg = snx_format("--frames=%ld", frames);
        game_command_add(base_command, arg);
        free(arg);
    }
    if (headless) game_command_add(base_command, "-H");
    {
        char* arg = snx_format("--addons=%s", dlc_arg);
        game_command_add(base_command, arg);
        free(arg);
        arg = snx_format("--mods_off=%s", mods_off);
        game_command_add(base_command, arg);
        free(arg);
        arg = snx_format("--cheats_off=%s", cheats_off);
        game_command_add(base_command, arg);
        free(arg);
    }
    if (script && *script) {
        char* arg = snx_format("--script=%s", script);
        game_command_add(base_command, arg);
        free(arg);
    }
    if (state_save) {
        char* arg = snx_format("--state_save=%s", state_save);
        game_command_add(base_command, arg);
        free(arg);
    }
    if (state_load) {
        char* arg = snx_format("--state_load=%s", state_load);
        game_command_add(base_command, arg);
        free(arg);
    }
    if (run_settings(base_command, settings, err)) goto done;
    if (tracy_capture) {
        game_command_add(base_command, "--tracy-capture");
        game_command_add(base_command, "--tracy_open_trace=0");
    } else if (tracy_client)
        game_command_add(base_command, "--tracy");
    root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "game", g.folder);
    if (g.has_id) {
        char id[17];
        game_title_text(g.id, id);
        cJSON_AddStringToObject(root, "title_id", id);
    }
    game_json_add_string(root, "update", update ? update->entry : NULL);
    {
        cJSON* list = cJSON_CreateArray();
        for (i = 0; i < ndlcs; ++i) cJSON_AddItemToArray(list, cJSON_CreateString(dlcs[i]));
        cJSON_AddItemToObject(root, "dlcs", list);
    }
    cJSON_AddStringToObject(root, "mods_off", mods_off);
    cJSON_AddStringToObject(root, "cheats_off", cheats_off);
    cJSON_AddStringToObject(root, "runtime",
                            runtime && (!strcmp(runtime, "game") || !strcmp(runtime, "deployed")) ? "game" : "current");
    cJSON_AddStringToObject(root, "program", program);
    cJSON_AddNumberToObject(root, "seconds", (double)seconds);
    cJSON_AddNumberToObject(root, "timeout", (double)(timeout > 0 ? timeout : 2 * seconds + 60));
    cJSON_AddNumberToObject(root, "runs", (double)runs);
    cJSON_AddBoolToObject(root, "headless", headless);
    cJSON_AddStringToObject(root, "out", out);
    if (tracy_capture)
        cJSON_AddStringToObject(root, "tracy", "capture");
    else if (tracy_client)
        cJSON_AddStringToObject(root, "tracy", "client");
    else
        cJSON_AddNullToObject(root, "tracy");
    first_command = run_command(base_command, out, 1, shot);
    cJSON_AddItemToObject(root, "command", first_command);
    if (dry_run) {
        cJSON_AddStringToObject(root, "status", "dry_run");
        cJSON_AddBoolToObject(root, "ok", 1);
        result = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
        goto done;
    }
    if (snx_mkdirs(out)) {
        cJSON_Delete(root);
        mcp_put(err, "cannot make the run's folder ");
        mcp_put(err, out);
        goto done;
    }
    runs_json = cJSON_CreateArray();
    for (i = 0; i < (size_t)runs; ++i) {
        cJSON* command = run_command(base_command, out, (long)i + 1, shot);
        const char** argv = run_argv(command);
        char *log = snx_format("%s/run%zu.log", out, i + 1), *snapshot = snx_format("%s/run%zu.game.log", out, i + 1);
        char* shot_path = shot ? snx_format("%s/run%zu.ppm", out, i + 1) : NULL;
        char* output = NULL;
        char* line = NULL;
        int rc, bad, timed_out = 0;
        long timeout_s = timeout > 0 ? timeout : 2 * seconds + 60;
        RunSummary summary;
        cJSON* item;
        rc = snx_run_timed(argv, g.folder, NULL, &output, (int)(timeout_s * 1000), &timed_out);
        if (snx_write_file(log, output ? output : "", output ? strlen(output) : 0)) {
            /* the run still happened; the answer says where its output should be */
        }
        if (g.log && snx_is_file(g.log)) {
            if (snx_copy_file(g.log, snapshot)) {
                free(snapshot);
                snapshot = NULL;
            }
        } else {
            free(snapshot);
            snapshot = NULL;
        }
        run_summary_parse(output, &summary);
        item = run_summary_json(&summary, seconds, rc, timed_out, &line, &bad);
        cJSON_AddNumberToObject(item, "run", (double)i + 1);
        cJSON_AddBoolToObject(item, "timed_out", timed_out);
        cJSON_AddStringToObject(item, "log", log);
        game_json_add_string(item, "game_log", g.log);
        game_json_add_string(item, "game_log_copy", snapshot);
        if (shot)
            game_json_add_string(item, "shot", shot_path);
        else
            cJSON_AddNullToObject(item, "shot");
        if (tracy_capture) {
            char* capture = run_newest_tracy(g.folder);
            game_json_add_string(item, "tracy", capture);
            cJSON_AddItemToArray(runs_json, item);
            free(capture);
        } else
            cJSON_AddItemToArray(runs_json, item);
        if (bad) ++failed;
        free(line);
        free(output);
        free(log);
        free(snapshot);
        free(shot_path);
        free(argv);
        cJSON_Delete(command);
        if (bad && stop) break;
    }
    cJSON_AddItemToObject(root, "runs", runs_json);
    cJSON_AddNumberToObject(root, "failed", (double)failed);
    cJSON_AddBoolToObject(root, "ok", failed == 0);
    cJSON_AddStringToObject(root, "status", "ran");
    /* the newest Tracy capture, if any, as the answer's convenience: each run has its own */
    if (tracy_capture) {
        tracy_path = run_newest_tracy(g.folder);
        game_json_add_string(root, "tracy_capture", tracy_path);
    }
    result = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
done:
    cJSON_Delete(base_command);
    free(tracy_path);
    free(dlc_arg);
    free(script);
    free(dlcs);
    free(mods);
    free(mods_off);
    free(cheats_off);
    free(base);
    free(program);
    free(code);
    free(stamp);
    free(out);
    addons_close(&addons);
    game_close(&g);
    return result;
}
