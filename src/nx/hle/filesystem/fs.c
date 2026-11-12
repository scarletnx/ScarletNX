/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from Eden (src/core/file_sys/fsmitm_romfsbuild.cpp, system_archive/mii_model.cpp):
   SPDX-FileCopyrightText: Copyright 2018, 2019 yuzu Emulator Project */
/* fsp-srv over the game's indexed data folder: the RomFS (a romfs.bin, an image made up from the romfs/ folder, or
   the game's dump read in place), save data, the SD card and the system archives */
#include "nx/hle/filesystem/fs.h"
#include "nx/hle/filesystem/fs_tree.h"
#include "common/utilities/mii_model.h"
#include "nx/runtime/main.h"
#include "nx/runtime/state_io.h"
#include "nx/runtime/options.h"
#include "nx/hle/services/services.h"
#include "nx/hle/services/services_int.h"
#include "nx/hle/kernel/ipc.h"
#include "nx/hle/hle.h"
#include "nx/runtime/platform/tracy.h"
#include "hle/filesystem/fs.swipc.inc"
#include "nx/memory/mem.h"
#include "nx/memory/own.h"
#include "nx/runtime/platform/backend.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"
#include "common/utilities/common.h"
#include "nx/runtime/driver/container.h"
#include "common/utilities/game_ini.h"
#include "common/utilities/mods.h"
#include "common/utilities/patches.h"
#include "common/utilities/ini_lines.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <io.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

static u32 tree_result(FsTreeResult r) {
    switch (r) {
        case FS_TREE_OK: return 0;
        case FS_TREE_NOTFOUND: return SNX_RC_FS_PATH_NOT_FOUND;
        case FS_TREE_EXISTS: return SNX_RC_FS_PATH_ALREADY_EXISTS;
        case FS_TREE_NOTEMPTY: return SNX_RC_FS_DIRECTORY_NOT_EMPTY;
        case FS_TREE_NOTDELETABLE: return SNX_RC_FS_DIRECTORY_UNDELETABLE;
        case FS_TREE_NOTRENAMABLE: return SNX_RC_FS_DIRECTORY_UNRENAMABLE;
        case FS_TREE_INVALID: return SNX_RC_FS_DIRECTORY_UNOBTAINABLE; /* a ".." above the root */
        case FS_TREE_TOOLONG: return SNX_RC_FS_TOO_LONG_PATH;
        case FS_TREE_READONLY: return SNX_RC_FS_WRITE_UNPERMITTED;
        default: return SNX_RC_FS_USABLE_SPACE_NOT_ENOUGH;
    }
}

/* where the tree's last index, graft or overlay that failed stopped, for a log line: the host path, folder (the one it
   was given) when it stopped at none; then why, the phrase after it ("is a symbolic link, which the index doesn't
   follow"), which a fatal's text, kept free of paths, says alone */
static const char* tree_at(const char* folder) { return g_hle.fs.tree.refused[0] ? g_hle.fs.tree.refused : folder; }
static const char* tree_why(FsTreeResult r) { return fs_tree_why(&g_hle.fs.tree, r); }

/* Extract to tmpfs (the launcher's setting): a data folder without a RomFS of its own reads the one its extraction
   keeps in snx_tmpfs_work, by the title ID of its main.npdm. The driver extracts it there again, from the dump the
   game's folder links to, when the system has emptied it. Under tree.lock, at startup */
static void romfs_from_tmpfs(void) {
    FsNode* node;
    char path[sizeof g_datadir + 16], *work, *romfs;
    unsigned char* npdm;
    size_t size;
    if (!fs_tree_lookup(&g_hle.fs.tree, NULL, "romfs", &node) || !fs_tree_lookup(&g_hle.fs.tree, NULL, "romfs.bin", &node))
        return;
    snprintf(path, sizeof path, "%sexefs/main.npdm", g_datadir);
    npdm = snx_read_file(path, &size);
    work = npdm ? snx_tmpfs_work(snx_npdm_title(npdm, size)) : NULL;
    free(npdm);
    if (!work) return;
    romfs = snx_path_join(work, "romfs");
    if (snx_is_dir(romfs)) {
        FsTreeResult r = fs_tree_graft(&g_hle.fs.tree, "romfs", romfs);
        if (r == FS_TREE_OK)
            LOG("fs: RomFS from %s\n", romfs);
        else
            WARN("fs: could not index the RomFS in %s: %s %s\n", romfs, tree_at(romfs), tree_why(r));
    }
    free(romfs);
    free(work);
}

/* hactool ends the process on a dump it cannot read: what it said first */
static void dump_exit(void* data) {
    (void)data;
    snx_hac_error("fs: hactool stopped on the game's dump");
}

/* the game's title ID, which its folder and data are named by: 0 for a game named otherwise */
static SnxU64 game_title(void) {
    SnxU64 id = 0;
    return snx_title_parse(g_gamename, &id) ? id : 0;
}
/* dump opened as snx_dump_open takes it (base, addon, either NULL; index the program of the application this is,
   g_program) and each container mapped read-only for the whole
   run: the base's into base_map, the add-on's into addon_map. Ends the run on a dump that cannot be read, and on one
   whose keys the keys file lacks, which would read as garbage: no ExeFS, its modules missing */
static void dump_map(SnxDump* dump, PFileMap* base_map, PFileMap* addon_map, const char* base, const char* addon,
                     int index, const char* keys, const char* title_keys, const char* repo) {
    snx_hac_on_exit(dump_exit, NULL);
    if (snx_dump_open(dump, base, addon, index, keys, title_keys, repo, 0))
        fatal("Could not read the game's dump %s, which it reads its files from: the line before says why",
              addon ? addon : base);
    if ((dump->program.found && snx_nca_keys(dump, &dump->program, "The game's Program NCA")) ||
        (dump->patch.found && snx_nca_keys(dump, &dump->patch, "The update's Program NCA")) ||
        (dump->aoc.found && snx_nca_keys(dump, &dump->aoc, "The DLC's Data NCA")))
        fatal("A dump the game reads in place needs keys its keys file does not have: the line before says which");
    if (base && (plat_file_map_sized(base_map, base, snx_file_size(base)) ||
                 snx_hac_map(dump->hac, dump->base, base_map->data, base_map->size)))
        fatal("Could not map the game's dump %s", base);
    if (addon && (plat_file_map_sized(addon_map, addon, snx_file_size(addon)) ||
                  snx_hac_map(dump->hac, dump->update, addon_map->data, addon_map->size)))
        fatal("Could not map the add-on's dump %s", addon);
    snx_hac_on_exit(NULL, NULL);
}

/* the folder games/ is in, where the driver looks for the keys: g_datadir is <repo>/games/<name>/<name>_data/. In
   repo, of g_datadir's size, which holds it: on the stack, so a fatal() while a dump is read leaves nothing allocated
   that only the caller's frame points at */
static void data_repo(char* repo, size_t size) {
    char *path = snx_strdup(g_datadir), *up;
    int i;
    for (i = 0; i < 3; ++i) {
        up = snx_dirname(path);
        free(path);
        path = up;
    }
    snprintf(repo, size, "%s", path);
    free(path);
}

/* source= an extracted game (snx_extracted_game): its ExeFS and RomFS read where they are, read-only, as a dump's are,
   on whatever disk the folder is. A game without a RomFS of its own runs without one, the log saying so. Under
   tree.lock, at startup */
static void folder_open(const char* folder) {
    static const char* const names[] = {"exefs", "romfs"};
    FsNode* node;
    FsTreeResult r;
    char* part;
    int i;
    for (i = 0; i < 2; i++) {
        if (!fs_tree_lookup(&g_hle.fs.tree, NULL, names[i], &node)) continue; /* the data folder's own */
        part = snx_extracted_part(folder, i);
        if (!part && !i)
            fatal("The game's folder %s, its source= setting, has no exefs/ to read the game's program from", folder);
        if (!part) {
            LOG("fs: the game's folder %s has no romfs/: the game runs without a RomFS\n", folder);
            continue;
        }
        r = fs_tree_graft(&g_hle.fs.tree, names[i], part);
        if (r != FS_TREE_OK) {
            WARN("fs: could not index %s, the game's %s: %s %s\n", part, i ? "RomFS" : "ExeFS", tree_at(part),
                 tree_why(r));
            fatal("Could not index the game's %s, as one of its files %s: the line before says which",
                  i ? "RomFS" : "ExeFS", tree_why(r));
        }
        free(part);
    }
    LOG("fs: ExeFS and RomFS read from the folder %s\n", folder);
}

/* Extract dump off: a data folder without an ExeFS of its own reads its ExeFS and RomFS from the dump the
   game was made from, mapped read-only for the whole run: source= in its .ini, else the dump <folder>/<name>.source
   beside <name>_data links to, as older versions recorded it; a line names both when neither is. Nothing of it is
   written anywhere. The keys are those given for this run, else where the driver finds them, in the data folder games/
   is in or in ~/.switch.
   An add-on (g_addon) not extracted into its own data folder reads its dump, source= in its own .ini: an update's
   ExeFS and RomFS over the game's dump, which it needs even when the game's files are extracted (its Patch RomFS
   is made of the game's), and a DLC's RomFS image beside the game's files (dump_aoc). At startup, under tree.lock */
static void dump_open(const char* keys, const char* title_keys) {
    FsNode* node;
    char *data, *source, *ini, *addon = NULL, *own, *at, repo[sizeof g_datadir];
    size_t n = strlen(g_datadir);
    int kind = g_addon ? SNX_ADDON_UPDATE : SNX_ADDON_NONE, extracted = 0;
    if (kind != SNX_ADDON_NONE) { /* its extraction, laid over the game's (fs_init), else its dump */
        at = snx_format("%sexefs", g_addondir);
        extracted = snx_is_dir(at);
        free(at);
        own = snx_format("%s%s.ini", g_confdir, g_addonrec); /* the update's own record, its programs' too */
        addon = extracted ? NULL : snx_ini_value(own, "source");
        if (!extracted && (!addon || !snx_is_file(addon)))
            fatal("The update %s can't be read: its settings %s name %s. Play its dump again to record it", g_exename,
                  own, addon ? "a dump that is not there" : "no dump (source=)");
        free(own);
        if (extracted) LOG("fs: the update's files from its extraction in %s\n", g_addondir);
    }
    /* the game's own ExeFS, or the update's extraction laid over it: no dump to read the program from */
    if (!fs_tree_lookup(&g_hle.fs.tree, NULL, "exefs", &node) && (kind != SNX_ADDON_UPDATE || extracted)) goto done;
    data = snx_strdup(g_datadir);
    while (n && (data[n - 1] == '/' || data[n - 1] == '\\')) data[--n] = 0;
    if (n < 5 || strcmp(data + n - 5, "_data")) {
        free(data);
        goto done;
    }
    data[n - 5] = 0;
    source = g_settings.game_dump[0] ? snx_strdup(g_settings.game_dump) : snx_format("%s.source", data);
    free(data);
    if (kind != SNX_ADDON_UPDATE && snx_extracted_game(source)) {
        folder_open(source);
        free(source);
        goto done;
    }
    if (!snx_is_file(source)) {
        ini = snx_absolute(ini_file()); /* a game beside its .ini runs in its folder: "./Game.ini" */
        /* the loader also takes the main module from the data folder's root, as older versions put it */
        if (addon && kind == SNX_ADDON_UPDATE)
            fatal("The update reads its files over the game's dump, and %s names %s: %s", ini ? ini : ini_file(),
                  !g_settings.game_dump[0]                   ? "none (source=)"
                  : snx_extracted_game(g_settings.game_dump) ? "the game's folder, which has no update's files to read"
                                                             : "a dump that is not there",
                  snx_extracted_game(g_settings.game_dump) ? "extract the update with Extract dump"
                                                           : "play the game's dump to record it");
        if (g_settings.game_dump[0])
            WARN("fs: the game's %s %s, its source= setting, is not there; its settings are %s\n",
                 snx_source_kind(source), source, ini ? ini : ini_file());
        else if (fs_tree_lookup(&g_hle.fs.tree, NULL, "main", &node) &&
                 fs_tree_lookup(&g_hle.fs.tree, NULL, "main.nro", &node))
            WARN("fs: the game has no ExeFS in %sexefs/ and no dump to read it from: %s sets no source=, and %s is not "
                 "there\n",
                 g_datadir, ini ? ini : ini_file(), source);
        free(ini);
        free(source);
        goto done;
    }
    data_repo(repo, sizeof repo);
    /* an update over the game: both in one hactool, its Patch RomFS reading the game's */
    dump_map(&g_hle.fs.dump, &g_hle.fs.dmap, &g_hle.fs.amap, source, kind == SNX_ADDON_UPDATE ? addon : NULL, g_program,
             keys, title_keys, repo);
    snx_hac_on_exit(dump_exit, NULL);
    g_hle.fs.dfs = snx_dump_fs(&g_hle.fs.dump);
    if (!g_hle.fs.dfs) fatal("Could not read the game's program in its dump %s: the line before says why", source);
    snx_hac_on_exit(NULL, NULL);
    pm_init(&g_host.fs.dlock);
    if (g_program)
        LOG("fs: program %d of the application read from the dump%s %s (its %d programs; %.1f MB mapped)\n", g_program,
            kind == SNX_ADDON_UPDATE ? ", its update's patch of it over the game's," : "", source,
            g_hle.fs.dump.programs, (double)(g_hle.fs.dmap.size + g_hle.fs.amap.size) / 1048576.0);
    if (kind == SNX_ADDON_UPDATE) {
        LOG("fs: ExeFS and RomFS read from the update %s over the game's dump %s (%.1f MB mapped)\n", addon, source,
            (double)(g_hle.fs.dmap.size + g_hle.fs.amap.size) / 1048576.0);
        free(addon);
        addon = NULL;
    } else
        LOG("fs: ExeFS and RomFS read from the dump %s (%.1f MB mapped)\n", source,
            (double)g_hle.fs.dmap.size / 1048576.0);
    free(source);
done:
    free(addon);
}

/* The DLCs checked in the game's addons= (g_settings.game_addons), each made beside it with its own .ini naming its
   dump (<game>_<its title ID>.ini): one extracted in its data folder is there already, laid over the game's
   (fs_init); any other is read from its dump in place, its RomFS image served by OpenDataStorageByDataId as
   installed. One whose dump is not there is left out, a line saying so. The keys are those given for this run, else
   where the driver finds them, in the data folder games/ is in or in ~/.switch. At startup */
static void dlcs_open(const char* keys, const char* title_keys) {
    SnxAddon* ids = malloc(SNX_ADDONS_MAX * sizeof *ids);
    size_t count, i;
    char repo[sizeof g_datadir];
    data_repo(repo, sizeof repo);
    if (!ids) fatal("Out of memory for the game's DLCs");
    count = snx_addons_parse(g_settings.game_addons, g_gamename, ids, SNX_ADDONS_MAX);
    g_hle.fs.aocs = calloc(count ? count : 1, sizeof *g_hle.fs.aocs);
    g_hle.fs.dlcs = calloc(count ? count : 1, sizeof *g_hle.fs.dlcs);
    if (!g_hle.fs.aocs || !g_hle.fs.dlcs) fatal("Out of memory for the game's DLCs");
    for (i = 0; i < count; i++) {
        char text[17], *image, *ini, *dump;
        FsAoc* aoc = &g_hle.fs.aocs[g_hle.fs.naocs];
        if (snx_addon_kind(game_title(), ids[i].id) == SNX_ADDON_UPDATE) {
            if (ids[i].id != g_addon)
                LOG("fs: the update %016llx is checked, and runs from its own program: the game's leaves it out\n",
                    (unsigned long long)ids[i].id);
            continue;
        }
        snx_title_text(ids[i].id, text);
        image = snx_format("%s%s_%s_data/system/archives/%s.romfs", g_exedir, g_gamename, text, text);
        ini = snx_format("%s%s_%s.ini", g_confdir, g_gamename, text);
        dump = snx_is_file(image) ? NULL : snx_ini_value(ini, "source");
        if (snx_is_file(image)) {
            LOG("fs: the DLC %s from its extraction\n", text);
            g_hle.fs.dlcs[g_hle.fs.ndlcs++] = ids[i].id;
        }
        else if (!dump || !snx_is_file(dump))
            WARN("fs: the DLC %s is checked, and its settings %s name %s: the game runs without it. Play its dump "
                 "again to record it\n",
                 text, ini, dump ? "a dump that is not there" : "no dump (source=)");
        else {
            dump_map(&aoc->dump, NULL, &aoc->map, NULL, dump, 0, keys, title_keys, repo);
            snx_hac_on_exit(dump_exit, NULL);
            aoc->fs = snx_dump_aoc_fs(&aoc->dump);
            if (!aoc->fs) fatal("Could not read the DLC in its dump %s: the line before says why", dump);
            snx_hac_on_exit(NULL, NULL);
            if (aoc->dump.aoc.title_id != ids[i].id)
                WARN("fs: the DLC's dump %s holds %016llx, and the DLC is %s: it is served by its own\n", dump,
                     (unsigned long long)aoc->dump.aoc.title_id, text);
            LOG("fs: the DLC %016llx read from its dump %s (%.1f MB)\n", (unsigned long long)aoc->dump.aoc.title_id,
                dump, (double)snx_hac_fs_romfs_size(aoc->fs) / 1048576.0);
            g_hle.fs.dlcs[g_hle.fs.ndlcs++] = aoc->dump.aoc.title_id;
            g_hle.fs.naocs++;
        }
        free(image);
        free(ini);
        free(dump);
    }
    if (g_hle.fs.naocs && !g_hle.fs.dfs) pm_init(&g_host.fs.dlock); /* dump_open, before this, made it for the game */
    free(ids);
}

/* The mods of folder (NULL for none) a symbolic link keeps out, said: a mod not turned off that is one, or whose
   exefs/ or romfs/ is one. common/utilities/mods.c reads none through a link, and said nothing */
static void mods_links(const char* folder) {
    static const char* const parts[] = {"exefs", "romfs"};
    char** names = NULL;
    size_t count = 0;
    if (!folder || !snx_is_dir(folder) || snx_list_dir(folder, &names, &count)) return;
    for (size_t i = 0; i < count; i++) {
        char* mod = snx_path_join(folder, names[i]);
        if (names[i][0] != '.' && snx_is_dir(mod) && !snx_mod_off(g_settings.game_mods_off, names[i])) {
            if (snx_is_link(mod))
                WARN("fs: the mod %s is a symbolic link, which mods aren't read through: it is left out\n", mod);
            for (size_t k = 0; k < 2 && !snx_is_link(mod); k++) {
                char* part = snx_path_join(mod, parts[k]);
                if (snx_is_dir(part) && snx_is_link(part))
                    WARN("fs: the mod's %s is a symbolic link, which mods aren't read through: it is left out\n", part);
                free(part);
            }
        }
        free(mod);
    }
    snx_free_strings(names, count);
}

/* The mods played (common/utilities/mods.h), from the game's mods folder, which the data folder's index leaves out:
   the files of their exefs/ that the game's program was recompiled with, read in place of its own, and their romfs/
   folders grafted as mods.0, mods.1... in the order they are laid, which romfs_init lays over the game's RomFS. A
   program recompiled with other mods' files (what its stamp, <program>.abi, records after " mods ") keeps its own
   ExeFS, which its code was made from, a line saying so. A mod whose files can't be indexed is left out, said, and
   so is one that is a symbolic link (mods_links). At startup, under tree.lock */
static void mods_open(void) {
    char *folder = snx_mods_folder(g_datadir), **romfs;
    char* own = snx_program_own_mods(g_exedir[0] ? g_exedir : ".", g_gamename, g_program); /* Eden's load/<its ID>/ */
    char* stamp = snx_mods_stamp_of(folder, own, g_settings.game_mods_off);
    char path[sizeof g_exedir + sizeof g_exename + 8], *built, *recorded;
    size_t count, i, size = 0;
    snprintf(path, sizeof path, "%s%s.abi", g_exedir, g_exename);
    built = (char*)snx_read_file(path, &size);
    if (built) built[snx_game_stamp_size(built, size)] = 0; /* the made line after it says how, not with what */
    recorded = built ? strstr(built, " mods ") : NULL;
    recorded = recorded ? recorded + 6 : "";
    if (!strcmp(recorded, stamp)) {
        g_hle.fs.nmexefs =
            snx_mods_exefs_of(folder, own, g_settings.game_mods_off, &g_hle.fs.mexefs_names, &g_hle.fs.mexefs_paths);
        for (i = 0; i < g_hle.fs.nmexefs; i++)
            LOG("fs: exefs/%s from the mod's %s\n", g_hle.fs.mexefs_names[i], g_hle.fs.mexefs_paths[i]);
        g_hle.fs.npatches = snx_mods_patches_of(folder, own, g_settings.game_mods_off, &g_hle.fs.patches);
    } else if (stamp[0])
        WARN("fs: the mods played replace ExeFS files, and the program was recompiled with %s: it runs the ExeFS it "
             "was made from until it is recompiled, which playing it from the launcher does\n",
             recorded[0] ? "other ones" : "none of them");
    else
        WARN("fs: the program was recompiled with ExeFS files of mods not played now: recompile it, which playing it "
             "from the launcher does\n");
    count = snx_mods_played_of(folder, own, g_settings.game_mods_off, "romfs", &romfs);
    for (i = 0; i < count; i++) {
        char name[32];
        FsTreeResult r;
        snprintf(name, sizeof name, "mods.%u", (unsigned)g_hle.fs.nmromfs);
        r = fs_tree_graft(&g_hle.fs.tree, name, romfs[i]);
        if (r == FS_TREE_OK) {
            g_hle.fs.nmromfs++;
            LOG("fs: the mod's RomFS %s laid over the game's\n", romfs[i]);
        } else
            WARN("fs: the mod's RomFS %s is left out: %s %s\n", romfs[i], tree_at(romfs[i]), tree_why(r));
    }
    snx_free_strings(romfs, count);
    mods_links(folder);
    mods_links(own);
    free(built);
    free(stamp);
    free(own);
    free(folder);
}

/* a mod's file played in place of path, exefs/<name>, malloc'd: NULL for none, or one that can't be read */
static u8* mod_exefs(const char* path, u64* size) {
    size_t i, n = 0;
    u8* data;
    if (strncmp(path, "exefs/", 6)) return NULL;
    for (i = 0; i < g_hle.fs.nmexefs && strcmp(g_hle.fs.mexefs_names[i], path + 6); i++) continue;
    if (i == g_hle.fs.nmexefs) return NULL;
    data = snx_read_file(g_hle.fs.mexefs_paths[i], &n);
    if (!data) WARN("fs: could not read the mod's %s\n", g_hle.fs.mexefs_paths[i]);
    *size = n;
    return data;
}

/* an ExeFS file of the dump read in place, malloc'd: path is exefs/<name>. NULL when the game has no dump open or
   the dump no such file */
static u8* dump_exefs(const char* path, u64* size) {
    const SnxHacEntry* files;
    size_t count, i;
    u8* data;
    int failed;
    char why[256] = {0};
    if (!g_hle.fs.dfs || strncmp(path, "exefs/", 6)) return NULL;
    count = snx_hac_fs_files(g_hle.fs.dfs, &files);
    for (i = 0; i < count && strcmp(files[i].name, path + 6); i++) continue;
    if (i == count || files[i].size > SIZE_MAX) return NULL;
    data = malloc(files[i].size ? (size_t)files[i].size : 1);
    if (!data) return NULL;
    pm_lock(&g_host.fs.dlock);
    failed = snx_hac_fs_read(g_hle.fs.dfs, i, 0, data, (size_t)files[i].size);
    if (failed) snprintf(why, sizeof why, "%s", snx_hac_message());
    pm_unlock(&g_host.fs.dlock);
    if (failed) {
        WARN("fs: could not read %s from the game's dump (hactool: %s)\n", path, why[0] ? why : "no reason given");
        free(data);
        return NULL;
    }
    *size = files[i].size;
    return data;
}
u8* fs_html_document(const char* path, u64* size) {
    char* full;
    u8* data = NULL;
    size_t n = 0;
    if (!g_hle.fs.dfs) return NULL;
    full = snx_format("html-document/%s", path);
    if (!full) return NULL;
    pm_lock(&g_host.fs.dlock);
    if (!g_hle.fs.html_tried) { /* opened when a page is first asked for, and kept, as the program's is */
        g_hle.fs.html_tried = 1;
        g_hle.fs.hfs = snx_dump_html_fs(&g_hle.fs.dump);
    }
    if (g_hle.fs.hfs) data = snx_romfs_file(g_hle.fs.hfs, full, &n);
    pm_unlock(&g_host.fs.dlock);
    free(full);
    if (data) *size = n;
    return data;
}

/* The checked DLCs' data folders, then the update's, laid over the game's, each read-only: what their extractions
   hold, a DLC's image in system/archives, an update's ExeFS and RomFS and its control.nacp, which replace the game's
   whole. Nothing else of theirs is: the game's saves and SD card stay its own, writable */
static void overlays(void) {
    static const char* const whole[] = {"exefs", "romfs", "romfs.bin", "control.nacp", NULL};
    static const char* const merged[] = {"system", NULL};
    SnxAddon* ids = malloc(SNX_ADDONS_MAX * sizeof *ids);
    size_t count, i;
    FsTreeResult r;
    if (!ids) fatal("Out of memory for the game's DLCs");
    count = snx_addons_parse(g_settings.game_addons, g_gamename, ids, SNX_ADDONS_MAX);
    for (i = 0; i < count; i++) {
        char text[17], *folder;
        if (snx_addon_kind(game_title(), ids[i].id) != SNX_ADDON_DLC) continue;
        snx_title_text(ids[i].id, text);
        folder = snx_format("%s%s_%s_data/", g_exedir, g_gamename, text);
        r = snx_is_dir(folder) ? fs_tree_overlay(&g_hle.fs.tree, folder, whole, merged) : FS_TREE_OK;
        if (r != FS_TREE_OK) {
            WARN("fs: could not index the DLC's files in %s: %s %s\n", folder, tree_at(folder), tree_why(r));
            fatal("Could not index the DLC's files, as one %s: the line before says which", tree_why(r));
        }
        free(folder);
    }
    free(ids);
    if (g_program) { /* another program of the application: the game's own program's files are none of its */
        for (i = 0; whole[i]; i++) {
            char* own = snx_format("%s%s", g_addondir, whole[i]);
            FsNode* node;
            if (!snx_path_exists(own) && !fs_tree_lookup(&g_hle.fs.tree, NULL, whole[i], &node))
                fs_tree_forget(&g_hle.fs.tree, whole[i]);
            free(own);
        }
    }
    if ((g_addon || g_program) && snx_is_dir(g_addondir)) { /* the update's or the program's, on top */
        r = fs_tree_overlay(&g_hle.fs.tree, g_addondir, whole, merged);
        if (r != FS_TREE_OK) {
            WARN("fs: could not index the %s's files in %s: %s %s\n", g_addon ? "update" : "program", g_addondir,
                 tree_at(g_addondir), tree_why(r));
            fatal("Could not index the %s's files, as one %s: the line before says which",
                  g_addon ? "update" : "program", tree_why(r));
        }
    }
}

/* 1 when path is in the ExeFS a dump read in place has: the program's whole ExeFS, an update's over any the game's
   data folder keeps */
static int dump_has_exefs(const char* path) { return g_hle.fs.dfs && !strncmp(path, "exefs/", 6); }

void fs_init(const char* keys, const char* title_keys) {
    static const char* const apart[] = {"mods", NULL}; /* mods_open's */
    FsTreeResult r = fs_tree_init_skipping(&g_hle.fs.tree, g_datadir, apart);
    if (r != FS_TREE_OK) {
        WARN("fs: could not index the game's files in %s: %s %s\n", g_datadir, tree_at(g_datadir), tree_why(r));
        fatal("Could not index the game's files, as one %s: the line before says which", tree_why(r));
    }
    size_t n = strlen(g_hle.fs.tree.base);
    if (n + 2 > sizeof g_datadir) fatal("Game filesystem path is too long");
    memcpy(g_datadir, g_hle.fs.tree.base, n);
    if (n && g_datadir[n - 1] != '/' && g_datadir[n - 1] != '\\') g_datadir[n++] = '/';
    g_datadir[n] = 0;
    /* RomFS and ExeFS are what games read most and never change: map them for the rest of the run, instead of opening
       them on demand like the save and SD card files. The ExeFS and a romfs.bin at startup; a romfs/ folder's files as
       the game first reads them (romfs_map), within the same limits: each mapped file holds a descriptor (Windows'
       C runtime has 8192, macOS allows 61440 a process) and a mapping (many Linux systems allow 65530, which guest
       memory needs too), and a game reads few of its files at a time (one has 304,000) */
    static const char* const shared_paths[] = {"exefs", "romfs", "romfs.bin"};
    FsShare share = {.bytes = (uint64_t)g_settings.map_limit_mb << 20, .files = (unsigned)g_settings.map_limit_files};
    pm_lock(&g_hle.fs.tree.lock);
    /* the console empties the application's temporary storage when the application is launched, and a game finds it
       empty at first use */
    r = fs_tree_remove(&g_hle.fs.tree, NULL, "save/temp", 1, 1, 0);
    if (r != FS_TREE_OK && r != FS_TREE_NOTFOUND) WARN("fs: could not empty save/temp (error %d)\n", r);
    overlays();
    dump_open(keys, title_keys);
    dlcs_open(keys, title_keys);
    if (!g_hle.fs.dfs) romfs_from_tmpfs();
    mods_open();
    for (size_t i = 0; i < sizeof shared_paths / sizeof shared_paths[0]; i++) {
        FsNode* node;
        if (g_hle.fs.dump.patch.found) break; /* an update's, read over the game's dump: the game's files unread */
        if (fs_tree_lookup(&g_hle.fs.tree, NULL, shared_paths[i], &node)) continue;
        share.lazy = !strcmp(shared_paths[i], "romfs");
        r = fs_tree_share(&g_hle.fs.tree, node, &share);
        if (r != FS_TREE_OK) fatal("Could not map %s%s (error %d)", g_datadir, shared_paths[i], r);
    }
    g_host.fs.romfs = share; /* what is left of the limits */
    g_host.fs.romfs.lazy = 1;
    pm_unlock(&g_hle.fs.tree.lock);
    LOG("fs: %u RomFS/ExeFS files mapped for the whole run, %.1f MB\n", share.shared,
        (double)share.mapped_bytes / 1048576.0);
    if (share.skipped)
        LOG("fs: %u RomFS/ExeFS files past map_limit_mb=%d or map_limit_files=%d are read on demand\n", share.skipped,
            g_settings.map_limit_mb, g_settings.map_limit_files);
    if (share.failed) WARN("fs: %u RomFS/ExeFS files could not be mapped, they are read on demand\n", share.failed);
}

/* Contents held in memory, read without the host: an injected file, or one mapped */
static int node_in_memory(const FsNode* node) { return node->is_virtual || fs_tree_mapped(node); }
/* tree.lock held: a RomFS file the game reads mapped, unless it is or the limits are reached (fs_tree_map). The first
   file past them is said in the log */
static void romfs_map(FsNode* node) {
    unsigned skipped = g_host.fs.romfs.skipped;
    if (!fs_tree_map(&g_hle.fs.tree, node, &g_host.fs.romfs) || skipped || g_host.fs.romfs.skipped == skipped) return;
    LOG("fs: %u RomFS/ExeFS files mapped, past map_limit_mb=%d or map_limit_files=%d: the game's other files are read "
        "on demand\n",
        g_host.fs.romfs.shared, g_settings.map_limit_mb, g_settings.map_limit_files);
}
static const u8* node_memory(const FsNode* node) { return node->is_virtual ? node->virtual_data : node->map; }

/* Callers hold tree.lock: content I/O is permitted only after resolving an indexed node. */
static FILE* node_open(FsNode* node, const char* mode) { return fs_tree_open(&g_hle.fs.tree, node, mode); }

FILE* fs_open_read(const char* path, u64* size) {
    FsNode* node;
    FILE* f = NULL;
    if (size) *size = 0;
    pm_lock(&g_hle.fs.tree.lock);
    if (!fs_tree_lookup(&g_hle.fs.tree, NULL, path, &node)) {
        f = node_open(node, "rb");
        if (f && size) *size = node->size;
    }
    pm_unlock(&g_hle.fs.tree.lock);
    return f;
}

size_t fs_patches(char* const** paths) {
    *paths = g_hle.fs.patches;
    return g_hle.fs.npatches;
}
int fs_map_file(const char* path, PFileMap* map) {
    FsNode* node;
    char host[FS_TREE_PATH_MAX];
    int r = -1;
    memset(map, 0, sizeof *map);
    if ((map->data = mod_exefs(path, &map->size)) || (map->data = dump_exefs(path, &map->size))) {
        /* a mod's, or read out of the dump: plat_file_unmap frees it */
        map->heap = 1;
        return 0;
    }
    if (dump_has_exefs(path)) return -1;
    pm_lock(&g_hle.fs.tree.lock);
    if (!fs_tree_lookup(&g_hle.fs.tree, NULL, path, &node) && !node->is_dir) {
        if (fs_tree_mapped(node)) { /* already mapped for the rest of the run: lend that view */
            map->data = node->map;
            map->size = node->size;
            map->borrowed = 1;
            r = 0;
        } else if (!node->is_virtual && !fs_tree_path(&g_hle.fs.tree, node, host, sizeof host))
            r = plat_file_map_sized(map, host, node->size);
    }
    pm_unlock(&g_hle.fs.tree.lock);
    return r;
}

u8* fs_read_file(const char* path, u64* size) {
    FsNode* node;
    u64 got;
    u8* data = NULL;
    if (size) *size = 0;
    if ((data = mod_exefs(path, &got)) || (data = dump_exefs(path, &got))) {
        if (size) *size = got;
        return data;
    }
    if (dump_has_exefs(path)) return NULL;
    pm_lock(&g_hle.fs.tree.lock);
    if (!fs_tree_lookup(&g_hle.fs.tree, NULL, path, &node) && !node->is_dir && node->size <= SIZE_MAX) {
        data = malloc(node->size ? (size_t)node->size : 1);
        if (data) {
            int ok = 1;
            if (node_in_memory(node)) {
                if (node->size) memcpy(data, node_memory(node), (size_t)node->size);
            } else {
                FILE* f = node_open(node, "rb");
                ok = f && fread(data, 1, (size_t)node->size, f) == node->size;
                if (f) fclose(f);
            }
            if (!ok) {
                free(data);
                data = NULL;
            } else if (size)
                *size = node->size;
        }
    }
    pm_unlock(&g_hle.fs.tree.lock);
    return data;
}

int fs_add_virtual_file(const char* path, const void* data, u64 size) {
    pm_lock(&g_hle.fs.tree.lock);
    FsTreeResult r = fs_tree_add_virtual(&g_hle.fs.tree, path, data, size, NULL);
    pm_unlock(&g_hle.fs.tree.lock);
    return r ? -1 : 0;
}
int fs_sd_write_new(const char* dir, const char* name, const void* data, u64 size) {
    char path[FS_TREE_PATH_MAX];
    FsNode *folder, *file = NULL;
    FsTreeResult r = FS_TREE_TOOLONG;
    FILE* f;
    if ((size_t)snprintf(path, sizeof path, "sdmc/%s", dir) >= sizeof path) return r;
    pm_lock(&g_hle.fs.tree.lock);
    r = fs_tree_mkdirs(&g_hle.fs.tree, path, &folder);
    if (r == FS_TREE_OK) r = fs_tree_create(&g_hle.fs.tree, folder, name, 0, size, &file);
    if (r == FS_TREE_OK) {
        f = fs_tree_open(&g_hle.fs.tree, file, "r+b");
        if (!f || fwrite(data, 1, (size_t)size, f) != size) r = FS_TREE_IO;
        if (f && fclose(f)) r = FS_TREE_IO;
        if (r) {
            int e = errno; /* the write's reason, which the removal must not change */
            fs_tree_remove(&g_hle.fs.tree, folder, name, 0, 0, 0);
            errno = e;
        }
    }
    pm_unlock(&g_hle.fs.tree.lock);
    return (int)r;
}

/* The RomFS image's layout (Switchbrew's "RomFS"; the hash and bucket count as Eden's fsmitm_romfsbuild): a header of
   ten u64s, then the directory hash table, the directory table, the file hash table and the file table, padded to
   0x200, then the file data. Table entries refer to each other by their offset in their table */
#define ROM_NONE      0xFFFFFFFFu
#define ROM_DIR_ENTRY 0x18 /* parent, sibling, first child, first file, next in bucket, name length; the name */
#define ROM_FILE_ENTRY                                                                                                 \
    0x20 /* parent, sibling, u64 data offset, u64 size, next in bucket, name length; the name */
static void put32(u8* p, u32 v) { snx_st_le32(p, v); } /* the image is the guest's: little-endian */
static void put64(u8* p, u64 v) { snx_st_le64(p, v); }
/* the bucket of an entry named name in the directory at table offset parent. The name's bytes count unsigned */
/* The format fixes both the seed (the parent's offset xor 123456789) and the step (rotate right by 5, then xor in the
   next byte): the game's own tables were built with it */
static u32 rhash(u32 parent, const char* name) {
    u32 h = parent ^ 123456789u;
    const unsigned char* p;
    for (p = (const unsigned char*)name; *p; p++) h = ((h >> 5) | (h << 27)) ^ *p;
    return h;
}
/* the buckets of a hash table for n entries */
static u32 hcount(u32 n) {
    u32 c = n;
    if (n < 3) return 3;
    if (n < 19) return n | 1;
    while (c % 2 == 0 || c % 3 == 0 || c % 5 == 0 || c % 7 == 0 || c % 11 == 0 || c % 13 == 0 || c % 17 == 0) c++;
    return c;
}
static u64 entry_size(u32 fixed, const char* name) { return fixed + ((strlen(name) + 3) & ~(size_t)3); }
/* A folder or file of the RomFS while its image is laid out: the game's, from the romfs/ folder (node) or from its own
   image (at base in it), or a mod's laid over it. Children in strcmp order, as the index keeps them */
typedef struct RNode {
    char* name;
    struct RNode *children, *next;
    FsNode* node; /* a file of a folder, else NULL */
    u64 base, size;
    int is_dir;
} RNode;
static RNode* rnode_new(const char* name, int is_dir) {
    RNode* n = calloc(1, sizeof *n);
    if (!n) fatal("RomFS: out of memory for its layout\n");
    n->name = snx_strdup(name);
    n->is_dir = is_dir;
    return n;
}
static void rnode_free(RNode* n) {
    while (n) {
        RNode* next = n->next;
        rnode_free(n->children);
        free(n->name);
        free(n);
        n = next;
    }
}
/* dir's child named name, made where it has none. One of the other kind is replaced, everything under it gone: a mod's
   file over the game's folder of its name, or its folder over a file. last: the child this returned before for dir
   (NULL for none), where the search starts when name comes after it. Its callers place a folder's entries in strcmp
   order, so each is found a step past the last, and a folder of n files is laid out in n steps, not n * n / 2 (one
   game's folder of 75,940 took a minute at every start) */
static RNode* rnode_child(RNode* dir, RNode* last, const char* name, int is_dir) {
    RNode **at = last && strcmp(last->name, name) < 0 ? &last->next : &dir->children, *n;
    int c = 1;
    while (*at && (c = strcmp((*at)->name, name)) < 0) at = &(*at)->next;
    if (*at && !c) {
        if ((*at)->is_dir == is_dir) return *at;
        n = *at;
        *at = n->next;
        n->next = NULL;
        rnode_free(n);
    }
    n = rnode_new(name, is_dir);
    n->next = *at;
    *at = n;
    return n;
}
/* the folder tree under dir laid over into (tree.lock held): a file over the one of its name, a folder's files and
   folders over those of the folder of its name */
static void rnode_lay(RNode* into, const FsNode* dir) {
    const FsNode* c;
    RNode* n = NULL;
    for (c = dir->children; c; c = c->next) {
        n = rnode_child(into, n, c->name, c->is_dir);
        if (c->is_dir)
            rnode_lay(n, c);
        else {
            n->node = (FsNode*)c;
            n->size = c->size;
        }
    }
}
/* adds the folders and files under dir to the counts and table sizes */
static void scan(const RNode* dir, u32* ndirs, u32* nfiles, u64* dir_bytes, u64* file_bytes) {
    const RNode* c;
    for (c = dir->children; c; c = c->next) {
        if (*ndirs >= INT32_MAX || *nfiles >= INT32_MAX) fatal("RomFS: too many entries in romfs/\n");
        if (c->is_dir) {
            ++*ndirs;
            *dir_bytes += entry_size(ROM_DIR_ENTRY, c->name);
            scan(c, ndirs, nfiles, dir_bytes, file_bytes);
        } else {
            ++*nfiles;
            *file_bytes += entry_size(ROM_FILE_ENTRY, c->name);
        }
    }
}
/* a folder while the image is made: its table offset and its entry's links */
typedef struct {
    const RNode* node;
    u32 parent; /* index in the folder list */
    u32 off, child, file;
} RDir;

/* Makes the image from the tree under dir (tree.lock held), what saying in the log what it is made of. Folders are
   listed breadth first, so each folder's subfolders are consecutive and chain as siblings, then each folder's files,
   folder by folder, in the index's strcmp order. File data follows the file table's order, each file at a multiple of
   16: the file list is in offset order as it is made, and must not be sorted (a zero-size file shares its offset with
   the next file) */
static void romfs_build(const RNode* dir, const char* what) {
    u32 ndirs = 1, nfiles = 0, i, nd, nf, dbuckets, fbuckets;
    u64 dir_bytes = ROM_DIR_ENTRY, file_bytes = 0, dhash, dtab, fhash, ftab, end, data = 0;
    RDir* dirs;
    u8* m;
    scan(dir, &ndirs, &nfiles, &dir_bytes, &file_bytes);
    if (dir_bytes >= ROM_NONE || file_bytes >= ROM_NONE) fatal("RomFS: the tables of romfs/ are too large\n");
    dbuckets = hcount(ndirs);
    fbuckets = hcount(nfiles);
    dhash = 0x50;
    dtab = dhash + (u64)dbuckets * 4;
    fhash = dtab + dir_bytes;
    ftab = fhash + (u64)fbuckets * 4;
    end = ftab + file_bytes;
    if (end > SIZE_MAX) fatal("RomFS: the tables of romfs/ are too large\n");
    if ((u64)ndirs * sizeof(RDir) > SIZE_MAX || (u64)nfiles * sizeof(RFile) > SIZE_MAX)
        fatal("RomFS: too many entries in romfs/\n");
    dirs = malloc(ndirs * sizeof *dirs);
    g_hle.fs.rfiles = malloc((nfiles ? nfiles : 1) * sizeof *g_hle.fs.rfiles);
    m = calloc(1, (size_t)end);
    if (!dirs || !g_hle.fs.rfiles || !m) fatal("RomFS: out of memory for the image of romfs/\n");
    memset(m + dhash, 0xFF, (size_t)dbuckets * 4);
    memset(m + fhash, 0xFF, (size_t)fbuckets * 4);

    /* the folders, breadth first, with their table offsets */
    dirs[0].node = dir;
    dirs[0].parent = 0;
    dirs[0].off = 0;
    nd = 1;
    {
        u32 off = ROM_DIR_ENTRY;
        for (i = 0; i < nd; i++) {
            const RNode* c;
            dirs[i].child = dirs[i].file = ROM_NONE;
            for (c = dirs[i].node->children; c; c = c->next) {
                if (!c->is_dir) continue;
                if (dirs[i].child == ROM_NONE) dirs[i].child = off;
                dirs[nd].node = c;
                dirs[nd].parent = i;
                dirs[nd].off = off;
                nd++;
                off += (u32)entry_size(ROM_DIR_ENTRY, c->name);
            }
        }
    }
    /* the files, folder by folder, with their data offsets */
    nf = 0;
    {
        u32 off = 0;
        for (i = 0; i < nd; i++) {
            const RNode* c;
            u8* e = NULL;
            for (c = dirs[i].node->children; c; c = c->next) {
                u32 len = (u32)strlen(c->name), b;
                if (c->is_dir) continue;
                if (e) put32(e + 4, off); /* the previous file's sibling */
                if (dirs[i].file == ROM_NONE) dirs[i].file = off;
                data = (data + 15) & ~(u64)15;
                if (data > (u64)INT64_MAX - c->size) fatal("RomFS: romfs/ is too large\n");
                e = m + ftab + off;
                put32(e, dirs[i].off);
                put32(e + 4, ROM_NONE);
                put64(e + 8, data);
                put64(e + 16, c->size);
                b = rhash(dirs[i].off, c->name) % fbuckets;
                memcpy(e + 24, m + fhash + (u64)b * 4, 4);
                put32(m + fhash + (u64)b * 4, off);
                put32(e + 28, len);
                memcpy(e + ROM_FILE_ENTRY, c->name, len);
                g_hle.fs.rfiles[nf].node = c->node;
                g_hle.fs.rfiles[nf].base = c->base;
                g_hle.fs.rfiles[nf].size = c->size;
                g_hle.fs.rfiles[nf].off = data; /* relative for now */
                g_hle.fs.rfiles[nf].logged = g_hle.fs.rfiles[nf].warned = 0;
                if (c->node) fs_tree_retain(c->node);
                nf++;
                data += c->size;
                off += (u32)entry_size(ROM_FILE_ENTRY, c->name);
            }
        }
    }
    /* the folder entries, now that their first files are known */
    for (i = 0; i < nd; i++) {
        u8* e = m + dtab + dirs[i].off;
        const char* name = i ? dirs[i].node->name : "";
        u32 len = (u32)strlen(name), b, parent = dirs[dirs[i].parent].off;
        int last = i + 1 == nd || dirs[i + 1].parent != dirs[i].parent || !i;
        put32(e, parent);
        put32(e + 4, last ? ROM_NONE : dirs[i + 1].off);
        put32(e + 8, dirs[i].child);
        put32(e + 12, dirs[i].file);
        b = rhash(parent, name) % dbuckets;
        memcpy(e + 16, m + dhash + (u64)b * 4, 4);
        put32(m + dhash + (u64)b * 4, dirs[i].off);
        put32(e + 20, len);
        memcpy(e + ROM_DIR_ENTRY, name, len);
    }
    free(dirs);

    g_hle.fs.rmeta = m;
    g_hle.fs.rmeta_size = end;
    g_hle.fs.rdata_off = (end + 0x1FF) & ~(u64)0x1FF;
    if (data > (u64)INT64_MAX - g_hle.fs.rdata_off) fatal("RomFS: romfs/ is too large\n");
    for (i = 0; i < nf; i++) g_hle.fs.rfiles[i].off += g_hle.fs.rdata_off;
    g_hle.fs.nrfiles = (int)nf;
    g_hle.fs.rtotal = g_hle.fs.rdata_off + data;
    put64(m, 0x50);
    put64(m + 8, dhash);
    put64(m + 16, (u64)dbuckets * 4);
    put64(m + 24, dtab);
    put64(m + 32, dir_bytes);
    put64(m + 40, fhash);
    put64(m + 48, (u64)fbuckets * 4);
    put64(m + 56, ftab);
    put64(m + 64, file_bytes);
    put64(m + 72, g_hle.fs.rdata_off);
    LOG("RomFS: built from %s (%u folders, %u files, %.1f MB)\n", what, nd, nf, (double)g_hle.fs.rtotal / 1048576.0);
}

/* size bytes of the game's own image at off into dst: romfs.bin, mapped or not, or the dump's. The caller checked the
   range against the image's size, and holds tree.lock when locked is set. 0, or -1 with the part that couldn't be read
   zeroed. quiet: the runtime's own read (romfs_peek), which warns of nothing */
static int image_read(u64 off, void* dst, u64 size, int locked, int quiet) {
    u8* out = dst;
    int rc;
    if (g_hle.fs.rimage) {
        memcpy(out, g_hle.fs.rimage->map + off, (size_t)size);
        return 0;
    }
    if (g_hle.fs.dfs) {
        char why[256] = {0};
        pm_lock(&g_host.fs.dlock);
        rc = size > SIZE_MAX ? -1 : snx_hac_fs_read_romfs(g_hle.fs.dfs, off, out, (size_t)size);
        if (rc) snprintf(why, sizeof why, "%s", snx_hac_message()); /* hactool's, which the next read clears */
        pm_unlock(&g_host.fs.dlock);
        if (rc) {
            memset(out, 0, (size_t)size);
            if (!quiet)
                WARN("RomFS: could not read the dump at %llx (%llx bytes; hactool: %s): the read fails, its buffer "
                     "zeroed\n",
                     (unsigned long long)off, (unsigned long long)size, why[0] ? why : "no reason given");
        }
        return rc;
    }
    if (g_hle.fs.rraw) {
        i64 got;
        if (!locked) pm_lock(&g_hle.fs.tree.lock);
        got = plat_pread(g_hle.fs.rraw, out, size, off);
        if (!locked) pm_unlock(&g_hle.fs.tree.lock);
        if (got == (i64)size) return 0;
        if (got < 0) got = 0;
        memset(out + got, 0, (size_t)(size - (u64)got));
        return -1;
    }
    memset(out, 0, (size_t)size);
    return -1;
}
/* the game's image's tables while its folders and files are laid out (image_dir): the folder and file tables, where
   its file data starts, its size, and how many more entries of each may be met, so that no loop in a broken image
   goes on */
typedef struct {
    const u8 *dirs, *files;
    u64 dir_bytes, file_bytes, data, total, dirs_left, files_left;
} RImage;
#define ROM_DEPTH_MAX 256
/* the name of length bytes at p, malloc'd: NULL for one that is no file's or folder's name */
static char* image_name(const u8* p, u32 length) {
    char* name;
    if (!length || memchr(p, 0, length) || memchr(p, '/', length)) return NULL;
    name = malloc((size_t)length + 1);
    if (!name) fatal("RomFS: out of memory for its layout\n");
    memcpy(name, p, length);
    name[length] = 0;
    return name;
}
/* the folder at off in the image's folder table laid out into into, its files and folders under it. 0, or -1 for one
   the tables don't hold */
static int image_dir(RImage* im, u32 off, RNode* into, unsigned depth) {
    u32 child, file;
    RNode* last = NULL;
    if (depth > ROM_DEPTH_MAX || (u64)off + ROM_DIR_ENTRY > im->dir_bytes) return -1;
    child = snx_ld_le32(im->dirs + off + 8);
    file = snx_ld_le32(im->dirs + off + 12);
    while (file != ROM_NONE) {
        const u8* e = im->files + file;
        u64 at, size;
        u32 length;
        char* name;
        RNode* n;
        if (!im->files_left-- || (u64)file + ROM_FILE_ENTRY > im->file_bytes) return -1;
        length = snx_ld_le32(e + 28);
        at = snx_ld_le64(e + 8);
        size = snx_ld_le64(e + 16);
        if (length > im->file_bytes - file - ROM_FILE_ENTRY || !(name = image_name(e + ROM_FILE_ENTRY, length)))
            return -1;
        if (at > im->total - im->data || size > im->total - im->data - at) {
            free(name);
            return -1;
        }
        n = last = rnode_child(into, last, name, 0);
        free(name);
        n->node = NULL;
        n->base = im->data + at;
        n->size = size;
        file = snx_ld_le32(e + 4);
    }
    while (child != ROM_NONE) {
        const u8* e = im->dirs + child;
        u32 length;
        char* name;
        RNode* n;
        if (!im->dirs_left-- || (u64)child + ROM_DIR_ENTRY > im->dir_bytes) return -1;
        length = snx_ld_le32(e + 20);
        if (length > im->dir_bytes - child - ROM_DIR_ENTRY || !(name = image_name(e + ROM_DIR_ENTRY, length)))
            return -1;
        n = last = rnode_child(into, last, name, 1);
        free(name);
        if (image_dir(im, child, n, depth + 1)) return -1;
        child = snx_ld_le32(e + 4);
    }
    return 0;
}
/* the game's own image, total bytes of it, laid out into root (tree.lock held), each file's data where the image has
   it. 0, or -1 for an image whose tables can't be read */
static int rnode_image(RNode* root, u64 total) {
    u8 head[0x50];
    u64 h[10];
    RImage im;
    u8 *dirs = NULL, *files = NULL;
    int rc = -1, i;
    if (total < sizeof head || image_read(0, head, sizeof head, 1, 0)) return -1;
    for (i = 0; i < 10; i++) h[i] = snx_ld_le64(head + i * 8);
    /* the header's size, then each table's offset and size (the folder hash table, the folder table, the file hash
       table, the file table), then where the file data starts */
    if (h[0] != sizeof head || h[3] > total || h[4] > total - h[3] || h[7] > total || h[8] > total - h[7] ||
        h[9] > total || h[4] < ROM_DIR_ENTRY || h[4] > UINT32_MAX || h[8] > UINT32_MAX)
        return -1;
    dirs = malloc((size_t)h[4]);
    files = malloc(h[8] ? (size_t)h[8] : 1);
    if (!dirs || !files) fatal("RomFS: out of memory for the game's image's tables\n");
    if (!image_read(h[3], dirs, h[4], 1, 0) && (!h[8] || !image_read(h[7], files, h[8], 1, 0))) {
        im.dirs = dirs;
        im.files = files;
        im.dir_bytes = h[4];
        im.file_bytes = h[8];
        im.data = h[9];
        im.total = total;
        im.dirs_left = h[4] / ROM_DIR_ENTRY;
        im.files_left = h[8] / ROM_FILE_ENTRY;
        rc = image_dir(&im, 0, root, 0);
    }
    free(dirs);
    free(files);
    return rc;
}

/* The dump's RomFS image read to its last byte at startup, which a dump cut short or damaged can't give: said here, not
   first where the game reads there. The image's size against the dump's tells nothing alone: compressed blocks and
   blocks of zeros make an image bigger than its dump */
static void dump_romfs_end(void) {
    char image[32], dump[32];
    u8 last;
    int rc;
    pm_lock(&g_host.fs.dlock);
    rc = snx_hac_fs_read_romfs(g_hle.fs.dfs, g_hle.fs.rtotal - 1, &last, 1);
    pm_unlock(&g_host.fs.dlock);
    if (!rc) return;
    snx_size_text(g_hle.fs.rtotal, image, sizeof image);
    snx_size_text(g_hle.fs.dmap.size + g_hle.fs.amap.size, dump, sizeof dump);
    WARN("RomFS: the dump's image is %s, and its last byte, at %llx, can't be read from the dump's %s (hactool: %s): "
         "the dump is incomplete or damaged\n",
         image, (unsigned long long)(g_hle.fs.rtotal - 1), dump,
         snx_hac_message()[0] ? snx_hac_message() : "no reason given");
}

/* The RomFS the game reads: the game's own image, else the image made up from its romfs/ folder; with mods played, made
   up from either with the mods' romfs/ folders laid over it (mods_open grafts them as mods.<i>) */
void romfs_init(void) {
    pm_lock(&g_hle.fs.tree.lock);
    FsNode *node, *folder = NULL;
    RNode* root;
    size_t i;
    int image = 0;
    if (!fs_tree_lookup(&g_hle.fs.tree, NULL, "romfs.bin", &node) && !node->is_dir) {
        if (node->fd >= 0)
            g_hle.fs.rimage = node;
        else if (!(g_hle.fs.rraw = node_open(node, "rb"))) {
            char path[FS_TREE_PATH_MAX], why[256];
            snx_os_error(why, sizeof why);
            if (fs_tree_path(&g_hle.fs.tree, node, path, sizeof path)) snprintf(path, sizeof path, "romfs.bin");
            /* the path in the log, not in the fatal: a game report's signature is the fatal's text */
            LOG("RomFS: cannot open %s: %s\n", path, why);
            fatal("Could not open the game's romfs.bin: %s", why);
        }
        g_hle.fs.rtotal = node->size;
        image = 1;
        LOG("RomFS: image (%.1f MB)\n", (double)g_hle.fs.rtotal / 1048576.0);
    } else if (g_hle.fs.dump.patch.found && g_hle.fs.dfs && snx_hac_fs_romfs_size(g_hle.fs.dfs)) {
        g_hle.fs.rtotal = snx_hac_fs_romfs_size(g_hle.fs.dfs); /* an update's over the game's, which it is made of */
        image = 1;
        LOG("RomFS: the update's over the game's dump, read in place (%.1f MB)\n",
            (double)g_hle.fs.rtotal / 1048576.0);
        dump_romfs_end();
    } else if (!fs_tree_lookup(&g_hle.fs.tree, NULL, "romfs:/", &node) && node->is_dir) {
        folder = node;
    } else if (g_hle.fs.dfs && snx_hac_fs_romfs_size(g_hle.fs.dfs)) {
        g_hle.fs.rtotal = snx_hac_fs_romfs_size(g_hle.fs.dfs);
        image = 1;
        LOG("RomFS: the dump's, read in place (%.1f MB)\n", (double)g_hle.fs.rtotal / 1048576.0);
        dump_romfs_end();
    } else if (g_hle.fs.dfs) {
        LOG("RomFS: none in the game's dump\n");
    } else {
        LOG("RomFS: none in %s\n", g_datadir);
        if (fs_tree_lookup(&g_hle.fs.tree, NULL, "exefs/main.nro", &node))
            WARN("Extract the game's RomFS into %sromfs/ before running it\n", g_datadir);
    }
    if (!folder && !g_hle.fs.nmromfs) {
        pm_unlock(&g_hle.fs.tree.lock);
        return;
    }
    root = rnode_new("", 1);
    if (folder)
        rnode_lay(root, folder);
    else if (image && rnode_image(root, g_hle.fs.rtotal)) {
        WARN("RomFS: the game's image has tables that can't be read, and the mods' RomFS is left out\n");
        rnode_free(root);
        pm_unlock(&g_hle.fs.tree.lock);
        return;
    }
    for (i = 0; i < g_hle.fs.nmromfs; i++) {
        char name[32];
        snprintf(name, sizeof name, "mods.%u", (unsigned)i);
        if (!fs_tree_lookup(&g_hle.fs.tree, NULL, name, &node)) rnode_lay(root, node);
    }
    if (g_hle.fs.nmromfs) {
        char what[64];
        snprintf(what, sizeof what, "the game's RomFS and %u mods' over it", (unsigned)g_hle.fs.nmromfs);
        g_hle.fs.rlayered = 1;
        romfs_build(root, what);
    } else
        romfs_build(root, "romfs/");
    rnode_free(root);
    pm_unlock(&g_hle.fs.tree.lock);
}

/* an unmapped RomFS file open for reading (tree.lock held), NULL when it can't be: a node deleted from the index
   isn't opened, and a failed open isn't kept */
static FILE* rf_open(FsNode* node) {
    ROpen* slot = &g_hle.fs.openf[0];
    FILE* f;
    unsigned i;
    if (!node->attached) return NULL;
    for (i = 0; i < NOPEN; i++) {
        if (g_hle.fs.openf[i].f && g_hle.fs.openf[i].node == node) {
            g_hle.fs.openf[i].used = ++g_hle.fs.openclk;
            return g_hle.fs.openf[i].f;
        }
        if (slot->f && (!g_hle.fs.openf[i].f || g_hle.fs.openf[i].used < slot->used)) slot = &g_hle.fs.openf[i];
    }
    f = node_open(node, "rb");
    if (!f) return NULL;
    if (slot->f) fclose(slot->f);
    slot->node = node;
    slot->f = f;
    slot->used = ++g_hle.fs.openclk;
    return f;
}
/* size bytes of the file at offset rel into dst: 0, or -1 with the part that couldn't be read zeroed. quiet: the
   runtime's own read, which doesn't count as the game's first of the file */
static int rfile_read(RFile* rf, u64 rel, u8* dst, u64 size, int quiet) {
    FsNode* node = rf->node;
    i64 got = (i64)size;
    if (!node) return image_read(rf->base + rel, dst, size, 0, quiet); /* the game's image's, under the mods' */
    if (!quiet && !__atomic_exchange_n(&rf->logged, 1, __ATOMIC_RELAXED))
        LOG("RomFS: reading %s (%.1f KB)\n", node->name, (double)node->size / 1024.0);
    if (fs_tree_mapped(node)) { /* mapped for the rest of the run, and never changes */
        memcpy(dst, node->map + rel, (size_t)size);
        return 0;
    }
    pm_lock(&g_hle.fs.tree.lock);
    if (!quiet) romfs_map(node); /* the game's read: the runtime's own (the shader pass) maps nothing */
    if (fs_tree_mapped(node)) {
        pm_unlock(&g_hle.fs.tree.lock);
        memcpy(dst, node->map + rel, (size_t)size);
        return 0;
    }
    if (node->is_virtual && node->attached) {
        memcpy(dst, node->virtual_data + rel, (size_t)size);
        pm_unlock(&g_hle.fs.tree.lock);
    } else if (quiet) {
        /* a file of its own, read outside tree.lock: the game's open files (rf_open's) stay open, and its file
           system commands don't wait for this read */
        FILE* f = node->attached ? node_open(node, "rb") : NULL;
        pm_unlock(&g_hle.fs.tree.lock);
        got = f ? plat_pread(f, dst, size, rel) : -1;
        if (f) fclose(f);
    } else {
        FILE* f = rf_open(node);
        got = f ? plat_pread(f, dst, size, rel) : -1;
        pm_unlock(&g_hle.fs.tree.lock);
    }
    if (got == (i64)size) return 0;
    if (got < 0) got = 0;
    memset(dst + got, 0, (size_t)(size - (u64)got));
    if (quiet) return -1; /* the runtime's own read says nothing: the game gets no zeros from it */
    if (!__atomic_exchange_n(&rf->warned, 1, __ATOMIC_RELAXED))
        WARN("RomFS: could not read %s at %llx (%llx bytes): the game gets zeros\n", node->name,
             (unsigned long long)rel, (unsigned long long)size);
    return -1;
}
/* size bytes of the image at off into dst. The caller checked the range against rtotal. 0, or -1 when part of it
   couldn't be read from the host, that part zeroed. quiet: as rfile_read's */
static int romfs_read(u64 off, void* dst, u64 size, int quiet) {
    u8* out = dst;
    int rc = 0;
    int lo, hi, k;
    if (!size) return 0;
    if (!g_hle.fs.rlayered && (g_hle.fs.rimage || g_hle.fs.dfs || g_hle.fs.rraw))
        return image_read(off, dst, size, 0, quiet);
    if (off < g_hle.fs.rmeta_size) {
        u64 n = size < g_hle.fs.rmeta_size - off ? size : g_hle.fs.rmeta_size - off;
        memcpy(out, g_hle.fs.rmeta + off, (size_t)n);
        out += n;
        off += n;
        size -= n;
    }
    /* the last file starting at or before off; a zero-size file comes before the one sharing its offset */
    lo = 0;
    hi = g_hle.fs.nrfiles;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (g_hle.fs.rfiles[mid].off <= off)
            lo = mid + 1;
        else
            hi = mid;
    }
    k = lo - 1;
    while (size) {
        u64 n;
        while (k + 1 < g_hle.fs.nrfiles && g_hle.fs.rfiles[k + 1].off <= off) k++; /* past the end of file k */
        if (k >= 0 && off - g_hle.fs.rfiles[k].off < g_hle.fs.rfiles[k].size) { /* inside file k */
            RFile* rf = &g_hle.fs.rfiles[k];
            n = rf->off + rf->size - off;
            if (n > size) n = size;
            if (rfile_read(rf, off - rf->off, out, n, quiet)) rc = -1;
        } else { /* padding up to the next file, or past the last */
            n = k + 1 < g_hle.fs.nrfiles ? g_hle.fs.rfiles[k + 1].off - off : size;
            if (n > size) n = size;
            memset(out, 0, (size_t)n);
        }
        out += n;
        off += n;
        size -= n;
    }
    return rc;
}
/* whether [off, off + size) lies in the image */
static int romfs_in(u64 off, u64 size) { return off <= g_hle.fs.rtotal && size <= g_hle.fs.rtotal - off; }
int romfs_peek(u64 off, void* dst, u64 size) {
    u8* out = dst;
    int rc = 0;
    if (!romfs_in(off, size)) return -1;
    while (size) {
        /* a piece at a time when a read can hold a lock (a dump's, an unmapped romfs.bin's, with or without mods
           over it), so the game's reads wait for one piece at most. A mapped image takes none, and one built from
           extracted files opens each file a read reaches: read whole */
        int locked = !g_hle.fs.rimage && (g_hle.fs.dfs || g_hle.fs.rraw);
        u64 n = locked && size > ((u64)1 << 20) ? (u64)1 << 20 : size;
        if (romfs_read(off, out, n, 1)) rc = -1;
        out += n;
        off += n;
        size -= n;
    }
    return rc;
}
/* the file table (the header's eighth and ninth u64s) read whole, its entries in their order */
int romfs_files(RomfsEach each, void* ctx) {
    u8 head[0x50];
    char name[0x301]; /* a RomFS name is at most 0x300 bytes */
    u64 table, size, data, at;
    u8* t;
    int rc = 0, malformed = 0;
    if (!g_hle.fs.rtotal) return 0;
    if (romfs_peek(0, head, sizeof head)) return -1;
    table = snx_ld_le64(head + 0x38);
    size = snx_ld_le64(head + 0x40);
    data = snx_ld_le64(head + 0x48);
    /* a header past the image, or a table bigger than any game's or past it: malformed, as on every call */
    if (data > g_hle.fs.rtotal || size > (64u << 20) || table > g_hle.fs.rtotal || size > g_hle.fs.rtotal - table)
        return -2;
    if (!(t = malloc(size ? (size_t)size : 1))) return -1;
    if (romfs_peek(table, t, size)) {
        free(t);
        return -1;
    }
    for (at = 0; !rc && at < size;) {
        u32 len;
        u64 off, bytes;
        if (size - at < ROM_FILE_ENTRY) {
            rc = -2; /* an entry cut short: the table ends there */
            break;
        }
        len = snx_ld_le32(t + at + 0x1C);
        off = snx_ld_le64(t + at + 8);
        bytes = snx_ld_le64(t + at + 0x10);
        if (len >= sizeof name || len > size - at - ROM_FILE_ENTRY) {
            rc = -2; /* a name past the table: no entry after it can be found */
            break;
        }
        memcpy(name, t + at + ROM_FILE_ENTRY, len);
        name[len] = 0;
        at += ROM_FILE_ENTRY + ((len + 3u) & ~3u);
        if (off > g_hle.fs.rtotal - data || bytes > g_hle.fs.rtotal - data - off) {
            malformed = 1; /* its data past the image: left out, the others listed */
            continue;
        }
        rc = each(ctx, name, data + off, bytes) ? 1 : 0;
    }
    free(t);
    return rc ? rc : malformed ? -2 : 0;
}
/* ============================================================ IStorage: the RomFS (no data) or a system archive */
/* u[0] of a storage without data: the RomFS, or a DLC's image read in place, u[1] its index in aocs */
enum { STORAGE_ROMFS, STORAGE_AOC };

/* the DLC read in place that is id, NULL for none */
static FsAoc* aoc_of(u64 id) {
    size_t i;
    for (i = 0; id && i < g_hle.fs.naocs; i++)
        if (g_hle.fs.aocs[i].dump.aoc.title_id == id) return &g_hle.fs.aocs[i];
    return NULL;
}
int fs_aoc_has(u64 id) { return aoc_of(id) != NULL; }
size_t fs_dlcs(const u64** ids) {
    *ids = g_hle.fs.dlcs;
    return g_hle.fs.ndlcs;
}

/* the first address of [addr, end), inside the address space, whose block can't take a command's bytes, with that
   block in *b; end when every one can */
static u64 buffer_stop(u64 addr, u64 end, MemBlock* b) {
    while (addr < end) {
        mem_query(addr, b);
        if (b->state == MS_FREE || b->state == MS_INACCESSIBLE || !(b->perm & PERM_W) || b->addr > addr ||
            b->size > AS_SIZE - b->addr || b->addr + b->size <= addr)
            return addr;
        addr = b->addr + b->size;
    }
    return end;
}
/* Whether [addr, addr + size) is a span the guest can hand a file system command: the range has to sit inside the
   address space, and every block of it has to be mapped and writable. A command that names a buffer the guest never
   reserved would otherwise let the runtime write past its reservation: one such buffer, in
   nn::fs::RomFsFileSystem::GetRequiredWorkingMemorySize's probe of the RomFS header, faulted on Windows where the
   page happened to be unmapped, and on Linux silently wrote over black because it was mapped. */
static int buffer_valid(u64 addr, u64 size) {
    u64 end = addr + size;
    MemBlock b;
    /* a span of no bytes needs no buffer: a read that runs past the end of a file has its length clamped to what is
       there, and one that starts at it is clamped to none, which the console answers with success and no bytes */
    return end >= addr && end <= AS_SIZE && buffer_stop(addr, end, &b) == end;
}
/* IStorage::Read's checks of [off, off + sz) of a storage of total bytes into guest memory at a: 0, or 2002-3005
   said with what refused it, as the result alone names nothing and a game may abort on it */
static int storage_span(u64 off, u64 sz, u64 total, u64 a) {
    u64 end = a + sz, at;
    MemBlock b;
    if (sz > (u64)INT64_MAX - off || off > total || sz > total - off) {
        WARN("fs: a storage read of %llx bytes at %llx passes the storage's end, %llx: it answers 2002-3005\n",
             (unsigned long long)sz, (unsigned long long)off, (unsigned long long)total);
        return SNX_RC_FS_OUT_OF_RANGE;
    }
    if (end < a || end > AS_SIZE) {
        WARN("fs: a storage read of %llx bytes at %llx goes into guest memory at %llx, past the address space's end, "
             "%llx: it answers 2002-3005\n",
             (unsigned long long)sz, (unsigned long long)off, (unsigned long long)a, (unsigned long long)AS_SIZE);
        return SNX_RC_FS_OUT_OF_RANGE;
    }
    if ((at = buffer_stop(a, end, &b)) < end) {
        WARN("fs: a storage read of %llx bytes at %llx goes into guest memory at %llx, whose address %llx can't take "
             "it (state %x, permission %x): it answers 2002-3005\n",
             (unsigned long long)sz, (unsigned long long)off, (unsigned long long)a, (unsigned long long)at, b.state,
             b.perm);
        return SNX_RC_FS_OUT_OF_RANGE;
    }
    return 0;
}
/* The same for a span the runtime only reads, as a path or a file's contents: the permission needed is a read one.
   A guest maps its paths and its read buffers in whatever mode its own code asked for, and demanding a write
   permission of a page the guest made read-only refused a command the console answers (a Card's file opened by path
   was one). */
static int readable_span(u64 addr, u64 size) {
    u64 end = addr + size;
    MemBlock b;
    if (end < addr || end > AS_SIZE) return 0;
    while (addr < end) {
        mem_query(addr, &b);
        if (b.state == MS_FREE || b.state == MS_INACCESSIBLE || !(b.perm & PERM_R) || b.addr > addr ||
            b.size > AS_SIZE - b.addr || b.addr + b.size <= addr)
            return 0;
        addr = b.addr + b.size;
    }
    return 1;
}
/* 64-bit file offsets: long is 32 bits on Windows. */
static int file_seek(FILE* f, u64 off) {
#if defined(_WIN32)
    return _fseeki64(f, (long long)off, SEEK_SET);
#else
    return fseeko(f, (off_t)off, SEEK_SET);
#endif
}

/* Atmosphere's StorageInterfaceAdapter and FileInterfaceAdapter validate the signed arguments before the backend, in
   this order, which is observable on errors */
static int check_transfer(i64 offset, IpcBuf buffer, i64 size) {
    R_UNLESS(offset >= 0, SNX_RC_FS_INVALID_OFFSET);
    R_UNLESS(size >= 0, SNX_RC_FS_INVALID_SIZE);
    R_UNLESS((i64)buffer.size >= 0 && (u64)size <= buffer.size, SNX_RC_FS_INVALID_SIZE);
    R_SUCCEED();
}
/* IStorage::Read's [off, off + sz) into guest memory at a, once the arguments are checked */
static int istorage_read(SvcObj* self, u64 off, u64 sz, u64 a) {
    FileData* data = self->data;
    int rc = 0;
    if (!data && self->u[0] == STORAGE_AOC) { /* a DLC's image, read in place */
        SnxHacFs* fs = g_hle.fs.aocs[self->u[1]].fs;
        u64 total = snx_hac_fs_romfs_size(fs);
        R_TRY(storage_span(off, sz, total, a));
        R_UNLESS(sz <= SIZE_MAX, SNX_RC_FS_OUT_OF_RANGE);
        char why[256] = {0};
        pm_lock(&g_host.fs.dlock);
        rc = snx_hac_fs_read_romfs(fs, off, GP(a), (size_t)sz);
        if (rc) snprintf(why, sizeof why, "%s", snx_hac_message());
        pm_unlock(&g_host.fs.dlock);
        if (rc)
            WARN("fs: could not read the RomFS of the DLC %016llx in its dump at %llx (%llx bytes; hactool: %s): the "
                 "read answers 2002-3005\n",
                 (unsigned long long)g_hle.fs.aocs[self->u[1]].dump.aoc.title_id, (unsigned long long)off,
                 (unsigned long long)sz, why[0] ? why : "no reason given");
        return rc ? SNX_RC_FS_OUT_OF_RANGE : 0;
    }
    if (!data) { /* the RomFS */
        R_TRY(storage_span(off, sz, g_hle.fs.rtotal, a));
        return romfs_read(off, GP(a), sz, 0) ? SNX_RC_FS_OUT_OF_RANGE : 0;
    }
    R_UNLESS(sz <= (u64)INT64_MAX - off && buffer_valid(a, sz), SNX_RC_FS_OUT_OF_RANGE);
    pm_lock(&g_hle.fs.tree.lock);
    if (off > data->node->size || sz > data->node->size - off)
        rc = SNX_RC_FS_OUT_OF_RANGE;
    else if (node_in_memory(data->node)) {
        if (sz) memcpy(GP(a), node_memory(data->node) + off, (size_t)sz);
    } else if (file_seek(data->f, off) || fread(GP(a), 1, (size_t)sz, data->f) != sz) {
        rc = SNX_RC_FS_OUT_OF_RANGE;
    }
    pm_unlock(&g_hle.fs.tree.lock);
    return rc;
}
/* A console's storage takes time for each read, and a game's read-ahead is timed against it: a movie library that
   caches its file a 64 KiB page at a time, as fast as the reads come back, filled a heap the game sized for the
   console before its decoder freed the buffers of its first configuration, and the game stopped at its own
   out-of-memory trap. So a stream, a read of FS_PACE_MAX_BYTES or less that starts where the same thread's last read
   of the same storage ended, takes the time FS_PACE_BYTES_PER_S gives it; a stream's first read, a bigger read (a
   load) and a read elsewhere take none. The game's decoder lets go of those buffers once the game takes its first
   picture, which a long frame there delayed by up to 1.2 s at opt 0: 8 MB/s keeps its cache under what fits
   for 1.8 s */
#define FS_PACE_MAX_BYTES   0x10000u
#define FS_PACE_BYTES_PER_S 8000000u
/* the ns a read of sz bytes at off of storage waits, for the thread whose last storage read t holds, which it
   becomes */
static u64 fs_pace_ns(TlsFs* t, const void* storage, u64 off, u64 sz) {
    int stream = t->storage == storage && t->next == off;
    t->storage = storage;
    t->next = off + sz;
    return stream && sz <= FS_PACE_MAX_BYTES ? sz * 1000000000ull / FS_PACE_BYTES_PER_S : 0;
}
static int nn_fssrv_sf_IStorage_Read(SvcObj* self, i64 offset, IpcBuf buffer, i64 size_in) {
    u64 off = (u64)offset, sz = (u64)size_in, a = buffer.addr;
    int rc = check_transfer(offset, buffer, size_in);
    if (rc) return rc;
    if (!sz) R_SUCCEED();
    if (!buffer_valid(a, sz)) return istorage_read(self, off, sz, a); /* which refuses it */
    /* the reads go through the kernel, which can't wait on memory the GPU holds (it lands first) nor fault on a page
       the write tracking protects (it opens, and stays open until the read is done) */
    own_acquire(a, sz);
    plat_ww_open(GP(a), sz);
    rc = istorage_read(self, off, sz, a);
    plat_ww_close(GP(a), sz);
    if (!rc) {
        u64 ns = fs_pace_ns(&t_hle.fs, self, off, sz);
        if (ns) {
            if (!__atomic_exchange_n(&g_host.fs.pace_told, 1, __ATOMIC_RELAXED))
                LOG("fs: storage reads of up to %u bytes that go on where the thread's last one ended take a "
                    "console's time from here, %u bytes a second (this one %llu bytes, %llu us)\n",
                    FS_PACE_MAX_BYTES, FS_PACE_BYTES_PER_S, (unsigned long long)sz, (unsigned long long)(ns / 1000));
            plat_sleep_ns((i64)ns);
        }
    }
    return rc;
}
/* the read-only storages: their backend rejects Write and SetSize, after the adapter's argument checks. Flush
   succeeds for these immutable images */
static int nn_fssrv_sf_IStorage_Write(SvcObj* self, i64 offset, IpcBuf buffer, i64 size) {
    R_TRY(check_transfer(offset, buffer, size));
    R_THROW(SNX_RC_FS_UNSUPPORTED_OPERATION);
}
static int nn_fssrv_sf_IStorage_Flush(SvcObj* self) { R_SUCCEED(); }
static int nn_fssrv_sf_IStorage_SetSize(SvcObj* self, i64 size) {
    return size < 0 ? SNX_RC_FS_INVALID_SIZE : SNX_RC_FS_UNSUPPORTED_OPERATION;
}
static int nn_fssrv_sf_IStorage_GetSize(SvcObj* self, i64* size) {
    FileData* data = self->data;
    if (!data) {
        *size = (i64)(self->u[0] == STORAGE_AOC ? snx_hac_fs_romfs_size(g_hle.fs.aocs[self->u[1]].fs)
                                                : g_hle.fs.rtotal);
        R_SUCCEED();
    }
    pm_lock(&g_hle.fs.tree.lock);
    *size = (i64)data->node->size;
    pm_unlock(&g_hle.fs.tree.lock);
    R_SUCCEED();
}
/* QueryRange (OperateRange): an all-zero QueryRangeInfo */
static int nn_fssrv_sf_IStorage_OperateRange(SvcObj* self, i32 operation_id, i64 offset, i64 size,
                                             nn_fs_QueryRangeInfo* info) {
    R_SUCCEED();
}

/* ============================================================ Indexed IFileSystem */
static u32 read_path(IpcBuf b, char out[0x301]) {
    /* the path is read, never written: it has to be mapped and readable, not writable (ldr.c's ro_readable) */
    R_UNLESS(b.addr && b.size && readable_span(b.addr, b.size), SNX_RC_FS_DIRECTORY_UNOBTAINABLE);
    size_t limit = b.size < 0x301 ? (size_t)b.size : 0x301;
    const char* start = (const char*)GP(b.addr);
    const char* end = memchr(start, 0, limit);
    R_UNLESS(end, SNX_RC_FS_TOO_LONG_PATH);
    memcpy(out, start, (size_t)(end - start) + 1);
    R_SUCCEED();
}

static int file_truncate(FILE* f, u64 n, u64* actual) {
    if (fflush(f)) return -1;
#if defined(_WIN32)
    int rc = _chsize_s(_fileno(f), (long long)n) ? -1 : 0;
    if (rc) {
        /* A failed extension can still change the file. Inspect this open handle, never rediscover its name. */
        i64 size = _filelengthi64(_fileno(f));
        if (size >= 0) *actual = (u64)size;
    }
#else
    int rc = ftruncate(fileno(f), (off_t)n);
    if (rc) {
        struct stat st;
        if (!fstat(fileno(f), &st) && st.st_size >= 0) *actual = (u64)st.st_size;
    }
#endif
    if (!rc) *actual = n;
    return rc;
}

/* All service operations share tree.lock, including file size updates across different handles: file_disp,
   dir_disp and fs_disp hold it around the commands. So these objects are made with those dispatchers (file_new,
   OpenDirectory, hostfs_new), never by the generated _new. ipc.c drops a failed request's objects once the dispatcher
   returned, outside the lock an IFile's destroy takes */
/* the game's own file I/O: the file and the bytes, as a zone, and the bytes as a counter for the rate */
u64 g_fs_bytes_read, g_fs_bytes_written;

static int nn_fssrv_sf_IFile_Read(SvcObj* self, nn_fs_ReadOption option, i64 offset, IpcBuf buffer, i64 size,
                                  i64* bytes_read) {
    FileData* f = self->data;
    u64 off = (u64)offset, sz = (u64)size, a = buffer.addr;
    /* IFile::Read -> DryRead: the order is observable on errors */
    R_TRY(check_transfer(offset, buffer, size));
    if (!sz) R_SUCCEED();
    R_UNLESS(sz <= (u64)INT64_MAX - off, SNX_RC_FS_OUT_OF_RANGE);
    R_UNLESS(f->mode & 1, SNX_RC_FS_READ_UNPERMITTED);
    R_UNLESS(off <= f->node->size, SNX_RC_FS_OUT_OF_RANGE);
    if (sz > f->node->size - off) sz = f->node->size - off;
    R_UNLESS(buffer_valid(a, sz), SNX_RC_FS_OUT_OF_RANGE);
    /* read() can't wait on memory the GPU holds (it lands first) nor fault on a page the write tracking protects (it
       opens, and stays open until the read is done) */
    own_acquire(a, sz);
    plat_ww_open(GP(a), sz);
    SNX_ZONE_TV(z, "fs read", sz, "%s", f->node->name);
    u64 n = sz;
    int rc = 0;
    if (node_in_memory(f->node)) {
        if (sz) memcpy(GP(a), node_memory(f->node) + off, (size_t)sz);
    } else if (file_seek(f->f, off))
        rc = SNX_RC_FS_OUT_OF_RANGE;
    else {
        clearerr(f->f);
        n = fread(GP(a), 1, (size_t)sz, f->f);
        if (ferror(f->f)) rc = SNX_RC_FS_OUT_OF_RANGE;
    }
    plat_ww_close(GP(a), sz);
    if (rc) return rc;
    __atomic_add_fetch(&g_fs_bytes_read, n, __ATOMIC_RELAXED);
    *bytes_read = (i64)n;
    R_SUCCEED();
}
static int nn_fssrv_sf_IFile_Write(SvcObj* self, nn_fs_WriteOption option, i64 offset, IpcBuf buffer, i64 size) {
    FileData* f = self->data;
    u64 off = (u64)offset, sz = (u64)size, a = buffer.addr;
    R_TRY(check_transfer(offset, buffer, size));
    /* IFile::Write succeeds without checking mode/EOF for an empty write, but honors Flush. */
    if (!sz) return (option & 1) && f->f && fflush(f->f) ? SNX_RC_FS_USABLE_SPACE_NOT_ENOUGH : 0;
    R_UNLESS(sz <= (u64)INT64_MAX - off, SNX_RC_FS_OUT_OF_RANGE);
    R_UNLESS((f->mode & 2) && !f->node->readonly, SNX_RC_FS_WRITE_UNPERMITTED);
    R_UNLESS(readable_span(a, sz), SNX_RC_FS_OUT_OF_RANGE); /* the buffer's bytes are read out, not written into */
    if (off + sz > f->node->size && !(f->mode & 4)) R_THROW(SNX_RC_FS_FILE_EXTENSION_WITHOUT_OPEN_MODE_ALLOW_APPEND);
    R_UNLESS(!file_seek(f->f, off), SNX_RC_FS_OUT_OF_RANGE);
    own_acquire(a, sz); /* write() can't wait on memory the GPU holds: it lands first */
    SNX_ZONE_TV(z, "fs write", sz, "%s", f->node->name);
    size_t n = fwrite(GP(a), 1, (size_t)sz, f->f);
    __atomic_add_fetch(&g_fs_bytes_written, n, __ATOMIC_RELAXED);
    if (n && off + n > f->node->size) f->node->size = off + n;
    R_UNLESS(n == sz && !fflush(f->f), SNX_RC_FS_USABLE_SPACE_NOT_ENOUGH);
    R_SUCCEED();
}
static int nn_fssrv_sf_IFile_Flush(SvcObj* self) {
    FileData* f = self->data;
    return f->f && fflush(f->f) ? SNX_RC_FS_USABLE_SPACE_NOT_ENOUGH : 0;
}
static int nn_fssrv_sf_IFile_SetSize(SvcObj* self, i64 size) {
    FileData* f = self->data;
    R_UNLESS(size >= 0, SNX_RC_FS_INVALID_SIZE);
    R_UNLESS((f->mode & 2) && !f->node->readonly, SNX_RC_FS_WRITE_UNPERMITTED);
    R_UNLESS(!file_truncate(f->f, (u64)size, &f->node->size), SNX_RC_FS_USABLE_SPACE_NOT_ENOUGH);
    R_SUCCEED();
}
static int nn_fssrv_sf_IFile_GetSize(SvcObj* self, i64* size) {
    *size = (i64)((FileData*)self->data)->node->size;
    R_SUCCEED();
}
static int nn_fssrv_sf_IFile_OperateRange(SvcObj* self, i32 operation_id, i64 offset, i64 size,
                                          nn_fs_QueryRangeInfo* info) {
    R_SUCCEED();
}
static int file_disp(SvcObj* o, IpcCtx* c) {
    pm_lock(&g_hle.fs.tree.lock);
    int r = nn_fssrv_sf_IFile_dispatch(o, c);
    pm_unlock(&g_hle.fs.tree.lock);
    return r;
}
static void file_destroy(SvcObj* o) {
    FileData* data = o->data;
    pm_lock(&g_hle.fs.tree.lock);
    if (data->f != NULL) fclose(data->f);
    fs_tree_release(data->node);
    data->node = NULL; /* the slot is in the pool until ipc.c takes it out: a save's commit looks at every one */
    pm_unlock(&g_hle.fs.tree.lock);
}
/* an IFile on the node, or an IStorage for a system archive */
static SvcObj* file_new(FsNode* node, u32 mode, int storage, u32* rc) {
    if (node->is_dir) {
        *rc = SNX_RC_FS_PATH_NOT_FOUND;
        return NULL;
    }
    if (!(mode & 3) || (mode & ~7u)) {
        *rc = SNX_RC_FS_INVALID_MODE_FOR_FILE_OPEN;
        return NULL;
    }
    if ((mode & 2) && node->readonly) {
        *rc = SNX_RC_FS_WRITE_UNPERMITTED;
        return NULL;
    }
    /* a mapped node is readonly, so this handle only reads, from the mapping */
    if (!(mode & 2)) romfs_map(node);
    FILE* host = node_in_memory(node) ? NULL : node_open(node, (mode & 2) ? "rb+" : "rb");
    if (!node_in_memory(node) && !host) {
        *rc = SNX_RC_FS_PATH_NOT_FOUND;
        return NULL;
    }
    SvcObj* o = storage
                    ? nn_fssrv_sf_IStorage_new_data(g_hle.fs.files, g_hle.fs.file_next, "files")
                    : SVCOBJ_NEW_DATA("nn::fssrv::sf::IFile", file_disp, g_hle.fs.files, g_hle.fs.file_next, "files");
    FileData* data = o->data;
    data->f = host;
    data->node = node;
    data->mode = mode;
    fs_tree_retain(node);
    o->destroy = file_destroy;
    *rc = 0;
    return o;
}

static int nn_fssrv_sf_IDirectory_Read(SvcObj* self, IpcBuf entries, i64* count) {
    DirData* d = self->data;
    u64 a = entries.addr, s = entries.size, n = 0;
    R_UNLESS(buffer_valid(a, s), SNX_RC_FS_OUT_OF_RANGE);
    u64 max = s / 0x310;
    FsNode* node = d->node->children;
    while (node && strcmp(node->name, d->last) <= 0) node = node->next;
    while (node && n < max && !d->done) {
        strcpy(d->last, node->name);
        if (node->is_dir ? (d->filter & 1) : (d->filter & 2)) {
            /* a 0x310-byte DirectoryEntry: the name (0x300 at most), the type at 0x304, the size at 0x308 */
            u8* entry = GP(a) + n++ * 0x310;
            u64 size = node->is_dir ? 0 : node->size;
            memset(entry, 0, 0x310);
            strncpy((char*)entry, node->name, 0x300);
            entry[0x304] = node->is_dir ? 0 : 1;
            snx_st_le64(entry + 0x308, size);
        }
        node = node->next;
    }
    if (!node) d->done = 1;
    *count = (i64)n;
    R_SUCCEED();
}
static int nn_fssrv_sf_IDirectory_GetEntryCount(SvcObj* self, i64* count) {
    DirData* d = self->data;
    u64 n = 0;
    for (FsNode* node = d->node->children; node; node = node->next)
        if (node->is_dir ? (d->filter & 1) : (d->filter & 2)) n++;
    *count = (i64)n;
    R_SUCCEED();
}
/* a directory deleted or moved away answers NotFound to everything */
static int dir_disp(SvcObj* o, IpcCtx* c) {
    pm_lock(&g_hle.fs.tree.lock);
    int r = ((DirData*)o->data)->node->attached ? nn_fssrv_sf_IDirectory_dispatch(o, c) : (int)SNX_RC_FS_PATH_NOT_FOUND;
    pm_unlock(&g_hle.fs.tree.lock);
    return r;
}
static void dir_destroy(SvcObj* o) {
    DirData* d = o->data;
    pm_lock(&g_hle.fs.tree.lock);
    fs_tree_release(d->node);
    pm_unlock(&g_hle.fs.tree.lock);
}
static FsNode* fs_root(SvcObj* self) { return ((FsData*)self->data)->root; }
static int nn_fssrv_sf_IFileSystem_CreateFile(SvcObj* self, IpcBuf path, i64 size, i32 option) {
    char p[0x301];
    R_UNLESS(size >= 0, SNX_RC_FS_INVALID_SIZE);
    R_TRY(read_path(path, p));
    return (int)tree_result(fs_tree_create(&g_hle.fs.tree, fs_root(self), p, 0, (u64)size, NULL));
}
static int nn_fssrv_sf_IFileSystem_DeleteFile(SvcObj* self, IpcBuf path) {
    char p[0x301];
    R_TRY(read_path(path, p));
    return (int)tree_result(fs_tree_remove(&g_hle.fs.tree, fs_root(self), p, 0, 0, 0));
}
static int nn_fssrv_sf_IFileSystem_CreateDirectory(SvcObj* self, IpcBuf path) {
    char p[0x301];
    R_TRY(read_path(path, p));
    return (int)tree_result(fs_tree_create(&g_hle.fs.tree, fs_root(self), p, 1, 0, NULL));
}
/* DeleteDirectory, DeleteDirectoryRecursively, CleanDirectoryRecursively (which keeps the directory itself) */
static int remove_dir(SvcObj* self, IpcBuf path, int recursive, int clean) {
    char p[0x301];
    R_TRY(read_path(path, p));
    return (int)tree_result(fs_tree_remove(&g_hle.fs.tree, fs_root(self), p, 1, recursive, clean));
}
static int nn_fssrv_sf_IFileSystem_DeleteDirectory(SvcObj* self, IpcBuf path) { return remove_dir(self, path, 0, 0); }
static int nn_fssrv_sf_IFileSystem_DeleteDirectoryRecursively(SvcObj* self, IpcBuf path) {
    return remove_dir(self, path, 1, 0);
}
static int nn_fssrv_sf_IFileSystem_CleanDirectoryRecursively(SvcObj* self, IpcBuf path) {
    return remove_dir(self, path, 1, 1);
}
static int rename_entry(SvcObj* self, IpcBuf old_path, IpcBuf new_path, int dir) {
    char p[0x301], p2[0x301];
    R_TRY(read_path(old_path, p));
    R_TRY(read_path(new_path, p2));
    return (int)tree_result(fs_tree_rename(&g_hle.fs.tree, fs_root(self), p, p2, dir));
}
static int nn_fssrv_sf_IFileSystem_RenameFile(SvcObj* self, IpcBuf old_path, IpcBuf new_path) {
    return rename_entry(self, old_path, new_path, 0);
}
static int nn_fssrv_sf_IFileSystem_RenameDirectory(SvcObj* self, IpcBuf old_path, IpcBuf new_path) {
    return rename_entry(self, old_path, new_path, 1);
}
static int nn_fssrv_sf_IFileSystem_GetEntryType(SvcObj* self, IpcBuf path, u32* type) {
    char p[0x301];
    FsNode* node;
    R_TRY(read_path(path, p));
    R_TRY(tree_result(fs_tree_lookup(&g_hle.fs.tree, fs_root(self), p, &node)));
    *type = node->is_dir ? 0 : 1;
    R_SUCCEED();
}
/* nn::fs::FileTimeStampRaw: the created, modified and accessed times in seconds since 1970, then 1 when they are set.
   The host tree keeps one time, the host file's modification, which stands for all three */
static int nn_fssrv_sf_IFileSystem_GetFileTimeStampRaw(SvcObj* self, IpcBuf path, nn_fs_FileTimeStampRaw* timestamp) {
    char p[0x301];
    FsNode* node;
    u64 ms, s;
    R_TRY(read_path(path, p));
    R_TRY(tree_result(fs_tree_lookup(&g_hle.fs.tree, fs_root(self), p, &node)));
    R_TRY(tree_result(fs_tree_mtime(&g_hle.fs.tree, node, &ms)));
    s = ms / 1000;
    memset(timestamp->v, 0, sizeof timestamp->v);
    snx_st_le64(timestamp->v, s); /* bytes in the guest's order, as the console lays them out */
    snx_st_le64(timestamp->v + 8, s);
    snx_st_le64(timestamp->v + 16, s);
    timestamp->v[24] = 1;
    R_SUCCEED();
}
static int nn_fssrv_sf_IFileSystem_OpenFile(SvcObj* self, IpcBuf path, u32 mode, SvcObj** file) {
    char p[0x301];
    u32 rc;
    FsNode* node;
    R_TRY(read_path(path, p));
    rc = tree_result(fs_tree_lookup(&g_hle.fs.tree, fs_root(self), p, &node));
    /* Lookup also normalizes the path. Normalization errors precede mode validation,
       but IFileSystem checks mode before its backend reports a missing file. */
    if (R_FAILED(rc) && rc != SNX_RC_FS_PATH_NOT_FOUND) return (int)rc;
    R_UNLESS((mode & 3) && !(mode & ~7u), SNX_RC_FS_INVALID_MODE_FOR_FILE_OPEN);
    if (rc) return (int)rc;
    *file = file_new(node, mode, 0, &rc);
    return (int)rc;
}
static int nn_fssrv_sf_IFileSystem_OpenDirectory(SvcObj* self, IpcBuf path, u32 mode, SvcObj** directory) {
    char p[0x301];
    u32 rc;
    FsNode* node;
    R_TRY(read_path(path, p));
    rc = tree_result(fs_tree_lookup(&g_hle.fs.tree, fs_root(self), p, &node));
    if (R_FAILED(rc) && rc != SNX_RC_FS_PATH_NOT_FOUND) return (int)rc;
    /* OpenDirectoryMode_NotRequireFileSize is bit 31, not another entry-type filter. */
    R_UNLESS((mode & 3) && !(mode & ~0x80000003u), SNX_RC_FS_INVALID_MODE_FOR_FILE_OPEN);
    if (rc) return (int)rc;
    R_UNLESS(node->is_dir, SNX_RC_FS_PATH_NOT_FOUND);
    SvcObj* dir =
        SVCOBJ_NEW_DATA("nn::fssrv::sf::IDirectory", dir_disp, g_hle.fs.dirs, g_hle.fs.dir_next, "directories");
    DirData* data = dir->data;
    data->node = node;
    data->filter = mode;
    fs_tree_retain(node);
    dir->destroy = dir_destroy;
    *directory = dir;
    R_SUCCEED();
}
static u32 fs_commit(FsData* data);
/* tree.lock held, as fs_disp holds it around every command */
static int nn_fssrv_sf_IFileSystem_Commit(SvcObj* self) { return (int)fs_commit(self->data); }
static int nn_fssrv_sf_IFileSystem_GetFreeSpaceSize(SvcObj* self, IpcBuf path, i64* size) {
    *size = 4ll << 30;
    R_SUCCEED();
}
static int nn_fssrv_sf_IFileSystem_GetTotalSpaceSize(SvcObj* self, IpcBuf path, i64* size) {
    *size = 8ll << 30;
    R_SUCCEED();
}
/* a filesystem whose root was deleted or moved away answers NotFound to everything */
static int fs_disp(SvcObj* o, IpcCtx* c) {
    pm_lock(&g_hle.fs.tree.lock);
    int r = ((FsData*)o->data)->root->attached ? nn_fssrv_sf_IFileSystem_dispatch(o, c) : (int)SNX_RC_FS_PATH_NOT_FOUND;
    pm_unlock(&g_hle.fs.tree.lock);
    return r;
}
static void fs_destroy(SvcObj* o) {
    FsData* data = o->data;
    pm_lock(&g_hle.fs.tree.lock);
    fs_tree_release(data->root);
    data->root = NULL; /* as an IFile's node: a save's open looks at every slot for one that has it open */
    pm_unlock(&g_hle.fs.tree.lock);
}
/* an IFileSystem of the folder root */
static SvcObj* fs_new(FsNode* root) { /* tree.lock held */
    SvcObj* o =
        SVCOBJ_NEW_DATA("nn::fssrv::sf::IFileSystem", fs_disp, g_hle.fs.fss, g_hle.fs.fs_next, "file systems");
    ((FsData*)o->data)->root = root;
    fs_tree_retain(root);
    o->destroy = fs_destroy;
    return o;
}
static SvcObj* hostfs_new(const char* root, u32* rc) { /* tree.lock held */
    FsNode* node;
    *rc = tree_result(fs_tree_mkdirs(&g_hle.fs.tree, root, &node));
    return *rc ? NULL : fs_new(node);
}

/* the system archives' stand-in: a RomFS with an empty root folder and nothing else, 0x200 bytes */
static void emptyrom_init(void) {
    u8* m = g_hle.fs.emptyrom;
    u32 dbuckets = hcount(1), fbuckets = hcount(0);
    u64 dtab = 0x50 + (u64)dbuckets * 4, fhash = dtab + ROM_DIR_ENTRY, ftab = fhash + (u64)fbuckets * 4;
    memset(m + 0x50, 0xFF, (size_t)(dtab - 0x50));
    memset(m + fhash, 0xFF, (size_t)(ftab - fhash));
    put32(m + 0x50 + (u64)(rhash(0, "") % dbuckets) * 4, 0); /* the root, at offset 0 */
    put32(m + dtab, 0);                                       /* its own parent */
    put32(m + dtab + 4, ROM_NONE);
    put32(m + dtab + 8, ROM_NONE);
    put32(m + dtab + 12, ROM_NONE);
    put32(m + dtab + 16, ROM_NONE);
    put32(m + dtab + 20, 0);
    put64(m, 0x50);
    put64(m + 8, 0x50);
    put64(m + 16, (u64)dbuckets * 4);
    put64(m + 24, dtab);
    put64(m + 32, ROM_DIR_ENTRY);
    put64(m + 40, fhash);
    put64(m + 48, (u64)fbuckets * 4);
    put64(m + 56, ftab);
    put64(m + 64, 0);
    put64(m + 72, sizeof g_hle.fs.emptyrom);
}

/* NgWord2, the profanity filter's pattern files, from Eden's stand-in (system_archive/ng_word.cpp): the 11.0.1 set,
   52 files, each ac_ file decoding to no bad words */
#define NGWORD2_DATA_ID  0x0100000000000823ull
#define NGWORD2_FILES    52
#define NGWORD2_AC_COUNT (NGWORD2_FILES - 1)
static const u8 ngword2_ac[] = {
    0x1F, 0x8B, 0x08, 0x08, 0xD5, 0x2C, 0x09, 0x5C, 0x04, 0x00, 0x61, 0x63, 0x72, 0x61, 0x77,
    0x00, 0xED, 0xC1, 0x01, 0x0D, 0x00, 0x00, 0x00, 0xC2, 0x20, 0xFB, 0xA7, 0xB6, 0xC7, 0x07,
    0x0C, 0x00, 0x00, 0x00, 0xC8, 0x3B, 0x11, 0x00, 0x1C, 0xC7, 0x00, 0x10, 0x00, 0x00,
};
#define NGWORD2_AC_SIZE (u32)(sizeof ngword2_ac)
static const u8 ngword2_version[4] = {0, 0, 0, 0x1A};

static void ngword2_name(u32 i, char name[32]) {
    static const char* const kinds[3] = {"b1", "b2", "not_b"};
    static const char* const common[3] = {"ac_common_b1_nx", "ac_common_b2_nx", "ac_common_not_b_nx"};
    if (i < 48)
        snprintf(name, 32, "ac_%u_%s_nx", i / 3, kinds[i % 3]);
    else if (i < NGWORD2_AC_COUNT)
        snprintf(name, 32, "%s", common[i - 48]);
    else
        snprintf(name, 32, "version.dat");
}
static u32 ngword2_file_size(u32 i) { return i < NGWORD2_AC_COUNT ? NGWORD2_AC_SIZE : 4; }
static u32 ngword2_file_write(u32 i, u8* out) {
    memcpy(out, i < NGWORD2_AC_COUNT ? ngword2_ac : ngword2_version, ngword2_file_size(i));
    return ngword2_file_size(i);
}

/* MiiModel (0100000000000802) when the game's data folder has none of the player's in system/archives: the one
   mii_model.c draws, its six files about 1 MB, made on the game's first open of it so that a game without Miis never
   makes it */
static u32 miimodel_file_room(u32 i) { return snx_mii_model_write(i, NULL); }
static u32 miimodel_file_write(u32 i, u8* out) { return snx_mii_model_write(i, out); }

/* a synthesized archive's files, in a RomFS laid out like romfs_build's with every file in its root folder, into m
   (cap bytes, nought where a file's writer leaves it); gives the bytes used. file_room(i) is the most bytes file i
   takes, and file_write(i, out) writes it and gives its bytes, 0 when it could not be made. A file file_same names an
   earlier one for (file_same NULL: none) has that one's bytes, and shares them */
#define FILESROM_MAX 64u
static u64 filesrom_init(u8* m, size_t cap, u32 count, void (*file_name)(u32, char[32]), u32 (*file_room)(u32),
                         u32 (*file_write)(u32, u8*), u32 (*file_same)(u32), const char* what) {
    u32 dbuckets = hcount(1), fbuckets = hcount(count);
    u64 dhash = 0x50;
    u64 dtab = dhash + (u64)dbuckets * 4;
    u64 fhash = dtab + ROM_DIR_ENTRY;
    u64 ftab = fhash + (u64)fbuckets * 4;
    u64 fsize = 0, room = 0, dsize = 0, fdata, off = 0, at[FILESROM_MAX], sizes[FILESROM_MAX];
    u32 i;
    char name[32];

    if (count > FILESROM_MAX)
        fatal("%s: %u files, more than a synthesized archive holds (%u)\n", what, count, FILESROM_MAX);
    for (i = 0; i < count; i++) {
        file_name(i, name);
        fsize += entry_size(ROM_FILE_ENTRY, name);
        if (!file_same || file_same(i) == i) room = ((room + 15) & ~(u64)15) + file_room(i);
    }
    fdata = (ftab + fsize + 0x1FF) & ~(u64)0x1FF;
    if (fdata + room > cap)
        fatal("%s: the synthesized archive takes up to %llu bytes, and has room for %llu\n", what,
              (unsigned long long)(fdata + room), (unsigned long long)cap);
    memset(m, 0, (size_t)(fdata + room));
    /* the files' bytes, each right after the one before: as none takes more than its room, they fit the rooms' sum */
    for (i = 0; i < count; i++) {
        u32 same = file_same ? file_same(i) : i;
        if (same != i) {
            at[i] = at[same];
            sizes[i] = sizes[same];
            continue;
        }
        dsize = (dsize + 15) & ~(u64)15;
        at[i] = dsize;
        sizes[i] = file_write(i, m + fdata + at[i]);
        if (!sizes[i]) {
            file_name(i, name);
            fatal("%s: %s could not be made: no memory for it\n", what, name);
        }
        dsize += sizes[i];
    }
    memset(m + dhash, 0xFF, (size_t)dbuckets * 4);
    memset(m + fhash, 0xFF, (size_t)fbuckets * 4);
    /* the root folder, its files starting at table offset 0 */
    put32(m + dhash + (u64)(rhash(0, "") % dbuckets) * 4, 0);
    put32(m + dtab, 0);
    put32(m + dtab + 4, ROM_NONE);
    put32(m + dtab + 8, ROM_NONE);
    put32(m + dtab + 12, 0);
    put32(m + dtab + 16, ROM_NONE);
    put32(m + dtab + 20, 0);
    for (i = 0; i < count; i++) {
        u32 len, b;
        u8* e;
        file_name(i, name);
        len = (u32)strlen(name);
        e = m + ftab + off;
        put32(e, 0); /* the root folder */
        put32(e + 4, i + 1 < count ? (u32)(off + entry_size(ROM_FILE_ENTRY, name)) : ROM_NONE);
        put64(e + 8, at[i]);
        put64(e + 16, sizes[i]);
        b = rhash(0, name) % fbuckets;
        memcpy(e + 24, m + fhash + (u64)b * 4, 4);
        put32(m + fhash + (u64)b * 4, (u32)off);
        put32(e + 28, len);
        memcpy(e + ROM_FILE_ENTRY, name, len);
        off += entry_size(ROM_FILE_ENTRY, name);
    }
    put64(m, 0x50);
    put64(m + 8, dhash);
    put64(m + 16, (u64)dbuckets * 4);
    put64(m + 24, dtab);
    put64(m + 32, ROM_DIR_ENTRY);
    put64(m + 40, fhash);
    put64(m + 48, (u64)fbuckets * 4);
    put64(m + 56, ftab);
    put64(m + 64, fsize);
    put64(m + 72, fdata);
    return fdata + dsize;
}
/* save data folders, by SaveDataType */
static const char* const g_save_kinds[] = {"system", "user", "bcat", "device", "temp", "cache", "sysbcat"};
/* the title ID the saves are kept by: the program's own, and a collection's other program (g_program) its
   application's, which fs_saves_init found */
static u64 save_title(void) { return g_program ? g_host.fs.save_title : g_hle.kernel.title_id; }
/* save_layout=global is in effect: its folder is known (fs_saves_init) and the program has a title ID of its own */
static int save_layout_global(void) {
    char title[17];
    return g_host.fs.save_root[0] && strcmp(g_settings.save_layout, "game") && snx_title_text(save_title(), title);
}
/* the folder name of a save's folder base, NULL when it has none of that name */
static FsNode* save_part(FsNode* base, const char* name) { /* tree.lock held */
    FsNode* node;
    return !fs_tree_lookup(&g_hle.fs.tree, base, name, &node) && node->is_dir ? node : NULL;
}
/* path in the tree is a save as the versions before kept one: a folder with the game's own files in it, and so
   without the file that says it is laid out as the console's (SNX_SAVE_JOURNALED) */
static int save_flat(const char* path) { /* tree.lock held */
    FsNode *node, *mark;
    return !fs_tree_lookup(&g_hle.fs.tree, NULL, path, &node) && node->is_dir && node->children &&
           fs_tree_lookup(&g_hle.fs.tree, node, SNX_SAVE_JOURNALED, &mark);
}
/* An Account save is save/user.<uid>, the folder of its user (users.c). The fixed uid's (am.c's g_user_id, user 1's
   when users.ini gives it none), while it has none, is the one an older version left, played where it is and said once
   a run (docs/user/deprecations.md): save/user, from before there were other users, else a nonempty save/system (earlier
   versions read the save type from a byte that is always 0).
   save/user and save/user.<uid> both there is two saves of one user: the game stops and leaves both, for the player to
   keep one. Puts the folder in p, and 1 in *both for that */
static u32 save_user(const nn_fs_SaveDataAttribute* attribute, char* p, size_t n, int* both) { /* tree.lock held */
    FsNode* node;
    const char* old = NULL;
    u64 uid[2];
    char text[33];
    int mine;
    uid[0] = snx_ld_le64(attribute->uid.v); /* a Uid is bytes: two little-endian words */
    uid[1] = snx_ld_le64(attribute->uid.v + 8);
    snx_users_uid_text(uid, text);
    snprintf(p, n, "save/user.%s", text);
    *both = 0;
    if (uid[0] != g_user_id[0] || uid[1] != g_user_id[1]) R_SUCCEED();
    mine = !fs_tree_lookup(&g_hle.fs.tree, NULL, p, &node);
    if (!fs_tree_lookup(&g_hle.fs.tree, NULL, "save/user", &node))
        old = "save/user";
    else if (!mine && save_flat("save/system")) /* a System save of this version's is not one of those */
        old = "save/system";
    if (!old) R_SUCCEED();
    if (mine) {
        *both = 1;
        R_THROW(SNX_RC_FS_PATH_ALREADY_EXISTS);
    }
    if (!save_layout_global()) /* save_layout=global says where it goes itself (save_folder) */
        WARN_LIMIT(1,
                   "fs: %s%s, the first user's saves, was left by an older version: this one keeps them in %s%s. The "
                   "game plays them where they are until you move the folder there (docs/user/deprecations.md)\n",
                   g_datadir, old, g_datadir, p);
    snprintf(p, n, "%s", old);
    R_SUCCEED();
}
/* the filesystems there are not here */
static int nn_fssrv_sf_IFileSystemProxy_OpenDataFileSystemByCurrentProcess(SvcObj* self, SvcObj** filesystem) {
    R_THROW(SNX_RC_FS_PARTITION_NOT_FOUND);
}
static int nn_fssrv_sf_IFileSystemProxy_OpenFileSystem(SvcObj* self, IpcBuf path, nn_fs_FileSystemProxyType type,
                                                       SvcObj** filesystem) {
    R_THROW(SNX_RC_FS_PARTITION_NOT_FOUND);
}
static int nn_fssrv_sf_IFileSystemProxy_OpenDataFileSystemByProgramId(SvcObj* self, nn_ncm_ProgramId program_id,
                                                                      SvcObj** filesystem) {
    R_THROW(SNX_RC_FS_PARTITION_NOT_FOUND);
}
static int nn_fssrv_sf_IFileSystemProxy_OpenBisStorage(SvcObj* self, u32 partition_id, SvcObj** storage) {
    R_THROW(SNX_RC_FS_PARTITION_NOT_FOUND);
}
static int nn_fssrv_sf_IFileSystemProxy_OpenGameCardStorage(SvcObj* self, u32 handle, u32 partition, SvcObj** storage) {
    R_THROW(SNX_RC_FS_PARTITION_NOT_FOUND);
}
static int nn_fssrv_sf_IFileSystemProxy_OpenImageDirectoryFileSystem(SvcObj* self, u32 storage_id,
                                                                     SvcObj** filesystem) {
    R_THROW(SNX_RC_FS_PARTITION_NOT_FOUND);
}
static int nn_fssrv_sf_IFileSystemProxy_OpenContentStorageFileSystem(SvcObj* self, u32 content_storage_id,
                                                                     SvcObj** filesystem) {
    R_THROW(SNX_RC_FS_PARTITION_NOT_FOUND);
}
static int nn_fssrv_sf_IFileSystemProxy_OpenDataStorageByPath(SvcObj* self, IpcBuf path,
                                                              nn_fs_ContentAttributes attributes, u32 type,
                                                              SvcObj** storage) {
    R_THROW(SNX_RC_FS_PARTITION_NOT_FOUND);
}
/* this program's own index (g_program, which GetProgramIndexForAccessLog reports): its data filesystem is not here
   either; another program's is TargetNotFound */
static int nn_fssrv_sf_IFileSystemProxy_OpenDataFileSystemWithProgramIndex(SvcObj* self, u8 program_index,
                                                                           SvcObj** filesystem) {
    R_THROW(program_index != g_program ? SNX_RC_FS_TARGET_NOT_FOUND : SNX_RC_FS_PARTITION_NOT_FOUND);
}
static int nn_fssrv_sf_IFileSystemProxy_OpenFileSystemWithPatch(SvcObj* self, nn_ncm_ProgramId program_id,
                                                                nn_fs_FileSystemProxyType type, SvcObj** filesystem) {
    R_THROW(SNX_RC_FS_PARTITION_NOT_FOUND);
}
static int nn_fssrv_sf_IFileSystemProxy_OpenFileSystemWithId(SvcObj* self, IpcBuf path, nn_ncm_ProgramId program_id,
                                                             nn_fs_FileSystemProxyType type, SvcObj** filesystem) {
    R_THROW(SNX_RC_FS_PARTITION_NOT_FOUND);
}
static int nn_fssrv_sf_IFileSystemProxy_OpenBisFileSystem(SvcObj* self, IpcBuf path, u32 partition_id,
                                                          SvcObj** filesystem) {
    R_THROW(SNX_RC_FS_PARTITION_NOT_FOUND);
}
static int nn_fssrv_sf_IFileSystemProxy_OpenHostFileSystem(SvcObj* self, IpcBuf path, SvcObj** filesystem) {
    R_THROW(SNX_RC_FS_PARTITION_NOT_FOUND);
}
static int nn_fssrv_sf_IFileSystemProxy_OpenHostFileSystemWithOption(SvcObj* self, IpcBuf path, u32 option,
                                                                     SvcObj** filesystem) {
    R_THROW(SNX_RC_FS_PARTITION_NOT_FOUND);
}
static int nn_fssrv_sf_IFileSystemProxy_FormatSdCardFileSystem(SvcObj* self) { return SNX_RC_FS_PARTITION_NOT_FOUND; }
static int nn_fssrv_sf_IFileSystemProxy_OpenGameCardFileSystem(SvcObj* self, u32 handle, u32 partition,
                                                               SvcObj** filesystem) {
    R_THROW(SNX_RC_FS_PARTITION_NOT_FOUND);
}
static int nn_fssrv_sf_IFileSystemProxy_OpenPatchDataStorageByCurrentProcess(SvcObj* self, SvcObj** storage) {
    R_THROW(SNX_RC_FS_PARTITION_NOT_FOUND);
}
static int nn_fssrv_sf_IFileSystemProxy_OpenRegisteredUpdatePartition(SvcObj* self, SvcObj** filesystem) {
    R_THROW(SNX_RC_FS_PARTITION_NOT_FOUND);
}
static int nn_fssrv_sf_IFileSystemProxy_OpenSdCardFileSystem(SvcObj* self, SvcObj** filesystem) {
    u32 rc;
    pm_lock(&g_hle.fs.tree.lock);
    *filesystem = hostfs_new("sdmc", &rc);
    pm_unlock(&g_hle.fs.tree.lock);
    return (int)rc;
}
/* the card OpenSdCardFileSystem serves is always there */
static int nn_fssrv_sf_IFileSystemProxy_IsSdCardAccessible(SvcObj* self, u8* accessible) {
    *accessible = 1;
    R_SUCCEED();
}
/* this program's index and its application's programs: those of the dump read in place, else as many as reach its
   index (a program count of 0 doesn't happen on the console) */
static int nn_fssrv_sf_IFileSystemProxy_GetProgramIndexForAccessLog(SvcObj* self, u32* program_index,
                                                                    u32* program_count) {
    (void)self;
    *program_index = (u32)g_program;
    *program_count = (u32)(g_hle.fs.dfs && g_hle.fs.dump.programs > g_program ? g_hle.fs.dump.programs : g_program + 1);
    R_SUCCEED();
}
/* save_layout=global: each save in one folder of every game's, the data folder's save/ (g_host.fs.save_root), apart
   from the games as the console keeps them: an Account save in user.<uid>/<title ID>/, Device's in device/<title ID>/,
   BCAT's in bcat/<title ID>/, cache storage in cache/<title ID>/<index>/, the system's in system/<save ID>/ and
   sysbcat/<save ID>/. Temporary storage stays the game's, which every start empties, and so do the saves of a program
   with no title ID of its own (a homebrew's), which would share one. Each folder is a graft of the tree: name, of host;
   the save is in it, or in its folder sub (an index, a save ID). 0 when the save has none */
static int save_global(const nn_fs_SaveDataAttribute* attribute, char* name, size_t name_size, char* host,
                       size_t host_size, char* sub, size_t sub_size) {
    char title[17], uid_text[33];
    u8 type = attribute->type;
    u64 uid[2];
    sub[0] = 0;
    if (type == 4 || !g_host.fs.save_root[0] || !snx_title_text(save_title(), title)) return 0;
    if (type == 1) {
        uid[0] = snx_ld_le64(attribute->uid.v);
        uid[1] = snx_ld_le64(attribute->uid.v + 8);
        snx_users_uid_text(uid, uid_text);
        snprintf(name, name_size, "global-user.%s", uid_text);
        snprintf(host, host_size, "%s/user.%s/%s", g_host.fs.save_root, uid_text, title);
    } else if (type == 2 || type == 3 || type == 5) {
        const char* kind = g_save_kinds[type];
        snprintf(name, name_size, "global-%s", kind);
        snprintf(host, host_size, "%s/%s/%s", g_host.fs.save_root, kind, title);
        if (type == 5) snprintf(sub, sub_size, "%u", (unsigned)attribute->index);
    } else { /* System, SystemBcat and the types sf doesn't name, which go as System */
        const char* kind = type == 6 ? "sysbcat" : "system";
        snprintf(name, name_size, "global-%s", kind);
        snprintf(host, host_size, "%s/%s", g_host.fs.save_root, kind);
        snprintf(sub, sub_size, "%016llX", (unsigned long long)attribute->system_save_data_id);
    }
    return 1;
}
/* path in the tree is a folder that holds a save, not one a mount made and nothing was committed to: the game's own
   files, as the versions before kept one, or a last commit with something in it; with no last commit, one cut short,
   a working copy with something in it */
static int save_there(const char* path) { /* tree.lock held */
    FsNode *node, *committed;
    if (save_flat(path)) return 1;
    if (fs_tree_lookup(&g_hle.fs.tree, NULL, path, &node) || !node->is_dir) return 0;
    committed = save_part(node, SNX_SAVE_COMMITTED);
    if (!committed) committed = save_part(node, SNX_SAVE_WORKING);
    return committed && committed->children;
}
/* save_layout=global's folders of this title there now, grafted: a save is found then without enumerating anything
   later. Called at startup, after the users are known */
/* A collection's other program keeps the saves of its application, by the ID its control.nacp names their owner
   (SaveDataOwnerId, at 0x3078), else its game's: the console's fs keeps the saves of a program of several by its
   main program (ProgramIndexMapInfo), and every program of such an application names that one owner, so they all
   play one save. Eden keeps one by each program's own ID */
static void save_title_find(void) {
    u64 size = 0, owner = 0;
    u8* nacp;
    if (!g_program) return;
    nacp = fs_read_file("control.nacp", &size);
    if (nacp && size >= 0x3080) owner = snx_ld_le64(nacp + 0x3078);
    free(nacp);
    g_host.fs.save_title = owner ? owner : game_title();
    LOG("fs: program %d keeps its saves as %016llx, %s\n", g_program, (unsigned long long)g_host.fs.save_title,
        owner ? "the owner its control.nacp names (SaveDataOwnerId)" : "its game's: its control.nacp names no owner");
}
void fs_saves_init(const char* root) {
    static const u8 types[] = {2, 3, 5, 0, 6};
    nn_fs_SaveDataAttribute a;
    char name[64], host[FS_TREE_PATH_MAX], sub[24];
    int i;
    save_title_find();
    snprintf(g_host.fs.save_root, sizeof g_host.fs.save_root, "%s", root ? root : "");
    pm_lock(&g_hle.fs.tree.lock);
    for (i = 0; i < g_user_count + (int)sizeof types; i++) {
        memset(&a, 0, sizeof a);
        if (i < g_user_count) {
            a.type = 1;
            snx_st_le64(a.uid.v, g_users[i].uid[0]);
            snx_st_le64(a.uid.v + 8, g_users[i].uid[1]);
        } else
            a.type = types[i - g_user_count];
        if (!save_global(&a, name, sizeof name, host, sizeof host, sub, sizeof sub) || !snx_is_dir(host)) continue;
        FsTreeResult r = fs_tree_graft_writable(&g_hle.fs.tree, name, host);
        if (r != FS_TREE_OK && r != FS_TREE_EXISTS) {
            WARN("fs: could not index the saves in %s: %s %s\n", host, tree_at(host), tree_why(r));
            fatal("Could not index the saves, as one %s: the line before says which", tree_why(r));
        }
    }
    pm_unlock(&g_hle.fs.tree.lock);
}
int fs_user_saves_take(const u64 uid[2], char (*paths)[FS_TREE_PATH_MAX], int max) {
    char text[33], name[64], game[64];
    FsNode* node;
    int n = 0;
    snx_users_uid_text(uid, text);
    snprintf(name, sizeof name, "global-user.%s", text);
    snprintf(game, sizeof game, "save/user.%s", text);
    pm_lock(&g_hle.fs.tree.lock);
    if (g_host.fs.save_root[0] && n < max) {
        snprintf(paths[n], FS_TREE_PATH_MAX, "%s/user.%s", g_host.fs.save_root, text);
        if (snx_is_dir(paths[n])) {
            if (!fs_tree_lookup(&g_hle.fs.tree, NULL, name, &node)) fs_tree_forget(&g_hle.fs.tree, name);
            n++;
        }
    }
    if (n < max && !fs_tree_lookup(&g_hle.fs.tree, NULL, game, &node) && node->is_dir &&
        !fs_tree_path(&g_hle.fs.tree, node, paths[n], FS_TREE_PATH_MAX) && !fs_tree_forget(&g_hle.fs.tree, game))
        n++;
    pm_unlock(&g_hle.fs.tree.lock);
    return n;
}
/* The folder of the attribute's save in the tree, into p, as save_layout says: global's (save_global), game's
   save/<kind> of the game's data folder, an Account save its user's (save_user). The other layout's while only it
   holds the save, played where it is and said once a run; both holding one is two saves of it, which stop says and
   the game stops for, leaving both */
static u32 save_folder(const nn_fs_SaveDataAttribute* attribute, char* p, size_t n, char* stop, size_t stop_size) {
    char name[64], host[FS_TREE_PATH_MAX], sub[24], global[96], game[64];
    u8 type = attribute->type;
    const char* kind = type < sizeof g_save_kinds / sizeof g_save_kinds[0] ? g_save_kinds[type] : "system";
    int both = 0, mine = strcmp(g_settings.save_layout, "game"), in_global, in_game;
    FsNode* node;
    u32 rc = 0;
    stop[0] = 0;
    snprintf(game, sizeof game, "save/%s", kind);
    if (type == 1) rc = save_user(attribute, game, sizeof game, &both);
    if (both) {
        snprintf(stop, stop_size,
                 "Two saves of the first user: %ssave/user, from before there were other users, and %s%s. Keep the "
                 "one you play and move the other out of %ssave/, then start the game again",
                 g_datadir, g_datadir, game, g_datadir);
        return rc;
    }
    if (R_FAILED(rc)) return rc;
    if (!save_global(attribute, name, sizeof name, host, sizeof host, sub, sizeof sub)) {
        snprintf(p, n, "%s", game);
        R_SUCCEED();
    }
    snprintf(global, sizeof global, "%s%s%s", name, sub[0] ? "/" : "", sub);
    in_global = save_there(global);
    in_game = save_there(game);
    if (in_global && in_game) {
        snprintf(stop, stop_size,
                 "Two saves of this game's %s save: %s%s, each game's own, and %s%s%s, the one of every game's "
                 "(save_layout). Keep the one you play and move the other out, then start the game again",
                 kind, g_datadir, game, host, sub[0] ? "/" : "", sub);
        R_SUCCEED();
    }
    if (mine ? in_game : in_global) {
        unsigned bit = type < 8 ? type : 0;
        if (!(g_host.fs.save_told >> bit & 1)) {
            g_host.fs.save_told |= 1u << bit;
            if (mine)
                WARN("fs: %s%s holds this game's %s save, which save_layout=global keeps in %s%s%s. The game plays it "
                     "where it is until you move the folder there (Settings > App > Move Old Files, "
                     "docs/user/deprecations.md)\n",
                     g_datadir, game, kind, host, sub[0] ? "/" : "", sub);
            else
                WARN("fs: %s%s%s holds this game's %s save, which save_layout=game keeps in %s%s. The game plays it "
                     "where it is until you move the folder there\n",
                     host, sub[0] ? "/" : "", sub, kind, g_datadir, game);
        }
        snprintf(p, n, "%s", mine ? game : global);
        R_SUCCEED();
    }
    if (!mine) {
        snprintf(p, n, "%s", game);
        R_SUCCEED();
    }
    /* global's, made now when it is not there: the folder above it first, then the graft */
    if (fs_tree_lookup(&g_hle.fs.tree, NULL, name, &node)) {
        char* parent = snx_strdup(host);
        char* cut = strrchr(parent, '/');
        if (cut) *cut = 0;
        if (cut && snx_mkdirs(parent)) {
            WARN("fs: could not make %s for this game's %s save\n", parent, kind);
            free(parent);
            R_THROW(SNX_RC_FS_PATH_NOT_FOUND);
        }
        free(parent);
        FsTreeResult r = fs_tree_graft_new(&g_hle.fs.tree, name, host);
        if (r != FS_TREE_OK) {
            WARN("fs: could not make %s for this game's %s save: %s %s\n", host, kind, tree_at(host), tree_why(r));
            return tree_result(r);
        }
    }
    snprintf(p, n, "%s", global);
    R_SUCCEED();
}
/* A save's note: the program that last committed it, in <the save's folder>.version (SNX_SAVE_NOTE) beside it on the
   host, where none of the game's mounts reaches. A game may not read a save a newer version of it wrote (an older one crashes on it),
   so opening a save whose note names a newer version says both, and the crash reports say it again (fs_save_report).
   An Account, Device and BCAT save have one; temporary and cache storage, the system's and the saves of a program with
   no title ID of its own (a homebrew's) none. A save with no note says nothing until its first commit writes one */
typedef struct {
    u64 program;      /* the title ID that ran, an update's own */
    u64 version;      /* its content meta's: 0 for the game's own program, SNX_NO_VERSION when not known */
    char display[16]; /* its control data's display version */
} SaveNote;
/* the program that runs, as its note names it. The version: an update's from its program's name (main.c's
   addon_init); the update's in the dump the game's program reads in place when the dump holds one, else 0; not known
   for the game's own program extracted (an update applied to it says its version nowhere). 0 for a program with no
   title ID of its own */
static int save_note_now(SaveNote* note) {
    char title[17], *c;
    size_t k = strlen(g_gamename);
    memset(note, 0, sizeof *note);
    if (!snx_title_text(g_hle.kernel.title_id, title)) return 0;
    if (g_addon) {
        note->program = g_addon;
        note->version = !strncmp(g_exename, g_gamename, k) && g_exename[k] == '_' ? snx_addon_version(g_exename + k + 1)
                                                                                  : SNX_NO_VERSION;
    } else {
        note->program = g_hle.kernel.title_id;
        note->version = !g_hle.fs.dfs ? SNX_NO_VERSION : g_hle.fs.dump.patch.found ? g_hle.fs.dump.version : 0;
    }
    am_display_version(note->display);
    for (c = note->display; *c; c++)
        if ((unsigned char)*c < 32 || (unsigned char)*c > 126) *c = '?'; /* one line of the note */
    return 1;
}
/* the note of the save whose folder is root: its host path into path. 0, or nonzero for one past size */
static int save_note_path(FsNode* root, char* path, size_t size) { /* tree.lock held */
    size_t n;
    if (fs_tree_path(&g_hle.fs.tree, root, path, size)) return 1;
    n = strlen(path);
    while (n > 1 && (path[n - 1] == '/' || path[n - 1] == '\\')) path[--n] = 0;
    if (size - n < sizeof SNX_SAVE_NOTE) return 1;
    memcpy(path + n, SNX_SAVE_NOTE, sizeof SNX_SAVE_NOTE);
    return 0;
}
/* the note at path into note: 1 when it is there and names a program, else 0 */
static int save_note_read(const char* path, SaveNote* note) {
    size_t size;
    char* text = (char*)snx_read_file(path, &size);
    char *key, *value, *end;
    SnxIni ini;
    memset(note, 0, sizeof *note);
    note->version = SNX_NO_VERSION;
    if (!text) return 0;
    snx_ini_start(&ini, text);
    while (snx_ini_next(&ini, &key, &value)) {
        if (!strcmp(key, "program"))
            snx_title_parse(value, &note->program);
        else if (!strcmp(key, "version") && *value >= '0' && *value <= '9') {
            unsigned long long v = strtoull(value, &end, 10);
            if (!*end) note->version = v;
        } else if (!strcmp(key, "display"))
            snprintf(note->display, sizeof note->display, "%s", value);
    }
    free(text);
    return note->program != 0;
}
/* the numbers of a display version, a run of digits each, compared in turn, a missing one as 0: 1 when a's are
   greater than b's (1.10.0 after 1.9.2), 0 when they are the same or either has none */
static int save_display_newer(const char* a, const char* b) {
    char* end;
    if (!strpbrk(a, "0123456789") || !strpbrk(b, "0123456789")) return 0;
    for (;;) {
        unsigned long long x = 0, y = 0;
        a += strcspn(a, "0123456789");
        b += strcspn(b, "0123456789");
        if (!*a && !*b) return 0;
        if (*a) {
            x = strtoull(a, &end, 10);
            a = end;
        }
        if (*b) {
            y = strtoull(b, &end, 10);
            b = end;
        }
        if (x != y) return x > y;
    }
}
/* 1 when a is a newer version than b: by their content meta versions when both are known, else by their display
   versions' numbers; 0 when they are the same or neither tells */
static int save_note_newer(const SaveNote* a, const SaveNote* b) {
    if (a->version != SNX_NO_VERSION && b->version != SNX_NO_VERSION) return a->version > b->version;
    return save_display_newer(a->display, b->display);
}
/* a version as a message names it: its display version, its content meta's after it when known */
static void save_note_name(const SaveNote* note, char* out, size_t size) {
    if (note->version != SNX_NO_VERSION)
        snprintf(out, size, "%s (%llu, program %016llX)", note->display, (unsigned long long)note->version,
                 (unsigned long long)note->program);
    else
        snprintf(out, size, "%s (program %016llX)", note->display, (unsigned long long)note->program);
}
/* a save opened: said when its note names a newer version of the game than this program, once for each save and
   version (a game opens its save again and again), and kept for the crash reports */
static void save_note_check(FsNode* root, const char* kind) { /* tree.lock held */
    SaveNote now, was;
    char path[FS_TREE_PATH_MAX + sizeof SNX_SAVE_NOTE], line[sizeof g_host.fs.save_newer], a[64], b[64];
    if (!save_note_now(&now) || save_note_path(root, path, sizeof path) || !save_note_read(path, &was)) return;
    /* the game's, its update's, or one of its application's other programs, which play the same save */
    if (snx_program_index(save_title(), was.program) < 0) {
        LOG_LIMIT(4, "fs: %s names the program %016llX, not this game's %016llX: its version is not compared\n", path,
                  (unsigned long long)was.program, (unsigned long long)save_title());
        return;
    }
    if (!save_note_newer(&was, &now)) return;
    save_note_name(&was, a, sizeof a);
    save_note_name(&now, b, sizeof b);
    snprintf(line, sizeof line,
             "%.*s, this game's %s save, was last written by version %s; this program is %s: a game may not read a "
             "save a newer version of it wrote",
             (int)(strlen(path) - (sizeof SNX_SAVE_NOTE - 1)), path, kind, a, b);
    if (!strcmp(line, g_host.fs.save_newer)) return;
    snprintf(g_host.fs.save_newer, sizeof g_host.fs.save_newer, "%s", line);
    WARN("fs: %s\n", line);
}
/* a save committed: its note names this program from then on (written when it named another) */
static void save_note_write(FsNode* root) { /* tree.lock held */
    SaveNote now;
    char path[FS_TREE_PATH_MAX + sizeof SNX_SAVE_NOTE], text[128], why[256];
    int n;
    if (!save_note_now(&now)) return;
    if (save_note_path(root, path, sizeof path)) {
        WARN_LIMIT(4, "fs: a save's folder has a path too long for its note beside it: no note of which version wrote "
                      "it\n");
        return;
    }
    n = snprintf(text, sizeof text, "program=%016llX\n", (unsigned long long)now.program);
    if (now.version != SNX_NO_VERSION)
        n += snprintf(text + n, sizeof text - (size_t)n, "version=%llu\n", (unsigned long long)now.version);
    snprintf(text + n, sizeof text - (size_t)n, "display=%s\n", now.display);
    if (snx_write_changed(path, text, strlen(text)))
        WARN_LIMIT(4, "fs: could not write %s, the note of which version wrote the save beside it: %s\n", path,
                   snx_os_error(why, sizeof why));
}
void fs_save_report(void) {
    if (g_host.fs.save_newer[0]) WARN("  %s\n", g_host.fs.save_newer);
}
/* ============================================================ a save's commit
   A save is kept as the console keeps one in a folder of the host (its DirectorySaveDataFileSystem, as LibHac has
   it): in the save's folder, 0 holds its last commit and 1 is the copy the game works in, the only one it sees.
   Opening the save makes 1 a copy of 0, so what a run wrote and did not commit is gone; with no 0, a commit was cut
   short (the game killed in it) and the open finishes it from 1. Commit renames 0 to _, makes _ a copy of 1 and
   renames it 0. The first rename is the commit and 0 is never written in place, so a game that dies at any moment
   leaves its last commit whole or the new one, as long as the host's rename of a folder is whole. Nothing is synced:
   a host that loses power in a commit is the host's file system's to answer for, as it is the console's.
   Temporary storage has no commit, on the console either, and is written where it is. A save the versions before
   kept, the game's files in its folder itself, has no SNX_SAVE_JOURNALED and is played where it is, as it was. */
#define SAVE_SYNCING "_"
#define SAVE_CHUNK   0x100000 /* the console's work buffer for the copy */
/* why a save's folder could not be changed, for a message: the system's reason when the host refused, else the
   index's */
static const char* save_why(FsTreeResult r, char* why, size_t size) {
    switch (r) {
        case FS_TREE_IO: return snx_os_error(why, size);
        case FS_TREE_EXISTS: return "something of that name is there";
        case FS_TREE_NOTFOUND: return "it is not there";
        case FS_TREE_TOOLONG: return "its path is too long";
        case FS_TREE_NOMEM: return "out of memory";
        case FS_TREE_READONLY: return "it is read-only";
        default: return "the index of the game's files refused it";
    }
}
/* the host path of node for a message, "a save's folder" when it has none */
static const char* save_host(const FsNode* node, char* path, size_t size) {
    return fs_tree_path(&g_hle.fs.tree, node, path, size) ? "a save's folder" : path;
}
/* the file from's bytes into the file to, made with its size */
static FsTreeResult save_copy_file(FsNode* to, FsNode* from, u8* chunk) {
    FILE *in = node_open(from, "rb"), *out = node_open(to, "rb+");
    u64 left = from->size;
    FsTreeResult rc = in && out ? FS_TREE_OK : FS_TREE_IO;
    while (!rc && left) {
        size_t n = left < SAVE_CHUNK ? (size_t)left : SAVE_CHUNK;
        if (fread(chunk, 1, n, in) != n || fwrite(chunk, 1, n, out) != n) rc = FS_TREE_IO;
        left -= n;
    }
    if (rc) { /* said here, where the system's reason is the copy's own */
        char a[FS_TREE_PATH_MAX], b[FS_TREE_PATH_MAX], why[256];
        snx_os_error(why, sizeof why);
        WARN("fs: could not copy %s to %s: %s\n", save_host(from, a, sizeof a), save_host(to, b, sizeof b),
             in && feof(in) ? "the file is shorter than the index of the game's files has it" : why);
    }
    if (in) fclose(in);
    if (out && fclose(out) && !rc) rc = FS_TREE_IO;
    return rc;
}
/* the folder from's folders and files into the folder into, which has none of them; bytes counts the files' */
static FsTreeResult save_copy(FsNode* into, FsNode* from, u8* chunk, u64* bytes) {
    FsNode *c, *made;
    FsTreeResult rc = FS_TREE_OK;
    for (c = from->children; c && !rc; c = c->next) {
        rc = fs_tree_create(&g_hle.fs.tree, into, c->name, c->is_dir, c->is_dir ? 0 : c->size, &made);
        if (rc) {
            char a[FS_TREE_PATH_MAX], why[256];
            WARN("fs: could not make %s in %s: %s\n", c->name, save_host(into, a, sizeof a),
                 save_why(rc, why, sizeof why));
        } else if (c->is_dir)
            rc = save_copy(made, c, chunk, bytes);
        else {
            if (c->size) rc = save_copy_file(made, c, chunk);
            *bytes += c->size;
        }
    }
    return rc;
}
/* The save's folder dst made a copy of its folder src, the console's SynchronizeDirectory. That one deletes dst and
   makes it again; dst is emptied here, so it is there throughout and what has it open keeps it */
static FsTreeResult save_sync(FsNode* base, const char* dst, const char* src, u64* bytes) {
    FsTree* tree = &g_hle.fs.tree;
    FsNode *from = save_part(base, src), *to;
    FsTreeResult rc;
    u8* chunk;
    if (!from) return FS_TREE_NOTFOUND;
    rc = fs_tree_remove(tree, base, dst, 1, 1, 1);
    if (rc == FS_TREE_NOTFOUND) rc = fs_tree_create(tree, base, dst, 1, 0, NULL);
    if (rc) return rc;
    to = save_part(base, dst);
    chunk = (u8*)malloc(SAVE_CHUNK);
    if (!to || !chunk) {
        free(chunk);
        return to ? FS_TREE_NOMEM : FS_TREE_NOTFOUND;
    }
    rc = save_copy(to, from, chunk, bytes);
    free(chunk);
    return rc;
}
/* the save whose working folder is work is open: an IFileSystem has it as its root */
static int save_is_open(const FsNode* work) {
    u32 i;
    for (i = 0; i < FS_FS_MAX; i++)
        if (g_hle.fs.fss[i].obj && g_hle.fs.fss[i].journaled && g_hle.fs.fss[i].root == work) return 1;
    return 0;
}
/* a file of the save whose working folder is work that is open for writing, NULL for none */
static const FsNode* save_written(const FsNode* work) {
    const FsNode* n;
    u32 i;
    for (i = 0; i < FS_FILE_MAX; i++) {
        const FileData* f = &g_hle.fs.files[i];
        if (!f->obj || !f->node || !(f->mode & 2)) continue;
        for (n = f->node; n && n != work; n = n->parent) {}
        if (n) return f->node;
    }
    return NULL;
}
/* The save's folder base opened, the console's Initialize: laid out when it is new, then its working folder made a
   copy of its last commit, or a commit that was cut short finished. A save that is open already (the console refuses
   a second mount; here two saves the console keeps apart can share a folder, save/system's) is left as it is, both
   file systems working in the one copy. The working folder into *work. tree.lock held */
static u32 save_open(FsNode* base, FsNode** work) {
    FsTree* tree = &g_hle.fs.tree;
    FsTreeResult rc = FS_TREE_OK;
    FsNode* node;
    char host[FS_TREE_PATH_MAX], why[256];
    u64 bytes = 0;
    SNX_ZONE(z, "fs save open");
    *work = NULL;
    if (fs_tree_lookup(tree, base, SNX_SAVE_JOURNALED, &node))
        rc = fs_tree_create(tree, base, SNX_SAVE_JOURNALED, 0, 0, NULL); /* first: the folders alone say nothing */
    if (!rc && !save_part(base, SNX_SAVE_WORKING)) {
        rc = fs_tree_create(tree, base, SNX_SAVE_WORKING, 1, 0, NULL);
        if (!rc) rc = fs_tree_create(tree, base, SNX_SAVE_COMMITTED, 1, 0, NULL);
        if (rc == FS_TREE_EXISTS) rc = FS_TREE_OK; /* a last commit with no working copy: copied below */
    }
    if (!rc && !save_is_open(save_part(base, SNX_SAVE_WORKING))) {
        if (save_part(base, SNX_SAVE_COMMITTED)) {
            /* a _ beside a last commit is no commit's: only a save put together by hand has both */
            rc = fs_tree_remove(tree, base, SAVE_SYNCING, 1, 1, 0);
            if (rc == FS_TREE_NOTFOUND) rc = FS_TREE_OK;
            if (!rc) rc = save_sync(base, SNX_SAVE_WORKING, SNX_SAVE_COMMITTED, &bytes);
        } else {
            LOG("fs: %s has no last commit (its %s): a commit of this save was cut short, and is finished now from "
                "its working copy (its %s)\n",
                save_host(base, host, sizeof host), SNX_SAVE_COMMITTED, SNX_SAVE_WORKING);
            rc = save_sync(base, SAVE_SYNCING, SNX_SAVE_WORKING, &bytes);
            if (!rc) rc = fs_tree_rename(tree, base, SAVE_SYNCING, SNX_SAVE_COMMITTED, 1);
        }
    }
    SNX_ZONE_VALUE(z, bytes);
    if (rc) {
        WARN("fs: could not open the save in %s: %s\n", save_host(base, host, sizeof host),
             save_why(rc, why, sizeof why));
        return tree_result(rc);
    }
    *work = save_part(base, SNX_SAVE_WORKING);
    R_SUCCEED();
}
/* The save whose working folder is work committed, the console's DoCommit: refused while a file of it is open for
   writing; then its last commit renamed away, made a copy of the working folder and renamed back. A commit that
   finds no last commit goes on from there: one before it stopped past the first rename. tree.lock held */
static u32 save_commit(FsNode* work) {
    FsTree* tree = &g_hle.fs.tree;
    FsNode* base = work->parent;
    const FsNode* held = save_written(work);
    FsTreeResult rc = FS_TREE_OK;
    char host[FS_TREE_PATH_MAX], why[256];
    u64 bytes = 0;
    int cut = 0;
    SNX_ZONE(z, "fs save commit");
    if (held) {
        WARN_LIMIT(8,
                   "fs: the game commits the save in %s with its %s open for writing: nothing is committed, and it "
                   "is answered WriteModeFileNotClosed, as the console answers\n",
                   save_host(base, host, sizeof host), held->name);
        R_THROW(SNX_RC_FS_WRITE_MODE_FILE_NOT_CLOSED);
    }
    if (save_part(base, SNX_SAVE_COMMITTED)) {
        rc = fs_tree_remove(tree, base, SAVE_SYNCING, 1, 1, 0);
        if (rc == FS_TREE_NOTFOUND) rc = FS_TREE_OK;
        if (!rc) rc = fs_tree_rename(tree, base, SNX_SAVE_COMMITTED, SAVE_SYNCING, 1);
    }
    if (!rc) {
        cut = 1; /* past the commit: the working folder is the save until the last rename */
        rc = save_sync(base, SAVE_SYNCING, SNX_SAVE_WORKING, &bytes);
    }
    if (!rc) rc = fs_tree_rename(tree, base, SAVE_SYNCING, SNX_SAVE_COMMITTED, 1);
    SNX_ZONE_VALUE(z, bytes);
    if (rc) {
        WARN("fs: the commit of the save in %s stopped: %s. %s\n", save_host(base, host, sizeof host),
             save_why(rc, why, sizeof why),
             cut ? "Its working copy (1) holds what the game wrote, and the next commit or open of the save finishes "
                   "this one"
                 : "Its last commit (0) is as it was");
        return tree_result(rc);
    }
    R_SUCCEED();
}
/* Commit, an IFileSystem's or one of a commit of several: a save's (save_commit), then its note (save_note). Any
   other file system has nothing to commit: what the game writes there is on the host as it writes it. tree.lock held */
static u32 fs_commit(FsData* data) {
    if (data->journaled) R_TRY(save_commit(data->root));
    if (data->noted) save_note_write(data->journaled ? data->root->parent : data->root);
    R_SUCCEED();
}
/* a save the versions before kept, opened: played where it is, said once a run for each save type with where its
   files go */
static void save_flat_said(FsNode* base, u8 type, const char* kind) { /* tree.lock held */
    unsigned bit = type < 8 ? type : 0;
    char host[FS_TREE_PATH_MAX];
    if (g_host.fs.save_flat_told >> bit & 1) return;
    g_host.fs.save_flat_told |= 1u << bit;
    WARN("fs: %s holds this game's %s save as an older version kept it, the game's files in the folder itself, where "
         "a game killed while it saves leaves part of a save. The game plays it where it is until you move its files "
         "into its %s/ (Settings > App > Move Old Files, docs/user/deprecations.md)\n",
         save_host(base, host, sizeof host), kind, SNX_SAVE_COMMITTED);
}
/* What a save's folder holds beside its last commit, its working copy and the file that says so: files put there by
   hand, a save from elsewhere pasted where the versions before kept one. The game reads none of it: said once a run
   for each save type, with the first of them and where a save's files go */
static void save_stray_said(FsNode* base, u8 type, const char* kind) { /* tree.lock held */
    unsigned bit = type < 8 ? type : 0, more = 0;
    const FsNode *c, *first = NULL;
    char host[FS_TREE_PATH_MAX];
    for (c = base->children; c; c = c->next) {
        if (!strcmp(c->name, SNX_SAVE_JOURNALED) || !strcmp(c->name, SNX_SAVE_COMMITTED) ||
            !strcmp(c->name, SNX_SAVE_WORKING) || !strcmp(c->name, SAVE_SYNCING))
            continue;
        if (first) more++;
        if (!first) first = c;
    }
    if (!first || (g_host.fs.save_stray_told >> bit & 1)) return;
    g_host.fs.save_stray_told |= 1u << bit;
    WARN("fs: %s, this game's %s save, holds %s and %u more beside its %s/ and %s/, which the game does not read: a "
         "save's files go in its %s/, the last commit (docs/platforms/nx/filesystem.md)\n",
         save_host(base, host, sizeof host), kind, first->name, more, SNX_SAVE_COMMITTED, SNX_SAVE_WORKING,
         SNX_SAVE_COMMITTED);
}
/* the attribute's save, where save_folder says: its working folder (save_open), or the folder itself for temporary
   storage and for a save the versions before kept; an Account, BCAT or Device save noted (save_note) */
static int open_save(const nn_fs_SaveDataAttribute* attribute, SvcObj** filesystem) {
    char p[96], stop[FS_TREE_PATH_MAX * 2 + 512];
    u32 rc;
    u8 type = attribute->type;
    const char* kind = type < sizeof g_save_kinds / sizeof g_save_kinds[0] ? g_save_kinds[type] : "system";
    FsNode *base = NULL, *root = NULL;
    int journaled = 0;
    *filesystem = NULL;
    pm_lock(&g_hle.fs.tree.lock);
    rc = save_folder(attribute, p, sizeof p, stop, sizeof stop);
    if (!R_FAILED(rc) && !stop[0]) rc = tree_result(fs_tree_mkdirs(&g_hle.fs.tree, p, &base));
    if (!R_FAILED(rc) && base) {
        FsNode* mark;
        root = base;
        journaled = type != 4 && (!base->children || !fs_tree_lookup(&g_hle.fs.tree, base, SNX_SAVE_JOURNALED, &mark));
        if (journaled) {
            rc = save_open(base, &root);
            if (!rc) save_stray_said(base, type, kind);
        } else if (type != 4)
            save_flat_said(base, type, kind);
    }
    if (!R_FAILED(rc) && root) {
        FsData* data;
        *filesystem = fs_new(root);
        data = (*filesystem)->data;
        data->journaled = (u32)journaled;
        if (type >= 1 && type <= 3) {
            data->noted = 1;
            save_note_check(base, kind);
        }
    }
    pm_unlock(&g_hle.fs.tree.lock);
    if (stop[0]) fatal("%s", stop);
    return (int)rc;
}
static int nn_fssrv_sf_IFileSystemProxy_OpenSaveDataFileSystem(SvcObj* self, nn_fs_SaveDataSpaceId space_id,
                                                               const nn_fs_SaveDataAttribute* attribute,
                                                               SvcObj** filesystem) {
    return open_save(attribute, filesystem);
}
static int nn_fssrv_sf_IFileSystemProxy_OpenSaveDataFileSystemBySystemSaveDataId(
    SvcObj* self, nn_fs_SaveDataSpaceId space_id, const nn_fs_SaveDataAttribute* attribute, SvcObj** filesystem) {
    return open_save(attribute, filesystem);
}
static int nn_fssrv_sf_IFileSystemProxy_OpenReadOnlySaveDataFileSystem(SvcObj* self, nn_fs_SaveDataSpaceId space_id,
                                                                       const nn_fs_SaveDataAttribute* attribute,
                                                                       SvcObj** filesystem) {
    return open_save(attribute, filesystem);
}
/* no save data to list */
static int info_reader(SvcObj** reader) {
    *reader = nn_fssrv_sf_ISaveDataInfoReader_new();
    R_SUCCEED();
}
static int nn_fssrv_sf_IFileSystemProxy_OpenSaveDataInfoReader(SvcObj* self, SvcObj** reader) {
    return info_reader(reader);
}
static int nn_fssrv_sf_IFileSystemProxy_OpenSaveDataInfoReaderBySaveDataSpaceId(SvcObj* self,
                                                                                nn_fs_SaveDataSpaceId space_id,
                                                                                SvcObj** reader) {
    return info_reader(reader);
}
static int nn_fssrv_sf_IFileSystemProxy_OpenSaveDataInfoReaderOnlyCacheStorage(SvcObj* self, SvcObj** reader) {
    return info_reader(reader);
}
static int nn_fssrv_sf_IFileSystemProxy_OpenDataStorageByCurrentProcess(SvcObj* self, SvcObj** storage) {
    if (!g_hle.fs.rtotal)
        WARN("fs: the game opens its RomFS, and %s has none (extract it into romfs/ there)\n", g_datadir);
    *storage = nn_fssrv_sf_IStorage_new();
    R_SUCCEED();
}
/* nn::fs::QueryMountRomCacheSize(size_t*, int) and MountRom's program index overloads, which a game calls.
   This program's own index (g_program, which GetProgramIndexForAccessLog reports) is its RomFS. Another program's
   is not read here: TargetNotFound, never a success without a storage, which the SDK wraps as a storage with a null
   backend and calls through. */
static int nn_fssrv_sf_IFileSystemProxy_OpenDataStorageWithProgramIndex(SvcObj* self, u8 program_index,
                                                                        SvcObj** storage) {
    if (program_index != g_program)
        WARN_LIMIT(4, "fs: program %d opens the RomFS of program %u, which is not read here: TargetNotFound\n",
                   g_program, (unsigned)program_index);
    R_UNLESS(program_index == g_program, SNX_RC_FS_TARGET_NOT_FOUND);
    return nn_fssrv_sf_IFileSystemProxy_OpenDataStorageByCurrentProcess(self, storage);
}
/* this program's id is the only one there is: another program's is TargetNotFound */
static int nn_fssrv_sf_IFileSystemProxy_OpenDataStorageByProgramId(SvcObj* self, nn_ncm_ProgramId program_id,
                                                                   SvcObj** storage) {
    R_UNLESS(program_id == g_hle.kernel.title_id, SNX_RC_FS_TARGET_NOT_FOUND);
    return nn_fssrv_sf_IFileSystemProxy_OpenDataStorageByCurrentProcess(self, storage);
}
static int nn_fssrv_sf_IFileSystemProxy_OpenDataStorageByDataId(SvcObj* self, nn_ncm_DataId data_id,
                                                                nn_ncm_StorageId storage_id, SvcObj** storage) {
    char p[64];
    if (aoc_of(data_id)) { /* a DLC read in place, as installed */
        *storage = nn_fssrv_sf_IStorage_new();
        (*storage)->u[0] = STORAGE_AOC;
        (*storage)->u[1] = (u64)(aoc_of(data_id) - g_hle.fs.aocs);
        R_SUCCEED();
    }
    snprintf(p, sizeof p, "system/archives/%016llx.romfs", (unsigned long long)data_id);
    pm_lock(&g_hle.fs.tree.lock);
    FsNode* node;
    FsTreeResult r = fs_tree_lookup(&g_hle.fs.tree, NULL, p, &node);
    if (r == FS_TREE_NOTFOUND) {
        if (data_id == NGWORD2_DATA_ID) {
            r = fs_tree_add_virtual(&g_hle.fs.tree, p, g_hle.fs.ngword2rom, g_hle.fs.ngword2rom_size, &node);
            if (!r) LOG("fs: provided synthesized NgWord2 system archive %016llx\n", (unsigned long long)data_id);
        } else if (data_id == SNX_MII_MODEL_DATA_ID) {
            if (!g_hle.fs.miimodelrom_size)
                g_hle.fs.miimodelrom_size = filesrom_init(g_hle.fs.miimodelrom, sizeof g_hle.fs.miimodelrom,
                                                          SNX_MII_MODEL_FILES, snx_mii_model_name, miimodel_file_room,
                                                          miimodel_file_write, snx_mii_model_same, "MiiModel");
            r = fs_tree_add_virtual(&g_hle.fs.tree, p, g_hle.fs.miimodelrom, g_hle.fs.miimodelrom_size, &node);
            if (!r)
                LOG("fs: provided generated MiiModel system archive %016llx (%.1f MB): ScarletNX's own Mii parts, "
                    "not the console's (the player's firmware's goes in system/archives/%016llx.romfs)\n",
                    (unsigned long long)data_id, (double)g_hle.fs.miimodelrom_size / 1048576.0,
                    (unsigned long long)data_id);
        } else {
            r = fs_tree_add_virtual(&g_hle.fs.tree, p, g_hle.fs.emptyrom, sizeof g_hle.fs.emptyrom, &node);
            if (!r) LOG("fs: provided empty system archive %016llx\n", (unsigned long long)data_id);
        }
    }
    u32 rc = tree_result(r);
    *storage = R_FAILED(rc) ? NULL : file_new(node, 1, 1, &rc);
    pm_unlock(&g_hle.fs.tree.lock);
    return (int)rc;
}
static int nn_fssrv_sf_IFileSystemProxy_OpenDeviceOperator(SvcObj* self, SvcObj** device_operator) {
    *device_operator = nn_fssrv_sf_IDeviceOperator_new();
    R_SUCCEED();
}
/* a notifier keeps one event (its id in u[0]), which GetEventHandle gives each time. The card is always there, no game card
   comes or goes and system data never updates, so it is never signaled */
static void notifier_destroy(SvcObj* o) { kobj_unref(kobj_at((u32)o->u[0])); }
static SvcObj* notifier_new(const char* name) {
    SvcObj* o = nn_fssrv_sf_IEventNotifier_new();
    o->u[0] = owned_event(name);
    o->destroy = notifier_destroy;
    return o;
}
static int nn_fssrv_sf_IEventNotifier_GetEventHandle(SvcObj* self, u32* event) {
    *event = ev_handle((u32)self->u[0]);
    R_UNLESS(*event, SNX_RC_SVC_OUT_OF_HANDLES);
    R_SUCCEED();
}
static int nn_fssrv_sf_IFileSystemProxy_OpenSdCardDetectionEventNotifier(SvcObj* self, SvcObj** notifier) {
    *notifier = notifier_new("sd card detection");
    R_SUCCEED();
}
static int nn_fssrv_sf_IFileSystemProxy_OpenGameCardDetectionEventNotifier(SvcObj* self, SvcObj** notifier) {
    *notifier = notifier_new("game card detection");
    R_SUCCEED();
}
static int nn_fssrv_sf_IFileSystemProxy_OpenSystemDataUpdateEventNotifier(SvcObj* self, SvcObj** notifier) {
    *notifier = notifier_new("system data update");
    R_SUCCEED();
}
/* the SDK holds the prohibiter while it writes a save that mustn't be moved to another console meanwhile: nothing
   moves saves here, so it has nothing to do */
static int nn_fssrv_sf_IFileSystemProxy_OpenSaveDataTransferProhibiter(SvcObj* self,
                                                                       nn_ncm_ApplicationId application_id,
                                                                       SvcObj** prohibiter) {
    *prohibiter = nn_fssrv_sf_ISaveDataTransferProhibiter_new();
    R_SUCCEED();
}
static int nn_fssrv_sf_IFileSystemProxy_OpenMultiCommitManager(SvcObj* self, SvcObj** manager) {
    *manager = nn_fssrv_sf_IMultiCommitManager_new();
    R_SUCCEED();
}
/* The file systems a manager commits together: a mask of their slots in g_hle.fs.fss (u[0]), at most the console's
   10, and each once: MultiCommitFileSystemLimit past them, MultiCommitHasOverlappingTargets for one twice */
typedef char fs_multi_commit_mask[FS_FS_MAX <= 64 ? 1 : -1]; /* a manager's mask has a bit for each slot */
#define FS_MULTI_COMMIT_MAX 10
static int nn_fssrv_sf_IMultiCommitManager_Add(SvcObj* self, SvcObj* filesystem) {
    u64 bit, added;
    int n = 0, i;
    if (!filesystem || filesystem->dispatch != fs_disp) {
        WARN("fs: the game adds %s to a commit of several file systems: InvalidArgument\n",
             filesystem ? filesystem->name : "no object");
        R_THROW(SNX_RC_FS_INVALID_ARGUMENT);
    }
    bit = 1ull << POOL_INDEX(g_hle.fs.fss, (FsData*)filesystem->data);
    pm_lock(&g_hle.fs.tree.lock);
    added = self->u[0];
    for (i = 0; i < FS_FS_MAX; i++) n += (int)(added >> i & 1);
    if (!(added & bit) && n < FS_MULTI_COMMIT_MAX) self->u[0] = added | bit;
    pm_unlock(&g_hle.fs.tree.lock);
    R_UNLESS(!(added & bit), SNX_RC_FS_MULTI_COMMIT_HAS_OVERLAPPING_TARGETS);
    R_UNLESS(n < FS_MULTI_COMMIT_MAX, SNX_RC_FS_MULTI_COMMIT_FILE_SYSTEM_LIMIT);
    R_SUCCEED();
}
/* each added file system committed as its Commit does it, in turn and every one even after one fails, the first
   failure answered, as the console commits them; or none when one of them is gone: closed, or its save deleted
   (NotFound, as its own Commit answers then) */
static int nn_fssrv_sf_IMultiCommitManager_Commit(SvcObj* self) {
    u64 added = self->u[0];
    u32 first = 0, rc;
    int i;
    pm_lock(&g_hle.fs.tree.lock);
    for (i = 0; i < FS_FS_MAX; i++)
        if ((added >> i & 1) && (!g_hle.fs.fss[i].root || !g_hle.fs.fss[i].root->attached)) {
            pm_unlock(&g_hle.fs.tree.lock);
            R_THROW(SNX_RC_FS_PATH_NOT_FOUND);
        }
    for (i = 0; i < FS_FS_MAX; i++)
        if ((added >> i & 1) && (rc = fs_commit(&g_hle.fs.fss[i])) && !first) first = rc;
    pm_unlock(&g_hle.fs.tree.lock);
    return (int)first;
}
/* nn::fs::SaveDataExtraData's fields past the attribute (Switchbrew) */
#define FS_EXTRA_DATA_SIZE 0x200
#define FS_EXTRA_OWNER     0x40
#define FS_EXTRA_AVAILABLE 0x58
#define FS_EXTRA_JOURNAL   0x60
/* A save's extra data as the system made it: its attribute with the program it belongs to (0 asks for the game's own),
   that program as its owner, and an account's or the device's sizes as am answers them (0 for the other types). Its
   time stamp, flags and commit id are 0: no save here keeps them. Every save of the game is there, as it is made when
   opened */
static int nn_fssrv_sf_IFileSystemProxy_ReadSaveDataFileSystemExtraDataWithMaskBySaveDataAttribute(
    SvcObj* self, nn_fs_SaveDataSpaceId space_id, const nn_fs_SaveDataAttribute* attribute, IpcBuf extra_data,
    IpcBuf mask) {
    u8 extra[FS_EXTRA_DATA_SIZE] = {0};
    nn_fs_SaveDataAttribute stored = *attribute;
    i64 available, journal;
    u64 i, n = extra_data.size < mask.size ? extra_data.size : mask.size;
    const u8* keep = GP(mask.addr);
    u8* out = GP(extra_data.addr);
    (void)self, (void)space_id;
    if (stored.type && !stored.program_id) stored.program_id = save_title();
    am_save_data_size(stored.type, &available, &journal);
    memcpy(extra, &stored, sizeof stored);
    swipc_le_nn_fs_SaveDataAttribute(extra);
    snx_st_le64(extra + FS_EXTRA_OWNER, stored.program_id);
    snx_st_le64(extra + FS_EXTRA_AVAILABLE, (u64)available);
    snx_st_le64(extra + FS_EXTRA_JOURNAL, (u64)journal);
    if (n > FS_EXTRA_DATA_SIZE) n = FS_EXTRA_DATA_SIZE;
    for (i = 0; i < n; i++) out[i] = extra[i] & keep[i];
    R_SUCCEED();
}
static SvcObj* new_fsp(void) { return nn_fssrv_sf_IFileSystemProxy_new(); }
static SvcObj* new_fsppr(void) { return nn_fssrv_sf_IProgramRegistry_new(); }
static SvcObj* new_fspldr(void) { return nn_fssrv_sf_IFileSystemProxyForLoader_new(); }

void svc_fs_init(void) {
    emptyrom_init();
    g_hle.fs.ngword2rom_size = filesrom_init(g_hle.fs.ngword2rom, sizeof g_hle.fs.ngword2rom, NGWORD2_FILES,
                                             ngword2_name, ngword2_file_size, ngword2_file_write, NULL, "NgWord2");
    nn_fssrv_sf_IFileSystemProxy_register(new_fsp);
    nn_fssrv_sf_IProgramRegistry_register(new_fsppr);
    nn_fssrv_sf_IFileSystemProxyForLoader_register(new_fspldr);
}

/* ============================================================ save states */
/* The open file systems, files and folders are their nodes in the tree, which a load finds again by path: each one's
   path from the data folder, tree.lock held. 0, or -1 when it doesn't fit */
static int node_path(const FsNode* node, char* out, size_t cap) {
    const FsNode* n;
    size_t len = 0;
    for (n = node; n && n != g_hle.fs.tree.root; n = n->parent) len += strlen(n->name) + 1;
    if (!n || len >= cap) return -1;
    out[len ? len - 1 : 0] = 0;
    for (n = node; n != g_hle.fs.tree.root; n = n->parent) {
        size_t k = strlen(n->name);
        len -= k + 1;
        memcpy(out + len, n->name, k);
        if (len) out[len - 1] = '/';
    }
    return 0;
}
static int put_node(StateIo* io, u32 kind, u32 slot, const FsNode* node) {
    char path[FS_TREE_PATH_MAX];
    u32 n;
    if (!node || node_path(node, path, sizeof path)) return -1;
    n = (u32)strlen(path);
    return io_put32(io, kind) | io_put32(io, slot) | io_put32(io, n) | io_put(io, path, n);
}
int fs_state_save(StateIo* io) {
    u32 i;
    int r = 0;
    pm_lock(&g_hle.fs.tree.lock);
    for (i = 0; i < FS_FS_MAX && !r; i++)
        if (g_hle.fs.fss[i].obj) r |= put_node(io, 1, i, g_hle.fs.fss[i].root);
    for (i = 0; i < FS_FILE_MAX && !r; i++)
        if (g_hle.fs.files[i].obj) r |= put_node(io, 2, i, g_hle.fs.files[i].node);
    for (i = 0; i < FS_DIR_MAX && !r; i++)
        if (g_hle.fs.dirs[i].obj) r |= put_node(io, 3, i, g_hle.fs.dirs[i].node);
    r |= io_put32(io, 0);
    pm_unlock(&g_hle.fs.tree.lock);
    return r ? -1 : 0;
}
/* a file's size and bytes into the state, tree.lock held */
static int temp_put_file(StateIo* io, FsNode* node) {
    u8 chunk[1 << 16];
    u64 left = node->size;
    FILE* f;
    if (io_put64(io, node->size)) return -1;
    if (node_in_memory(node))
        return node->size ? io_put(io, node->is_virtual ? node->virtual_data : node->map, node->size) : 0;
    if (!(f = node_open(node, "rb"))) return -1;
    while (left) {
        size_t n = left < sizeof chunk ? (size_t)left : sizeof chunk;
        if (fread(chunk, 1, n, f) != n || io_put(io, chunk, n)) break;
        left -= n;
    }
    fclose(f);
    return left ? -1 : 0;
}
/* dir's folders and files, each before what it holds: its kind (3 a folder, 2 a file), its path under save/temp, and a
   file's size and bytes; path holds at bytes of dir's own */
static int temp_put(StateIo* io, FsNode* dir, char* path, size_t at) {
    FsNode* c;
    for (c = dir->children; c; c = c->next) {
        size_t k = strlen(c->name), n = at + !!at + k;
        if (n >= FS_TREE_PATH_MAX) return -1;
        if (at) path[at] = '/';
        memcpy(path + at + !!at, c->name, k);
        path[n] = 0;
        if (io_put32(io, c->is_dir ? 3 : 2) || io_put32(io, (u32)n) || io_put(io, path, n) ||
            (c->is_dir ? temp_put(io, c, path, n) : temp_put_file(io, c)))
            return -1;
    }
    return 0;
}
int fs_state_temp_save(StateIo* io) {
    char path[FS_TREE_PATH_MAX];
    FsNode* temp = NULL;
    int had, r;
    pm_lock(&g_hle.fs.tree.lock);
    had = fs_tree_lookup(&g_hle.fs.tree, NULL, "save/temp", &temp) == FS_TREE_OK && temp->is_dir;
    r = io_put32(io, (u32)had) || (had && temp_put(io, temp, path, 0)) || io_put32(io, 0) ? -1 : 0;
    pm_unlock(&g_hle.fs.tree.lock);
    return r;
}
/* what the game had open goes, and the RomFS's open files (a cache), before the load writes g_hle over them */
void fs_state_drop(void) {
    u32 i;
    pm_lock(&g_hle.fs.tree.lock);
    for (i = 0; i < FS_FS_MAX; i++)
        if (g_hle.fs.fss[i].obj) fs_tree_release(g_hle.fs.fss[i].root);
    for (i = 0; i < FS_FILE_MAX; i++)
        if (g_hle.fs.files[i].obj) {
            if (g_hle.fs.files[i].f) fclose(g_hle.fs.files[i].f);
            fs_tree_release(g_hle.fs.files[i].node);
        }
    for (i = 0; i < FS_DIR_MAX; i++)
        if (g_hle.fs.dirs[i].obj) fs_tree_release(g_hle.fs.dirs[i].node);
    for (i = 0; i < NOPEN; i++)
        if (g_hle.fs.openf[i].f) fclose(g_hle.fs.openf[i].f);
    memset(g_hle.fs.openf, 0, sizeof g_hle.fs.openf);
    pm_unlock(&g_hle.fs.tree.lock);
}
/* A state's open file systems, files and folders, found again by path in the running process's tree and opened, before
   the running game is touched (a file the game had open that isn't there any more refuses the load), then put in
   place once the state's g_hle is */
typedef struct {
    u32 kind, slot;
    FsNode* node; /* retained; NULL for one under save/temp, found once the state's temporary storage is in place */
    FILE* f;
    char* temp;       /* that one's path, else NULL */
    const char* mode; /* and a file's mode, as the state's had it */
} FsReopen;
/* a folder (3) or file (2) of the state's save/temp, by its path under it, a file with its bytes */
typedef struct {
    u32 kind;
    char* path;
    u8* data;
    u64 size;
} FsTempEntry;
struct FsStaged {
    FsReopen* e;
    u32 n;
    int temp; /* the state's save/temp: 1 there, 0 not there, -1 a state from before they held it */
    FsTempEntry* t;
    u32 nt;
};
void fs_state_staged_free(FsStaged* st) {
    u32 i;
    if (!st) return;
    pm_lock(&g_hle.fs.tree.lock);
    for (i = 0; i < st->n; i++) {
        if (st->e[i].f) fclose(st->e[i].f);
        fs_tree_release(st->e[i].node);
        free(st->e[i].temp);
    }
    pm_unlock(&g_hle.fs.tree.lock);
    for (i = 0; i < st->nt; i++) {
        free(st->t[i].path);
        free(st->t[i].data);
    }
    free(st->t);
    free(st->e);
    free(st);
}
/* a path under save/temp a state names: its folders by name, none empty, "." or "..", so it stays under save/temp */
static int temp_path_ok(const char* p, u32 n) {
    u32 i, at = 0;
    for (i = 0; i <= n; i++) {
        if (i < n && p[i] != '/') {
            if (p[i] == '\\' || p[i] == ':' || !p[i]) return 0;
            continue;
        }
        if (i == at || (i - at == 1 && p[at] == '.') || (i - at == 2 && p[at] == '.' && p[at + 1] == '.')) return 0;
        at = i + 1;
    }
    return 1;
}
/* the state's save/temp read into st: 0, or -1 with why */
static int temp_read(StateIo* io, FsStaged* st, char* why, size_t whysize) {
    u32 had, kind, n, cap = 0;
    if (io_get32(io, &had) || had > 1) goto bad;
    st->temp = (int)had;
    for (;;) {
        FsTempEntry* e;
        if (io_get32(io, &kind)) goto bad;
        if (!kind) return 0;
        if (!had || (kind != 2 && kind != 3) || io_get32(io, &n) || !n || n >= FS_TREE_PATH_MAX) goto bad;
        if (st->nt == cap) {
            FsTempEntry* more = (FsTempEntry*)realloc(st->t, sizeof *more * (cap = cap ? cap * 2 : 16));
            if (!more) {
                snprintf(why, whysize, "out of memory");
                return -1;
            }
            st->t = more;
        }
        e = &st->t[st->nt++];
        memset(e, 0, sizeof *e);
        e->kind = kind;
        if (!(e->path = (char*)malloc(n + 1)) || io_get(io, e->path, n)) goto bad;
        e->path[n] = 0;
        if (!temp_path_ok(e->path, n)) goto bad;
        if (kind == 2 && (io_get64(io, &e->size) || e->size > ((u64)1 << 40) ||
                          !(e->data = (u8*)malloc(e->size ? (size_t)e->size : 1)) || io_get(io, e->data, e->size)))
            goto bad;
    }
bad:
    snprintf(why, whysize, "its temporary storage is damaged");
    return -1;
}
/* path, one the state had open, is save/temp or under it: 1, and 0 with why when the state's save/temp doesn't hold
   it as kind (2 a file, 1 or 3 a folder) */
static int temp_holds(const FsStaged* st, const char* path, u32 kind, char* why, size_t whysize) {
    u32 i;
    if (!strcmp(path, "save/temp") && kind != 2 && st->temp == 1) return 1;
    for (i = 0; i < st->nt; i++)
        if (!strcmp(st->t[i].path, path + 10) && (st->t[i].kind == 2) == (kind == 2)) return 1;
    snprintf(why, whysize, "the game had %s open, which the state's temporary storage doesn't hold", path);
    return 0;
}
FsStaged* fs_state_resolve(StateIo* io, StateIo* temp, const HleFs* loaded, char* why, size_t whysize) {
    char path[FS_TREE_PATH_MAX];
    FsStaged* st = (FsStaged*)calloc(1, sizeof *st);
    u32 kind, slot, n, cap = 0;
    if (!st) {
        snprintf(why, whysize, "out of memory");
        return NULL;
    }
    st->temp = -1;
    if (temp && temp_read(temp, st, why, whysize)) {
        fs_state_staged_free(st);
        return NULL;
    }
    pm_lock(&g_hle.fs.tree.lock);
    for (; io;) {
        FsReopen* r;
        FsNode* node = NULL;
        if (io_get32(io, &kind) ||
            (kind && (io_get32(io, &slot) || io_get32(io, &n) || n >= sizeof path || io_get(io, path, n)))) {
            snprintf(why, whysize, "its list of open files is damaged");
            goto bad;
        }
        if (!kind) break;
        path[n] = 0;
        if ((kind == 1 && slot >= FS_FS_MAX) || (kind == 2 && slot >= FS_FILE_MAX) ||
            (kind == 3 && slot >= FS_DIR_MAX) || kind > 3) {
            snprintf(why, whysize, "its list of open files is damaged");
            goto bad;
        }
        /* the state's save/temp takes the running game's place once the load can't be refused: found then */
        if (st->temp >= 0 && !strncmp(path, "save/temp", 9) && (!path[9] || path[9] == '/')) {
            if (!temp_holds(st, path, kind, why, whysize)) goto bad;
        } else if (n ? fs_tree_lookup(&g_hle.fs.tree, NULL, path, &node) != FS_TREE_OK : !(node = g_hle.fs.tree.root)) {
            snprintf(why, whysize, "the game had %s open, which isn't there any more", path);
            goto bad;
        }
        if (st->n == cap) {
            FsReopen* more = (FsReopen*)realloc(st->e, sizeof *more * (cap = cap ? cap * 2 : 64));
            if (!more) {
                snprintf(why, whysize, "out of memory");
                goto bad;
            }
            st->e = more;
        }
        r = &st->e[st->n++];
        r->kind = kind;
        r->slot = slot;
        r->node = node;
        r->f = NULL;
        r->mode = (loaded->files[slot].mode & 2) ? "rb+" : "rb";
        r->temp = NULL;
        if (!node) {
            if (!(r->temp = snx_strdup(path))) {
                snprintf(why, whysize, "out of memory");
                goto bad;
            }
            continue;
        }
        fs_tree_retain(node);
        if (kind == 2 && !node_in_memory(node) && !(r->f = node_open(node, r->mode))) {
            snprintf(why, whysize, "%s can't be opened again", path);
            goto bad;
        }
    }
    pm_unlock(&g_hle.fs.tree.lock);
    return st;
bad:
    pm_unlock(&g_hle.fs.tree.lock);
    fs_state_staged_free(st);
    return NULL;
}
/* the state's save/temp in place of the running game's, files and folders as it had them; past the point a load can be
   refused, so a failure ends the game (its path in the log, the fatal's text without it) */
static void temp_restore(const FsStaged* st) {
    char path[FS_TREE_PATH_MAX];
    FsTreeResult r = fs_tree_remove(&g_hle.fs.tree, NULL, "save/temp", 1, 1, 0);
    FsNode* node;
    u32 i;
    if (r != FS_TREE_OK && r != FS_TREE_NOTFOUND) {
        LOG("state: save/temp couldn't be emptied for the save state's (error %d)\n", r);
        fatal("The save state's temporary storage couldn't be put back");
    }
    if (st->temp != 1) return;
    if ((r = fs_tree_mkdirs(&g_hle.fs.tree, "save/temp", &node))) {
        LOG("state: save/temp couldn't be made for the save state's (error %d)\n", r);
        fatal("The save state's temporary storage couldn't be put back");
    }
    for (i = 0; i < st->nt; i++) {
        const FsTempEntry* e = &st->t[i];
        FILE* f = NULL;
        snprintf(path, sizeof path, "save/temp/%s", e->path);
        r = fs_tree_create(&g_hle.fs.tree, NULL, path, e->kind == 3, e->kind == 3 ? 0 : e->size, &node);
        if (!r && e->kind == 2 && e->size &&
            (!(f = node_open(node, "rb+")) || fwrite(e->data, 1, (size_t)e->size, f) != e->size))
            r = FS_TREE_IO;
        if (f && fclose(f)) r = FS_TREE_IO;
        if (r) {
            LOG("state: %s of the save state's temporary storage couldn't be written (error %d)\n", path, r);
            fatal("The save state's temporary storage couldn't be put back");
        }
    }
}
void fs_state_install(FsStaged* st) {
    u32 i;
    if (!st) return;
    pm_lock(&g_hle.fs.tree.lock);
    if (st->temp >= 0) temp_restore(st);
    for (i = 0; i < st->n; i++) { /* the ones under save/temp, found in the state's */
        FsReopen* r = &st->e[i];
        if (!r->temp) continue;
        if (fs_tree_lookup(&g_hle.fs.tree, NULL, r->temp, &r->node) != FS_TREE_OK) r->node = NULL;
        fs_tree_retain(r->node);
        if (!r->node || (r->kind == 2 && !node_in_memory(r->node) && !(r->f = node_open(r->node, r->mode)))) {
            LOG("state: %s, which the game had open, couldn't be opened in the save state's temporary storage\n",
                r->temp);
            fatal("The save state's temporary storage couldn't be put back");
        }
    }
    for (i = 0; i < st->n; i++) {
        FsReopen* r = &st->e[i];
        if (r->kind == 1)
            g_hle.fs.fss[r->slot].root = r->node;
        else if (r->kind == 3)
            g_hle.fs.dirs[r->slot].node = r->node;
        else {
            g_hle.fs.files[r->slot].node = r->node;
            g_hle.fs.files[r->slot].f = r->f;
        }
        free(r->temp);
    }
    pm_unlock(&g_hle.fs.tree.lock);
    for (i = 0; i < st->nt; i++) {
        free(st->t[i].path);
        free(st->t[i].data);
    }
    free(st->t);
    free(st->e);
    free(st);
}
/* the functions a save state may hold of this file (state_fns.h) */
STATE_FN_TABLE(g_fs_state_fns,
               STATE_FN("fs", file_destroy),
               STATE_FN("fs", dir_destroy),
               STATE_FN("fs", fs_destroy),
               STATE_FN("fs", notifier_destroy),
               STATE_FN("fs", file_disp),
               STATE_FN("fs", dir_disp),
               STATE_FN("fs", fs_disp));
