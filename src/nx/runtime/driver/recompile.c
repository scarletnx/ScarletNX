/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#define _POSIX_C_SOURCE 200809L
#include "nx/runtime/driver/recompile.h"
#include "nx/runtime/driver/container.h"
#include "nx/runtime/driver/extract.h"
#include "nx/runtime/driver/game_name.h"
#include "nx/runtime/driver/nros.h"
#include "nx/loader/loader.h"
#include "nx/loader/module.h"
#include "nx/loader/module_llvm.h"
#include "nx/loader/codegen_view.h"
#include "nx/runtime/nce/nce_host.h"
#include "nx/runtime/nce/nce_patch.h"
#if defined(__ANDROID__)
#include <android/api-level.h>
#endif
#include "common/cpu/recompiler/llvm/ir.h"

/* the triple of the iOS app's code: Darwin's calling convention and the generic AArch64 CPU, in the object format the
   JIT loader reads (recompiler/llvm/ir.c's host_triple). The version only names the ABI */
#define SNX_IOS_TRIPLE "arm64-apple-ios16.0-elf"
#include "nx/runtime/driver/toolchain.h"
#include "snx_settings.h"
#include "common/utilities/build_info.h"
#include "common/utilities/cli.h"
#include "common/utilities/mods.h"
#include "common/utilities/patches.h"
#include "common/platform/file.h"
#include <ctype.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

const IniOpt* snx_recompile_setting(const char* key) {
    const IniOpt* o = ini_find(g_recompile_ini, key);
    if (!o) o = ini_find(g_llvm_ini, key);
    return o ? o : ini_find(g_recompile_game_ini, key);
}
static void usage(void) {
    puts("Usage: scarletnx -r INPUT [NAME] [OPTIONS]\n"
         "  INPUT                  Game/ExeFS folder, .nro, or an XCI or NSP dump (read in place)\n"
         "  --name NAME            Output name (default: the title ID, else the NPDM's or input name)\n"
         "  --outdir DIR           Where the game's folder goes (default: recompiled/)\n"
         "  --layout FILE          Put the code of these functions first, in this order (hex PCs, one per line):\n"
         "                         a profile's hot functions, as tools/layout.py orders them\n"
         "  --keys FILE            prod.keys for a dump (default: the data folder, then ~/.switch)\n"
         "  --title-keys FILE      title.keys for a dump, found the same way (optional)\n"
         "  --program N            Only program N of a dump's application of several (default: every one)");
    puts("  --KEY=VALUE            A recompile setting: opt, llvm_batch, aot_threads, unit_cache_gb,\n"
         "                         llvm_pass_<pass>=0 (scarletnx --settings=recompile lists them)");
    puts("  --target auto|linux|win64  The host only: modules are built for the machine that builds them\n"
         "  --target ios           A pack of objects for the iOS app, which loads them into JIT memory (docs/hosts/apple.md)\n"
         "  --no-build             Stage data/INI only, generate no code\n"
         "  --tmpfs                Leave the RomFS in the extraction (in tmpfs), which the game reads it from");
    puts("  --runtime FILE         Deploy this runtime instead of building one\n"
         "  --hook-direct-calls    Route calls and branches between functions through dispatch (NVN)");
    snx_cli_help(stdout, SNX_CLI_RECOMPILE_OPTIONS);
    printf("On the Switch, --jobs's Auto starts on 1 thread and opens one more each second while the memory holds it,\n"
           "--chunk's default is the setting unit_instructions (%d), whole functions to a unit, and --jit-cache\n"
           "takes a profile run's .jitcache, linked into the module as runtime code.\n",
           g_settings.unit_size);
    puts("Each game is a folder of <outdir>, <NAME>/: its program <NAME>, <NAME>_code and <NAME>_data/.\n"
         "An update's dump is made beside its game, <ID>/<ID>_<its own title ID>, which runs with the game's\n"
         "settings and saves and reads the dump in place. A DLC has no program: the driver checks it for its game.\n"
         "An application of several programs (a collection) has each other one beside it, <ID>/<ID>_<its ID>: its\n"
         "code and data folder, run on the game's runtime with its settings and saves.\n"
         "Hints: <outdir>/<NAME>/<NAME>.hints (hex PCs, one per line).");
}
/* SNX_LLVM_BRANCH_PROFILE, which an older version read (docs/user/deprecations.md): for this version still the value of
   ir_branch_profile when the recompile is given none, and said either way. Removed with tests/architecture.py's
   ENV_DEPRECATED row */
static void old_branch_profile(void) {
    const char* old = getenv("SNX_LLVM_BRANCH_PROFILE");
    if (!old) return;
    if (g_settings.ir_branch_profile[0])
        snx_error("SNX_LLVM_BRANCH_PROFILE=%s is deprecated, and the recompile gives ir_branch_profile first: "
                  "ir_branch_profile=%s (docs/user/deprecations.md)",
                  old, g_settings.ir_branch_profile);
    else if (strlen(old) < sizeof g_settings.ir_branch_profile) {
        memcpy(g_settings.ir_branch_profile, old, strlen(old) + 1);
        snx_error("SNX_LLVM_BRANCH_PROFILE=%s is deprecated: this recompile reads it as --ir_branch_profile=%s "
                  "(docs/user/deprecations.md)",
                  old, old);
    } else
        snx_error("SNX_LLVM_BRANCH_PROFILE=%s is deprecated, and longer than ir_branch_profile's %u bytes: "
                  "ir_branch_profile is empty, no branch weights (docs/user/deprecations.md)",
                  old, (unsigned)sizeof g_settings.ir_branch_profile - 1);
}
/* This build's, not the checkout's cpu.h: the modules are laid out as this build's headers say, and an installed
   build has no checkout */
unsigned snx_game_abi(void) { return snx_module_abi(); }
/* the ABI alone: a game made by another build of it keeps its code, and its copy of the runtime is replaced by the
   driver that plays it (driver.c's refresh_runtime) */
char* snx_game_stamp(const char* mods) { return snx_game_stamp_for(snx_game_abi(), mods); }
char* snx_own_runtime(const SnxPaths* paths) {
#ifdef SNX_WITH_RUNTIME
    /* In an AppImage this program is a launcher in its mount, which runs nothing once copied out: the AppImage is the
       runtime, and a copy named after the game runs it. APPIMAGE is inherited, so it counts only inside the mount */
    const char *appimage = getenv("APPIMAGE"), *appdir = getenv("APPDIR");
    if (appimage && *appimage && appdir && *appdir && snx_path_within(paths->executable, appdir))
        return snx_absolute(appimage);
    return snx_strdup(paths->executable);
#else
    (void)paths;
    return NULL;
#endif
}
#ifdef _WIN32
/* Windows finds a program's DLLs next to it: the game's folder gets those next to the runtime (the zip's LLVM), or
   it does not start. Links where the disk takes them, so each game does not hold copies */
static int copy_dlls(const char* run, const char* out) {
    char **names = NULL, *from = snx_dirname(run), *src, *dst;
    size_t i, count = 0, n;
    int rc = snx_list_dir(from, &names, &count);
    for (i = 0; !rc && i < count; ++i) {
        n = strlen(names[i]);
        if (n < 4 || names[i][n - 4] != '.' || tolower((unsigned char)names[i][n - 3]) != 'd' ||
            tolower((unsigned char)names[i][n - 2]) != 'l' || tolower((unsigned char)names[i][n - 1]) != 'l')
            continue;
        src = snx_path_join(from, names[i]);
        dst = snx_path_join(out, names[i]);
        /* a new one linked, else copied; one there already copied over when it differs (a newer zip's) */
        rc = snx_is_file(src) && (snx_path_exists(dst) || snx_link_file(src, dst)) && snx_copy_file(src, dst);
        free(src);
        free(dst);
    }
    snx_free_strings(names, count);
    free(from);
    return rc;
}
#endif
int snx_deploy_runtime(const char* run, const char* exe) {
    char* tmp = snx_format("%s.tmp", exe);
    int rc;
#if defined(_WIN32) || defined(__APPLE__)
    char* folder = snx_dirname(exe); /* what a program loads from its own folder goes beside it before it does */
#endif
#ifdef _WIN32
    rc = copy_dlls(run, folder);
    if (rc) snx_error("cannot copy the runtime's DLLs to %s", folder);
    if (rc) {
        free(folder);
        free(tmp);
        return rc;
    }
#endif
#if defined(__APPLE__) && !defined(SNX_IOS)
    /* MoltenVK, the Vulkan driver the app carries in its Frameworks: a game's copy of the runtime plays outside the
       bundle, where SDL finds neither the app's Frameworks nor a system loader (docs/hosts/apple.md). A copy the disk refuses
       (a game folder on a read-only image) is said, and no reason to refuse the run: the system's loader may serve it */
    if (folder) snx_deploy_vulkan_driver(run, folder);
#endif
#if defined(_WIN32) || defined(__APPLE__)
    free(folder);
#endif
#ifdef __ANDROID__
    /* the shared storage runs nothing (the game runs on the app's own runtime), and before Android 11 it refuses a
       mode change: there the copy only marks the recompile done */
    rc = snx_copy_file(run, tmp) || plat_replace_file(tmp, exe);
#else
    rc = snx_copy_file(run, tmp) || snx_make_executable(tmp) || plat_replace_file(tmp, exe);
#endif
    if (rc) {
        snx_error("cannot deploy the runtime %s to %s", run, exe);
        if (snx_is_file(tmp)) snx_remove_tree(tmp);
    }
    free(tmp);
    return rc;
}
/* The game's settings: beside it, or in an installed build's config folder (snx_user_path), where the runtime reads
   them. An older build's beside the game stays, a line saying so. NULL when that folder cannot be made */
static char* settings_path(const char* out, const char* title) {
    char *folder = snx_user_path(out, SNX_USER_CONFIG), *base, *path = NULL, *old;
    if (!strcmp(folder, out) || !snx_mkdirs(folder)) path = snx_game_ini(out, title);
    if (path && strcmp(folder, out)) {
        base = snx_path_join(out, title);
        old = snx_format("%s.ini", base);
        free(base);
        if (snx_left_behind(old, path))
            fprintf(stderr,
                    "scarletnx: %s was left by an older version: this one keeps it in %s (docs/user/deprecations.md)\n", old,
                    path);
        free(old);
    }
    free(folder);
    return path;
}
/* a file of code addresses, hex, one per line, in its order (none when it is not there): the hints file, code
   addresses the runtime met that the recompiler hadn't found, or --layout's. A64 code is word aligned; an AArch32
   address has the Thumb bit (bit 0) for a Thumb block, and A32 ones are word aligned */
static SnxU64* read_addresses(const char* path, int is64, size_t* count) {
    char line[256], *s;
    FILE* f = snx_fopen(path, "r");
    SnxU64 *hints = NULL, value;
    size_t n = 0;
    int digits, invalid, c;
    if (f) {
        while (fgets(line, sizeof(line), f)) {
            value = 0;
            digits = 0;
            invalid = 0;
            s = line;
            while (isspace((unsigned char)*s)) ++s;
            if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
            while (isxdigit((unsigned char)*s)) {
                if (++digits > 16) invalid = 1;
                c = tolower((unsigned char)*s++);
                value = (value << 4) | (SnxU64)(c <= '9' ? c - '0' : c - 'a' + 10);
            }
            while (isspace((unsigned char)*s)) ++s;
            if (!invalid && digits && !*s && (is64 ? !(value & 3) : (value & 3) != 2 && !(value >> 32))) {
                hints = (SnxU64*)snx_realloc(hints, (n + 1) * sizeof(*hints));
                hints[n++] = value;
            }
        }
        fclose(f);
    }
    *count = n;
    return hints;
}
static SnxU64* read_hints(const char* base, int is64, size_t* count) {
    char* path = snx_format("%s.hints", base);
    SnxU64* hints = read_addresses(path, is64, count);
    free(path);
    return hints;
}
/* the SNX_ABI the game at base was made with, the number its stamp starts with (<base>.abi): 0 without a stamp */
static unsigned stamp_abi(const char* base) {
    char* path = snx_format("%s.abi", base);
    size_t size = 0, i;
    unsigned char* text = snx_read_file(path, &size);
    unsigned abi = 0;
    for (i = 0; text && i < size && i < 6 && isdigit(text[i]); ++i) abi = abi * 10 + (unsigned)(text[i] - '0');
    free(text);
    free(path);
    return abi;
}
/* The hints of a game made before SNX_ABI 18 (made_by, 0 for a game with no stamp, whose hints stay), when its modules
   each lay a page past the one before: moved to where that code lies now, in hints and in the file, which the
   runtime's JIT starts from too. Else each would name code a page or more from the function it was recorded for, in
   every recompile from now on. An NCE recompile lays the modules its own way, then as now. 0, or an error, said */
static int move_hints(const char* base, unsigned made_by, const SnxProgram* program, SnxU64* hints, size_t* count,
                      int quiet) {
    char *path, *nce = snx_format("%s.nce", base), *tmp, why[256];
    size_t before = *count, i;
    int was_nce = snx_is_file(nce), failed;
    FILE* f;
    free(nce);
    if (!made_by || made_by >= 18 || was_nce || !before || program->module_count < 2) return 0;
    *count = snx_program_unguard_addresses(program, hints, before);
    path = snx_format("%s.hints", base);
    tmp = snx_format("%s.tmp", path);
    f = snx_fopen(tmp, "w");
    failed = !f;
    for (i = 0; f && i < *count; ++i)
        if (fprintf(f, "%llx\n", (unsigned long long)hints[i]) < 0) failed = 1;
    if (f && fclose(f)) failed = 1;
    if (!failed && plat_replace_file(tmp, path)) failed = 1;
    if (failed) {
        snx_os_error(why, sizeof why);
        snx_error("cannot write %s, the game's %zu hints at the addresses its modules lie at now: %s", path, *count,
                  why);
        remove(tmp);
    } else if (!quiet)
        printf("Hints: the %zu of %s were recorded by a game made with SNX_ABI %u, a page left free after each "
               "module: %zu moved to where that code lies now, %zu lay in no module and are left out\n",
               before, path, made_by, *count, before - *count);
    free(tmp);
    free(path);
    return failed;
}
/* an ExeFS file of a dump read in place (SnxExefsReader) */
static int dump_read(void* source, const char* name, unsigned char** data, size_t* size) {
    SnxHacFs* fs = (SnxHacFs*)source;
    const SnxHacEntry* files;
    size_t count = snx_hac_fs_files(fs, &files), i;
    for (i = 0; i < count; ++i) {
        if (strcmp(files[i].name, name)) continue;
        if (files[i].size > SIZE_MAX) return -1;
        *size = (size_t)files[i].size;
        *data = (unsigned char*)malloc(*size ? *size : 1);
        if (*data && !snx_hac_fs_read(fs, i, 0, *data, *size)) return 0;
        free(*data);
        *data = NULL;
        return -1;
    }
    return 1;
}
/* a dump's program of index, its ExeFS read in place with the mods' files names[i] from paths[i] over it (count of
   them), and its control.nacp out of its Control NCA: nothing is extracted. 0, else nonzero, said why */
static int load_dump(const char* dump, const char* update, int index, const SnxPaths* paths, int quiet,
                     char* const* names, char* const* files, size_t count, SnxProgram* program, char* error,
                     size_t error_size) {
    /* paths->keys and paths->title_keys, when given, are the files the keys are read from */
    SnxDump opened;
    SnxHacFs* fs = NULL;
    SnxExefsReader reader;
    SnxExefsMods mods;
    unsigned char* nacp;
    const char* why;
    size_t nacp_size = 0;
    /* an update's ExeFS is whole: read without its game's, which only its RomFS is made of */
    int rc = snx_dump_open(&opened, update ? NULL : dump, update, index, paths->keys, paths->title_keys, paths->repo,
                           !quiet);
    if (!rc && opened.program.found) rc = snx_nca_keys(&opened, &opened.program, "The game's Program NCA");
    if (!rc && opened.patch.found) rc = snx_nca_keys(&opened, &opened.patch, "The update's Program NCA");
    if (!rc && !(fs = snx_dump_fs(&opened))) rc = 1;
    if (!rc) {
        nacp = snx_dump_nacp(&opened, &nacp_size, &why);
        if (!nacp && !quiet) printf("The game keeps the name of its file: %s\n", why);
        reader.read = dump_read;
        reader.source = fs;
        snx_exefs_mods(&mods, &reader, names, files, count);
        rc = snx_load_program_from(&mods.reader, update ? update : dump, nacp, nacp_size, program, error, error_size);
        if (rc) snx_error("%s", error);
    }
    snx_hac_fs_free(fs);
    snx_dump_close(&opened);
    return rc;
}
int snx_dump_modules_of_dump(const char* dump, const SnxPaths* paths, const char* outdir) {
    SnxProgram program;
    SnxU64 addon = 0, owner = 0, version = SNX_NO_VERSION;
    char error[1024];
    int rc, kind;
    if (snx_extract_title(dump, paths, &addon, &owner, &version, NULL)) return -1;
    kind = snx_addon_kind(owner, addon);
    if (kind == SNX_ADDON_DLC) {
        snx_error("%s is a DLC, which has no program of its own", dump);
        return -1;
    }
    /* an update's ExeFS is whole, read without its game */
    if (load_dump(dump, kind == SNX_ADDON_UPDATE ? dump : NULL, 0, paths, 1, NULL, NULL, 0, &program, error,
                  sizeof error))
        return -1;
    rc = snx_dump_loaded(&program, outdir, error, sizeof error);
    if (rc) snx_error("%s", error);
    snx_free_program(&program);
    return rc;
}
/* The NROs of a dump's program of index (nros.h), from its RomFS read in place. An update's RomFS is its own over its
   game's, so its dump is read with the game's (base, the dump the game's .ini records): alone it has no RomFS, and
   the update's NROs, the game's unless it brings its own, would not be found. 0, else nonzero, said why */
static int dump_nros(const char* base, const char* update, int index, const SnxPaths* paths, int quiet,
                     SnxNroSet* nros) {
    SnxDump opened;
    SnxHacFs* fs = NULL;
    int rc = snx_dump_open(&opened, base, update, index, paths->keys, paths->title_keys, paths->repo, 0);
    if (!rc && opened.program.found) rc = snx_nca_keys(&opened, &opened.program, "The game's Program NCA");
    if (!rc && opened.patch.found) rc = snx_nca_keys(&opened, &opened.patch, "The update's Program NCA");
    if (!rc && !(fs = snx_dump_fs(&opened))) rc = 1;
    if (!rc) rc = snx_nros_dump(nros, fs, quiet);
    snx_hac_fs_free(fs);
    snx_dump_close(&opened);
    return rc;
}
/* An update's NROs: its dump read with its game's, the dump the game's .ini in folder records as its source. Without
   that dump (the game was made from an extracted folder, or its dump is gone) the update has no RomFS to look in:
   said, and its NROs run in the JIT. 0, else nonzero, said why */
static int update_nros(const char* folder, const char* update, SnxU64 owner, int index, const SnxPaths* paths,
                       int quiet, SnxNroSet* nros) {
    char of[17], *base;
    int rc = 0, recorded = 0;
    snx_title_text(owner, of);
    base = snx_game_source(folder, of, &recorded);
    if (base && snx_is_file(base) && snx_is_dump(base))
        rc = dump_nros(base, update, index, paths, quiet, nros);
    else if (!quiet && recorded && base)
        printf("NROs: the update's RomFS is its game's with its own files over it, and the game's dump, %s, is not "
               "there: no code is made for the NROs it maps, the JIT runs them\n",
               base);
    else if (!quiet)
        printf("NROs: the update's RomFS is its game's with its own files over it, and the game's settings name no "
               "dump of it: no code is made for the NROs it maps, the JIT runs them\n");
    free(base);
    return rc;
}
/* a game made from its dump, or an extracted game's folder (kind "dump" or "folder"), read in place: what an
   extraction left in its data folder taken away, as the runtime would read it over the source */
static int drop_extraction(const char* dir, const char* kind, int quiet) {
    static const char* const parts[] = {"exefs", "romfs", "romfs.bin", "romfs.copying"};
    char* part;
    size_t i;
    int rc = 0;
    for (i = 0; !rc && i < sizeof parts / sizeof *parts; ++i) {
        part = snx_path_join(dir, parts[i]);
        if (snx_path_exists(part) || snx_is_link(part)) {
            if (!quiet) printf("Removing %s: the game reads it from its %s\n", part, kind);
            rc = snx_remove_tree(part);
        }
        free(part);
    }
    return rc || snx_mkdirs(dir);
}
/* 1 when input is the extracted game the game's .ini names as its source= (snx_extracted_game), which it reads in
   place */
static int recorded_source(const char* folder, const char* game, const char* input) {
    char* source = snx_extracted_game(input) ? snx_game_source(folder, game, NULL) : NULL;
    int same = source && snx_same_file(source, input);
    free(source);
    return same;
}
/* the game's data folder: its ExeFS (or NRO) and RomFS, the RomFS left where it is with tmpfs */
static int copy_data(const SnxProgram* p, const char* dir, int quiet, int tmpfs) {
    char *ex = snx_path_join(dir, "exefs"), *dst, *src, **names;
    size_t n, i;
    int rc = 0;
    if (snx_is_link(ex) || snx_mkdirs(ex)) {
        free(ex);
        return 1;
    }
    /* A cached mount of the other format can take precedence in the runtime. */
    dst = snx_path_join(ex, p->modules[0].is_nro ? "main" : "main.nro");
    if (snx_is_file(dst)) {
        snx_error("cached data has a different format; use another name or remove %s/exefs and its RomFS cache", dir);
        free(dst);
        free(ex);
        return 1;
    }
    free(dst);
    if (!p->modules[0].is_nro) {
        dst = snx_path_join(dir, "romfs.bin");
        if (snx_is_file(dst)) {
            snx_error("%s conflicts with the NSO input: remove it before recompiling", dst);
            free(dst);
            free(ex);
            return 1;
        }
        free(dst);
    }
    if (p->module_count == 1 && p->modules[0].is_nro) {
        dst = snx_path_join(ex, "main.nro");
        rc = snx_copy_file(p->entry_path, dst);
        free(dst);
        if (!rc) {
            dst = snx_path_join(dir, "romfs.bin");
            if (p->romfs_size)
                rc = snx_copy_file_range(p->entry_path, p->romfs_offset, p->romfs_size, dst);
            else if (snx_is_file(dst))
                /* An updated NRO without embedded RomFS must not inherit the old image's assets. */
                rc = snx_remove_tree(dst);
            free(dst);
        }
    } else {
        if (snx_list_dir(p->exefs_path, &names, &n)) {
            free(ex);
            return 1;
        }
        for (i = 0; i < n && !rc; ++i) {
            src = snx_path_join(p->exefs_path, names[i]);
            dst = snx_path_join(ex, names[i]);
            if (snx_is_link(src) || snx_is_link(dst))
                rc = 1;
            else if (snx_is_file(src))
                rc = snx_copy_file(src, dst);
            free(src);
            free(dst);
        }
        snx_free_strings(names, n);
        if (!rc && p->romfs_path && !tmpfs) {
            dst = snx_path_join(dir, "romfs");
            if (!snx_is_dir(dst)) {
                /* A stopped copy must not look like a complete cache on the next run. */
                char* pending = snx_format("%s.copying", dst);
                if (!quiet) puts("Copying RomFS...");
                rc = ((snx_is_dir(pending) || snx_is_file(pending) || snx_is_link(pending)) &&
                      snx_remove_tree(pending)) ||
                     snx_copy_tree(p->romfs_path, pending) || plat_replace_file(pending, dst);
                free(pending);
            }
            free(dst);
        }
    }
    if (!rc && !p->npdm_data) {
        dst = snx_path_join(ex, "main.npdm");
        if (snx_is_file(dst)) rc = snx_remove_tree(dst);
        free(dst);
    }
    free(ex);
    return rc;
}
/* the played mods' patches (common/utilities/patches.h) written over each NSO module of program they are made for,
   every one said: 0, else nonzero with the patch that could not be applied said */
/* NCE (the setting nce, on an AArch64 Linux host): the game's code runs natively on the host's CPU, so the module
   holds no code made ahead of time (as --jit-only). The analysis finds here the words that the runtime patches
   (nce_patch.h), with the FDEs and the symbols of the modules. Each module then moves up to leave pools for the stubs
   of its sites, which the branches of its sites reach (nce_layout). The pools and the sites go in <base>.nce, which the
   runtime reads. A recompile without NCE removes that file, because its addresses are of another layout. 0, or an
   error, said: a word of the code that reaches no pool among them, which ends the recompile with nce=0 to set, as
   running it any other way would make code the player did not ask for */
#define NCE_CODE_END 0x2000000000ull
static int nce_prepare(SnxProgram* p, const char* base, int quiet) {
    SnxCodegen cg;
    NceSites sites;
    NceSpan* mods;
    NceRange* pools = NULL;
    size_t pool_count = 0;
    char* path = snx_format("%s.nce", base);
    char why[320];
    int r;
    if (!p->module_count) {
        free(path);
        return snx_error("NCE: the program has no modules");
    }
    /* the instructions the recompile decodes: an Ounce game's too, as module_llvm.c's analysis does */
    SnxU64 features = g_settings.cpu_extensions || p->npdm.ounce ? SNX_A64_TARGET_SWITCH_EXTENDED : 0;
    if (snx_nx_codegen_analyze(&cg, p, NULL, 0, quiet, features)) {
        free(path);
        return snx_error("NCE: the analysis of the game's code failed");
    }
    nce_find_sites(&cg, &sites);
    snx_codegen_free(&cg);
    mods = (NceSpan*)snx_alloc(p->module_count * sizeof *mods);
    for (size_t i = 0; i < p->module_count; i++) {
        mods[i].base = p->modules[i].base;
        mods[i].size = p->modules[i].size;
    }
    r = nce_layout(mods, p->module_count, NCE_CODE_END, &sites, &pools, &pool_count, why, sizeof why);
    if (r)
        r = snx_error("NCE: %s", why);
    else {
        for (size_t i = 0; i < p->module_count; i++) p->modules[i].base = mods[i].base;
        if (nce_file_write(path, p->modules[0].base, pools, pool_count, &sites)) r = snx_error("cannot write %s", path);
    }
    if (!r && !quiet) {
        /* outside: by kind, as the runtime's net catches only an SVC there (nce_host_net), and the others would read
           the host's registers */
        char kinds[256] = "";
        size_t at = 0;
        for (int k = NCE_NONE + 1; k < NCE_KINDS && at < sizeof kinds; k++)
            if (sites.outside_kind[k])
                at += (size_t)snprintf(kinds + at, sizeof kinds - at, "%s%s %zu", at ? ", " : ": ",
                                       nce_kind_name((NceKind)k), sites.outside_kind[k]);
        printf("NCE: %zu words to patch (%zu that the flow reaches), %zu kept as data, %zu outside the functions%s; "
               "%zu pools of stubs\n",
               sites.count, sites.proven, sites.data, sites.outside, kinds, pool_count);
    }
    nce_sites_free(&sites);
    free(pools);
    free(mods);
    free(path);
    return r;
}

static int patch_modules(SnxProgram* program, const char* mods, const char* own, const char* off, int quiet) {
    char error[1024], **patches;
    size_t count = snx_mods_patches_of(mods, own, off, &patches), i, j;
    unsigned char* applied = (unsigned char*)snx_alloc(count + 1);
    int rc = 0;
    for (i = 0; !rc && i < program->module_count; ++i) {
        SnxModuleImage* m = &program->modules[i];
        if (m->is_nro) continue;
        if (snx_patch_module(m->image, m->image_size, m->build_id, patches, count, applied, error, sizeof error))
            rc = snx_error("%s: the mod's patch can't be applied to %s, so it is not recompiled", error, m->name);
        for (j = 0; !rc && !quiet && j < count; ++j)
            if (applied[j]) printf("%s patched by the mod's %s\n", m->name, patches[j]);
    }
    free(applied);
    snx_free_strings(patches, count);
    return rc;
}
/* what snx_recompile_game's arguments say, which each program of a dump is made with */
typedef struct {
    const char *input, *name, *outdir, *runtime, *jit_cache, *layout;
    SnxPaths key_paths;
    const SnxPaths* paths;
    SnxU64 addon, owner, version;
    SnxCompileOptions compile;
    SnxCodegenOptions gen;
    int ios, no_build, keep_c, host_win, tmpfs, dump;
    int programs, primary; /* the application's programs, and the one the profile's --jit-cache and --layout are of */
} MakeArgs;
/* program index of the input, made in its game's folder: the game's own (0) as before, another program of its
   application (snx_program_index) beside it, <game>_<its ID>, with its code and stamp and its data folder alone, the
   game's runtime and settings its own. 0, else nonzero, said why */
static int make_program(const MakeArgs* a, int index) {
    const char *input = a->input, *name = index ? NULL : a->name, *outdir = a->outdir, *runtime = a->runtime;
    const char *jit_cache = index == a->primary ? a->jit_cache : NULL, *layout = index == a->primary ? a->layout : NULL;
    SnxPaths key_paths = a->key_paths;
    const SnxPaths* paths = a->paths;
    SnxU64 addon = a->addon, owner = a->owner, version = a->version;
    SnxCompileOptions compile = a->compile;
    SnxCodegenOptions gen = a->gen;
    int ios = a->ios, no_build = a->no_build, keep_c = a->keep_c, host_win = a->host_win, tmpfs = a->tmpfs,
        dump = a->dump;
    SnxLlvmBuild llvm;
    SnxProgram program;
    SnxNroSet nros = {0};
    int want_nros;
    char *title = NULL, *game = NULL, *out = NULL, *folder = NULL, *base = NULL, *data = NULL, *code = NULL;
    char *ini = NULL, *ini_text = NULL, *run = NULL, *exe = NULL, *mods = NULL, *mods_off = NULL, *mods_stamp = NULL;
    char **mod_names = NULL, **mod_files = NULL, *own_mods = NULL;
    size_t mod_count = 0, found = 0;
    char error[1024], why[256], reason[256];
    SnxU64* hints;
    unsigned made_by = 0;
    int i, rc = 1;
    memset(&program, 0, sizeof(program));
    memset(&llvm, 0, sizeof(llvm));
    /* the NROs of its RomFS, where code is made ahead of time for this host: an iOS pack and an NCE recompile have
       none */
    want_nros = !no_build && !ios && !gen.interp_only;
    if (dump ? load_dump(input, addon ? input : NULL, index, &key_paths, compile.quiet, NULL, NULL, 0, &program, error,
                         sizeof(error))
             : snx_load_program(input, &program, error, sizeof(error)))
        return dump ? 1 : snx_error("%s", error);
    if (addon && program.npdm.title_id != owner + (SnxU64)index) {
        char of[17], other[17];
        snx_title_text(owner, of);
        snx_title_text(program.npdm.title_id, other);
        snx_error("%s is an update of the game %s, and its program is %s", input, of,
                  other[0] ? other : "none with a title ID");
        snx_free_program(&program);
        return 1;
    }
    title = snx_game_shown(name, &program, reason, sizeof reason);
    if (reason[0] && !compile.quiet)
        printf("The game's control.nacp gives it no name, %s: it shows as %s\n", reason, title);
    if (index) { /* <game>_<its ID>, or its update's patch of it, <game>_<the update's ID + index>v<version> */
        char own[32], of[17], named[SNX_PATH_SIZE];
        snx_title_text(owner, of);
        if (addon) snx_addon_entry(owner, addon, version, own);
        if (snx_program_name(of, addon ? own : NULL, index, named, sizeof named)) {
            snx_error("program %d of %s has no name a folder takes", index, input);
            goto done;
        }
        game = snx_strdup(named);
    } else if (addon) { /* <game>_<add-on>, in the game's folder */
        char own[32], of[17];
        snx_addon_entry(owner, addon, version, own); /* an update's with its version: each version its own */
        snx_title_text(owner, of);
        game = snx_format("%s_%s", of, own);
    } else
        /* a game read from the folder the player named takes that folder's name; a dump keeps its title ID, which
           names its files whatever the dump was called */
        game = dump || !program.entry_path || !snx_is_dir(program.entry_path)
                   ? snx_game_name(name, &program.npdm, program.entry_path)
                   : snx_game_title(name, &program.npdm, program.entry_path);
    out = outdir ? snx_absolute(outdir) : snx_recompiled_dir(paths->repo);
    if (!out) {
        snx_error("cannot resolve %s", outdir ? outdir : paths->repo);
        goto done;
    }
    if ((program.romfs_path && snx_path_within(out, program.romfs_path)) ||
        (program.exefs_path && snx_path_within(out, program.exefs_path))) {
        snx_error("%s must be outside the input ExeFS/RomFS", out);
        goto done;
    }
    if (snx_mkdirs(out)) {
        snx_error("cannot create %s: %s", out, snx_os_error(why, sizeof why));
        goto done;
    }
    /* each game in a folder of its own, where an older version put the game's program (a file); an add-on in its
       game's */
    if (addon || index) {
        char of[17];
        snx_title_text(owner, of);
        folder = snx_path_join(out, of);
    } else
        folder = snx_path_join(out, game);
    if (snx_is_link(folder) || (snx_path_exists(folder) && !snx_is_dir(folder))) {
        snx_error("%s is a file where this game's folder goes: a game an older version made there, named %s; move "
                  "its files as docs/user/deprecations.md says",
                  folder, game);
        goto done;
    }
    if (snx_mkdirs(folder)) {
        snx_error("cannot create the game's folder %s: %s", folder, snx_os_error(why, sizeof why));
        goto done;
    }
    /* the mods played, in the game's data folder: the program made with their ExeFS files over its own, loaded again */
    {
        char of[17];
        snx_title_text(owner, of);
        mods = snx_game_mods(folder, addon || index ? of : game, &mods_off);
        own_mods = snx_program_own_mods(folder, of, index); /* another program's own, Eden's load/<its ID>/ */
        mods_stamp = snx_mods_stamp_of(mods, own_mods, mods_off);
        mod_count = snx_mods_exefs_of(mods, own_mods, mods_off, &mod_names, &mod_files);
    }
    if (mod_count) {
        snx_free_program(&program);
        if (dump ? load_dump(input, addon ? input : NULL, index, &key_paths, compile.quiet, mod_names, mod_files,
                             mod_count, &program, error, sizeof error)
                 : snx_load_program_mods(input, mod_names, mod_files, mod_count, &program, error, sizeof error)) {
            if (!dump) snx_error("%s", error);
            goto done;
        }
        if (!compile.quiet)
            for (i = 0; i < (int)mod_count; ++i) printf("ExeFS %s from the mod's %s\n", mod_names[i], mod_files[i]);
    }
    if (patch_modules(&program, mods, own_mods, mods_off, compile.quiet))
        goto done; /* before any code is made from them */
    base = snx_path_join(folder, game);
    /* Invalidate the completion stamp before touching an existing game's data
       or module. Failed recompilation must never leave a trusted checkpoint. */
    if (!no_build) {
        char* stamp = snx_format("%s.abi", base);
        int failed;
        made_by = stamp_abi(base); /* before it goes: what the game's hints were recorded with */
        failed = snx_is_file(stamp) && snx_remove_tree(stamp);
        free(stamp);
        if (failed) goto done;
    }
    data = snx_format("%s_data", base);
    if (snx_is_link(data)) {
        snx_error("%s is a link, and the game's data must be a folder", data);
        goto done;
    }
    gen.title = title;
    gen.jit_cache = jit_cache;
    gen.hints = hints = read_hints(base, program.npdm.is64, &gen.hint_count);
    if (move_hints(base, made_by, &program, hints, &gen.hint_count, compile.quiet)) goto done;
    if (!compile.quiet && index)
        printf("Recompiling %s, program %d of %d (%zu modules)\n", title, index, a->programs, program.module_count);
    else if (!compile.quiet)
        printf("Recompiling %s (%zu modules)\n", title, program.module_count);
    /* an add-on's files: its dump, read in place, or what the driver extracts into its data folder. An extracted game
       the game's .ini names as its source= is read in place as a dump is: nothing of it copied */
    if (addon ? snx_mkdirs(data)
        : dump || (!tmpfs && !index && recorded_source(folder, game, input))
            ? drop_extraction(data, dump ? "dump" : "folder", compile.quiet)
            : copy_data(&program, data, compile.quiet, tmpfs)) {
        snx_error("cannot stage game data in %s", data);
        goto done;
    }
    if (program.nacp) { /* the name the launcher lists the game by */
        char* nacp = snx_path_join(data, "control.nacp");
        int failed = !nacp || snx_write_file(nacp, program.nacp, program.nacp_size);
        free(nacp);
        if (failed) {
            snx_error("cannot write the game's control.nacp in %s", data);
            goto done;
        }
    }
    if (program.module_count == 1 && program.modules[0].is_nro) { /* its icon for Games, as an extraction keeps one */
        unsigned char* jpeg;
        size_t size;
        char why[512], *icon = NULL;
        int none = snx_nro_icon(program.entry_path, &jpeg, &size, why, sizeof why);
        if (none < 0)
            printf("Games shows no icon for the game: %s\n", why);
        else if (!none && (!(icon = snx_path_join(data, "icon.jpg")) || snx_write_file(icon, jpeg, size)))
            printf("Games shows no icon for the game: its icon.jpg could not be written in %s\n", data);
        free(icon);
        free(jpeg);
    } else if (program.root_path) {
        /* An extracted game keeps its icon.jpg beside exefs/, as -x writes one beside a dump's: Games shows that one,
           so the game's data folder takes a copy, as it takes the control.nacp that names it */
        char *from = snx_path_join(program.root_path, "icon.jpg");
        char* icon = snx_path_join(data, "icon.jpg");
        if (from && icon && snx_is_file(from) && snx_copy_file(from, icon))
            printf("Games shows no icon for the game: its icon.jpg could not be copied to %s\n", data);
        free(from);
        free(icon);
    }
    /* another program plays with its game's settings, and reads its game's dump or its update's: neither of its own */
    ini = index ? NULL : settings_path(folder, game);
    if (!ini && !index) {
        snx_error("cannot create the settings folder for %s", title);
        goto done;
    }
    if (ini && !snx_is_file(ini) && !addon) { /* an add-on's names its dump only: it has its game's settings */
        ini_text = snx_format(
            "; ScarletNX settings for %s\n; Full option help is written on first run.\n[Display]\nlanguage=1\n", title);
        if (snx_write_file(ini, ini_text, strlen(ini_text))) {
            snx_error("cannot write %s", ini);
            goto done;
        }
    }
    if (snx_record_folders(folder, game)) goto done;
    if (dump && !index) { /* what the game reads in place, and Extract to tmpfs extracts again */
        char* absolute = snx_absolute(input);
        int failed = !absolute || snx_record_source(folder, game, absolute);
        free(absolute);
        if (failed) goto done;
    }
    if (no_build) {
        rc = 0;
        goto done;
    }
    if (ios && !program.npdm.is64) {
        snx_error("--target ios: 32-bit games are not supported yet");
        goto done;
    }
    {
        char* stale = snx_format("%s.nce", base);
        int nce = 0;
#if SNX_NCE_HOST /* where the runtime runs it: else its JIT would compile all of the code */
        nce = !ios && g_settings.nce;
        if (nce) {
            char why[240];
#if defined(__ANDROID__)
            int android = 1, api = android_get_device_api_level();
#else
            int android = 0, api = 0;
#endif
            if (nce_refused(why, sizeof why, program.npdm.is64, program.npdm.aspace, android, api)) {
                printf("NCE: nce=1, but %s: this recompile makes the code ahead of time (nce=0)\n", why);
                nce = 0;
            }
        }
#endif
        i = nce ? nce_prepare(&program, base, compile.quiet) : 0;
        if (nce)
            gen.interp_only = 1;
        else if (snx_is_file(stale) && remove(stale))
            i = snx_error("cannot remove %s, which is of an NCE recompile", stale);
        free(stale);
        if (i) goto done;
    }
    /* the NROs the program maps while it runs, each given its address behind the modules (nros.h): an extracted
       game's from its RomFS folder, a dump's from its RomFS read in place, an update's over its game's */
    if (want_nros && !gen.interp_only && program.npdm.is64) {
        if (!dump)
            i = program.romfs_path ? snx_nros_folder(&nros, program.romfs_path, compile.quiet) : 0;
        else if (!addon)
            i = dump_nros(input, NULL, index, &key_paths, compile.quiet, &nros);
        else
            i = update_nros(folder, input, owner, index, &key_paths, compile.quiet, &nros);
        if (i) goto done;
    }
    /* the calls through the modules' PLT stubs made direct where one module alone defines the stub's function
       (loader.h's SnxBoundBranch). With every call through its dispatch slot (--hook-direct-calls) none is */
    if (program.npdm.is64 && !gen.interp_only && !gen.hook_direct_calls) {
        SnxBoundBranch* bound = NULL;
        size_t own = 0, sites = 0;
        if (snx_program_bound(&program, &bound, &gen.bound_count, &own, &sites, error, sizeof error))
            printf("PLT: %s: the modules' calls through their PLT stubs go through the dispatch table\n", error);
        else if (!compile.quiet)
            printf("PLT: %zu calls through %zu stubs made direct, %zu stubs of their module's own functions and %zu of "
                   "another module's\n",
                   sites, gen.bound_count, own, gen.bound_count - own);
        gen.bound = bound;
    }
    if (snx_nros_place(&nros, &program, compile.quiet)) goto done;
    gen.nros = nros.places;
    gen.nro_count = nros.count;
    gen.nro_end = nros.end;
    code = ios ? snx_format("%s_code.snxobj", base) : snx_format("%s_code.%s", base, compile.target_win ? "dll" : "so");
    {
        char *kept = keep_c ? snx_format("%s_build", base) : NULL, *units = NULL;
        llvm.output = code;
        llvm.triple = ios ? SNX_IOS_TRIPLE : NULL;
        llvm.opt = compile.opt[0];
        snx_ir_slp_vectorizer(g_settings.slp_vectorizer);
        snx_ir_fpsr_flags(g_settings.fpsr_flags);
        snx_ir_volatile_memory(g_settings.volatile_load_stores);
        snx_ir_abi_contract(g_settings.abi_contract);
        snx_ir_diagnostics(g_settings.ir_stats, g_settings.ir_dump_pc);
        old_branch_profile();
        snx_ir_branch_profile(g_settings.ir_branch_profile);
        llvm.tuning = snx_llvm_tuning(llvm.opt, error, sizeof error);
        if (!llvm.tuning) {
            snx_error("%s", error);
            goto done;
        }
        if (!compile.quiet) printf("LLVM: -O%c, llvm_batch=%s\n", llvm.opt, g_settings.llvm_batch);
        llvm.jobs = compile.jobs;
        llvm.jobs_open = compile.jobs_open;
        llvm.quiet = compile.quiet;
        llvm.keep_ir = kept;
        llvm.found = &found;
        if (layout && !snx_is_file(layout)) {
            snx_error("--layout %s: no such file", layout);
            free(kept);
            free(units);
            goto done;
        }
        if (layout) llvm.layout = read_addresses(layout, 1, &llvm.layout_count);
        {
            /* the user's cache folder, or a portable copy's: every game's recompile shares it */
            char* folder = snx_menu_dir(paths->repo, SNX_USER_CACHE);
            units = folder ? snx_path_join(folder, "units") : NULL;
            free(folder);
            if (!units) {
                snx_error(
                    "unit cache: HOME is not set (APPDATA on Windows), so there is no cache folder: the game is not "
                    "recompiled");
                free(kept);
                goto done;
            }
            llvm.unit_cache = units;
            llvm.unit_cache_limit = (SnxU64)g_settings.unit_cache_limit * 1000000000ull;
        }
        i = kept && (snx_is_link(kept) || snx_mkdirs(kept));
        if (i) snx_error("cannot save the generated code in %s", kept);
        if (!i && !program.npdm.is64)
            i = snx_build_module_arm(&program, &gen, &llvm);
        else if (!i)
            i = snx_build_module_llvm(&program, &gen, &llvm);
        if (!i && !ios && program.npdm.is64) { /* each NRO's own module, and an earlier recompile's taken away */
            char* folder = snx_format("%s" SNX_NROS_FOLDER, base);
            i = snx_nros_build(&nros, &program, &gen, &llvm, folder, compile.target_win);
            free(folder);
        }
        free(kept);
        free(units);
        if (i) goto done;
    }
    if (ios) { /* the iOS app is the runtime: only the code is made here */
        if (!compile.quiet) printf("Built %s\n", code);
        rc = 0;
        goto done;
    }
    /* another program runs on its game's copy of the runtime (--game=), its stamp its checkpoint */
    if (!index && runtime) run = snx_absolute(runtime);
#ifdef SNX_WITH_RUNTIME
    else if (!index && host_win == compile.target_win)
        run = snx_own_runtime(paths);
#endif
    else if (!index)
        run = snx_build_runtime(paths, compile.target_win, compile.quiet);
    if (!index && (!run || !snx_is_file(run))) {
        snx_error("runtime unavailable; specify --runtime FILE");
        goto done;
    }
    /* Deploy the executable last: automatic play uses it as the completion checkpoint. */
    exe = index ? NULL : snx_format("%s%s", base, compile.target_win ? ".exe" : "");
    if (exe && snx_deploy_runtime(run, exe)) goto done;
    {
        /* what the launcher says the game was made with: the AArch64 analysis's instructions of the modules' code (a
           32-bit game's blocks are counted otherwise: 0, unknown) */
        SnxGameMade made = {compile.opt[0], !gen.interp_only, found, 0};
        char *stamp = snx_format("%s.abi", base), *version = snx_game_stamp(mods_stamp), *line, *text;
        size_t m;
        for (m = 0; program.npdm.is64 && m < program.module_count; ++m) made.code += program.modules[m].text_size / 4;
        if (made.found > made.code) made.found = made.code;
        line = snx_game_made_line(&made);
        text = snx_format("%s%s", version, line);
        i = snx_write_file(stamp, text, strlen(text));
        free(text);
        free(line);
        free(stamp);
        free(version);
        if (i) {
            snx_error("cannot record the compiled game's ABI");
            goto done;
        }
    }
    if (!compile.quiet) printf("Built %s\n", exe ? exe : code);
    rc = 0;
done:
    snx_free_program(&program);
    snx_nros_free(&nros);
    snx_free_strings(mod_names, mod_count);
    snx_free_strings(mod_files, mod_count);
    free(mods);
    free(own_mods);
    free(mods_off);
    free(mods_stamp);
    free((void*)gen.hints);
    free((void*)gen.bound);
    free((void*)llvm.layout);
    free(title);
    free(game);
    free(out);
    free(folder);
    free(base);
    free(data);
    free(code);
    free(ini);
    free(ini_text);
    free(run);
    free(exe);
    return rc;
}
int snx_recompile_game(int argc, char** argv, const SnxPaths* paths) {
    const char *input = NULL, *name = NULL, *outdir = NULL, *runtime = NULL, *target = "auto", *jit_cache = NULL,
               *layout = NULL, *keys = NULL, *title_keys = NULL, *arg, *value;
    SnxPaths key_paths;
    SnxU64 addon = 0, owner = 0, version = SNX_NO_VERSION; /* an update, its game and version, as its content meta
                                                                names them */
    SnxCompileOptions compile;
    SnxCodegenOptions gen;
    MakeArgs make;
    char error[1024], option[64];
    const char* equal;
    size_t option_size;
    SnxCliArg given;
    int i, rc, ios = 0, no_build = 0, keep_c = 0, host_win = 0, positional = 0, tmpfs = 0, dump = 0, asked = -1,
               programs = 1, common;
    memset(&compile, 0, sizeof(compile));
    memset(&gen, 0, sizeof(gen));
#ifdef _WIN32
    host_win = 1;
#endif
    compile.target_win = host_win;
    for (i = 0; i < argc; ++i) {
        arg = argv[i];
        if (positional) {
            if (!input)
                input = arg;
            else if (!name)
                name = arg;
            else
                return snx_error("unexpected argument: %s", arg);
            continue;
        }
        if (!strcmp(arg, "--help") || !strcmp(arg, "-h")) {
            usage();
            return 0;
        }
        if (!strcmp(arg, "--")) {
            positional = 1;
            continue;
        }
        /* the recompile's common options (common/utilities/cli.h), the table's every driver reads; another common
           one (--outdir, --keys) is this one's own below */
        common = snx_cli_match(argc, argv, &i, SNX_CLI_RECOMPILE_OPTIONS, "scarletnx", &given, error, sizeof error);
        if (common == SNX_CLI_ERROR) {
            fprintf(stderr, "%s\n", error);
            return 1;
        }
        if (common == SNX_CLI_OPT)
            strcpy(g_settings.optimization_level, given.value);
        else if (common == SNX_CLI_JOBS)
            g_settings.aot_threads = (int)given.number;
        else if (common == SNX_CLI_CHUNK)
            g_settings.unit_size = (int)given.number;
        else if (common == SNX_CLI_JIT_CACHE)
            jit_cache = given.value;
        else if (common == SNX_CLI_JIT_ONLY)
            gen.interp_only = 1;
        else if (common == SNX_CLI_KEEP_IR)
            keep_c = 1;
        else if (common == SNX_CLI_QUIET)
            compile.quiet = gen.quiet = 1;
        if (common > 0) continue;
        if (!strcmp(arg, "--no-build"))
            no_build = 1;
        else if (!strcmp(arg, "--tmpfs"))
            tmpfs = 1;
        else if (!strcmp(arg, "--hook-direct-calls"))
            gen.hook_direct_calls = 1;
        else if (arg[0] == '-') {
            value = NULL;
            if ((equal = strchr(arg, '=')) != NULL) {
                option_size = (size_t)(equal - arg);
                if (option_size >= sizeof(option))
                    return snx_error("unknown option: %s (scarletnx -r -h lists them)", arg);
                memcpy(option, arg, option_size);
                option[option_size] = 0;
                value = equal + 1;
                arg = option;
                if (arg[1] == '-' && snx_recompile_setting(arg + 2)) {
                    const IniOpt* o = snx_recompile_setting(arg + 2);
                    if (ini_check(o, value, error, sizeof error)) return snx_error("%s", error);
                    ini_read(o, value);
                    continue;
                }
            }
            if (strcmp(arg, "-o") && strcmp(arg, "--name") && strcmp(arg, "--out") && strcmp(arg, "--outname") &&
                strcmp(arg, "--keys") && strcmp(arg, "--title-keys") && strcmp(arg, "--outdir") &&
                strcmp(arg, "--runtime") && strcmp(arg, "--target") && strcmp(arg, "--layout") &&
                strcmp(arg, "--program"))
                return snx_error("unknown option: %s (scarletnx -r -h lists them)", arg);
            if (!value) {
                if (++i == argc) return snx_error("%s requires a value", arg);
                value = argv[i];
            }
            if (!*value) return snx_error("%s requires a nonempty value", arg);
            if (!strcmp(arg, "-o") || !strcmp(arg, "--out") || !strcmp(arg, "--outname"))
                snx_option_renamed(!strcmp(arg, "-o") ? "-o" : !strcmp(arg, "--out") ? "--out" : "--outname", "--name");
            if (!strcmp(arg, "-o") || !strcmp(arg, "--name") || !strcmp(arg, "--out") || !strcmp(arg, "--outname"))
                name = value;
            else if (!strcmp(arg, "--keys"))
                keys = value;
            else if (!strcmp(arg, "--title-keys"))
                title_keys = value;
            else if (!strcmp(arg, "--outdir"))
                outdir = value;
            else if (!strcmp(arg, "--runtime"))
                runtime = value;
            else if (!strcmp(arg, "--target"))
                target = value;
            else if (!strcmp(arg, "--program")) {
                char* end;
                long n = strtol(value, &end, 10);
                if (*end || n < 0 || n >= SNX_PROGRAMS_MAX)
                    return snx_error("--program %s: not a whole number from 0 to %d, an application's program index",
                                     value, SNX_PROGRAMS_MAX - 1);
                asked = (int)n;
            } else
                layout = value;
        } else if (!input)
            input = arg;
        else if (!name)
            name = arg;
        else
            return snx_error("unexpected argument: %s", arg);
    }
    if (!input) {
        usage();
        return 2;
    }
    /* the settings, whichever way they came: -O and --jobs, or --opt= and --aot_threads= as the launcher passes them */
    if (strlen(g_settings.optimization_level) != 1 || !strchr("0123sz", g_settings.optimization_level[0]))
        return snx_error("opt = %s: not 0, 1, 2, 3, s or z", g_settings.optimization_level);
    if (strcmp(g_settings.llvm_batch, "full") && strcmp(g_settings.llvm_batch, "ipo"))
        return snx_error("llvm_batch = %s: not full or ipo", g_settings.llvm_batch);
    if (!strcmp(g_settings.llvm_batch, "ipo") && g_settings.optimization_level[0] == '0')
        return snx_error("llvm_batch = ipo takes the place of -O1 to -O3, s and z's passes: opt = 0 runs none");
    compile.opt = g_settings.optimization_level;
    gen.chunk_instrs = (size_t)g_settings.unit_size;
    {
        char note[160];
        compile.jobs = snx_aot_threads(g_settings.aot_threads, g_settings.aot_threads_most, note, sizeof note);
        if (note[0]) fprintf(stderr, "scarletnx: %s\n", note);
    }
    /* Auto: up to that many, as the units' memory is measured; 2 when aot_threads_most set that many */
    compile.jobs_open = g_settings.aot_threads > 0 ? 0 : g_settings.aot_threads_most > 0 ? 2 : 1;
    if (!strcmp(target, "win64"))
        compile.target_win = 1;
    else if (!strcmp(target, "linux"))
        compile.target_win = 0;
    else if (!strcmp(target, "ios"))
        ios = 1;
    else if (strcmp(target, "auto"))
        return snx_error("--target %s: not auto, linux, win64 or ios", target);
    /* The backends write the module for the machine they run on (build == host == target) */
    if (compile.target_win != host_win)
        return snx_error("--target %s: modules are built for this host only (%s)", target,
                         host_win ? "win64" : "linux");
    gen.abi = snx_game_abi();
    /* a dump, read in place, else an ExeFS folder or an NRO. An update's dump is its own program; a DLC has none */
    dump = snx_is_file(input) && snx_is_dump(input);
    key_paths = *paths;
    if (keys) key_paths.keys = keys;
    if (title_keys) key_paths.title_keys = title_keys;
    if (dump && snx_extract_title(input, &key_paths, &addon, &owner, &version, &programs)) return 1;
    if (snx_addon_kind(owner, addon) == SNX_ADDON_NONE)
        addon = 0;
    else if (name)
        return snx_error("an update or a DLC is named after its game and itself: --name names a game");
    if (addon && snx_addon_kind(owner, addon) == SNX_ADDON_DLC)
        return snx_error("a DLC runs its game's code and has no program of its own: play its dump, which checks it "
                         "in the game's list of updates and DLCs");
    if (asked >= 0 && !dump)
        return snx_error("--program %d names a program of a dump's application, and %s is no dump", asked, input);
    if (asked >= programs)
        return snx_error("--program %d: %s has %d program%s, 0 to %d", asked, input, programs, programs == 1 ? "" : "s",
                         programs - 1);
    if (programs > 1 && name) { /* its own ID, which the driver gives a dump read in place, names it as it is */
        char of[17];
        snx_title_text(owner, of);
        if (strcmp(name, of))
            return snx_error("%s: an application of %d programs is named by its title ID, %s, which names each "
                             "program: a name names a game of one",
                             name, programs, of);
    }
    make.input = input;
    make.name = name;
    make.outdir = outdir;
    make.runtime = runtime;
    make.jit_cache = jit_cache;
    make.layout = layout;
    make.key_paths = key_paths;
    make.paths = paths;
    make.addon = addon;
    make.owner = owner;
    make.version = version;
    make.compile = compile;
    make.gen = gen;
    make.ios = ios;
    make.no_build = no_build;
    make.keep_c = keep_c;
    make.host_win = host_win;
    make.tmpfs = tmpfs;
    make.dump = dump;
    make.programs = programs;
    make.primary = asked >= 0 ? asked : 0;
    if (asked >= 0) return make_program(&make, asked);
    /* every program of the application, its own first: what the collection's menu starts is there when it does */
    rc = make_program(&make, 0);
    for (i = 1; !rc && i < programs; ++i) rc = make_program(&make, i);
    return rc;
}
