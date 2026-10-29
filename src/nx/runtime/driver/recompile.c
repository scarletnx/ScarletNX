/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#define _POSIX_C_SOURCE 200809L
#include "nx/runtime/driver/recompile.h"
#include "nx/runtime/driver/container.h"
#include "nx/runtime/driver/extract.h"
#include "nx/runtime/driver/game_name.h"
#include "nx/loader/loader.h"
#include "nx/loader/module.h"
#include "nx/loader/module_llvm.h"
#include "common/cpu/recompiler/llvm/ir.h"

/* the triple of the iOS app's code: Darwin's calling convention and the generic AArch64 CPU, in the object format the
   JIT loader reads (recompiler/llvm/ir.c's host_triple). The version only names the ABI */
#define SNX_IOS_TRIPLE "arm64-apple-ios16.0-elf"
#include "nx/runtime/driver/toolchain.h"
#include "snx_settings.h"
#include "common/utilities/build_info.h"
#include "common/utilities/mods.h"
#include "common/utilities/patches.h"
#include <ctype.h>
#include <errno.h>
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
         "  -O LEVEL               Optimization: 0,1,2,3,s,z (default: 2), the setting opt\n"
         "  --jobs N               Parallel compiles (default: CPU count), the setting aot_threads");
    puts("  --jit-cache FILE       Link a profile run's .jitcache into the module as runtime code\n"
         "  --layout FILE          Put the code of these functions first, in this order (hex PCs, one per line):\n"
         "                         a profile's hot functions, as tools/layout.py orders them\n"
         "  --keys FILE            prod.keys for a dump (default: the data folder, then ~/.switch)\n"
         "  --title-keys FILE      title.keys for a dump, found the same way (optional)");
    puts("  --KEY=VALUE            A recompile setting: opt, aot_threads, unit_cache_gb, llvm_pass_<pass>=0\n"
         "                         (scarletnx --settings=recompile lists them)");
    printf("  --chunk N              Instructions per code unit, whole functions batched (default: %d), the setting\n"
           "                         unit_instructions\n",
           g_settings.unit_size);
    puts("  --target auto|linux|win64  The host only: modules are built for the machine that builds them\n"
         "  --target ios           A pack of objects for the iOS app, which loads them into JIT memory (docs/APPLE.md)\n"
         "  --no-build             Stage data/INI only, generate no code\n"
         "  --tmpfs                Leave the RomFS in the extraction (in tmpfs), which the game reads it from");
    puts("  --jit-only             Generate no code: the runtime JIT compiles all of it\n"
         "  --runtime FILE         Deploy this runtime instead of building one\n"
         "  --hook-direct-calls    Route calls and branches between functions through dispatch (nvn_trace)\n"
         "  --keep-ir              Save the generated code (LLVM IR) in <NAME>/<NAME>_build\n"
         "  -q, --quiet            Suppress progress");
    puts("Each game is a folder of <outdir>, <NAME>/: its program <NAME>, <NAME>_code and <NAME>_data/.\n"
         "An update's dump is made beside its game, <ID>/<ID>_<its own title ID>, which runs with the game's\n"
         "settings and saves and reads the dump in place. A DLC has no program: the driver checks it for its game.\n"
         "Hints: <outdir>/<NAME>/<NAME>.hints (hex PCs, one per line).");
}
static unsigned number(const char* text, unsigned max) {
    char* end;
    unsigned long n;
    errno = 0;
    n = strtoul(text, &end, 10);
    return errno || !*text || *end || n == 0 || n > max ? 0 : (unsigned)n;
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
       bundle, where SDL finds neither the app's Frameworks nor a system loader (docs/APPLE.md). A copy the disk refuses
       (a game folder on a read-only image) is said, and no reason to refuse the run: the system's loader may serve it */
    if (folder) snx_deploy_vulkan_driver(run, folder);
#endif
#if defined(_WIN32) || defined(__APPLE__)
    free(folder);
#endif
#ifdef __ANDROID__
    /* the shared storage runs nothing (the game runs on the app's own runtime), and before Android 11 it refuses a
       mode change: there the copy only marks the recompile done */
    rc = snx_copy_file(run, tmp) || snx_replace(tmp, exe);
#else
    rc = snx_copy_file(run, tmp) || snx_make_executable(tmp) || snx_replace(tmp, exe);
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
                    "scarletnx: %s was left by an older version: this one keeps it in %s (docs/DEPRECATIONS.md)\n", old,
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
/* a dump's program, its ExeFS read in place with the mods' files names[i] from paths[i] over it (count of them), and
   its control.nacp out of its Control NCA: nothing is extracted. 0, else nonzero, said why */
static int load_dump(const char* dump, const char* update, const SnxPaths* paths, int quiet, char* const* names,
                     char* const* files, size_t count, SnxProgram* program, char* error, size_t error_size) {
    /* paths->keys and paths->title_keys, when given, are the files the keys are read from */
    SnxDump opened;
    SnxHacFs* fs = NULL;
    SnxExefsReader reader;
    SnxExefsMods mods;
    unsigned char* nacp;
    const char* why;
    size_t nacp_size = 0;
    /* an update's ExeFS is whole: read without its game's, which only its RomFS is made of */
    int rc = snx_dump_open(&opened, update ? NULL : dump, update, paths->keys, paths->title_keys, paths->repo, !quiet);
    if (!rc && opened.program.found) rc = snx_nca_keys(&opened.program, "The game's Program NCA");
    if (!rc && opened.patch.found) rc = snx_nca_keys(&opened.patch, "The update's Program NCA");
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
/* a game made from its dump read in place: what an extraction left in its data folder taken away, as the runtime would
   read it over the dump */
static int drop_extraction(const char* dir, int quiet) {
    static const char* const parts[] = {"exefs", "romfs", "romfs.bin", "romfs.copying"};
    char* part;
    size_t i;
    int rc = 0;
    for (i = 0; !rc && i < sizeof parts / sizeof *parts; ++i) {
        part = snx_path_join(dir, parts[i]);
        if (snx_path_exists(part) || snx_is_link(part)) {
            if (!quiet) printf("Removing %s: the game reads it from its dump\n", part);
            rc = snx_remove_tree(part);
        }
        free(part);
    }
    return rc || snx_mkdirs(dir);
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
                     snx_copy_tree(p->romfs_path, pending) || snx_replace(pending, dst);
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
static int patch_modules(SnxProgram* program, const char* mods, const char* off, int quiet) {
    char error[1024], **patches;
    size_t count = snx_mods_patches(mods, off, &patches), i, j;
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
int snx_recompile_game(int argc, char** argv, const SnxPaths* paths) {
    const char *input = NULL, *name = NULL, *outdir = NULL, *runtime = NULL, *target = "auto", *jit_cache = NULL,
               *layout = NULL, *keys = NULL, *title_keys = NULL, *arg, *value;
    SnxPaths key_paths;
    SnxU64 addon = 0, owner = 0, version = SNX_NO_VERSION; /* an update, its game and version, as its content meta
                                                                names them */
    SnxLlvmBuild llvm;
    SnxCompileOptions compile;
    SnxCodegenOptions gen;
    SnxProgram program;
    char *title = NULL, *game = NULL, *out = NULL, *folder = NULL, *base = NULL, *data = NULL, *code = NULL;
    char *ini = NULL, *ini_text = NULL, *run = NULL, *exe = NULL, *mods = NULL, *mods_off = NULL, *mods_stamp = NULL;
    char **mod_names = NULL, **mod_files = NULL;
    size_t mod_count = 0, found = 0;
    char error[1024], option[64], why[256];
    const char* equal;
    size_t option_size;
    int i, rc = 1, ios = 0, no_build = 0, keep_c = 0, host_win = 0, positional = 0, tmpfs = 0, dump = 0;
    memset(&compile, 0, sizeof(compile));
    memset(&gen, 0, sizeof(gen));
    memset(&program, 0, sizeof(program));
    memset(&llvm, 0, sizeof(llvm));
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
        if (!strcmp(arg, "--no-build"))
            no_build = 1;
        else if (!strcmp(arg, "--tmpfs"))
            tmpfs = 1;
        else if (!strcmp(arg, "--jit-only") || !strcmp(arg, "--interp-only")) {
            if (!strcmp(arg, "--interp-only")) snx_option_renamed("--interp-only", "--jit-only");
            gen.interp_only = 1;
        } else if (!strcmp(arg, "--hook-direct-calls"))
            gen.hook_direct_calls = 1;
        else if (!strcmp(arg, "--keep-ir") || !strcmp(arg, "--keep-c")) {
            if (!strcmp(arg, "--keep-c"))
                fprintf(stderr, "scarletnx: --keep-c is deprecated: use --keep-ir (docs/DEPRECATIONS.md)\n");
            keep_c = 1;
        } else if (!strcmp(arg, "--quiet") || !strcmp(arg, "-q"))
            compile.quiet = gen.quiet = 1;
        else if (arg[0] == '-') {
            value = NULL;
            if (arg[1] == 'O' && arg[2]) {
                value = arg + 2;
                arg = "-O";
            } else if ((equal = strchr(arg, '=')) != NULL) {
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
                strcmp(arg, "--runtime") && strcmp(arg, "--target") && strcmp(arg, "-O") && strcmp(arg, "--jobs") &&
                strcmp(arg, "--chunk") && strcmp(arg, "--jit-cache") && strcmp(arg, "--layout"))
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
            else if (!strcmp(arg, "-O")) {
                if (strlen(value) != 1 || !strchr("0123sz", value[0]))
                    return snx_error("-O %s: not 0, 1, 2, 3, s or z", value);
                strcpy(g_settings.optimization_level, value);
            } else if (!strcmp(arg, "--jobs")) {
                g_settings.aot_threads = (int)number(value, 1024);
                if (!g_settings.aot_threads) return snx_error("--jobs %s: not a whole number from 1 to 1024", value);
            } else if (!strcmp(arg, "--jit-cache")) {
                jit_cache = value;
            } else if (!strcmp(arg, "--layout")) {
                layout = value;
            } else {
                g_settings.unit_size = (int)number(value, INT_MAX);
                if (!g_settings.unit_size)
                    return snx_error("--chunk %s: not a whole number of instructions from 1", value);
            }
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
    compile.opt = g_settings.optimization_level;
    gen.chunk_instrs = (size_t)g_settings.unit_size;
    compile.jobs = g_settings.aot_threads > 0 ? (unsigned)g_settings.aot_threads : snx_cpu_count();
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
    if (dump && snx_extract_title(input, &key_paths, &addon, &owner, &version)) return 1;
    if (snx_addon_kind(owner, addon) == SNX_ADDON_NONE)
        addon = 0;
    else if (name)
        return snx_error("an update or a DLC is named after its game and itself: --name names a game");
    if (addon && snx_addon_kind(owner, addon) == SNX_ADDON_DLC)
        return snx_error("a DLC runs its game's code and has no program of its own: play its dump, which checks it "
                         "in the game's list of updates and DLCs");
    if (dump ? load_dump(input, addon ? input : NULL, &key_paths, compile.quiet, NULL, NULL, 0, &program, error,
                         sizeof(error))
             : snx_load_program(input, &program, error, sizeof(error)))
        return dump ? 1 : snx_error("%s", error);
    if (addon && program.npdm.title_id != owner) {
        char of[17], other[17];
        snx_title_text(owner, of);
        snx_title_text(program.npdm.title_id, other);
        snx_error("%s is an update of the game %s, and its program is %s", input, of,
                  other[0] ? other : "none with a title ID");
        snx_free_program(&program);
        return 1;
    }
    title = snx_game_shown(name, &program);
    if (addon) { /* <game>_<add-on>, in the game's folder */
        char own[32], of[17];
        snx_addon_entry(owner, addon, version, own); /* an update's with its version: each version its own */
        snx_title_text(owner, of);
        game = snx_format("%s_%s", of, own);
    } else
        game = snx_game_name(name, &program.npdm, program.entry_path);
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
    if (addon) {
        char of[17];
        snx_title_text(owner, of);
        folder = snx_path_join(out, of);
    } else
        folder = snx_path_join(out, game);
    if (snx_is_link(folder) || (snx_path_exists(folder) && !snx_is_dir(folder))) {
        snx_error("%s is a file where this game's folder goes: a game an older version made there, named %s; move "
                  "its files as docs/DEPRECATIONS.md says",
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
        mods = snx_game_mods(folder, addon ? of : game, &mods_off);
        mods_stamp = snx_mods_stamp(mods, mods_off);
        mod_count = snx_mods_exefs(mods, mods_off, &mod_names, &mod_files);
    }
    if (mod_count) {
        snx_free_program(&program);
        if (dump ? load_dump(input, addon ? input : NULL, &key_paths, compile.quiet, mod_names, mod_files, mod_count,
                             &program, error, sizeof error)
                 : snx_load_program_mods(input, mod_names, mod_files, mod_count, &program, error, sizeof error)) {
            if (!dump) snx_error("%s", error);
            goto done;
        }
        if (!compile.quiet)
            for (i = 0; i < (int)mod_count; ++i) printf("ExeFS %s from the mod's %s\n", mod_names[i], mod_files[i]);
    }
    if (patch_modules(&program, mods, mods_off, compile.quiet)) goto done; /* before any code is made from them */
    base = snx_path_join(folder, game);
    /* Invalidate the completion stamp before touching an existing game's data
       or module. Failed recompilation must never leave a trusted checkpoint. */
    if (!no_build) {
        char* stamp = snx_format("%s.abi", base);
        int failed = snx_is_file(stamp) && snx_remove_tree(stamp);
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
    gen.hints = read_hints(base, program.npdm.is64, &gen.hint_count);
    if (!compile.quiet) printf("Recompiling %s (%zu modules)\n", title, program.module_count);
    /* an add-on's files: its dump, read in place, or what the driver extracts into its data folder */
    if (addon ? snx_mkdirs(data)
              : dump ? drop_extraction(data, compile.quiet) : copy_data(&program, data, compile.quiet, tmpfs)) {
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
    ini = settings_path(folder, game);
    if (!ini) {
        snx_error("cannot create the settings folder for %s", title);
        goto done;
    }
    if (!snx_is_file(ini) && !addon) { /* an add-on's names its dump only: it has its game's settings */
        ini_text = snx_format(
            "; ScarletNX settings for %s\n; Full option help is written on first run.\n[Display]\nlanguage=1\n", title);
        if (snx_write_file(ini, ini_text, strlen(ini_text))) {
            snx_error("cannot write %s", ini);
            goto done;
        }
    }
    if (snx_record_folders(folder, game)) goto done;
    if (dump) { /* what the game reads in place, and Extract to tmpfs extracts again */
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
    code = ios ? snx_format("%s_code.snxobj", base) : snx_format("%s_code.%s", base, compile.target_win ? "dll" : "so");
    {
        char *kept = keep_c ? snx_format("%s_build", base) : NULL, *units = NULL;
        llvm.output = code;
        llvm.triple = ios ? SNX_IOS_TRIPLE : NULL;
        llvm.opt = compile.opt[0];
        snx_ir_slp_vectorizer(g_settings.slp_vectorizer);
        snx_ir_fpsr_flags(g_settings.fpsr_flags);
        snx_ir_volatile_memory(g_settings.volatile_load_stores);
        llvm.tuning = snx_llvm_tuning(llvm.opt, error, sizeof error);
        if (!llvm.tuning) {
            snx_error("%s", error);
            goto done;
        }
        llvm.jobs = compile.jobs;
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
        free(kept);
        free(units);
        if (i) goto done;
    }
    if (ios) { /* the iOS app is the runtime: only the code is made here */
        if (!compile.quiet) printf("Built %s\n", code);
        rc = 0;
        goto done;
    }
    if (runtime) run = snx_absolute(runtime);
#ifdef SNX_WITH_RUNTIME
    else if (host_win == compile.target_win)
        run = snx_own_runtime(paths);
#endif
    else
        run = snx_build_runtime(paths, compile.target_win, compile.quiet);
    if (!run || !snx_is_file(run)) {
        snx_error("runtime unavailable; specify --runtime FILE");
        goto done;
    }
    /* Deploy the executable last: automatic play uses it as the completion checkpoint. */
    exe = snx_format("%s%s", base, compile.target_win ? ".exe" : "");
    if (snx_deploy_runtime(run, exe)) goto done;
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
    if (!compile.quiet) printf("Built %s\n", exe);
    rc = 0;
done:
    snx_free_program(&program);
    snx_free_strings(mod_names, mod_count);
    snx_free_strings(mod_files, mod_count);
    free(mods);
    free(mods_off);
    free(mods_stamp);
    free((void*)gen.hints);
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
