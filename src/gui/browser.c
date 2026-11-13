/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "gui/browser.h"
#include "common/utilities/common.h"
#include "common/platform/file.h"
#include "common/utilities/cheat_list.h"
#include "gui/aot.h"
#include "common/utilities/mods.h"
#include "common/utilities/game_ini.h"
#include "common/utilities/ini_lines.h"
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
bool gui_is_cia(const char* name) {
    /* a 3DS game by its extension: the driver reads the file's own header, this is the browser's filter. A
       .bcci/.bcxi/.bcia is a bundle of one, as Azahar's is */
    return suffix(name, ".cia") || suffix(name, ".cci") || suffix(name, ".3ds") || suffix(name, ".cxi") ||
           suffix(name, ".app") || suffix(name, ".hbb") || suffix(name, ".bcci") || suffix(name, ".bcxi") ||
           suffix(name, ".bcia") || suffix(name, ".zcci") || suffix(name, ".zcxi") || suffix(name, ".zcia");
}
bool gui_is_vpk(const char* name) { return suffix(name, ".vpk"); }
/* a GBA ROM: a cartridge's image, or a MultiBoot ROM (.mb), the program the console receives through the link cable */
bool gui_is_gba(const char* name) { return suffix(name, ".gba") || gui_is_multiboot(name); }
bool gui_is_multiboot(const char* name) { return suffix(name, ".mb"); }
/* an Android app, one APK or a bundle of split APKs (an XAPK, an APKS) that scarletnx-android installs */
bool gui_is_apk(const char* name) { return suffix(name, ".apk") || suffix(name, ".xapk") || suffix(name, ".apks"); }
bool gui_is_dump(const char* name) {
    return suffix(name, ".nro") || suffix(name, ".xci") || suffix(name, ".nsp") || suffix(name, ".dxci") ||
           suffix(name, ".dnsp") || suffix(name, "/");
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
static void sets_free(GuiAddonDumps* sets, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        free(sets[i].name);
        free(sets[i].program);
        free(sets[i].dump);
        snx_free_strings(sets[i].files, sets[i].count + sets[i].left);
    }
    free(sets);
}
void gui_games_free(GuiGames* games) {
    snx_free_strings(games->names, games->count);
    snx_free_strings(games->shown, games->count);
    snx_free_strings(games->paths, games->count);
    free(games->addons);
    sets_free(games->sets, games->set_count);
    games->names = games->shown = games->paths = NULL;
    games->addons = NULL;
    games->sets = NULL;
    games->count = games->room = games->set_count = 0;
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
    if (gui_is_cia(name) || gui_is_vpk(name) || gui_is_gba(name) || gui_is_apk(name)) {
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
/* an extracted game's folder at path shown: the name in its control.nacp, else its title ID, which its ExeFS's NPDM
   gives, as a dump without a name; its folder's name for one without either */
static char* extracted_shown(const char* path, const char* name) {
    char nacp[SNX_PATH_SIZE], npdm[SNX_PATH_SIZE], text[0x200], *exefs = snx_extracted_part(path, 0);
    unsigned char* bytes = NULL;
    size_t size = 0;
    if (!snx_path_format(nacp, sizeof nacp, "%s/control.nacp", path) && snx_is_file(nacp))
        bytes = (unsigned char*)snx_read_file(nacp, &size);
    if (bytes && snx_nacp_name(bytes, size, text, sizeof text)) {
        free(bytes);
        free(exefs);
        return snx_strdup(text);
    }
    free(bytes);
    bytes = exefs && !snx_path_format(npdm, sizeof npdm, "%s/main.npdm", exefs) && snx_is_file(npdm)
                ? (unsigned char*)snx_read_file(npdm, &size)
                : NULL;
    free(exefs);
    if (bytes && snx_title_text(snx_npdm_title(bytes, size), text)) {
        free(bytes);
        return snx_strdup(text);
    }
    free(bytes);
    snprintf(text, sizeof text, "%s", name);
    if (*text) text[strlen(text) - 1] = 0; /* its / */
    return snx_strdup(text);
}
/* a Vita game's name, the TITLE in the sce_sys/param.sfo of its folder (the update's before the game's); owned, NULL
   for a folder that has none */
static char* vita_name(const char* folder) {
    static const char* const parts[] = {"patch", "app"};
    for (size_t i = 0; i < sizeof parts / sizeof *parts; ++i) {
        char file[SNX_PATH_SIZE], *title;
        size_t size = 0;
        unsigned char* sfo = !snx_path_format(file, sizeof file, "%s/%s/sce_sys/param.sfo", folder, parts[i]) &&
                                     snx_is_file(file)
                                 ? (unsigned char*)snx_read_file(file, &size)
                                 : NULL;
        title = sfo ? snx_sfo_string(sfo, size, "TITLE") : NULL;
        free(sfo);
        if (title && *title) return title;
        free(title);
    }
    return NULL;
}
/* whether path is a game's program in the folder named for it, <folder>/<name> (.exe cut, _code.snxobj on iOS): a 3DS
   game has no <name>_data folder, so gui_game_folder's test does not hold for it */
bool gui_game_program(const char* path) {
    char *folder, *name;
    const char *leaf, *own;
    bool found;
    if (!path || !snx_is_file(path) || !(folder = snx_dirname(path))) return false;
    leaf = folder + strlen(folder);
    while (leaf > folder && leaf[-1] != '/' && leaf[-1] != '\\') --leaf;
    own = path + strlen(path);
    while (own > path && own[-1] != '/' && own[-1] != '\\') --own;
    name = snx_strdup(own);
#ifdef _WIN32
    if (name && suffix(name, ".exe")) name[strlen(name) - 4] = 0;
#endif
#ifdef SNX_IOS
    if (name && suffix(name, "_code.snxobj")) name[strlen(name) - 12] = 0;
#endif
#ifdef __ANDROID__
    if (name && suffix(name, GAME_CODE)) name[strlen(name) - strlen(GAME_CODE)] = 0;
#endif
    found = name && *name && !strcmp(leaf, name);
    free(name);
    free(folder);
    return found;
}
/* what the game at path, named name, is shown as: the name in its <name>_data/control.nacp, which the recompile
   copies there, or a Vita game's in its param.sfo, or the one its driver recorded in an .ini, else name */
static char* shown_name(const char* path, const char* name) {
    if (suffix(name, "/")) return extracted_shown(path, name);
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
    if (!shown && folder) shown = vita_name(folder);
    /* the name the driver recorded for a game whose dump carries none: a title ID says nothing */
    if (!shown && folder && stem) {
        char* ini = snx_game_ini(folder, stem);
        if (ini) shown = snx_ini_value(ini, "name");
        free(ini);
    }
    /* the driver's own .ini beside the game, as the 3DS driver writes it (its folder is not divided from the config) */
    if (!shown && folder && stem) {
        char* ini = snx_format("%s/%s.ini", folder, stem);
        if (ini) shown = snx_ini_value(ini, "name");
        free(ini);
    }
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
/* a Switch game's file to play from when it is not there, owned: source= in its .ini, or an older version's link
   <name>.source by where it points, naming a dump or an extracted game's folder that it reads in place, its ExeFS not
   extracted into its <name>_data (driver.c's drop_extraction); else NULL */
static char* switch_missing(const char* folder, const char* name) {
    char *source, *target, link[SNX_PATH_SIZE], exefs[SNX_PATH_SIZE];
    int recorded = 0;
    if (snx_path_format(link, sizeof link, "%s/%s.source", folder, name) ||
        snx_path_format(exefs, sizeof exefs, "%s/%s_data/exefs", folder, name) ||
        !(source = snx_game_source(folder, name, &recorded)))
        return NULL;
    if (recorded && !strcmp(source, link) && (target = snx_link_target(link)) != NULL) {
        free(source);
        source = target;
    }
    if (!recorded || snx_is_file(source) || snx_extracted_game(source) || snx_is_dir(exefs)) {
        free(source);
        return NULL;
    }
    return source;
}
/* a GBA game's ROM, which its driver reads at every run, from source= in the .ini of its folder (src/agb/runtime),
   owned when it is not there; else NULL */
static char* gba_missing(const char* folder, const char* name) {
    char* ini = snx_format("%s/%s.ini", folder, name);
    char* rom = ini ? snx_ini_value(ini, "source") : NULL;
    free(ini);
    if (rom && snx_is_file(rom)) {
        free(rom);
        return NULL;
    }
    return rom;
}
/* a 3DS game's file, as its driver plays the game in its folder (src/3ds/runtime/main.c's play_folder), owned when
   it is not there: source= in the .ini of its folder, with no extraction in the folder (exheader.bin) and no copy of it
   decrypted in the driver's folder of them, <work>/3ds/<its name>.decrypted.<its extension>, which the driver reads
   in its place; else NULL */
static char* ctr_missing(const char* folder, const char* name, const char* work) {
    char *ini = snx_format("%s/%s.ini", folder, name), *source = ini ? snx_ini_value(ini, "source") : NULL;
    char *dumped = source ? snx_dump_name(source) : NULL, exheader[SNX_PATH_SIZE], copy[SNX_PATH_SIZE];
    const char* dot = source ? strrchr(source, '.') : NULL;
    bool there = !source || snx_is_file(source) ||
                 (!snx_path_format(exheader, sizeof exheader, "%s/exheader.bin", folder) && snx_is_file(exheader)) ||
                 (work && dumped &&
                  !snx_path_format(copy, sizeof copy, "%s/3ds/%s.decrypted.%s", work, dumped, dot ? dot + 1 : "game") &&
                  snx_is_file(copy));
    free(dumped);
    free(ini);
    if (there) {
        free(source);
        return NULL;
    }
    return source;
}
/* a Vita game made from a dumped game's folder, which the driver links as its app/ (src/vita/runtime/main.c's
   link_dump): where that link points, owned, when it is gone; else NULL, as for a game extracted from a VPK, which keeps
   its app/ */
static char* vita_missing(const char* folder) {
    char app[SNX_PATH_SIZE], *target;
    if (snx_path_format(app, sizeof app, "%s/app", folder) || !snx_is_link(app) || snx_is_dir(app)) return NULL;
    target = snx_link_target(app);
    return target ? target : snx_strdup(app);
}
bool gui_game_source_missing(const char* path, const char* work, char* out, size_t size) {
    char *folder, *name, *file = NULL;
    if (out && size) out[0] = 0;
    if (!gui_game_program(path) || !(folder = snx_dirname(path))) return false;
    name = snx_strdup(snx_path_leaf(folder));
    switch (snx_game_system(folder, name)) {
        case SNX_SYSTEM_SWITCH: file = switch_missing(folder, name); break;
        case SNX_SYSTEM_GBA:
        case SNX_SYSTEM_NDS: file = gba_missing(folder, name); break; /* a card image, named as a ROM is */
        case SNX_SYSTEM_3DS: file = ctr_missing(folder, name, work); break;
        case SNX_SYSTEM_VITA: file = vita_missing(folder); break;
    }
    if (file && out && size) snprintf(out, size, "%s", file);
    free(file);
    free(name);
    free(folder);
    return file != NULL;
}
char* gui_game_icon(const char* path) {
    char *folder, icon[SNX_PATH_SIZE], *found = NULL;
    const char* leaf;
    static const char* const vita[] = {"patch", "app"}; /* the folders the Vita driver reads a game's files from */
    size_t i;
    if (!path || !(folder = snx_dirname(path))) return NULL;
    leaf = folder + strlen(folder);
    while (leaf > folder && leaf[-1] != '/' && leaf[-1] != '\\') --leaf;
    if (gui_game_folder(path) && !snx_path_format(icon, sizeof icon, "%s/%s_data/icon.jpg", folder, leaf) &&
        snx_is_file(icon))
        found = snx_strdup(icon);
    /* a Vita game keeps its dump beside its program: its own icon0.png, the update's before the game's */
    for (i = 0; !found && i < sizeof vita / sizeof *vita; ++i)
        if (!snx_path_format(icon, sizeof icon, "%s/%s/sce_sys/icon0.png", folder, vita[i]) && snx_is_file(icon))
            found = snx_strdup(icon);
    /* a 3DS game has no data folder: its driver writes the SMDH's icon beside the game (icon.rgba); a GBA game's box
       goes there too, as the launcher downloads it (Settings > GBA > Covers) */
    if (!found && gui_game_program(path) && !snx_path_format(icon, sizeof icon, "%s/icon.rgba", folder) &&
        snx_is_file(icon))
        found = snx_strdup(icon);
    if (!found && gui_game_program(path) && !snx_path_format(icon, sizeof icon, "%s/icon.png", folder) &&
        snx_is_file(icon))
        found = snx_strdup(icon);
    free(folder);
    return found;
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
        /* another program of a collection's is no add-on: its game plays it (gui_game_programs) */
        listed = listed && snx_addon_name(leaf, stem, &id) && snx_addon_kind(game, id) != SNX_ADDON_PROGRAM;
        /* an update with a program of its own, listed once it is made; a 3DS game's has none, its game reads it */
        if (listed && snx_addon_own_program(game, id)) {
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
            else if (!snx_addon_own_program(game, id) && snx_addon_version(stem + strlen(leaf) + 1) != SNX_NO_VERSION) {
                /* a 3DS game's update: its TMD's title version */
                snprintf(version, sizeof version, "v%llu",
                         (unsigned long long)snx_addon_version(stem + strlen(leaf) + 1));
                snprintf(text, sizeof text, snx_tr("Update %s"), version);
            } else
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
int gui_game_programs(const char* path, int* programs) {
    unsigned char made[SNX_PROGRAMS_MAX];
    char* dir;
    const char* leaf;
    int i, count = 0;
    if (!gui_game_folder(path)) return 0;
    dir = snx_dirname(path);
    for (leaf = dir + strlen(dir); leaf > dir && leaf[-1] != '/' && leaf[-1] != '\\'; --leaf) continue;
    snx_game_programs(dir, leaf, made);
    for (i = 1; i < SNX_PROGRAMS_MAX; i++)
        if (made[i]) programs[count++] = i;
    free(dir);
    return count;
}
char* gui_ready_game(const char* file, const char* games, const char* name) {
    char *folder = file ? snx_dirname(file) : snx_path_join(games, name), *game = snx_path_join(folder, name);
    free(folder);
    return game;
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
        if (ok && plat_replace_file(staged, to)) {
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
        if (!path && (kinds & GUI_GAMES_DUMP) && names[i][0] != '.') { /* an extracted game's folder: named with a / */
            char* sub = snx_path_join(directory, names[i]);
            if (sub && snx_extracted_game(sub)) {
                char* named = snx_format("%s/", names[i]);
                free(names[i]);
                names[i] = named;
                path = sub;
            } else
                free(sub);
        }
        if (!path && depth > 0 && names[i][0] != '.') { /* a folder of games, not one */
            /* a recompiled game's folder holds its program's NRO and its romfs's, which are no dumps of the player's,
               when this library lists only dumps */
            char *sub = snx_path_join(directory, names[i]), *game = folder_game(directory, names[i], GUI_GAMES_RUN);
            if (sub && !game && snx_is_dir(sub)) add_games(games, sub, kinds, first, depth - 1, error, 0);
            free(game);
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
bool gui_games_dumps_group(const GuiGames* games, size_t i) {
    return gui_is_dump(games->names[i]) || gui_is_cia(games->names[i]) || gui_is_vpk(games->names[i]) ||
           gui_is_gba(games->names[i]) || gui_is_apk(games->names[i]);
}
bool gui_dump_title(const char* path, GuiDumpTitle* title) {
    size_t size = 0;
    char *text = path ? (char*)snx_read_file(path, &size) : NULL, *key, *value, *end;
    SnxIni ini;
    bool own = false, of = false;
    memset(title, 0, sizeof *title);
    title->version = SNX_NO_VERSION;
    if (!text) return false;
    snx_ini_start(&ini, text);
    while (snx_ini_next(&ini, &key, &value))
        if (!strcmp(key, "title"))
            own = snx_title_parse(value, &title->title) != 0;
        else if (!strcmp(key, "game"))
            of = snx_title_parse(value, &title->game) != 0;
        else if (!strcmp(key, "version") && *value >= '0' && *value <= '9') {
            unsigned long long version = strtoull(value, &end, 10);
            title->version = *end ? SNX_NO_VERSION : (uint64_t)version;
        } else if (!strcmp(key, "name"))
            snprintf(title->name, sizeof title->name, "%s", value);
        else if (!strcmp(key, "display_version"))
            snprintf(title->display, sizeof title->display, "%s", value);
    free(text);
    return own && of;
}
const GuiAddonDumps* gui_games_addon_set(const GuiGames* games, size_t i) {
    if (i >= games->count || !games->paths[i]) return NULL;
    for (size_t k = 0; k < games->set_count; ++k)
        if (!strcmp(games->sets[k].files[0], games->paths[i])) return &games->sets[k];
    return NULL;
}
/* an update's version a is newer than b's: one without any is older than every other */
static bool newer(uint64_t a, uint64_t b) { return a != SNX_NO_VERSION && (b == SNX_NO_VERSION || a > b); }
/* what an Updates and DLCs item is shown as: its game's name, its update's version and how many DLCs. Owned */
static char* set_text(const GuiAddonDumps* set) {
    size_t dlcs = set->count - set->update;
    if (set->update && dlcs == 1) return snx_format(snx_tr("%s: update %s + 1 DLC"), set->name, set->version);
    if (set->update && dlcs) return snx_format(snx_tr("%s: update %s + %zu DLCs"), set->name, set->version, dlcs);
    if (set->update) return snx_format(snx_tr("%s: update %s"), set->name, set->version);
    return dlcs == 1 ? snx_format(snx_tr("%s: 1 DLC"), set->name) : snx_format(snx_tr("%s: %zu DLCs"), set->name, dlcs);
}
/* whether the program at path is the game id's own, <id>/<id> as Games lists it */
static bool program_of(const char* path, const char* id) {
    char* folder = gui_game_folder(path) ? snx_dirname(path) : NULL;
    bool own = folder && !strcmp(snx_path_leaf(folder), id);
    free(folder);
    return own;
}
/* An entry gui_games_group groups: a row of the list, or a dump of an item the list holds already (an earlier
   grouping's), file its index in the item's files */
typedef struct {
    size_t row, file;   /* file: SIZE_MAX for the row itself */
    int group;          /* 0 ready, 1 a dump, 2 an update's or a DLC's, -1 left out */
    size_t set;         /* an update's or a DLC's item, in sets */
    GuiDumpTitle title; /* a dump's, when titled knows it: title 0 else */
} GroupEntry;
static const char* entry_path(const GuiGames* games, const GroupEntry* e) {
    const GuiAddonDumps* set = gui_games_addon_set(games, e->row);
    return e->file == SIZE_MAX ? games->paths[e->row] : set->files[e->file];
}
/* the items' dumps, each set's: the newest update's first, then each DLC's by its title ID, a second dump of one left
   out; the game they go in (its program among the ready games) or with (its dump among the dumps); its name and its
   update's version */
static void sets_fill(const GuiGames* games, GroupEntry* entries, size_t count, GuiAddonDumps* sets, size_t set_count) {
    for (size_t k = 0; k < set_count; ++k) {
        GuiAddonDumps* set = &sets[k];
        size_t i, members = 0, update = count;
        char id[17];
        const char* named = NULL;
        uint64_t last = 0;
        snx_title_text(set->game, id);
        for (i = 0; i < count; ++i)
            if (entries[i].group == 2 && entries[i].set == k) {
                members++;
                if (snx_addon_kind(set->game, entries[i].title.title) == SNX_ADDON_UPDATE &&
                    (update == count || newer(entries[i].title.version, entries[update].title.version)))
                    update = i;
            }
        set->files = (char**)snx_alloc(members * sizeof *set->files);
        if (update < count) {
            const GuiDumpTitle* t = &entries[update].title;
            set->files[set->count++] = snx_strdup(entry_path(games, &entries[update]));
            set->update = true;
            if (t->display[0])
                snprintf(set->version, sizeof set->version, "%s", t->display);
            else if (t->version != SNX_NO_VERSION)
                snprintf(set->version, sizeof set->version, "v%llu", (unsigned long long)t->version);
            else
                snprintf(set->version, sizeof set->version, "?");
            if (t->name[0]) named = t->name;
        }
        for (;;) {
            size_t take = count;
            for (i = 0; i < count; ++i)
                if (entries[i].group == 2 && entries[i].set == k &&
                    snx_addon_kind(set->game, entries[i].title.title) == SNX_ADDON_DLC &&
                    entries[i].title.title > last &&
                    (take == count || entries[i].title.title < entries[take].title.title))
                    take = i;
            if (take == count) break;
            set->files[set->count++] = snx_strdup(entry_path(games, &entries[take]));
            last = entries[take].title.title;
        }
        for (i = 0; i < count; ++i) { /* the others, kept to be grouped again */
            const char* path = entry_path(games, &entries[i]);
            bool in = false;
            if (entries[i].group != 2 || entries[i].set != k) continue;
            for (size_t f = 0; f < set->count && !in; ++f) in = !strcmp(set->files[f], path);
            if (!in) set->files[set->count + set->left++] = snx_strdup(path);
        }
        for (i = 0; i < count && !set->program; ++i)
            if (entries[i].group == 0 && entries[i].file == SIZE_MAX && program_of(games->paths[entries[i].row], id))
                set->program = snx_strdup(games->paths[entries[i].row]);
        for (i = 0; i < count && !set->program && !set->dump; ++i)
            if (entries[i].group == 1 && entries[i].title.title == set->game && entries[i].title.game == set->game) {
                set->dump = snx_strdup(entry_path(games, &entries[i]));
                if (!named && entries[i].title.name[0]) named = entries[i].title.name;
            }
        set->name = named ? snx_strdup(named) : set->program ? gui_game_shown(set->program) : snx_strdup(id);
    }
}
bool gui_games_group(GuiGames* games, const GuiGroups* groups) {
    const char* texts[3] = {groups->ready, groups->dumps, groups->addons};
    size_t count = 0, room, n = 0, i, k, set_count = 0, in[3] = {0, 0, 0}, filled = 0;
    GroupEntry* entries;
    GuiAddonDumps* sets;
    char** shown;
    GuiGames out = {0};
    int group;
    /* an item's dumps are entries of their own again, and its row the first's */
    for (i = 0; i < games->count; ++i) {
        const GuiAddonDumps* set = gui_games_addon_set(games, i);
        count += set ? set->count + set->left : 1;
    }
    entries = (GroupEntry*)calloc(count + 1, sizeof *entries);
    sets = (GuiAddonDumps*)calloc(count + 1, sizeof *sets);
    shown = (char**)calloc(count + 1, sizeof *shown);
    if (!entries || !sets || !shown) {
        free(entries);
        free(sets);
        free(shown);
        return false;
    }
    for (i = 0; i < games->count; ++i) {
        const GuiAddonDumps* set = gui_games_addon_set(games, i);
        for (k = 0; k < (set ? set->count + set->left : 1); ++k) {
            entries[filled].row = i;
            entries[filled++].file = set ? k : SIZE_MAX;
        }
    }
    for (i = 0; i < count; ++i) {
        GroupEntry* e = &entries[i];
        const char* path = entry_path(games, e);
        int kind;
        if (!path) { /* an earlier heading goes */
            e->group = -1;
            continue;
        }
        if (e->file == SIZE_MAX && !gui_games_dumps_group(games, e->row)) { /* ready: 0 */
            if (groups->removed && groups->removed(path, groups->user)) e->group = -1;
            continue;
        }
        e->group = !groups->dumps || (groups->made && groups->made(path, groups->user)) ? -1 : 1;
        if (e->group < 0 || !gui_is_dump(snx_path_leaf(path)) || !groups->titled ||
            !groups->titled(path, groups->user, &e->title)) {
            memset(&e->title, 0, sizeof e->title);
            continue;
        }
        kind = snx_addon_kind(e->title.game, e->title.title);
        if (kind != SNX_ADDON_UPDATE && kind != SNX_ADDON_DLC) continue;
        e->group = 2;
        for (k = 0; k < set_count && sets[k].game != e->title.game; ++k) {}
        if (k == set_count) sets[set_count++].game = e->title.game;
        e->set = k;
    }
    sets_fill(games, entries, count, sets, set_count);
    for (k = 0; k < set_count; ++k) { /* by what each is shown as */
        GuiAddonDumps item = sets[k];
        char* text = set_text(&item);
        for (i = k; i > 0 && before(text, shown[i - 1]); --i) {
            sets[i] = sets[i - 1];
            shown[i] = shown[i - 1];
        }
        sets[i] = item;
        shown[i] = text;
    }
    for (i = 0; i < count; ++i)
        if (entries[i].group == 0 || entries[i].group == 1) in[entries[i].group]++;
    in[2] = set_count;
    room = count + 4; /* and the headings */
    out.names = (char**)calloc(room, sizeof *out.names);
    out.shown = (char**)calloc(room, sizeof *out.shown);
    out.paths = (char**)calloc(room, sizeof *out.paths);
    out.addons = (size_t*)calloc(room, sizeof *out.addons);
    out.room = room;
    if (!out.names || !out.shown || !out.paths || !out.addons) { /* games as it was */
        gui_games_free(&out);
        snx_free_strings(shown, set_count);
        sets_free(sets, set_count);
        free(entries);
        return false;
    }
    for (group = 0; group < 3; ++group) {
        if (!in[group]) continue;
        if ((in[0] > 0) + (in[1] > 0) + (in[2] > 0) > 1) { /* one group alone has nothing to be told apart from */
            out.names[n] = snx_strdup("");
            out.shown[n] = snx_strdup(texts[group] ? texts[group] : "");
            out.paths[n] = NULL;
            out.addons[n++] = 0;
        }
        for (i = 0; group < 2 && i < count; ++i) {
            const GroupEntry* e = &entries[i];
            const char* path = entry_path(games, e);
            if (e->group != group) continue;
            out.names[n] = snx_strdup(e->file == SIZE_MAX ? games->names[e->row] : snx_path_leaf(path));
            out.shown[n] = snx_strdup(e->file == SIZE_MAX ? games->shown[e->row] : snx_path_leaf(path));
            out.paths[n] = snx_strdup(path);
            out.addons[n++] = e->file == SIZE_MAX ? games->addons[e->row] : 0;
        }
        for (k = 0; group == 2 && k < set_count; ++k) {
            out.names[n] = snx_strdup(snx_path_leaf(sets[k].files[0]));
            out.shown[n] = shown[k];
            shown[k] = NULL;
            out.paths[n] = snx_strdup(sets[k].files[0]);
            out.addons[n++] = 0;
        }
    }
    out.count = n;
    out.sets = sets;
    out.set_count = set_count;
    gui_games_free(games); /* what was left out: dumps made already, earlier headings */
    *games = out;
    free(shown);
    free(entries);
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
        if (games->paths[i] && !gui_games_dumps_group(games, i)) { /* a game ready to run: its log's, 0 for none */
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
