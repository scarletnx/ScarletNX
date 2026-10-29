/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "nx/runtime/driver/container.h"
#include "nx/runtime/driver/driver.h"
#include "common/utilities/common.h"
#include "common/utilities/demangle.h"
#include "common/utilities/logscan.h"
#include "nx/runtime/driver/extract.h"
#include "nx/runtime/driver/game_name.h"
#include "nx/runtime/driver/recompile.h"
#include "nx/runtime/driver/setup.h"
#include "common/utilities/mods.h"
#include "common/utilities/setup.h"
#include "nx/runtime/cli.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#ifndef SNX_SOURCE_DIR
#define SNX_SOURCE_DIR "."
#endif
#ifndef SNX_BUILD_DIR
#define SNX_BUILD_DIR "build"
#endif
/* play_dump's: the game made and ready, not played (an update's or a DLC's game, before it) */
#define PLAY_PREPARE 0x10000u
/* a game's program file: <name>.exe on Windows, <name> elsewhere */
#ifdef _WIN32
#define PROGRAM_SUFFIX ".exe"
#else
#define PROGRAM_SUFFIX ""
#endif

static void usage(void) {
    puts("Usage: scarletnx [OPTIONS] INPUT [-- GAME_ARGUMENTS...]\n"
         "  INPUT                  Extract, recompile if needed, then play XCI/NSP/DXCI/DNSP/NRO\n"
         "  -x, --extract          Extract XCI/NSP/DXCI/DNSP\n"
         "  -r, --recompile        Recompile ExeFS/NRO, or prepare a dump, without playing\n"
         "  -H, --headless         Run without a window or swapchain");
    puts("  --tracy                Start the Tracy client for a GUI connection (SNX_TRACY builds)\n"
         "  --tracy-capture        Record the run with tracy-capture, from Tracy 0.14.1 on PATH (SNX_TRACY builds)");
    puts(
        "  -f, --fullscreen       Start fullscreen for this run (--fullscreen=0: windowed); else the game's .ini decides\n"
        "  --touch=0|1|2          The game's touch screen: off, fingers, or fingers and the mouse\n"
        "  --controls=0|1         The on-screen controls: hidden or shown\n"
        "  --KEY=VALUE            Any setting of the game's .ini, for this run\n"
        "  --settings[=recompile] List the settings, tab-separated");
    puts("  --update FILE          An update's or a DLC's dump, played or prepared with the game; once each\n"
         "  -x -h / -r -h          Extraction / recompilation options\n"
         "  --game=DIR             Play the recompiled game in DIR (recompiled/<title ID>)");
    puts("  --extract-container    Extract the dump into the game's data folder; without it the game reads the dump\n"
         "                         in place\n"
         "  --tmpfs                Extract to tmpfs, <temp>/scarletnx/work/<title ID> (implies --extract-container)\n"
         "  -O LEVEL               The recompile's optimization: 0, 1, 2, 3, s or z (default: 2); a game made\n"
         "                         already keeps its code until -r");
    puts("  --outdir DIR           Where the game's folder goes (default: recompiled/)\n"
         "  --out DIR              The dump's extraction (default: work/<its name>)\n"
         "  --data DIR             The data folder: recompiled/, work/ and the keys (default: a\n"
         "                         .scarletnx folder beside the AppImage or the program,\n"
         "                         else the user's data folder,\n"
#ifdef _WIN32
         "                         %APPDATA%\\scarletnx)");
#elif defined(__APPLE__)
         "                         ~/Library/Application Support/scarletnx)");
#else
         "                         ~/.local/share/scarletnx)");
#endif
    puts("  --keys FILE            prod.keys for this run (default: the data folder, then ~/.switch)\n"
         "  --title-keys FILE      title.keys for this run, found the same way (optional)");
    puts("  --setup                Go through the setup in the terminal: the language, the install, the\n"
         "                         folders, the keys and the first user (asked by itself the first time\n"
         "                         a terminal runs a dump, or after an update says what changed)\n"
         "  --no-setup             Leave the setup out of this run, writing nothing");
    puts("  --build-dir DIR        Override this build's build directory\n"
         "  --dump-modules PROGRAM DIR\n"
         "                         Write an extracted program's (or an NRO's) modules as laid out\n"
         "                         in guest memory into DIR, for tools/symbols.py and tools/disasm.py");
    puts("  --scan-log LOG... [--format json|markdown] [--name NAME]\n"
         "                         Summarize a game's log or the launcher's Share Log file: the build,\n"
         "                         the game, how the run ended and where, unimplemented services");
#ifndef _WIN32
    puts("  -S                     Enable the native GDB stub on localhost:1234\n"
         "  -s                     Enable the stub and stop at startup\n"
         "Short options may be combined, for example -rx or -Hs.");
#endif
}
static int is_nro_path(const char* path) {
    const char* ext = strrchr(path, '.');
    return ext && strlen(ext) == 4 && tolower((unsigned char)ext[1]) == 'n' && tolower((unsigned char)ext[2]) == 'r' &&
           tolower((unsigned char)ext[3]) == 'o';
}
/* the keys' options of a stage, --keys and --title-keys as the driver has them, put in command at n: the next n */
static int add_keys(const char** command, int n, const SnxPaths* paths) {
    if (paths->keys) {
        command[n++] = "--keys";
        command[n++] = paths->keys;
    }
    if (paths->title_keys) {
        command[n++] = "--title-keys";
        command[n++] = paths->title_keys;
    }
    return n;
}
/* --keys or --title-keys, as a flag in this driver's arguments. Sets consumed to 1 when its value is next;
   returns 0 for another option, -1 on malformed input. */
static int keys_option(const char* arg, const char* next, const char** prod_flag, const char** title_flag,
                       int* consumed) {
    const char *equal = strchr(arg, '='), *value = NULL;
    *consumed = 0;
    if (strncmp(arg, "--", 2)) return 0;
    if (!strcmp(arg, "--keys") || !strcmp(arg, "--title-keys")) {
        value = next;
        if (!value || !value[0] || value[0] == '-') {
            snx_error("%s requires a file", arg);
            return -1;
        }
        *consumed = 1;
        if (!strcmp(arg, "--keys")) *prod_flag = value;
        else *title_flag = value;
        return 1;
    }
    if (equal && (!strncmp(arg, "--keys=", 7) || !strncmp(arg, "--title-keys=", 13))) {
        value = equal + 1;
        if (!value[0]) {
            snx_error("%.*s requires a file", (int)(equal - arg), arg);
            return -1;
        }
        if (!strncmp(arg, "--keys=", 7)) *prod_flag = value;
        else *title_flag = value;
        return 1;
    }
    return 0;
}
/* A key flag's file, made absolute because the stages run from other folders. NULL, said why, when it is not a file. */
static char* keys_flag_file(const char* key, const char* value) {
    char* path = snx_absolute(value);
    if (!path) {
        snx_error("cannot resolve %s: %s", key, value);
        return NULL;
    }
    if (!snx_is_file(path)) {
        snx_error("%s is not a file: %s", key, path);
        free(path);
        return NULL;
    }
    return path;
}
/* 1 when the files at a and b hold the same bytes */
static int same_bytes(const char* a, const char* b) {
    FILE *x = snx_fopen(a, "rb"), *y = snx_fopen(b, "rb");
    unsigned char p[16384], q[16384];
    size_t n;
    int same = x && y;
    while (same && (n = fread(p, 1, sizeof p, x)) > 0) same = fread(q, 1, n, y) == n && !memcmp(p, q, n);
    same = same && !ferror(x) && fread(q, 1, 1, y) == 0 && !ferror(y);
    if (x) fclose(x);
    if (y) fclose(y);
    return same;
}
/* 1 when the runtime a game runs carries a Vulkan driver its folder has not (snx_vulkan_driver): the deploy puts it
   there, and a folder that lost it gets it again (docs/APPLE.md) */
static int driver_missing(const char* run, const char* game) {
#if defined(__APPLE__) && !defined(SNX_IOS)
    char* carried = snx_vulkan_driver(run);
    char* there = carried ? snx_vulkan_driver(game) : NULL;
    int missing = carried && !there;
    free(carried);
    free(there);
    return missing;
#else
    (void)run;
    (void)game;
    return 0;
#endif
}
/* A game made by another build of this ABI runs the code it was recompiled into (stamped), with its folder's copy of
   that build's runtime: replaced by this build's when the two differ, so its fixes reach the games made before it. 0,
   or nonzero with a line saying why */
static int refresh_runtime(const SnxPaths* paths, const char* game) {
    char* run = snx_own_runtime(paths);
    int rc = 0, same;
    /* a build without the runtime deploys one it builds, not itself: there is nothing here to hold the copy to */
    if (!run) return 0;
    same = same_bytes(run, game);
    /* the library beside the runtime is deployed with it (snx_deploy_runtime): a folder that lost it gets it back */
    if (!same || driver_missing(run, game)) {
        rc = snx_deploy_runtime(run, game);
        if (!rc && !same) printf("%s ran another build's runtime: it runs %s now\n", game, run);
    }
    free(run);
    return rc;
}
#ifndef SNX_IOS /* the pack of an app has no stamp: the loader checks its ABI */
/* whether this build made the game at base (<folder>/<name>, a game's or its update's), with the mods its game plays
   now (mods.h's snx_game_stamp_is) */
static int stamped(const char* base) {
    char *made = snx_game_mods_stamp(base), *expected = snx_game_stamp(made);
    int same = snx_game_stamp_is(base, expected);
    free(expected);
    free(made);
    return same;
}
#endif
/* a game the recompile made, named by its folder (recompiled/<id>) or its program there (recompiled/<id>/<id>, .exe on
   Windows): 1 with its folder and name, owned when asked for. A game an older version laid out beside the others is not
   one */
static int prepared_game(const char* input, char** folder, char** name) {
    char *path = snx_absolute(input), *dir = NULL, *own = NULL, program[SNX_PATH_SIZE], data[SNX_PATH_SIZE];
    const char* leaf;
    size_t n;
    int found = 0;
    if (!path) return 0;
    n = strlen(path);
    while (n > 1 && (path[n - 1] == '/' || path[n - 1] == '\\')) path[--n] = 0;
    dir = snx_is_dir(path) ? snx_strdup(path) : snx_is_file(path) ? snx_dirname(path) : NULL;
    for (leaf = path + n; leaf > path && leaf[-1] != '/' && leaf[-1] != '\\'; --leaf) continue;
    own = snx_strdup(leaf);
#ifdef _WIN32
    n = strlen(own);
    if (n > 4 && own[n - 4] == '.' && tolower((unsigned char)own[n - 3]) == 'e' &&
        tolower((unsigned char)own[n - 2]) == 'x' && tolower((unsigned char)own[n - 1]) == 'e' && !snx_is_dir(path))
        own[n - 4] = 0;
#endif
    if (dir) {
        for (leaf = dir + strlen(dir); leaf > dir && leaf[-1] != '/' && leaf[-1] != '\\'; --leaf) continue;
        found = *own && !strcmp(leaf, own) &&
                !snx_path_format(program, sizeof program, "%s/%s" PROGRAM_SUFFIX, dir, own) && snx_is_file(program) &&
                !snx_path_format(data, sizeof data, "%s/%s_data", dir, own) && snx_is_dir(data);
    }
    if (found && folder) {
        *folder = dir;
        dir = NULL;
    }
    if (found && name) {
        *name = own;
        own = NULL;
    }
    free(path);
    free(dir);
    free(own);
    return found;
}
/* an add-on the recompile made, named by its program in its game's folder (recompiled/<id>/<id>_<add-on's id>, .exe on
   Windows): 1 with that folder, its name and its title ID, owned */
static int prepared_addon(const char* input, char** folder, char** name, SnxU64* addon) {
    char *path = snx_absolute(input), *dir, *own;
    const char* leaf;
    size_t n;
    int found = 0;
    if (!path || !snx_is_file(path)) {
        free(path);
        return 0;
    }
    dir = snx_dirname(path);
    for (leaf = path + strlen(path); leaf > path && leaf[-1] != '/' && leaf[-1] != '\\'; --leaf) continue;
    own = snx_strdup(leaf);
    n = strlen(own);
#ifdef _WIN32
    if (n > 4 && own[n - 4] == '.' && tolower((unsigned char)own[n - 3]) == 'e' &&
        tolower((unsigned char)own[n - 2]) == 'x' && tolower((unsigned char)own[n - 1]) == 'e')
        own[n - 4] = 0;
#endif
    (void)n;
    for (leaf = dir + strlen(dir); leaf > dir && leaf[-1] != '/' && leaf[-1] != '\\'; --leaf) continue;
    found = snx_addon_name(leaf, own, addon);
    if (found) {
        *folder = dir;
        *name = own;
    } else {
        free(dir);
        free(own);
    }
    free(path);
    return found;
}
/* a game an older version laid out beside the others (games/<name>, games/<name>_data/), which runs where it is but is
   neither played nor recompiled by the driver until it has a folder of its own */
static int left_beside(const char* input) {
    size_t n = strlen(input);
    char* data;
    int found;
#ifdef _WIN32
    if (n > 4 && input[n - 4] == '.' && tolower((unsigned char)input[n - 3]) == 'e' &&
        tolower((unsigned char)input[n - 2]) == 'x' && tolower((unsigned char)input[n - 1]) == 'e')
        n -= 4;
#endif
    data = snx_format("%.*s_data", (int)n, input);
    found = snx_is_file(input) && snx_is_dir(data) && !prepared_game(input, NULL, NULL);
    free(data);
    if (found) { /* an add-on beside its game, in the game's folder */
        char *folder, *name;
        SnxU64 addon;
        if (prepared_addon(input, &folder, &name, &addon)) {
            free(folder);
            free(name);
            found = 0;
        }
    }
    return found;
}
/* dump extracted straight into target, the game's data folder or its folder in tmpfs, with an update's over it or a
   DLC's image beside it when update is not NULL: what an extraction left there taken away first, its saves and the
   rest kept. made, the part of target the extraction makes (exefs, a DLC's image), is there once it is done. marker,
   <name>.extracting in the game's folder, says it is incomplete until it is done */
static int extract_game(const char** command, const char* dump, const char* update, const char* target,
                        const char* made, const char* marker, const SnxPaths* paths) {
    static const char* const parts[] = {"exefs", "romfs", "control.nacp", "control.romfs"};
    char *folder = snx_dirname(marker), *part;
    size_t i;
    int n = 5, failed = snx_mkdirs(folder) || snx_write_file(marker, "", 0);
    free(folder);
    for (i = 0; !failed && i < sizeof parts / sizeof *parts; ++i) {
        part = snx_path_join(target, parts[i]);
        failed = snx_path_exists(part) && snx_remove_tree(part);
        free(part);
    }
    if (failed) return snx_error("cannot prepare %s for an extraction", target);
    command[n++] = "-x";
    command[n++] = "--out";
    command[n++] = target;
    if (update) {
        command[n++] = "--update";
        command[n++] = update;
    }
    n = add_keys(command, n, paths);
    command[n++] = "--";
    command[n++] = dump;
    command[n] = NULL;
    failed = snx_run_stage(command, NULL, "Extraction"); /* its status, the run's */
    if (failed) return failed;
    part = snx_path_join(target, made);
    failed = !snx_path_exists(part);
    free(part);
    if (failed) return snx_error("extraction finished without creating %s/%s", target, made);
    return snx_remove_tree(marker) ? snx_error("cannot remove %s", marker) : 0;
}
/* Extract dump off: a game read from its dump in place, source (recorded: 1 when its .ini or its old link
   names one, snx_game_source), takes away what an extraction left in its data folder, and in tmpfs (by the title ID of
   its ExeFS's npdm, its marker said so). A game whose dump is not there keeps its extraction, a line saying so, and
   one made from no dump is left as it is. 0, or nonzero when something could not be taken away or the game has
   nothing to run from */
static int drop_extraction(const char* data, const char* source, int recorded, const char* npdm, const char* marker) {
    static const char* const parts[] = {"exefs", "romfs", "control.romfs", "romfs.copying"};
    unsigned char* text;
    char *part, *work, *exefs;
    size_t i, size;
    int rc = 0, extracted;
    if (!recorded) return 0; /* made from no dump: an NRO, an ExeFS folder */
    if (!snx_is_file(source)) {
        exefs = snx_path_join(data, "exefs");
        extracted = snx_is_dir(exefs);
        free(exefs);
        if (extracted)
            printf("Runs from its extraction: its dump %s is not there\n", source);
        else
            rc = snx_error("cannot read the game from its dump: %s is not there. Put it back, or play it with "
                           "--extract-container",
                           source);
        return rc;
    }
    text = snx_is_file(marker) ? snx_read_file(npdm, &size) : NULL;
    work = text ? snx_tmpfs_work(snx_npdm_title(text, size)) : NULL;
    free(text);
    rc = (work && snx_is_dir(work) && snx_remove_tree(work)) || (snx_is_file(marker) && snx_remove_tree(marker));
    free(work);
    for (i = 0; !rc && i < sizeof parts / sizeof *parts; ++i) {
        part = snx_path_join(data, parts[i]);
        if (snx_path_exists(part) || snx_is_link(part)) {
            printf("Removing %s: the game reads it from its dump\n", part);
            rc = snx_remove_tree(part);
        }
        free(part);
    }
    if (rc) snx_error("cannot take the extraction away from %s", data);
    return rc;
}
/* The game's RomFS where Extract to tmpfs wants it before it runs. On: its extraction in snx_tmpfs_work, the copy in
   its data folder taken away, and <name>.tmpfs in its folder saying so. Off: a game with that file gets its RomFS back
   in its data folder, and the extraction in tmpfs is taken away. Extracted again, straight where it goes, from the dump
   it was made from (snx_game_source) when it is in neither (a reboot empties tmpfs) or its extraction stopped, or said why not. An
   NRO's romfs.bin and a program without a title ID stay as they are. 0, or nonzero when an extraction or a copy
   failed */
static int ensure_romfs(const char** command, const char* folder, const char* name, int tmpfs, int extract,
                        const SnxPaths* paths) {
    char *base = snx_path_join(folder, name), *data = snx_format("%s_data", base);
    int recorded;
    char* source = snx_game_source(folder, name, &recorded);
    char *disk = snx_path_join(data, "romfs"), *bin = snx_path_join(data, "romfs.bin"), *marker = NULL;
    char *npdm = snx_path_join(data, "exefs/main.npdm"), *work = NULL, *ram = NULL, *staged = NULL, *exefs = NULL;
    char* extracting = snx_format("%s.extracting", base);
    unsigned char* text;
    size_t size;
    int rc = !source; /* its path did not fit, said */
    if (rc) goto done;
    if (!extract) { /* read in place: what an extraction left, here or in tmpfs, taken away */
        rc = drop_extraction(data, source, recorded, npdm, marker = snx_format("%s.tmpfs", base));
        goto done;
    }
    exefs = snx_path_join(data, "exefs");
    if (!snx_is_dir(exefs) && snx_is_file(source)) { /* made from its dump in place: extracted now */
        rc = extract_game(command, source, NULL, data, "exefs", extracting, paths);
        if (rc) goto done;
    }
    text = snx_read_file(npdm, &size);
    work = text ? snx_tmpfs_work(snx_npdm_title(text, size)) : NULL;
    free(text);
    if (!work || snx_is_file(bin)) goto done;
    ram = snx_path_join(work, "romfs");
    marker = snx_format("%s.tmpfs", base);
    if (tmpfs && snx_is_dir(work) && !snx_is_file(extracting)) { /* there already */
        rc = (snx_is_dir(disk) && snx_remove_tree(disk)) || snx_write_file(marker, "", 0);
        goto done;
    }
    if (!tmpfs && (snx_is_dir(disk) || !snx_is_file(marker))) { /* on disk, or never in tmpfs */
        rc = (snx_is_dir(disk) && snx_is_dir(work) && snx_remove_tree(work)) ||
             (snx_is_file(marker) && snx_remove_tree(marker));
        goto done;
    }
    if (!tmpfs && snx_is_dir(ram)) { /* tmpfs's copied into its data folder */
        staged = snx_format("%s.copying", disk);
        rc = (snx_path_exists(staged) && snx_remove_tree(staged)) || snx_copy_tree(ram, staged) ||
             snx_replace(staged, disk) || snx_remove_tree(work) || snx_remove_tree(marker);
        if (rc) snx_error("cannot copy the RomFS in %s into %s", ram, data);
        goto done;
    }
    if (!snx_is_file(source)) {
        if (recorded)
            printf("The game cannot be extracted %s: its dump %s is not there\n",
                   tmpfs ? "to tmpfs" : "again from tmpfs's emptied copy", source);
        else
            printf(
                "The game cannot be extracted %s: its .ini names no dump (source=). Play the dump once to record it\n",
                tmpfs ? "to tmpfs" : "again from tmpfs's emptied copy");
        goto done;
    }
    rc = extract_game(command, source, NULL, tmpfs ? work : data, "exefs", extracting, paths);
    if (!rc && tmpfs)
        rc = (snx_is_dir(disk) && snx_remove_tree(disk)) || snx_write_file(marker, "", 0);
    else if (!rc)
        rc = snx_remove_tree(marker);
done:
    free(base);
    free(data);
    free(source);
    free(disk);
    free(bin);
    free(marker);
    free(npdm);
    free(work);
    free(ram);
    free(staged);
    free(exefs);
    free(extracting);
    return rc;
}
/* the title ID a game's name starts with, its add-on's (<game>_<own>) too: 0 for a game named otherwise */
static SnxU64 game_id(const char* name) {
    char text[17];
    SnxU64 id = 0;
    if (strlen(name) < 16) return 0;
    memcpy(text, name, 16);
    text[16] = 0;
    return snx_title_parse(text, &id) && (!name[16] || name[16] == '_') ? id : 0;
}
/* An add-on's files where Extract dump wants them before it runs (the runtime lays its data folder over its
   game's, fs.c). On: extracted into its data folder, an update's ExeFS and RomFS over its game's dump (game), a DLC's
   RomFS image as system/archives/<its title ID>.romfs, from its dump. Off: what an extraction left there taken away,
   the add-on read from its dump in place. 0, or nonzero when an extraction failed or a part could not be taken away */
static int ensure_addon(const char** command, const char* folder, const char* name, SnxU64 addon, const char* dump,
                        const char* game, int extract, const SnxPaths* paths) {
    char *base = snx_path_join(folder, name), *data = snx_format("%s_data", base), text[17], *made, *path;
    char* extracting = snx_format("%s.extracting", base);
    int update = snx_addon_kind(game_id(name), addon) == SNX_ADDON_UPDATE, rc = 0, i;
    snx_title_text(addon, text);
    made = update ? snx_strdup("exefs") : snx_format("system/archives/%s.romfs", text);
    path = snx_path_join(data, made);
    if (!extract) {
        const char* parts[3];
        parts[0] = "exefs";
        parts[1] = "romfs";
        parts[2] = made;
        for (i = 0; !rc && i < 3; ++i) {
            char* part = snx_path_join(data, parts[i]);
            if (snx_path_exists(part)) {
                printf("Removing %s: the %s reads it from its dump\n", part, update ? "update" : "DLC");
                rc = snx_remove_tree(part);
                if (rc) snx_error("cannot take the extraction away from %s", data);
            }
            free(part);
        }
        if (!rc && snx_is_file(extracting)) rc = snx_remove_tree(extracting);
    } else if (!snx_path_exists(path) || snx_is_file(extracting)) {
        if (update && (!game || !snx_is_file(game)))
            rc = game ? snx_error("the update goes over its game's dump, and the game's .ini names %s, which is not "
                                  "there: play the game's dump to record it",
                                  game)
                      : snx_error(
                            "the update goes over its game's dump, and the game's .ini names none: play the game's "
                            "dump to record it");
        else
            rc = update ? extract_game(command, game, dump, data, made, extracting, paths)
                        : extract_game(command, dump, NULL, data, made, extracting, paths);
    }
    free(base);
    free(data);
    free(made);
    free(path);
    free(extracting);
    return rc;
}
/* the game's checked updates and DLCs (addons= in its .ini, snx_addons_parse), malloc'd SNX_ADDONS_MAX long: their
   count in *count */
static SnxAddon* checked_addons(const char* folder, const char* name, size_t* count) {
    char *ini = snx_game_ini(folder, name), *text = snx_ini_value(ini, "addons");
    SnxAddon* list = (SnxAddon*)snx_alloc(SNX_ADDONS_MAX * sizeof *list);
    *count = snx_addons_parse(text, name, list, SNX_ADDONS_MAX);
    free(text);
    free(ini);
    return list;
}
/* the add-on of entry checked in the game's addons=, what else is checked kept: an update in place of the one
   checked, of whichever version (every version of it has the same title ID), so the update installed last is the
   one played, older or newer. 0, or nonzero with a line saying why */
static int check_addon(const char* folder, const char* name, const char* entry) {
    size_t count, i;
    SnxAddon* list = checked_addons(folder, name, &count);
    char *ini, *text;
    SnxU64 id = 0;
    int rc;
    snx_addon_entry_id(game_id(name), entry, &id);
    for (i = 0; i < count && list[i].id != id; i++) continue;
    if (i == count && count < SNX_ADDONS_MAX) list[count++].id = id;
    if (i < count && strlen(entry) < sizeof list[i].entry) strcpy(list[i].entry, entry);
    ini = snx_game_ini(folder, name);
    text = snx_addons_text(list, count);
    rc = snx_ini_record(ini, "addons", text);
    free(text);
    free(ini);
    free(list);
    return rc;
}
/* the program that plays the game named name in folder: its checked update's, when it has one made, else its own.
   Owned */
static char* game_program(const char* folder, const char* name) {
    size_t count, i;
    SnxAddon* list = checked_addons(folder, name, &count);
    char program[SNX_PATH_SIZE];
    int found = 0;
    for (i = 0; i < count && !found; i++)
        found = snx_addon_kind(game_id(name), list[i].id) == SNX_ADDON_UPDATE &&
                !snx_path_format(program, sizeof program, "%s/%s_%s" PROGRAM_SUFFIX, folder, name, list[i].entry) &&
                snx_is_file(program);
    free(list);
    if (!found && snx_path_format(program, sizeof program, "%s/%s" PROGRAM_SUFFIX, folder, name)) return NULL;
    return snx_strdup(program);
}
/* The game's checked DLCs readied as ensure_addon readies one, each from the dump its own .ini names; one whose dump
   is not there is left to the runtime, which plays without it and says so. 0, or nonzero when one failed */
static int ensure_dlcs(const char** command, const char* folder, const char* name, int extract,
                       const SnxPaths* paths) {
    size_t count, i;
    SnxAddon* list = checked_addons(folder, name, &count);
    char *own, *dump;
    int rc = 0;
    for (i = 0; !rc && i < count; i++) {
        if (snx_addon_kind(game_id(name), list[i].id) != SNX_ADDON_DLC) continue;
        own = snx_format("%s_%s", name, list[i].entry);
        dump = snx_game_source(folder, own, NULL);
        if (!dump)
            rc = 1; /* its path did not fit, said */
        else if (snx_is_file(dump) || !extract)
            rc = ensure_addon(command, folder, own, list[i].id, dump, NULL, extract, paths);
        free(own);
        free(dump);
    }
    free(list);
    return rc;
}
/* what an older version made of a DLC as a program of its own (its runtime's copy, its code, its stamp, its hints),
   taken away: a DLC plays with its game now. 0, or nonzero with a line saying why */
static int drop_dlc_program(const char* folder, const char* name) {
    static const char* const parts[] = {"", ".exe", "_code.so", "_code.dll", ".abi", ".hints", ".jitcache"};
    size_t i;
    int rc = 0;
    for (i = 0; !rc && i < sizeof parts / sizeof *parts; ++i) {
        char part[SNX_PATH_SIZE];
        if (snx_path_format(part, sizeof part, "%s/%s%s", folder, name, parts[i])) return 1;
        if (snx_is_file(part)) {
            printf("Removing %s: a DLC has no program, it plays with its game\n", part);
            rc = snx_remove_tree(part);
            if (rc) snx_error("cannot remove %s", part);
        }
    }
    return rc;
}
/* game name's folder: in the folder outdir names, else the data folder's recompiled/ (games/ of an older version while
   the game is there, snx_recompiled_game) */
static char* game_folder(const char* outdir, const char* repo, const char* name) {
    char *games, *folder;
    if (!outdir) return snx_recompiled_game(repo, name);
    games = snx_absolute(outdir);
    folder = games ? snx_path_join(games, name) : NULL;
    free(games);
    return folder;
}
/* recompiled/ of the data folder, else the folder outdir names. A dump is extracted straight into its game's data
   folder, recompiled/<title ID>/<title ID>_data (its title ID read from its NCAs' headers first), or with --tmpfs into
   tmpfs, and its game made from it there: no copy is kept. --out names a folder the extraction is kept in instead, the
   game made from a copy. A game made already stays where it is */
/* whether the recompile of the game from source is given name, its folder's: only when it would name it otherwise by
   itself (snx_game_name), or cannot read its NPDM to, as a game named by its title ID is then shown by the name in
   its control.nacp, not by the ID */
static int named_otherwise(const char* source, const char* name) {
    SnxNpdm npdm;
    char error[256], *own;
    int otherwise;
    if (snx_load_npdm(source, &npdm, error, sizeof error)) return 1;
    own = snx_game_name(NULL, &npdm, source);
    otherwise = !own || strcmp(own, name) != 0;
    free(own);
    return otherwise;
}
static int play_dump(const char* input, const SnxPaths* paths, unsigned flags, const char* outdir, const char* out,
                     const char* opt, int recompiling, char** recompile, int settings, char** setting, int game_argc,
                     char** game_argv, const char* const* updates, int nupdates);
static int play_dump_at(const char* input, const SnxPaths* paths, unsigned flags, const char* outdir, const char* out,
                     const char* opt, int recompiling, char** recompile, int settings, char** setting, int game_argc,
                     char** game_argv, const char* const* updates, int nupdates) {
    char *dumped = NULL, *dump = snx_absolute(input), *name = NULL, *games = NULL, *extracted = NULL, *work = NULL;
    char *pending = NULL, *game = NULL, *folder = NULL, *base = NULL, *code = NULL, *source = NULL, error[256];
    char* game_dump = NULL; /* an update's game's dump, which its RomFS is read over */
    SnxU64 addon = 0, owner = 0; /* an update or a DLC, and the game it belongs to */
    const char** command =
        (const char**)snx_alloc((size_t)(game_argc + settings + recompiling + 24) * sizeof(*command));
    int rc = 1, nro = is_nro_path(input), prepared = prepared_game(input, &folder, &name), named = 0, n, j;
    /* -rx on a dump extracts it, as --extract-container does: -r alone reads it in place */
    int tmpfs = (flags & SNX_TMPFS) != 0, extract = tmpfs || (flags & (SNX_EXTRACT_CONTAINER | SNX_EXTRACT)) != 0;
    SnxNpdm npdm;
    command[0] = paths->executable;
    command[1] = "--data";
    command[2] = paths->repo;
    command[3] = "--build-dir";
    command[4] = paths->build;
    if (tmpfs && out) {
        snx_error("--out and --tmpfs cannot go together: --tmpfs puts the extraction in tmpfs");
        goto done;
    }
    if (nupdates) { /* the game made first, then each update or DLC made and checked, then the game played with them */
        SnxU64 title, own, of_game;
        char of[17], *program;
        if (out || nro || prepared) {
            snx_error("--update takes a game's dump, and an update or a DLC goes into the game's folder");
            goto done;
        }
        rc = play_dump(input, paths, flags | PLAY_PREPARE, outdir, NULL, opt, recompiling, recompile, 0, NULL, 0, NULL,
                       NULL, 0);
        if (!rc) rc = snx_extract_title(input, paths, &title, &of_game, NULL);
        for (j = 0; !rc && j < nupdates; ++j) {
            rc = snx_extract_title(updates[j], paths, &own, &of_game, NULL);
            if (!rc && (snx_addon_kind(title, own) == SNX_ADDON_NONE || of_game != title)) {
                char named[17], given[17];
                snx_title_text(of_game, named);
                snx_title_text(title, given);
                rc = snx_error("%s is not an update or a DLC of %s (%s): its content meta names the game %s",
                               updates[j], input, given, named);
            }
            if (!rc)
                rc = play_dump(updates[j], paths, flags | PLAY_PREPARE, outdir, NULL, opt, recompiling, recompile, 0,
                               NULL, 0, NULL, NULL, 0);
        }
        if (rc || (flags & (SNX_RECOMP | PLAY_PREPARE))) goto done;
        snx_title_text(title, of);
        folder = game_folder(outdir, paths->repo, of);
        program = game_program(folder, of);
        rc = !program || play_dump(program, paths, flags, NULL, NULL, opt, recompiling, recompile, settings, setting,
                                   game_argc, game_argv, NULL, 0);
        free(program);
        goto done;
    }
    if (!prepared && !nro && out && dump && snx_is_file(dump)) { /* an update's or a DLC's goes into its own data folder */
        SnxU64 title, of_game;
        if (snx_extract_title(dump, paths, &title, &of_game, NULL)) goto done;
        if (snx_addon_kind(of_game, title) != SNX_ADDON_NONE) out = NULL;
    }
    if (!prepared && !nro && !out && prepared_addon(input, &folder, &name, &addon)) {
        if (outdir) {
            snx_error("--outdir cannot go with %s: an update or a DLC goes into its game's folder", input);
            goto done;
        }
        games = snx_dirname(folder);
        source = snx_game_source(folder, name, NULL);
        owner = game_id(name);
        goto addon;
    }
    if (!prepared && !nro && !out) {
        SnxU64 title, version;
        char text[17], *data, *extracting, *exefs;
        int fresh;
        if (!dump || !snx_is_file(dump)) {
            snx_error("%s is not a file", input);
            goto done;
        }
        games = outdir ? snx_absolute(outdir) : snx_recompiled_dir(paths->repo);
        if (!games || snx_extract_title(dump, paths, &title, &owner, &version)) goto done;
        if (snx_addon_kind(owner, title) != SNX_ADDON_NONE) { /* recompiled/<game>/<game>_<its entry> */
            char of[17], own[32];
            snx_title_text(owner, of);
            snx_addon_entry(owner, title, version, own);
            addon = title;
            folder = game_folder(outdir, paths->repo, of);
            name = snx_format("%s_%s", of, own);
            source = snx_strdup(dump);
            goto addon;
        }
        /* named as the recompile names it, by its title ID; a dump without one of its own by its file's stem */
        name = snx_title_text(title, text) ? snx_strdup(text) : snx_dump_name(input);
        work = tmpfs ? snx_tmpfs_work(title) : NULL;
        if (tmpfs && !work) {
            snx_error("%s has no title ID of its own to extract it to tmpfs by", input);
            goto done;
        }
        folder = game_folder(outdir, paths->repo, name);
        free(games); /* its parent, games/ of an older version while the game is there */
        games = folder ? snx_dirname(folder) : NULL;
        base = snx_path_join(folder, name);
        data = snx_format("%s_data", base);
        extracting = snx_format("%s.extracting", base);
        exefs = snx_path_join(data, "exefs");
        /* extracted unless its game has its ExeFS, from a finished extraction; else its game made from the dump read
           in place */
        fresh = extract && (snx_is_file(extracting) || !snx_is_dir(exefs));
        rc = fresh ? extract_game(command, dump, NULL, tmpfs ? work : data, "exefs", extracting, paths)
                   : snx_mkdirs(folder);
        source = snx_strdup(!extract ? dump : fresh && tmpfs ? work : data);
        free(data);
        free(extracting);
        free(exefs);
        if (rc) goto done;
        if (snx_record_source(folder, name, dump) && !extract) { /* read in place, from where its .ini says */
            rc = snx_error("%s can't be read in place without it. Turn on Extract dump "
                           "(--extract-container)",
                           input);
            goto done;
        }
        named = named_otherwise(source, name); /* a dump without a title ID of its own, named by its file */
        goto made;
    }
    if (prepared) {
        /* recompiled in its folder, from what its _data keeps (the ExeFS or the NRO, the RomFS, control.nacp), when
           another build made it: no dump, keys or extraction */
        if (outdir || out) {
            snx_error("--outdir and --out cannot go with %s: it is a game made already", input);
            goto done;
        }
        games = snx_dirname(folder);
        base = snx_path_join(folder, name);
        source = snx_format("%s_data/exefs/main.nro", base);
        if (!snx_is_file(source)) {
            char* exefs = snx_format("%s_data/exefs", base);
            free(source);
            /* made from its dump read in place, else from what its _data keeps */
            source = snx_is_dir(exefs) ? NULL : snx_game_source(folder, name, NULL);
            if (source && !snx_is_file(source)) {
                free(source);
                source = NULL;
            }
            if (!source) source = snx_format("%s_data", base);
            free(exefs);
        }
        named = named_otherwise(source, name); /* an NRO's, one named when it was made */
        { /* made before its extraction kept its icon: the icon alone, from the dump its .ini names, while it has none */
            char *icon = snx_format("%s_data/icon.jpg", base), *data = snx_format("%s_data", base),
                 *from = snx_game_source(folder, name, NULL);
            if (!snx_is_file(icon) && from && snx_is_file(from) && !is_nro_path(from))
                snx_extract_icon(from, paths, data); /* the game plays without it, the line saying why */
            free(icon);
            free(data);
            free(from);
        }
        goto made;
    }
    dumped = snx_dump_name(input);
    games = outdir ? snx_absolute(outdir) : snx_recompiled_dir(paths->repo);
    extracted = out ? snx_absolute(out) : NULL;
    work = extracted ? snx_dirname(extracted) : NULL;
    pending = extracted ? snx_format("%s.extracting", extracted) : NULL;
    if ((!nro && !pending) || !games) {
        snx_error("cannot resolve the folders of %s", input);
        goto done;
    }
    if (nro && out) {
        snx_error("--out cannot go with an NRO: an NRO has no extraction");
        goto done;
    }
    if (!dump || !snx_is_file(dump)) {
        snx_error("%s is not a file", input);
        goto done;
    }
    /* NROs are already executable images. No keys, hactool or extraction checkpoint is needed. */
    if (!nro && !snx_is_dir(extracted)) {
        /* Publish only a finished extraction. A killed extractor may leave its staging directory. */
        if (snx_mkdirs(work) ||
            ((snx_is_dir(pending) || snx_is_file(pending) || snx_is_link(pending)) && snx_remove_tree(pending))) {
            perror("scarletnx: preparing extraction directory");
            goto done;
        }
        command[5] = "-x";
        command[6] = "--out";
        command[7] = pending;
        n = 8;
        n = add_keys(command, n, paths);
        command[n++] = "--";
        command[n++] = dump;
        command[n] = NULL;
        rc = snx_run_stage(command, NULL, "Extraction");
        if (rc) goto done;
        if (!snx_is_dir(pending)) {
            rc = snx_error("extraction finished without creating %s", pending);
            goto done;
        }
        if (snx_replace(pending, extracted)) {
            perror("scarletnx: publishing extraction directory");
            rc = 1;
            goto done;
        }
    }
    /* named as the recompile names it: by the title ID, which the extraction's NPDM gives */
    if (snx_load_npdm(nro ? dump : extracted, &npdm, error, sizeof error)) {
        rc = snx_error("%s", error);
        goto done;
    }
    name = snx_game_name(NULL, &npdm, nro ? dump : extracted);
    folder = game_folder(outdir, paths->repo, name); /* the game's own, as the recompile makes it */
    free(games);
    games = folder ? snx_dirname(folder) : NULL;
    base = snx_path_join(folder, name);
    source = snx_strdup(nro ? dump : extracted);
addon:
    /* an update or a DLC of a game made already: its own .ini names its dump, and it is checked in the game's
       addons=. An update is recompiled into its own program, which plays it; a DLC has none, and the game plays it */
    if (addon) {
        char of[17];
        const char* what = snx_addon_kind(owner, addon) == SNX_ADDON_UPDATE ? "the update" : "a DLC";
        snx_title_text(owner, of);
        base = snx_path_join(folder, name);
        rc = 0;
        if (!prepared_game(folder, NULL, NULL)) {
            rc = snx_error("%s is %s of %s, which is not recompiled yet: play the game's dump first, or give both: "
                           "scarletnx GAME --update %s",
                           input, what, of, input);
        } else if (!source || !snx_is_file(source)) {
            rc = snx_error("%s's dump %s is not there: play it again", what, source ? source : "(none recorded)");
        } else if (snx_record_source(folder, name, source)) {
            rc = 1;
        } else if (snx_addon_kind(owner, addon) == SNX_ADDON_DLC) {
            /* recorded and checked, its game's code its own: the game played with it */
            char *data = snx_format("%s_data", base), *program;
            rc = snx_mkdirs(data) || drop_dlc_program(folder, name) || check_addon(folder, of, name + strlen(of) + 1);
            free(data);
            if (!rc && !(flags & (SNX_RECOMP | PLAY_PREPARE))) {
                program = game_program(folder, of);
                rc = !program || play_dump(program, paths, flags, NULL, NULL, opt, recompiling, recompile, settings,
                                           setting, game_argc, game_argv, NULL, 0);
                free(program);
            }
            goto done;
        } else {
            game_dump = snx_game_source(folder, of, NULL);
            if (game_dump && !snx_is_file(game_dump)) {
                free(game_dump);
                game_dump = NULL;
            }
            if (!game_dump && !extract)
                rc =
                    snx_error("the update reads its files over its game's dump, and the game's .ini names none that is "
                              "there: play the game's dump again");
            if (!rc) rc = check_addon(folder, of, name + strlen(of) + 1); /* the update played, over the one checked before */
            if (tmpfs && !rc) puts("An update is extracted into its data folder, not to tmpfs");
        }
        if (rc) goto done;
    }
made:
#ifdef _WIN32
    code = snx_format("%s_code.dll", base);
    game = snx_format("%s.exe", base);
#elif defined(SNX_IOS)
    /* the app is the runtime: a game is its pack of objects, made on the device (--target ios) */
    code = snx_format("%s_code.snxobj", base);
    game = snx_strdup(code);
#else
    code = snx_format("%s_code.so", base);
    game = snx_strdup(base);
#endif
    /* Always restage an NRO: a same-named cached game may belong to another input or an older build. */
#ifdef SNX_IOS
    if (nro || (flags & SNX_RECOMP) || !snx_is_file(code)) {
#else
    if (nro || (flags & SNX_RECOMP) || !snx_is_file(game) || !snx_is_file(code) || !stamped(base)) {
#endif
        command[5] = "-r";
        command[6] = "--outdir";
        command[7] = games;
        n = 8;
        if (tmpfs && !addon) command[n++] = "--tmpfs";
        if (opt) {
            command[n++] = "-O";
            command[n++] = opt;
        }
        for (j = 0; j < recompiling; ++j) command[n++] = recompile[j];
        n = add_keys(command, n, paths);
#ifdef SNX_IOS
        command[n++] = "--target";
        command[n++] = "ios";
#endif
        command[n++] = "--";
        command[n++] = source;
        if (named) command[n++] = name;
        command[n] = NULL;
        rc = snx_run_stage(command, NULL, "Recompilation");
        if (rc) goto done;
        if (!snx_is_file(game)) {
            rc = snx_error("recompilation finished without creating %s", game);
            goto done;
        }
    }
    /* made from the extraction --out keeps: its dump recorded all the same, which its update's RomFS is read over and
       Extract dump off reads it from */
    if (out && !nro && snx_record_source(folder, name, dump)) {
        rc = 1;
        goto done;
    }
    if (flags & (SNX_RECOMP | PLAY_PREPARE)) {
        rc = 0;
        goto done;
    }
    rc = nro      ? 0
         : addon ? ensure_addon(command, folder, name, addon, source, game_dump, extract, paths)
                 : ensure_romfs(command, folder, name, tmpfs, extract, paths);
    if (!rc && !nro) { /* its checked DLCs; and the game played with its checked update, from the update's program */
        char of[17], *program;
        if (addon) snx_title_text(owner, of);
        if (addon) rc = ensure_romfs(command, folder, of, tmpfs, extract, paths); /* the game's own files under it */
        if (!rc) rc = ensure_dlcs(command, folder, addon ? of : name, extract, paths);
#ifdef SNX_IOS
        program = NULL; /* the pack is the whole game: no program beside it to play instead */
#else
        program = addon ? NULL : game_program(folder, name);
#endif
        if (!rc && program && strcmp(program, game)) {
            rc = play_dump(program, paths, flags, NULL, NULL, opt, recompiling, recompile, settings, setting, game_argc,
                           game_argv, NULL, 0);
            free(program);
            goto done;
        }
        free(program);
    }
    /* the game may start with another environment than this run's (its copy outside an AppImage, a shortcut) */
    if (!rc) rc = snx_record_folders(folder, name);
    if (rc) goto done;
#if defined(__ANDROID__) || defined(SNX_IOS)
    /* an app's child process has no window to play in: the app runs the game this names (docs/ANDROID.md) */
    printf("Ready %s\n", name);
    rc = 0;
    goto done;
#endif
    rc = refresh_runtime(paths, game);
    if (rc) goto done;
    n = 0;
    command[n++] = game;
    n = add_keys(command, n, paths);
    if (flags & SNX_HEADLESS) command[n++] = "-H";
    if (flags & SNX_FULLSCREEN) command[n++] = "-f";
    if (flags & SNX_WINDOWED) command[n++] = "--fullscreen=0";
    for (j = 0; j < settings; ++j) command[n++] = setting[j];
    if (flags & SNX_STOP)
        command[n++] = "-s";
    else if (flags & SNX_GDB)
        command[n++] = "-S";
    if (flags & SNX_TRACY_CLIENT) command[n++] = "--tracy";
    if (flags & SNX_TRACY_CAPTURE) command[n++] = "--tracy-capture";
    /* Keep the boundary so the deployed launcher cannot interpret game
       arguments as another extraction/recompilation request. */
    if (game_argc) command[n++] = "--";
    for (j = 0; j < game_argc; ++j) command[n++] = game_argv[j];
    command[n] = NULL;
    rc = (flags & SNX_GDB) ? snx_run_debug_stage(command, folder, "Game") : snx_run_stage(command, folder, "Game");
done:
    free(dumped);
    free(name);
    free(dump);
    free(work);
    free(games);
    free(extracted);
    free(pending);
    free(game);
    free(folder);
    free(base);
    free(code);
    free(source);
    free(game_dump);
    free(command);
    return rc;
}
#ifdef SNX_IOS
/* a dump picked from elsewhere (the Files app's folders, Downloads) goes into dumps/ first: the game is made from, and
   listed with, the copy there. A copy already in dumps/ of the same size is kept. NULL when the input needs none (it is
   there, or no dump), else the copy's path, owned; *failed set when it could not be made */
static char* dump_into_dumps(const char* input, const SnxPaths* paths, int* failed) {
    char *dumps = snx_path_join(paths->repo, "dumps"), *from = snx_absolute(input), *to = NULL, *part = NULL;
    char* copy = NULL;
    const char* leaf = input;
    const char* s;
    *failed = 0;
    if (!dumps || !from || !snx_is_file(from) || !snx_is_dump(from) || snx_path_within(from, dumps)) goto done;
    for (s = input; *s; ++s)
        if (*s == '/' || *s == '\\') leaf = s + 1;
    to = snx_path_join(dumps, leaf);
    if (!to || snx_mkdirs(dumps)) {
        snx_error("cannot make %s", dumps);
        *failed = 1;
        goto done;
    }
    if (!(snx_is_file(to) && snx_file_size(to) == snx_file_size(from))) {
        printf("Copying %s into dumps/ (%llu MB)\n", leaf, (unsigned long long)(snx_file_size(from) >> 20));
        fflush(stdout);
        part = snx_format("%s.part", to);
        if (!part || snx_copy_file(from, part) || snx_replace(part, to)) {
            snx_error("cannot copy %s into %s", leaf, dumps);
            if (part) snx_remove_tree(part);
            *failed = 1;
            goto done;
        }
    }
    copy = to;
    to = NULL;
done:
    free(dumps);
    free(from);
    free(part);
    free(to);
    return copy;
}
#endif
static int play_dump(const char* input, const SnxPaths* paths, unsigned flags, const char* outdir, const char* out,
                     const char* opt, int recompiling, char** recompile, int settings, char** setting, int game_argc,
                     char** game_argv, const char* const* updates, int nupdates) {
#ifdef SNX_IOS
    int failed;
    char* copy = dump_into_dumps(input, paths, &failed);
    int rc;
    if (failed) return 1;
    rc = play_dump_at(copy ? copy : input, paths, flags, outdir, out, opt, recompiling, recompile, settings, setting,
                      game_argc, game_argv, updates, nupdates);
    free(copy);
    return rc;
#else
    return play_dump_at(input, paths, flags, outdir, out, opt, recompiling, recompile, settings, setting, game_argc,
                        game_argv, updates, nupdates);
#endif
}
/* 1 for an update's or a DLC's program in its game's folder (prepared_addon) */
static int addon_program(const char* input) {
    char *folder, *name;
    SnxU64 addon;
    if (!prepared_addon(input, &folder, &name, &addon)) return 0;
    free(folder);
    free(name);
    return 1;
}
/* Options whose following token belongs to a tool, not to the launcher. */
static int takes_value(const char* arg) {
    static const char* const names[] = {"--update", "--name",    "--out",    "--keys",    "--title-keys",
                                        "-o",       "--outname", "--outdir", "--runtime", "--target",
                                        "-O",       "--jobs",    "--chunk",  "--layout"};
    size_t i;
    for (i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
        if (!strcmp(arg, names[i])) return 1;
    return 0;
}
/* key=value's key, a setting the runtime knows */
static int setting_known(const char* option) {
    char key[64];
    size_t n = (size_t)(strchr(option, '=') - option);
    if (!snx_setting_known || n >= sizeof key) return 0;
    memcpy(key, option, n);
    key[n] = 0;
    return snx_setting_known(key);
}
static int container_path(const char* path) {
    const char* ext = strrchr(path, '.');
    char lower[6];
    size_t i, size;
    size = ext ? strlen(ext) : 0;
    if (size != 4 && size != 5) return 0;
    for (i = 0; i <= size; ++i) lower[i] = (char)tolower((unsigned char)ext[i]);
    return !strcmp(lower, ".xci") || !strcmp(lower, ".nsp") || !strcmp(lower, ".dxci") || !strcmp(lower, ".dnsp");
}
/* --dump-modules PROGRAM DIR: each module's image and symbols, at the bases the runtime gives them (loader.h's
   snx_dump_program has the format). Tools read game code through it */
static int dump_modules(int argc, char** argv) {
    char error[1024];
    if (argc != 4) return snx_error("--dump-modules takes a program (an extracted game or an NRO) and a folder");
    if (snx_dump_program(argv[2], argv[3], error, sizeof error)) return snx_error("%s", error);
    return 0;
}
/* the setup in the terminal for the program at argv0: all of it, or what setup.ini has left. 0 once done */
static int setup_run(const char* argv0, int all) {
    char *self = snx_executable(argv0), *dir = self ? snx_dirname(self) : NULL;
    int rc;
    snx_tool_console();
    rc = snx_setup_terminal(stdin, stdout, dir, all);
    free(dir);
    free(self);
    return rc;
}
int snx_driver(int argc, char** argv) {
    SnxPaths paths;
    SnxSetupKeysOverride previous_keys_override;
    const char *repo = NULL, *build = SNX_BUILD_DIR, *input = NULL, *outdir = NULL, *out = NULL, *opt = NULL;
    const char** updates = NULL;
    int nupdates = 0;
    char *r = NULL, *b = NULL, *exe = NULL, **rest = NULL, **setting, **recompile, *self, *data = NULL, *source = NULL;
    char *keys_path = NULL, *title_keys_path = NULL;
    const char *prod_keys_flag = NULL, *title_keys_flag = NULL;
    unsigned flags = 0;
    int i, j, n = 0, separator = argc, result = -1, tool_paths = 0, help = 0, parsed, automatic, tmpfs_args = 0;
    int repo_given = 0, settings = 0, recompiling = 0, taken = 0;
    previous_keys_override = snx_setup_keys_override_get();
    snx_setup_keys_override_set(NULL, NULL);
#ifdef SNX_IOS
    snx_set_stage_runner(snx_driver); /* no child process there: the stages are this driver again, on this thread */
#endif
    if (argc <= 1 && snx_setup_again) {
        result = setup_run(argv[0], 1); /* scarletnx --setup */
        goto done;
    }
    if (argc <= 1) {
        result = -1;
        goto done;
    }
    if (!strcmp(argv[1], "--dump-modules")) {
        result = dump_modules(argc, argv);
        goto done;
    }
    if (!strcmp(argv[1], "--scan-log")) {
        result = snx_logscan_main(argc - 1, argv + 1, snx_demangle);
        goto done;
    }
    rest = (char**)snx_alloc((size_t)(3 * argc + 1) * sizeof(*rest));
    setting = rest + argc + 1;
    recompile = setting + argc;
    for (i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--")) {
            separator = i;
            break;
        }
        if (!strcmp(argv[i], "--repo")) snx_option_renamed("--repo", "--data");
        if (!strcmp(argv[i], "--data") || !strcmp(argv[i], "--repo") || !strcmp(argv[i], "--build-dir")) {
            int is_repo = strcmp(argv[i], "--build-dir") != 0;
            tool_paths = 1;
            if (++i == argc) {
                result = snx_error("%s requires a folder", argv[i - 1]);
                goto done;
            }
            if (is_repo) {
                repo = argv[i];
                repo_given = 1;
            } else
                build = argv[i];
            continue;
        }
        if (!strcmp(argv[i], "--extract-container")) {
            flags |= SNX_EXTRACT_CONTAINER;
            continue;
        }
        if (!strcmp(argv[i], "--tmpfs")) { /* -r's too, which takes it from rest */
            flags |= SNX_TMPFS;
            rest[n++] = argv[i];
            ++tmpfs_args;
            continue;
        }
        parsed = snx_cli_flag(argv[i], &flags);
        if (parsed < 0) {
            result = snx_error("unknown option: %s", argv[i]);
            goto done;
        }
#ifdef _WIN32
        if (!strcmp(argv[i], "-s") || !strcmp(argv[i], "-S")) {
            result = snx_error("the GDB stub is UNIX only");
            goto done;
        }
#endif
        if (parsed) continue;
        if (!strcmp(argv[i], "--backend") || !strncmp(argv[i], "--backend=", 10)) {
            /* LLVM is the only code generator: older scripts name it */
            const char* backend = argv[i][9] ? argv[i] + 10 : i + 1 < argc ? argv[++i] : "";
            if (strcmp(backend, "llvm")) {
                result = snx_error("--backend %s: LLVM is the only backend", backend);
                goto done;
            }
            continue;
        }
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) help = 1;
        if (!input && argv[i][0] != '-') input = argv[i];
        rest[n++] = argv[i];
        if (takes_value(argv[i]) && i + 1 < argc) rest[n++] = argv[++i];
    }
    if (help && !(flags & (SNX_EXTRACT | SNX_RECOMP))) {
        snx_tool_console();
        usage();
        result = 0;
        goto done;
    }
    /* --keys and --title-keys, in every mode: taken out of rest, and carried on to the stages */
    for (i = 0, j = 0; i < n; ++i) {
        int consumed = 0;
        parsed = keys_option(rest[i], i + 1 < n ? rest[i + 1] : NULL, &prod_keys_flag, &title_keys_flag, &consumed);
        if (parsed < 0) {
            result = 1;
            goto done;
        }
        if (!parsed)
            rest[j++] = rest[i];
        else if (consumed)
            ++i;
    }
    n = j;
    /* a game made already is played or, with -r, recompiled where it is */
    automatic = input && (!(flags & (SNX_EXTRACT | SNX_RECOMP)) ||
                          ((flags & SNX_RECOMP) &&
                           (container_path(input) ||
                            (!(flags & SNX_EXTRACT) && (prepared_game(input, NULL, NULL) || addon_program(input))))));
    if (input && (automatic || (flags & SNX_RECOMP)) && left_beside(input)) {
        result =
            snx_error("%s was laid out by an older version: move its files as docs/DEPRECATIONS.md says, or run it "
                      "as it is with --game=%s",
                      input, input);
        goto done;
    }
    /* played, a --key=value that is not the driver's own is a setting of the recompile, passed on to it when the game is
       made, or of the game's .ini, passed on to the game when the runtime knows the key; any other is refused */
    for (i = 0; automatic && i < n; ++i)
        if (!strncmp(rest[i], "--", 2) && strchr(rest[i], '=')) {
            char key[64];
            size_t length = (size_t)(strchr(rest[i], '=') - rest[i] - 2);
            if (length < sizeof key) {
                memcpy(key, rest[i] + 2, length);
                key[length] = 0;
            }
            int recompiles = length < sizeof key && snx_recompile_setting(key), plays = setting_known(rest[i] + 2);
            if (!recompiles && !plays) {
                result = snx_error("unknown option: %s", rest[i]);
                goto done;
            }
            if (recompiles) recompile[recompiling++] = rest[i];
            if (plays) setting[settings++] = rest[i]; /* both for one both take (slp_vectorizer) */
            ++taken;
        } else
            rest[i - taken] = rest[i];
    n -= taken;
    /* played, --outdir names the games' folder and --out the dump's extraction, as -r and -x take them, and --update
       an update or a DLC played over the game */
    for (i = 0; automatic && i < n; ++i)
        if (!strcmp(rest[i], "--outdir") || !strcmp(rest[i], "--out") || !strcmp(rest[i], "--update")) {
            if (i + 1 == n) {
                result = snx_error("%s requires %s", rest[i], strcmp(rest[i], "--update") ? "a directory" : "a dump");
                goto done;
            }
            if (!strcmp(rest[i], "--update")) {
                if (!updates) updates = (const char**)snx_alloc((size_t)argc * sizeof *updates);
                updates[nupdates++] = rest[i + 1];
            } else
                *(strcmp(rest[i], "--out") ? &outdir : &out) = rest[i + 1];
            memmove(rest + i, rest + i + 2, (size_t)(n - i - 2) * sizeof(*rest));
            n -= 2;
            --i;
        }
    /* played or prepared, -O LEVEL (or -OLEVEL) is the recompile's, for when the game is made */
    for (i = 0; automatic && i < n; ++i)
        if (!strncmp(rest[i], "-O", 2)) {
            int own = rest[i][2] == 0; /* its value the next token */
            if (own && i + 1 == n) {
                result = snx_error("-O requires a level");
                goto done;
            }
            opt = own ? rest[i + 1] : rest[i] + 2;
            if (strlen(opt) != 1 || !strchr("0123sz", opt[0])) {
                result = snx_error("-O %s: not 0, 1, 2, 3, s or z", opt);
                goto done;
            }
            memmove(rest + i, rest + i + 1 + own, (size_t)(n - i - 1 - own) * sizeof(*rest));
            n -= 1 + own;
            --i;
        }
    /* -r's AOT build names the profile cache and hooks direct calls; both are the recompiler's, not the game's.
       Left in rest they count as extra dump inputs */
    for (i = 0; automatic && (flags & SNX_RECOMP) && i < n; ++i) {
        if (!strcmp(rest[i], "--jit-cache")) {
            if (i + 1 == n) {
                result = snx_error("--jit-cache requires a file");
                goto done;
            }
            recompile[recompiling++] = rest[i];
            recompile[recompiling++] = rest[i + 1];
            memmove(rest + i, rest + i + 2, (size_t)(n - i - 2) * sizeof(*rest));
            n -= 2;
            --i;
        } else if (!strcmp(rest[i], "--hook-direct-calls")) {
            recompile[recompiling++] = rest[i];
            memmove(rest + i, rest + i + 1, (size_t)(n - i - 1) * sizeof(*rest));
            --n;
            --i;
        }
    }
    /* played (a dump, a game, or nothing), an option nothing here took is refused by its name: -x and -r refuse their
       own */
    if (!(flags & (SNX_EXTRACT | SNX_RECOMP)) || automatic)
        for (i = 0; i < n; ++i) {
            if (takes_value(rest[i])) {
                ++i; /* its value */
                continue;
            }
            /* the runtime's own, which it reads after: the settings' list and the game to play; without a dump, as a
               game's copy is started, a setting it knows and -v too */
            if (!strcmp(rest[i], "--settings") || !strncmp(rest[i], "--settings=", 11) ||
                !strncmp(rest[i], "--game=", 7))
                continue;
            if (!input && (!strcmp(rest[i], "-v") ||
                           (!strncmp(rest[i], "--", 2) && strchr(rest[i], '=') && setting_known(rest[i] + 2))))
                continue;
            if (rest[i][0] == '-' && rest[i][1] && strcmp(rest[i], "--tmpfs") && strcmp(rest[i], "-h") &&
                strcmp(rest[i], "--help") && strcmp(rest[i], "--hook-direct-calls")) {
                result = snx_error("unknown option: %s (scarletnx --help lists them)", rest[i]);
                goto done;
            }
        }
    if (automatic && (n != 1 + tmpfs_args || help)) {
        result = help ? snx_error("-h goes with -x or -r, or alone (scarletnx --help)")
                      : snx_error("one dump at a time, and %s is a second: the game's own arguments go after --",
                                  rest[n - 1]);
        goto done;
    }
    if ((flags & (SNX_EXTRACT | SNX_RECOMP)) &&
        (flags & (SNX_HEADLESS | SNX_GDB | SNX_TRACY_CLIENT | SNX_TRACY_CAPTURE | SNX_FULLSCREEN | SNX_WINDOWED))) {
        result =
            snx_error("%s plays a game, and -x and -r only prepare one", flags & SNX_HEADLESS        ? "--headless"
                                                                         : flags & SNX_GDB           ? "-S"
                                                                         : flags & SNX_TRACY_CLIENT  ? "--tracy"
                                                                         : flags & SNX_TRACY_CAPTURE ? "--tracy-capture"
                                                                         : flags & SNX_FULLSCREEN ? "--fullscreen"
                                                                                                  : "--fullscreen=0");
        goto done;
    }
    if (!automatic && !(flags & (SNX_EXTRACT | SNX_RECOMP))) {
        if (tool_paths) result = snx_error("expected a dump, -x or -r");
        goto done;
    }
    if (!automatic && (flags & SNX_EXTRACT) && (flags & SNX_RECOMP)) {
        result =
            snx_error("-rx takes an XCI, NSP, DXCI or DNSP dump, and %s is not one", input ? input : "none was given");
        goto done;
    }
    if (automatic && (flags & SNX_RECOMP) && separator < argc) {
        result = snx_error("game arguments require game execution");
        goto done;
    }
    /* Explicit tool modes retain their positional -- escape (including names
       beginning with '-'). Automatic play uses it for game arguments instead. */
    if (!automatic)
        for (i = separator; i < argc; ++i) rest[n++] = argv[i];
    rest[n] = NULL;
    /* work/, recompiled/ and prod.keys are in the data folder (snx_data_dir), unless --repo names one (the menu does),
       for a build tree's driver as for an installed one. Not the current folder: a desktop entry or Explorer start the
       driver wherever they happen to be, often somewhere read-only (/, the AppImage's mount, Program Files) */
    self = snx_executable(argv[0]);
    exe = self ? snx_dirname(self) : NULL;
    free(self);
    source = snx_checkout(SNX_SOURCE_DIR, build, exe);
    if (!repo_given) {
        data = snx_data_dir(exe);
        if (data && snx_mkdirs(data)) {
            result = snx_error("cannot create the data folder %s", data);
            goto done;
        }
        repo = data ? data : ".";
    }
    free(exe);
    exe = NULL;
    snx_tool_console();
    if (data && source) snx_checkout_left(source, data);
    if (data) snx_recompiled_left(data);
    /* Resolve and validate key paths before setup checks for keys. */
    if (prod_keys_flag) {
        keys_path = keys_flag_file("--keys", prod_keys_flag);
        if (!keys_path) {
            result = 1;
            goto done;
        }
    }
    if (title_keys_flag) {
        title_keys_path = keys_flag_file("--title-keys", title_keys_flag);
        if (!title_keys_path) {
            result = 1;
            goto done;
        }
    }
    snx_setup_keys_override_set(keys_path, title_keys_path);
    /* what the setup has left, before the first run a terminal makes and the first after an update, or all of it */
    if (!snx_setup_skip && (snx_setup_again || snx_terminal()) && setup_run(argv[0], snx_setup_again)) {
        result = 1;
        goto done;
    }
    r = snx_absolute(repo);
    b = snx_absolute(build);
    exe = snx_executable(argv[0]);
    if (!r || !b || !exe) {
        result = snx_error("cannot resolve %s", !r ? repo : !b ? build : argv[0]);
        goto done;
    }
    paths.repo = r;
    paths.build = b;
    paths.executable = exe;
    paths.source = source;
    paths.keys = keys_path;
    paths.title_keys = title_keys_path;
    if (automatic)
        result =
            play_dump(input, &paths, flags, outdir, out, opt, recompiling, recompile, settings, setting,
                      separator < argc ? argc - separator - 1 : 0, argv + (separator < argc ? separator + 1 : argc),
                      updates, nupdates);
    else
        result = (flags & SNX_RECOMP) ? snx_recompile_game(n, rest, &paths) : snx_extract(n, rest, &paths);
done:
    free((void*)updates);
    free(rest);
    free(r);
    free(b);
    free(exe);
    free(data);
    free(source);
    snx_setup_keys_override_set(previous_keys_override.prod, previous_keys_override.title);
    free(keys_path);
    free(title_keys_path);
    return result;
}
