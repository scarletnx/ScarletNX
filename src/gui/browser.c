/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "gui/browser.h"
#include "common/utilities/common.h"
#include "common/utilities/cheat_list.h"
#include "gui/aot.h"
#include "common/utilities/mods.h"
#include "gui/i18n.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <unistd.h>
#endif

static bool suffix(const char* name, const char* extension) {
    size_t n = strlen(name), e = strlen(extension);
    if (n < e) return false;
    for (size_t i = 0; i < e; ++i)
        if (tolower((unsigned char)name[n - e + i]) != extension[i]) return false;
    return true;
}
bool gui_is_cia(const char* name) { return suffix(name, ".cia"); }
bool gui_is_vpk(const char* name) { return suffix(name, ".vpk"); }
bool gui_is_gba(const char* name) { return suffix(name, ".gba"); }
bool gui_is_dump(const char* name) {
    return suffix(name, ".nro") || suffix(name, ".xci") || suffix(name, ".nsp") || suffix(name, ".dxci") ||
           suffix(name, ".dnsp");
}
bool gui_game_folder(const char* path) {
    char *folder = snx_dirname(path), *name, data[SNX_PATH_SIZE];
    const char *own = path + strlen(path), *leaf = folder + strlen(folder);
    bool found;
    while (own > path && own[-1] != '/' && own[-1] != '\\') --own;
    while (leaf > folder && leaf[-1] != '/' && leaf[-1] != '\\') --leaf;
    name = snx_strdup(own);
#ifdef _WIN32
    if (suffix(name, ".exe")) name[strlen(name) - 4] = 0;
#endif
#ifdef SNX_IOS
    if (suffix(name, "_code.snxobj")) name[strlen(name) - 12] = 0; /* the pack is the game's program there */
#endif
    found = *name && (!strcmp(leaf, name) || snx_addon_name(leaf, name, NULL)) && snx_is_file(path) &&
            !snx_path_format(data, sizeof data, "%s/%s_data", folder, name) && snx_is_dir(data);
    free(name);
    free(folder);
    return found;
}
void gui_games_free(GuiGames* games) {
    snx_free_strings(games->names, games->count);
    snx_free_strings(games->shown, games->count);
    snx_free_strings(games->paths, games->count);
    free(games->addons);
    games->names = games->shown = games->paths = NULL;
    games->addons = NULL;
    games->count = games->room = 0;
}
uint64_t gui_tree_size(const char* path, const int* stop) {
    char** names = NULL;
    size_t count = 0, i;
    uint64_t total = 0;
    if (!*path || snx_list_dir(path, &names, &count)) return 0; /* "" is none: Windows would list the working folder */
    for (i = 0; i < count && !__atomic_load_n(stop, __ATOMIC_RELAXED); ++i) {
        char* entry = snx_path_join(path, names[i]);
        if (entry && !snx_is_link(entry))
            total += snx_is_dir(entry) ? gui_tree_size(entry, stop) : snx_file_size(entry);
        free(entry);
    }
    snx_free_strings(names, count);
    return total;
}
/* a game's code on an app that runs no program of its own: Android's library, or iOS's pack of objects
   (recompile --target ios), which the loader places in memory */
#ifdef SNX_IOS
#define GAME_CODE "_code.snxobj"
#else
#define GAME_CODE "_code.so"
#endif
/* whether a folder's entry is a game of kinds: a CIA or VPK, or one ready to run. Libraries often carry executable bits but
   are not games, and hidden files are not shown. On Android the shared storage runs nothing: a game is <name> beside
   <name>_code.so (name is cut there), and a dump put there, to be prepared, shows with its own name */
static bool playable(const char* directory, char* name, int kinds) {
    char* path;
    bool listed;
    if (name[0] == '.') return false;
    if (gui_is_cia(name) || gui_is_vpk(name) || gui_is_gba(name)) {
        if (!(kinds & GUI_GAMES_CIA)) return false;
        path = snx_path_join(directory, name);
        listed = path && snx_is_file(path);
        free(path);
        return listed;
    }
    if ((kinds & GUI_GAMES_DUMP) && gui_is_dump(name)) {
        path = snx_path_join(directory, name);
        listed = path && snx_is_file(path);
        free(path);
        return listed;
    }
    if (!(kinds & GUI_GAMES_RUN)) return false;
#if defined(__ANDROID__) || defined(SNX_IOS)
    { /* a game's code or a dump, a file: a folder named so is neither. An app's games have no runtime copy */
        bool code = suffix(name, GAME_CODE);
        if (!code && !gui_is_dump(name)) return false;
        path = snx_path_join(directory, name);
        listed = path && snx_is_file(path);
        free(path);
        if (listed && code) name[strlen(name) - strlen(GAME_CODE)] = 0;
        return listed;
    }
#else
#ifdef _WIN32
    if (!suffix(name, ".exe")) return false;
#else
    if (suffix(name, ".so") || strstr(name, ".so.") || suffix(name, ".dll") || suffix(name, ".dylib")) return false;
#endif
    path = snx_path_join(directory, name);
#ifndef _WIN32
    if (!path || access(path, X_OK)) {
        free(path);
        return false;
    }
#endif
    {
        char* executable = path ? snx_find_tool(path) : NULL; /* regular file, with UTF-8 paths on Windows */
        free(path);
        listed = executable != NULL;
        free(executable);
    }
    return listed;
#endif
}
/* a game in a folder of its own, <name>/<name> (<name>.exe on Windows, <name>_code.so on Android): where it is, else
   NULL. Any other folder (an older version's <name>_data/, the player's) is no game */
static char* folder_game(const char* directory, const char* name, int kinds) {
    char *folder, *own = NULL, *path = NULL;
    if (!(kinds & GUI_GAMES_RUN) || name[0] == '.') return NULL;
    folder = snx_path_join(directory, name);
    if (folder && snx_is_dir(folder) && !snx_is_link(folder)) {
#if defined(__ANDROID__) || defined(SNX_IOS)
        own = snx_format("%s" GAME_CODE, name);
#elif defined(_WIN32)
        own = snx_format("%s.exe", name);
#else
        own = snx_strdup(name);
#endif
        if (own && playable(folder, own, GUI_GAMES_RUN)) {
#ifdef SNX_IOS
            /* playable() cut the suffix: the runtime takes the pack itself, there is no program named <name> */
            char made[SNX_PATH_SIZE];
            path = snx_path_format(made, sizeof made, "%s/%s" GAME_CODE, folder, own) ? NULL : snx_strdup(made);
#else
            path = snx_path_join(folder, own);
#endif
        }
    }
    free(own);
    free(folder);
    return path;
}
char* gui_folder_game(const char* directory, const char* name) { return folder_game(directory, name, GUI_GAMES_RUN); }
/* what the game at path, named name, is shown as: the name in its <name>_data/control.nacp, which the recompile
   copies there, else name */
static char* shown_name(const char* path, const char* name) {
    char *folder = snx_dirname(path), *stem = snx_strdup(name), *shown = NULL, nacp[SNX_PATH_SIZE], text[0x200];
    unsigned char* bytes;
    size_t size = 0;
    if (stem && suffix(stem, ".exe")) stem[strlen(stem) - 4] = 0;
    bytes = folder && stem && !snx_path_format(nacp, sizeof nacp, "%s/%s_data/control.nacp", folder, stem) &&
                    snx_is_file(nacp)
                ? (unsigned char*)snx_read_file(nacp, &size)
                : NULL;
    if (bytes && snx_nacp_name(bytes, size, text, sizeof text)) shown = snx_strdup(text);
    free(bytes);
    free(stem);
    free(folder);
    return shown ? shown : snx_strdup(name);
}
char* gui_game_shown(const char* path) {
    char *folder = snx_dirname(path), *shown;
    const char* leaf = folder ? folder + strlen(folder) : NULL;
    if (!leaf) return NULL;
    while (leaf > folder && leaf[-1] != '/' && leaf[-1] != '\\') --leaf;
    shown = shown_name(path, leaf);
    free(folder);
    return shown;
}
char* gui_game_icon(const char* path) {
    char *folder, icon[SNX_PATH_SIZE];
    const char* leaf;
    int cut;
    if (!path || !gui_game_folder(path)) return NULL;
    folder = snx_dirname(path);
    leaf = folder + strlen(folder);
    while (leaf > folder && leaf[-1] != '/' && leaf[-1] != '\\') --leaf;
    cut = snx_path_format(icon, sizeof icon, "%s/%s_data/icon.jpg", folder, leaf);
    free(folder);
    return !cut && snx_is_file(icon) ? snx_strdup(icon) : NULL;
}
/* whether the add-on named a (<game>_<entry>) is listed before b: updates first, the newest version first (one
   without a version last), then the DLCs by title ID */
static bool addon_before(SnxU64 game, const char* a, const char* b) {
    const char *ea = strchr(a, '_') + 1, *eb = strchr(b, '_') + 1;
    SnxU64 ia = 0, ib = 0, va, vb;
    int ua, ub;
    snx_addon_entry_id(game, ea, &ia);
    snx_addon_entry_id(game, eb, &ib);
    ua = snx_addon_kind(game, ia) == SNX_ADDON_UPDATE;
    ub = snx_addon_kind(game, ib) == SNX_ADDON_UPDATE;
    if (ua != ub) return ua;
    if (!ua) return strcmp(ea, eb) < 0;
    va = snx_addon_version(ea);
    vb = snx_addon_version(eb);
    if (va == SNX_NO_VERSION || vb == SNX_NO_VERSION) return vb == SNX_NO_VERSION && va != SNX_NO_VERSION;
    return va > vb;
}
/* the updates and DLCs made beside the game at path, in its folder (snx_addon_name): each a data folder, an update's
   with its program beside it. Their count, and with addons not NULL each added to it, in their names' order (an
   update's title ID before a DLC's) */
static size_t addons_of(const char* path, GuiGames* addons) {
    char *folder = snx_dirname(path), **names = NULL;
    const char* leaf = folder + strlen(folder);
    const char* own = path + strlen(path);
    size_t count = 0, found = 0, i, j;
    SnxU64 game = 0, aoc_base = 0;
    while (leaf > folder && leaf[-1] != '/' && leaf[-1] != '\\') --leaf;
    if (snx_title_parse(leaf, &game) && addons) { /* what its DLCs are numbered from: its control.nacp's base ID */
        char file[SNX_PATH_SIZE];
        size_t size = 0;
        unsigned char* nacp = snx_path_format(file, sizeof file, "%s/%s_data/control.nacp", folder, leaf)
                                  ? NULL
                                  : (unsigned char*)snx_read_file(file, &size);
        aoc_base = snx_nacp_aoc_base(nacp, size, game);
        free(nacp);
    }
    while (own > path && own[-1] != '/' && own[-1] != '\\') --own;
    /* the game's own program, <name>/<name>: an add-on has none of its own */
    if (!gui_game_folder(path) || strncmp(own, leaf, strlen(leaf)) || (own[strlen(leaf)] && own[strlen(leaf)] != '.') ||
        snx_list_dir(folder, &names, &count))
        count = 0;
    for (i = 0; i < count; ++i) {
        size_t n = strlen(names[i]);
        char *stem = snx_strdup(names[i]), *data = snx_path_join(folder, names[i]), *program = NULL;
        char text[0x40], version[0x20], nacp_file[SNX_PATH_SIZE];
        SnxU64 id;
        /* <game>_<its ID>_data: a DLC's record, or an update's beside its program */
        bool listed = n > 5 && !strcmp(names[i] + n - 5, "_data") && snx_is_dir(data);
        if (listed) stem[n - 5] = 0;
        listed = listed && snx_addon_name(leaf, stem, &id);
        if (listed && snx_addon_kind(game, id) == SNX_ADDON_UPDATE) {
#if defined(__ANDROID__) || defined(SNX_IOS)
            char* file = snx_format("%s" GAME_CODE, stem);
#elif defined(_WIN32)
            char* file = snx_format("%s.exe", stem);
#else
            char* file = snx_strdup(stem);
#endif
            listed = playable(folder, file, GUI_GAMES_RUN);
            free(file);
#if defined(_WIN32)
            char made[SNX_PATH_SIZE];
            program = snx_path_format(made, sizeof made, "%s/%s.exe", folder, stem) ? NULL : snx_strdup(made);
            listed = listed && program;
#else
            program = snx_path_join(folder, stem);
#endif
        }
        if (listed && addons) {
            unsigned char* nacp;
            size_t size = 0;
            nacp = snx_addon_kind(game, id) == SNX_ADDON_UPDATE &&
                           !snx_path_format(nacp_file, sizeof nacp_file, "%s/control.nacp", data)
                       ? (unsigned char*)snx_read_file(nacp_file, &size)
                       : NULL;
            if (snx_addon_kind(game, id) == SNX_ADDON_DLC && id > aoc_base && id - aoc_base < 0x800)
                snprintf(text, sizeof text, snx_tr("DLC %u"), (unsigned)(id - aoc_base));
            else if (snx_addon_kind(game, id) == SNX_ADDON_DLC) { /* numbered from no base the game names */
                char title[17];
                snx_title_text(id, title);
                snprintf(text, sizeof text, snx_tr("DLC %s"), title);
            } else if (snx_nacp_version(nacp, size, version, sizeof version))
                snprintf(text, sizeof text, snx_tr("Update %s"), version);
            else
                snprintf(text, sizeof text, "%s", snx_tr("Update"));
            free(nacp);
            for (j = addons->count; j > 0 && addon_before(game, stem, addons->names[j - 1]); --j) {
                addons->names[j] = addons->names[j - 1];
                addons->shown[j] = addons->shown[j - 1];
                addons->paths[j] = addons->paths[j - 1];
                addons->addons[j] = 0;
            }
            addons->names[j] = snx_strdup(stem);
            addons->shown[j] = snx_strdup(text);
            addons->paths[j] = program ? snx_strdup(program) : snx_strdup(data);
            addons->addons[j] = 0;
            addons->count++;
        }
        found += listed;
        free(program);
        free(data);
        free(stem);
    }
    snx_free_strings(names, count);
    free(folder);
    return found;
}
bool gui_game_addons(const char* path, GuiGames* addons) {
    size_t room = addons_of(path, NULL) + 1;
    gui_games_free(addons);
    addons->names = (char**)calloc(room, sizeof *addons->names);
    addons->shown = (char**)calloc(room, sizeof *addons->shown);
    addons->paths = (char**)calloc(room, sizeof *addons->paths);
    addons->addons = (size_t*)calloc(room, sizeof *addons->addons);
    if (!addons->names || !addons->shown || !addons->paths || !addons->addons) {
        gui_games_free(addons);
        return false;
    }
    addons->room = room;
    addons_of(path, addons);
    return addons->count > 0;
}
size_t gui_game_mods(const char* path, char** folder, char*** names) {
    char *dir, data[SNX_PATH_SIZE];
    const char* leaf;
    size_t count;
    char** listed = NULL;
    *folder = NULL;
    if (names) *names = NULL;
    if (!gui_game_folder(path)) return 0;
    dir = snx_dirname(path);
    for (leaf = dir + strlen(dir); leaf > dir && leaf[-1] != '/' && leaf[-1] != '\\'; --leaf) continue;
    if (snx_path_format(data, sizeof data, "%s/%s_data", dir, leaf)) {
        free(dir);
        return 0;
    }
    *folder = snx_mods_folder(data);
    count = snx_mods_list(*folder, &listed);
    if (names)
        *names = listed;
    else
        snx_free_strings(listed, count);
    free(dir);
    return count;
}
size_t gui_game_cheats(const char* path, char** folder, char*** names) {
    char *dir, data[SNX_PATH_SIZE];
    const char* leaf;
    size_t count;
    char** listed = NULL;
    *folder = NULL;
    if (names) *names = NULL;
    if (!gui_game_folder(path)) return 0;
    dir = snx_dirname(path);
    for (leaf = dir + strlen(dir); leaf > dir && leaf[-1] != '/' && leaf[-1] != '\\'; --leaf) continue;
    if (snx_path_format(data, sizeof data, "%s/%s_data", dir, leaf)) {
        free(dir);
        return 0;
    }
    *folder = snx_cheats_folder(data);
    count = snx_cheats_list(*folder, &listed);
    if (names)
        *names = listed;
    else
        snx_free_strings(listed, count);
    free(dir);
    return count;
}
/* from's files and folders into to, depth folders under the mod's own one, whose link is followed: 0, else 1 */
static int copy_into(const char* from, const char* to, unsigned depth, char* why, size_t size) {
    char reason[256], **names = NULL;
    size_t n = 0, i;
    int rc = 0;
    if (depth && snx_is_link(from)) {
        snprintf(why, size, snx_tr("%s is a link, which a mod cannot hold"), from);
        return 1;
    }
    if (!snx_is_dir(from)) {
        if (!snx_copy_file(from, to)) return 0;
        snprintf(why, size, snx_tr("cannot copy %s to %s: %s"), from, to, snx_os_error(reason, sizeof reason));
        return 1;
    }
    if (depth > 128) { /* as snx_copy_tree stops */
        snprintf(why, size, snx_tr("%s is more than 128 folders deep"), from);
        return 1;
    }
    if (snx_mkdirs(to)) {
        snprintf(why, size, snx_tr("cannot make %s: %s"), to, snx_os_error(reason, sizeof reason));
        return 1;
    }
    if (snx_list_dir(from, &names, &n)) {
        snprintf(why, size, snx_tr("cannot open %s: %s"), from, snx_os_error(reason, sizeof reason));
        return 1;
    }
    for (i = 0; i < n && !rc; ++i) {
        char *a = snx_path_join(from, names[i]), *b = snx_path_join(to, names[i]);
        rc = copy_into(a, b, depth + 1, why, size);
        free(a);
        free(b);
    }
    snx_free_strings(names, n);
    return rc;
}
bool gui_copy_mod(const char* from, const char* to, char* why, size_t size) {
    char *folder = snx_dirname(to), *hidden, *staged, reason[256];
    const char* leaf = to + strlen(to);
    bool ok;
    while (leaf > to && leaf[-1] != '/' && leaf[-1] != '\\') --leaf;
    hidden = snx_format(".%s.adding", leaf);
    staged = snx_path_join(folder, hidden);
    if (snx_path_exists(staged) && snx_remove_tree(staged)) {
        snprintf(why, size, snx_tr("cannot take away %s, which a copy that stopped left: %s"), staged,
                 snx_os_error(reason, sizeof reason));
        ok = false;
    } else {
        ok = !copy_into(from, staged, 0, why, size);
        if (ok && snx_replace(staged, to)) {
            snprintf(why, size, snx_tr("cannot rename %s to %s: %s"), staged, to, snx_os_error(reason, sizeof reason));
            ok = false;
        }
        if (!ok && snx_path_exists(staged) && snx_remove_tree(staged)) { /* the next copy makes it again */
            size_t n = strlen(why);
            snprintf(why + n, size - n, snx_tr(" (%s stays, part of the copy)"), staged);
        }
    }
    free(staged);
    free(hidden);
    free(folder);
    return ok;
}
static int before(const char* a, const char* b) {
    for (; *a && tolower((unsigned char)*a) == tolower((unsigned char)*b); ++a, ++b) {}
    return tolower((unsigned char)*a) < tolower((unsigned char)*b);
}
/* directory's games into games, sorted from first on by what they are shown as; with depth, its folders' games too, down
   that many levels, a folder that is a game or hidden left alone */
/* room in the lists for n entries, never fewer than they have: a folder deeper in adds its games while its
   parent's are still to come, as many as it holds */
static bool games_room(GuiGames* games, size_t n) {
    char** more;
    size_t* addons;
    if (n <= games->room) return true;
    if (n < 2 * games->room) n = 2 * games->room;
    if (!(more = (char**)realloc(games->names, n * sizeof *more))) return false;
    games->names = more;
    if (!(more = (char**)realloc(games->shown, n * sizeof *more))) return false;
    games->shown = more;
    if (!(more = (char**)realloc(games->paths, n * sizeof *more))) return false;
    games->paths = more;
    if (!(addons = (size_t*)realloc(games->addons, n * sizeof *addons))) return false;
    games->addons = addons;
    games->room = n;
    return true;
}
static bool add_games(GuiGames* games, const char* directory, int kinds, size_t first, int depth, char* error,
                      size_t size) {
    char** names = NULL;
    size_t count = 0, i;
    if (snx_list_dir(directory, &names, &count)) {
        char why[256];
        snprintf(error, size, snx_tr("Cannot open %s: %s"), directory, snx_os_error(why, sizeof why));
        return false;
    }
    size_t j;
    for (i = 0; i < count; ++i) {
        char* path = folder_game(directory, names[i], kinds);
        char* shown;
        if (!path && playable(directory, names[i], kinds)) path = snx_path_join(directory, names[i]);
        if (!path && depth > 0 && names[i][0] != '.') { /* a folder of games, not one */
            char* sub = snx_path_join(directory, names[i]);
            if (sub && snx_is_dir(sub)) add_games(games, sub, kinds, first, depth - 1, error, 0);
            free(sub);
            continue;
        }
        if (!path) continue;
        if (!games_room(games, games->count + 2)) { /* it and the end */
            free(path);
            snx_free_strings(names, count);
            snprintf(error, size, snx_tr("Not enough memory to list %s."), directory);
            return false;
        }
        shown = shown_name(path, names[i]);
        if (!shown) {
            free(path);
            continue;
        }
        /* in among the folder's ones by what they are shown as, a game's folder being its title ID */
        for (j = games->count; j > first && before(shown, games->shown[j - 1]); --j) {
            games->names[j] = games->names[j - 1];
            games->shown[j] = games->shown[j - 1];
            games->paths[j] = games->paths[j - 1];
            games->addons[j] = games->addons[j - 1];
        }
        games->names[j] = names[i];
        games->shown[j] = shown;
        games->paths[j] = path;
        {
            char *mods, *cheats;
            games->addons[j] =
                addons_of(path, NULL) + gui_game_mods(path, &mods, NULL) + gui_game_cheats(path, &cheats, NULL);
            free(mods);
            free(cheats);
        }
        games->count++;
        names[i] = NULL;
    }
    snx_free_strings(names, count);
    return true;
}
bool gui_games_add(GuiGames* games, const char* directory, int kinds, char* error, size_t size) {
    return gui_games_add_deep(games, directory, kinds, 0, error, size);
}
bool gui_games_add_deep(GuiGames* games, const char* directory, int kinds, int depth, char* error, size_t size) {
    if (size) error[0] = 0;
    return add_games(games, directory, kinds, games->count, depth, error, size);
}
bool gui_games_heading(const GuiGames* games, size_t i) { return i < games->count && !games->paths[i]; }
static bool dump_entry(const GuiGames* games, size_t i) {
    return gui_is_dump(games->names[i]) || gui_is_cia(games->names[i]) || gui_is_vpk(games->names[i]) ||
           gui_is_gba(games->names[i]);
}
bool gui_games_group(GuiGames* games, const char* ready, const char* dumps,
                     bool (*made)(const char* path, const void* user), const void* user) {
    size_t room = games->count + 2, n = 0, i, in[2] = {0, 0};
    char* heading[2][2] = {{NULL, NULL}, {NULL, NULL}}; /* each group's name and text */
    GuiGames out;
    int group;
    bool ok;
    for (i = 0; i < games->count; ++i) /* no dumps' heading: none of the second group counted, so none moved */
        if (games->paths[i] && !(dump_entry(games, i) && (!dumps || (made && made(games->paths[i], user)))))
            in[dump_entry(games, i)]++;
    memset(&out, 0, sizeof out);
    out.names = (char**)calloc(room, sizeof *out.names);
    out.shown = (char**)calloc(room, sizeof *out.shown);
    out.paths = (char**)calloc(room, sizeof *out.paths);
    out.addons = (size_t*)calloc(room, sizeof *out.addons);
    out.room = room;
    ok = out.names && out.shown && out.paths && out.addons;
    for (group = 0; group < 2 && ok; ++group)
        if (in[0] && in[1]) { /* one group alone has nothing to be told apart from */
            heading[group][0] = snx_strdup("");
            heading[group][1] = snx_strdup(group ? dumps : ready);
            ok = heading[group][0] && heading[group][1];
        }
    if (!ok) { /* nothing moved yet: games stays as it was */
        for (group = 0; group < 2; ++group) {
            free(heading[group][0]);
            free(heading[group][1]);
        }
        gui_games_free(&out);
        return false;
    }
    for (group = 0; group < 2; ++group) {
        if (!in[group]) continue;
        if (heading[group][1]) {
            out.names[n] = heading[group][0];
            out.shown[n++] = heading[group][1];
        }
        for (i = 0; i < games->count; ++i) {
            if (!games->paths[i] || (int)dump_entry(games, i) != group) continue; /* an earlier heading goes */
            if (group && made && made(games->paths[i], user)) continue;
            out.names[n] = games->names[i];
            out.shown[n] = games->shown[i];
            out.paths[n] = games->paths[i];
            out.addons[n++] = games->addons[i];
            games->names[i] = games->shown[i] = games->paths[i] = NULL; /* moved to out */
        }
    }
    out.count = n;
    gui_games_free(games); /* what was left out: dumps made already, earlier headings */
    *games = out;
    return true;
}
char* gui_game_log(const char* path) {
    char *folder = snx_dirname(path), *name, *state, *log;
    const char* leaf;
    if (!folder) return NULL;
    if (gui_game_folder(path)) { /* <name>/<name>: its folder's name, whatever the program's suffix */
        for (leaf = folder + strlen(folder); leaf > folder && leaf[-1] != '/' && leaf[-1] != '\\'; --leaf) continue;
        name = snx_strdup(leaf);
    } else {
        for (leaf = path + strlen(path); leaf > path && leaf[-1] != '/' && leaf[-1] != '\\'; --leaf) continue;
        name = snx_strdup(leaf);
        if (suffix(name, ".exe")) name[strlen(name) - 4] = 0;
    }
    state = name ? gui_game_kind_dir(folder, name, SNX_USER_STATE) : NULL;
    log = state ? snx_format("%s/%s.log", state, name) : NULL;
    free(state);
    free(name);
    free(folder);
    return log;
}
bool gui_games_by_last_opened(GuiGames* games) {
    SnxU64* times = (SnxU64*)calloc(games->count + 1, sizeof *times);
    size_t i, j;
    if (!times) return false;
    for (i = 0; i < games->count; ++i)
        if (games->paths[i] && !dump_entry(games, i)) { /* a game ready to run: its log's, 0 for none */
            char* log = gui_game_log(games->paths[i]);
            if (!log) {
                free(times);
                return false;
            }
            times[i] = snx_file_time(log);
            free(log);
        }
    /* each moved up past those that ran before it, not past a heading: the others' order kept */
    for (i = 1; i < games->count; ++i)
        for (j = i; j > 0 && games->paths[j] && games->paths[j - 1] && times[j - 1] < times[j]; --j) {
            char *name = games->names[j], *shown = games->shown[j], *path = games->paths[j];
            size_t addons = games->addons[j];
            SnxU64 time = times[j];
            games->names[j] = games->names[j - 1];
            games->shown[j] = games->shown[j - 1];
            games->paths[j] = games->paths[j - 1];
            games->addons[j] = games->addons[j - 1];
            times[j] = times[j - 1];
            games->names[j - 1] = name;
            games->shown[j - 1] = shown;
            games->paths[j - 1] = path;
            games->addons[j - 1] = addons;
            times[j - 1] = time;
        }
    free(times);
    return true;
}
bool gui_has_folders(const char* directory, int kinds) {
    char** names = NULL;
    size_t count = 0, i;
    bool found = false;
    if (snx_list_dir(directory, &names, &count)) return false;
    for (i = 0; i < count && !found; ++i) {
        char *sub, *game;
        if (names[i][0] == '.') continue;
        sub = snx_path_join(directory, names[i]);
        game = folder_game(directory, names[i], kinds);
        found = sub && snx_is_dir(sub) && !game;
        free(game);
        free(sub);
    }
    snx_free_strings(names, count);
    return found;
}
bool gui_games_scan(GuiGames* games, const char* directory, bool cia, char* error, size_t size) {
    gui_games_free(games);
    return gui_games_add(games, directory, cia ? GUI_GAMES_CIA : GUI_GAMES_RUN, error, size);
}
