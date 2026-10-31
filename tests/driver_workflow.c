/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Exercise the real driver/supervisor with synthetic extraction, compilation and game children. */
#define _POSIX_C_SOURCE 200809L
#include "common/utilities/common.h"
#include "common/utilities/setup.h"
#include "nx/runtime/driver/driver.h"
#include "nx/runtime/driver/extract.h"
#include "nx/runtime/driver/game_name.h"
#include "nx/runtime/driver/recompile.h"
#include "snx_settings.h"
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <unistd.h>
#endif

static unsigned checks;
static void check(int good, const char* expression, int line) {
    ++checks;
    if (!good) {
        fprintf(stderr, "driver_workflow.c:%d: check failed: %s\n", line, expression);
        exit(90);
    }
}
#define CHECK(value) check((value) != 0, #value, __LINE__)
static void set_env(const char* name, const char* value) {
#ifdef _WIN32
    CHECK(!_putenv_s(name, value));
#else
    CHECK(!setenv(name, value, 1));
#endif
}
static int event(char stage) {
    FILE* log = snx_fopen(getenv("SNX_WORKFLOW_LOG"), "a");
    const char *fail = getenv("SNX_WORKFLOW_FAIL"), *mode = getenv("SNX_WORKFLOW_MODE");
    CHECK(log != NULL);
    CHECK(fputc(stage, log) != EOF);
    CHECK(!fclose(log));
    if (!fail || fail[0] != stage || !strcmp(mode, "missing")) return 0;
#ifndef _WIN32
    if (!strcmp(mode, "interrupt")) raise(SIGINT);
    if (!strcmp(mode, "terminate")) raise(SIGTERM);
    if (!strcmp(mode, "stop")) raise(SIGSTOP);
    if (!strcmp(mode, "parent")) {
        int ready[2];
        char byte;
        pid_t worker;
        CHECK(!pipe(ready));
        worker = fork();
        CHECK(worker >= 0);
        if (!worker) {
            close(ready[0]);
            signal(SIGTERM, SIG_IGN);
            CHECK(write(ready[1], "x", 1) == 1);
            close(ready[1]);
            sleep(2);
            /* A surviving compiler grandchild would keep the capture pipe open and log X. */
            event('X');
            _exit(0);
        }
        close(ready[1]);
        CHECK(read(ready[0], &byte, 1) == 1);
        close(ready[0]);
        CHECK(!kill(getppid(), SIGTERM));
        for (;;) pause();
    }
#else
    (void)mode;
#endif
    return 7;
}
/* SNX_WORKFLOW_ADDON's dumps: the game's, its update's ("Update" in the name) and a DLC's ("Dlc"), by their file */
#define ADDON_GAME 0x0100ABCD12340000ull
#define ADDON_DLC  0x0100ABCD5555A001ull
static SnxU64 addon_title(const char* dump) {
    const char* leaf = strrchr(dump, '/') ? strrchr(dump, '/') + 1 : dump;
    /* a DLC numbered otherwise than its game's 0x1000: its content meta names its game all the same */
    return strstr(leaf, "Update") ? ADDON_GAME | 0x800 : strstr(leaf, "Dlc") ? ADDON_DLC : ADDON_GAME;
}
static int addon_mode(void) { return getenv("SNX_WORKFLOW_ADDON") && *getenv("SNX_WORKFLOW_ADDON"); }
static void check_key_path(const char* path, const char* supplied) {
    char* expected = supplied && *supplied ? snx_absolute(supplied) : NULL;
    CHECK(expected ? path && !strcmp(path, expected) : !path);
    free(expected);
}
static void check_key_paths(const SnxPaths* paths) {
    const char *keys = getenv("SNX_WORKFLOW_KEYS"), *title_keys = getenv("SNX_WORKFLOW_TITLE_KEYS");
    check_key_path(paths->keys, keys);
    check_key_path(paths->title_keys, title_keys);
}
static void check_runtime_keys(int* argc, char** argv) {
    const char *keys = getenv("SNX_WORKFLOW_KEYS"), *title_keys = getenv("SNX_WORKFLOW_TITLE_KEYS");
    char *expected_keys = snx_absolute(keys), *expected_title_keys = snx_absolute(title_keys);
    int read = 1, write = 1, found_keys = 0, found_title_keys = 0;
    while (read < *argc && strcmp(argv[read], "--")) {
        if (!strcmp(argv[read], "--keys") || !strcmp(argv[read], "--title-keys")) {
            int title = !strcmp(argv[read], "--title-keys");
            const char* expected = title ? expected_title_keys : expected_keys;
            CHECK(read + 1 < *argc && expected && !strcmp(argv[read + 1], expected));
            if (title) found_title_keys = 1;
            else found_keys = 1;
            read += 2;
        } else
            argv[write++] = argv[read++];
    }
    while (read < *argc) argv[write++] = argv[read++];
    argv[write] = NULL;
    *argc = write;
    CHECK(found_keys && found_title_keys);
    free(expected_keys);
    free(expected_title_keys);
}
/* the title ID the extraction's NPDM gives (SNX_WORKFLOW_NPDM "id"), else none */
int snx_extract_title(const char* dump, const SnxPaths* paths, SnxU64* title, SnxU64* game, SnxU64* version) {
    const char* kind = getenv("SNX_WORKFLOW_NPDM");
    check_key_paths(paths);
    CHECK(snx_is_file(dump));
    *title = addon_mode() ? addon_title(dump) : kind && !strcmp(kind, "id") ? 0x0100ABCD12345000ull : 0;
    *game = addon_mode() ? ADDON_GAME : *title;
    /* an update's version from its name: "Update" 65536, "Update old" 0 */
    if (version) *version = strstr(dump, "Update old") ? 0 : strstr(dump, "Update") ? 65536 : SNX_NO_VERSION;
    return 0;
}
/* the icon alone, as -x keeps it: what a game made before it kept one gets from its dump */
int snx_extract_icon(const char* dump, const SnxPaths* paths, const char* out) {
    char* to = snx_path_join(out, "icon.jpg");
    int rc;
    check_key_paths(paths);
    CHECK(snx_is_file(dump));
    rc = snx_write_file(to, "icon", 4);
    free(to);
    return rc;
}
int snx_extract(int argc, char** argv, const SnxPaths* paths) {
    char* partial;
    int rc;
    check_key_paths(paths);
    if (addon_mode() && argc == 6 && !strcmp(argv[2], "--update")) { /* an update over its game: its ExeFS */
        CHECK(!strcmp(argv[0], "--out") && !strcmp(argv[4], "--") && snx_is_file(argv[3]) && snx_is_file(argv[5]));
        partial = snx_path_join(argv[1], "exefs");
        CHECK(!snx_mkdirs(partial));
        free(partial);
        return event('E');
    }
    if (addon_mode() && argc == 4 && snx_addon_kind(ADDON_GAME, addon_title(argv[3])) == SNX_ADDON_DLC) { /* its image */
        char text[17], *image;
        CHECK(!strcmp(argv[0], "--out") && !strcmp(argv[2], "--"));
        snx_title_text(addon_title(argv[3]), text);
        partial = snx_path_join(argv[1], "system/archives");
        image = snx_format("%s/%s.romfs", partial, text);
        CHECK(!snx_mkdirs(partial) && !snx_write_file(image, "image", 5));
        free(partial);
        free(image);
        return event('E');
    }
    CHECK(argc == 4 && !strcmp(argv[0], "--out") && !strcmp(argv[2], "--"));
    CHECK(snx_is_file(argv[3]));
    partial = snx_path_join(argv[1], "exefs"); /* into a game's data folder: what an extraction left taken away */
    CHECK(!snx_path_exists(partial));
    free(partial);
    CHECK(!snx_mkdirs(argv[1]));
    partial = snx_path_join(argv[1], "partial");
    CHECK(!snx_write_file(partial, "partial", 7));
    free(partial);
    /* an ExeFS, with the NPDM SNX_WORKFLOW_NPDM names: "id" one with a title ID, "bad" a broken one */
    partial = snx_path_join(argv[1], "exefs");
    CHECK(!snx_mkdirs(partial));
    {
        char *main = snx_path_join(partial, "main"), *npdm = snx_path_join(partial, "main.npdm");
        const char* kind = getenv("SNX_WORKFLOW_NPDM");
        unsigned char meta[0xa0];
        SnxU64 id = 0x0100ABCD12345000ull;
        int i;
        memset(meta, 0, sizeof meta);
        memcpy(meta, "META", 4);
        meta[12] = 1;
        meta[0x70] = 0x80;                                   /* the ACI0 at 0x80 */
        meta[0x74] = 0x20;                                   /* its size */
        if (kind && !strcmp(kind, "bad")) meta[0x75] = 0x10; /* past the file's end */
        memcpy(meta + 0x80, "ACI0", 4);
        for (i = 0; i < 8; ++i) meta[0x90 + i] = (unsigned char)(id >> (8 * i));
        CHECK(!snx_write_file(main, "main", 4));
        if (kind && *kind) CHECK(!snx_write_file(npdm, meta, sizeof meta));
        free(main);
        free(npdm);
    }
    free(partial);
    partial = snx_path_join(argv[1], "romfs"); /* and a RomFS */
    CHECK(!snx_mkdirs(partial));
    free(partial);
    partial = snx_path_join(argv[1], "romfs/data");
    CHECK(!snx_write_file(partial, "data", 4));
    free(partial);
    rc = event('E');
    if (!strcmp(getenv("SNX_WORKFLOW_MODE"), "missing") && getenv("SNX_WORKFLOW_FAIL") &&
        !strcmp(getenv("SNX_WORKFLOW_FAIL"), "E"))
        CHECK(!snx_remove_tree(argv[1]));
    return rc;
}
/* a dump given to -r, read in place (Extract dump off): a file, not an NRO */
static int dump_input(const char* input) {
    size_t n = strlen(input);
    return snx_is_file(input) && !(n > 4 && input[n - 4] == '.' && (input[n - 3] | 32) == 'n' &&
                                   (input[n - 2] | 32) == 'r' && (input[n - 1] | 32) == 'o');
}
/* this build's stamp: ABI 4, made from commit abc1234 */
#define SNX_WORKFLOW_STAMP "4\nabc1234\n"
char* snx_game_stamp(const char* mods) {
    return mods[0] ? snx_format("%s mods %s", SNX_WORKFLOW_STAMP, mods) : snx_strdup(SNX_WORKFLOW_STAMP);
}
/* recompile.c's: the runtime a game's folder gets, this program (which the recompile below copies as the game), and
   its deploy, as recompile.c's: a copy made executable that replaces the game's program, a new file */
char* snx_own_runtime(const SnxPaths* paths) { return snx_strdup(paths->executable); }
int snx_deploy_runtime(const char* run, const char* exe) {
    char* tmp = snx_format("%s.tmp", exe);
    int rc = snx_copy_file(run, tmp) || snx_make_executable(tmp) || snx_replace(tmp, exe);
    free(tmp);
    return rc;
}
/* recompile.c's: a setting of the recompile, from the generated tables */
const IniOpt* snx_recompile_setting(const char* key) {
    const IniOpt* o = ini_find(g_recompile_ini, key);
    if (!o) o = ini_find(g_llvm_ini, key);
    return o ? o : ini_find(g_recompile_game_ini, key);
}
/* SNX_WORKFLOW_ADDON's recompile: --outdir GAMES -- DUMP [NAME]. A game's dump is named by its title ID, an update
   goes into its game's folder as <game>_<its own>; a DLC is never recompiled */
static int addon_recompile(int argc, char** argv, const SnxPaths* paths) {
    char of[17], own[32], *folder, *name, *game, *stamp, *code, *data;
    SnxU64 title;
    CHECK((argc == 4 || argc == 5) && !strcmp(argv[0], "--outdir") && !strcmp(argv[2], "--"));
    title = addon_title(argv[3]);
    snx_title_text(ADDON_GAME, of);
    snx_addon_entry(ADDON_GAME, title, strstr(argv[3], "Update old") ? 0 : 65536, own); /* each version its own */
    CHECK(snx_addon_kind(ADDON_GAME, title) != SNX_ADDON_DLC &&
          argc == (snx_addon_kind(ADDON_GAME, title) == SNX_ADDON_NONE ? 5 : 4));
    folder = snx_path_join(argv[1], of);
    name = snx_addon_kind(ADDON_GAME, title) == SNX_ADDON_NONE ? snx_strdup(of) : snx_format("%s_%s", of, own);
#ifdef _WIN32
    game = snx_format("%s/%s.exe", folder, name);
#else
    game = snx_format("%s/%s", folder, name);
#endif
    stamp = snx_format("%s/%s.abi", folder, name);
    code = snx_format("%s/%s_code.%s", folder, name,
#ifdef _WIN32
                      "dll"
#else
                      "so"
#endif
    );
    data = snx_format("%s/%s_data", folder, name);
    CHECK(!snx_mkdirs(data) && !snx_copy_file(paths->executable, game) && !snx_make_executable(game));
    CHECK(!snx_write_file(stamp, SNX_WORKFLOW_STAMP, strlen(SNX_WORKFLOW_STAMP)) && !snx_write_file(code, "x", 1));
    free(folder);
    free(name);
    free(game);
    free(stamp);
    free(code);
    free(data);
    return event('R');
}
int snx_recompile_game(int argc, char** argv, const SnxPaths* paths) {
    char *game, *partial, *name, *folder;
    int rc, tmpfs;
    check_key_paths(paths);
    if (addon_mode()) return addon_recompile(argc, argv, paths);
    /* Extract to tmpfs: the RomFS left in the extraction */
    tmpfs = argc > 2 && !strcmp(argv[2], "--tmpfs");
    CHECK(tmpfs == (getenv("SNX_WORKFLOW_TMPFS") && *getenv("SNX_WORKFLOW_TMPFS")));
    if (tmpfs) {
        memmove(argv + 2, argv + 3, (size_t)(argc - 3) * sizeof *argv);
        --argc;
    }
    /* the optimization level a played dump was given, passed on */
    if (getenv("SNX_WORKFLOW_OPT") && *getenv("SNX_WORKFLOW_OPT")) {
        CHECK(argc > 4 && !strcmp(argv[2], "-O") && !strcmp(argv[3], getenv("SNX_WORKFLOW_OPT")));
        memmove(argv + 2, argv + 4, (size_t)(argc - 4) * sizeof *argv);
        argc -= 2;
    }
    /* a recompile's setting the dump was played with (SNX_WORKFLOW_FLAGS), passed on as it was given */
    {
        const char* flags = getenv("SNX_WORKFLOW_FLAGS");
        char key[64];
        size_t length = flags && strchr(flags, '=') ? (size_t)(strchr(flags, '=') - flags) : 0;
        if (length > 2 && length - 2 < sizeof key) {
            memcpy(key, flags + 2, length - 2);
            key[length - 2] = 0;
            if (snx_recompile_setting(key)) {
                CHECK(argc > 3 && !strcmp(argv[2], flags));
                memmove(argv + 2, argv + 3, (size_t)(argc - 3) * sizeof *argv);
                --argc;
            }
        }
    }
    /* a dump's: no name given, the recompile names the game; a game made already: its folder's name */
    CHECK((argc == 4 || argc == 5) && !strcmp(argv[0], "--outdir") && !strcmp(argv[2], "--"));
    if (getenv("SNX_WORKFLOW_SOURCE") && *getenv("SNX_WORKFLOW_SOURCE")) {
        char *given = snx_absolute(argv[3]), *expected = snx_absolute(getenv("SNX_WORKFLOW_SOURCE"));
        const char* named = getenv("SNX_WORKFLOW_NAMED"); /* the name a game made already is given, if any */
        CHECK(named && *named ? argc == 5 && !strcmp(argv[4], named) : argc == 4);
        CHECK(given && expected && !strcmp(given, expected));
        free(given);
        free(expected);
    } else if (getenv("SNX_WORKFLOW_NRO") && *getenv("SNX_WORKFLOW_NRO")) {
        char *given, *expected;
        CHECK(snx_is_file(argv[3]));
        /* The driver hands the stage a resolved path (Windows: backslashes and long
           names), so compare canonical spellings like the game's cwd check does. */
        given = snx_absolute(argv[3]);
        expected = snx_absolute(getenv("SNX_WORKFLOW_NRO"));
        CHECK(given && expected && !strcmp(given, expected));
        free(given);
        free(expected);
    } else if (getenv("SNX_WORKFLOW_DUMP") && *getenv("SNX_WORKFLOW_DUMP")) {
        /* Extract dump off: the dump itself, read in place, named as its game is */
        char *given = snx_absolute(argv[3]), *expected = snx_absolute(getenv("SNX_WORKFLOW_DUMP"));
        CHECK(argc == 5 && given && expected && snx_same_file(given, expected));
        free(given);
        free(expected);
    } else
        CHECK(snx_is_dir(argv[3]));
    CHECK(!snx_mkdirs(argv[1]));
    partial = snx_path_join(argv[1], "partial-build");
    CHECK(!snx_write_file(partial, "partial", 7));
    free(partial);
    rc = event('R');
    if (rc || (!strcmp(getenv("SNX_WORKFLOW_MODE"), "missing") && getenv("SNX_WORKFLOW_FAIL") &&
               !strcmp(getenv("SNX_WORKFLOW_FAIL"), "R")))
        return rc;
    { /* the program loaded, as the recompile loads it: a broken NPDM stops it */
        SnxNpdm npdm;
        char error[256];
        if (!dump_input(argv[3]) && snx_load_npdm(argv[3], &npdm, error, sizeof error)) {
            fprintf(stderr, "scarletnx: %s\n", error);
            return 1;
        }
        name = argc == 5 ? snx_strdup(argv[4]) : snx_game_name(NULL, &npdm, argv[3]);
    }
    /* the game in a folder of its own, with its data, as the recompile makes it */
    folder = snx_path_join(argv[1], name);
    CHECK(!snx_mkdirs(folder));
    partial = snx_format("%s/%s_data/exefs", folder, name);
    /* the extraction's ExeFS in it, as the recompile stages it; a dump's stays in the dump, its data folder empty */
    CHECK(!snx_mkdirs(partial));
    if (dump_input(argv[3])) CHECK(!snx_remove_tree(partial));
    {
        static const char* const files[] = {"main", "main.npdm"};
        size_t k;
        for (k = 0; k < 2; ++k) {
            char *from = snx_format("%s/exefs/%s", argv[3], files[k]), *to = snx_path_join(partial, files[k]);
            if (snx_is_file(from) && strcmp(from, to)) CHECK(!snx_copy_file(from, to));
            free(from);
            free(to);
        }
    }
    free(partial);
    { /* and its RomFS, unless it stays in tmpfs or is there already */
        char *from = snx_path_join(argv[3], "romfs"), *to = snx_format("%s/%s_data/romfs", folder, name);
        if (!tmpfs && snx_is_dir(from) && !snx_is_dir(to)) CHECK(!snx_copy_tree(from, to));
        free(from);
        free(to);
    }
#ifdef _WIN32
    game = snx_format("%s/%s.exe", folder, name);
#else
    game = snx_path_join(folder, name);
#endif
    CHECK(!snx_copy_file(paths->executable, game));
    CHECK(!snx_make_executable(game));
    {
        char* stamp = snx_format("%s/%s.abi", folder, name);
        char* code = snx_format("%s/%s_code.%s", folder, name,
#ifdef _WIN32
                                "dll"
#else
                                "so"
#endif
        );
        CHECK(!snx_write_file(stamp, SNX_WORKFLOW_STAMP, strlen(SNX_WORKFLOW_STAMP)));
        CHECK(!snx_write_file(code, "synthetic", 9));
        free(stamp);
        free(code);
    }
    free(game);
    free(folder);
    free(name);
    return 0;
}
static void run(const char* exe, const char* repo, const char* dump, const char* log, int expected, const char* events,
                const char* diagnostic) {
    const char* args[32];
    const char *headless = getenv("SNX_WORKFLOW_HEADLESS"), *flags = getenv("SNX_WORKFLOW_FLAGS"),
               *keys = getenv("SNX_WORKFLOW_KEYS"), *title_keys = getenv("SNX_WORKFLOW_TITLE_KEYS");
    const char *outdir = getenv("SNX_WORKFLOW_OUTDIR"), *out = getenv("SNX_WORKFLOW_OUT");
    int n = 5;
    char* output = NULL;
    unsigned char* recorded;
    size_t size;
    int rc;
    CHECK(!snx_write_file(log, "", 0));
    args[0] = exe;
    args[1] = "--repo";
    args[2] = repo;
    args[3] = "--build-dir";
    args[4] = repo;
    if (getenv("SNX_WORKFLOW_EXTRACT") && *getenv("SNX_WORKFLOW_EXTRACT")) args[n++] = "--extract-container";
    if (flags && *flags && strncmp(flags, "--backend=", 10)) args[n++] = flags;
    if (headless && *headless && !strcmp(getenv("SNX_WORKFLOW_PREFIX"), "1")) args[n++] = headless;
    args[n++] = dump;
    if (keys && *keys) {
        args[n++] = "--keys";
        args[n++] = keys;
    }
    if (title_keys && *title_keys) {
        args[n++] = "--title-keys";
        args[n++] = title_keys;
    }
    if (flags && !strncmp(flags, "--backend=", 10)) args[n++] = flags;
    if (headless && *headless && strcmp(getenv("SNX_WORKFLOW_PREFIX"), "1")) args[n++] = headless;
    if (outdir && *outdir) { /* "none": the option without its folder */
        args[n++] = "--outdir";
        if (strcmp(outdir, "none")) args[n++] = outdir;
    }
    if (out && *out) {
        args[n++] = "--out";
        args[n++] = out;
    }
    if (getenv("SNX_WORKFLOW_UPDATE") && *getenv("SNX_WORKFLOW_UPDATE")) { /* one, or two with a | between */
        static char two[2048];
        char* bar;
        snprintf(two, sizeof two, "%s", getenv("SNX_WORKFLOW_UPDATE"));
        bar = strchr(two, '|');
        if (bar) *bar = 0;
        args[n++] = "--update";
        args[n++] = two;
        if (bar) {
            args[n++] = "--update";
            args[n++] = bar + 1;
        }
    }
    if (getenv("SNX_WORKFLOW_ARGS") && *getenv("SNX_WORKFLOW_ARGS")) {
        args[n++] = "--";
        args[n++] = "some arg";
        args[n++] = "";
        args[n++] = "-rx";
        args[n++] = "--headless=0";
    }
    args[n] = NULL;
    rc = snx_run(args, NULL, NULL, &output);
    if (rc != expected) fprintf(stderr, "expected %d, got %d: %s\n", expected, rc, output);
    CHECK(rc == expected);
    if (diagnostic && !strstr(output, diagnostic))
        fprintf(stderr, "expected it to say %s, got: %s\n", diagnostic, output);
    if (diagnostic) CHECK(strstr(output, diagnostic) != NULL);
    recorded = snx_read_file(log, &size);
    if (recorded && strcmp((const char*)recorded, events))
        fprintf(stderr, "expected the stages %s, got %s: %s\n", events, (const char*)recorded, output);
    CHECK(recorded && !strcmp((const char*)recorded, events));
    free(recorded);
    free(output);
}
/* the runtime's settings (runtime/options.c): those this test gives */
int snx_setting_known(const char* key) {
    static const char* const known[] = {"touch", "controls", "resolution", "controls_a", "seconds", "slp_vectorizer"};
    size_t i;
    for (i = 0; i < sizeof known / sizeof *known; ++i)
        if (!strcmp(key, known[i])) return 1;
    return 0;
}
/* the runtime's --probe (runtime/main.c): how many times the driver ran it */
static int probes;
int snx_probe(void) {
    ++probes;
    return 0;
}
#ifndef _WIN32
/* 1 when the game's .ini records dump as its source (snx_game_source), its folder making no link to it */
static int records_dump(const char* folder, const char* name, const char* dump) {
    char *link = snx_format("%s/%s.source", folder, name), *source;
    int recorded, good;
    source = snx_game_source(folder, name, &recorded);
    good = recorded && strcmp(source, link) && snx_same_file(source, dump) && !snx_is_link(link);
    free(source);
    free(link);
    return good;
}
#endif
/* snx_record_source on the .ini a game has: made with [Game] when there is none, an older source= anywhere taken out
   and the new one put under [Game], CRLF lines and a last line without its newline kept, a file naming the dump
   already left as it is, and a path with a line break refused */
static void record_source(const char* root) {
    static const char before[] = "; a comment\r\n[Display]\r\nlanguage=2\r\n[Game]\r\n[Other]\r\n source = old\r\nkey=1";
    static const char after[] = "; a comment\r\n[Display]\r\nlanguage=2\r\n[Game]\r\nsource=/new dump.nsp\n[Other]\r\n"
                                "key=1\n";
    static const char twice[] = "source=/x\n[Display]\nsource = /x\n";
    char *folder = snx_path_join(root, "record"), *ini = snx_game_ini(folder, "G"), *text, *source;
    size_t size;
    int recorded;
    CHECK(!snx_record_source(folder, "G", "/a dump.xci"));
    text = (char*)snx_read_file(ini, &size);
    CHECK(text && !strcmp(text, "[Game]\nsource=/a dump.xci\n"));
    free(text);
    CHECK(!snx_write_file(ini, before, sizeof before - 1) && !snx_record_source(folder, "G", "/new dump.nsp"));
    text = (char*)snx_read_file(ini, &size);
    CHECK(text && !strcmp(text, after));
    free(text);
    source = snx_game_source(folder, "G", &recorded);
    CHECK(recorded && !strcmp(source, "/new dump.nsp"));
    free(source);
    CHECK(!snx_write_file(ini, twice, sizeof twice - 1) && !snx_record_source(folder, "G", "/x"));
    text = (char*)snx_read_file(ini, &size);
    CHECK(text && !strcmp(text, twice)); /* named already */
    free(text);
    CHECK(snx_record_source(folder, "G", "/a\nb"));
    CHECK(!snx_remove_tree(ini));
    source = snx_game_source(folder, "G", &recorded);
    text = snx_path_join(folder, "G.source");
    CHECK(!recorded && source && !strcmp(source, text));
    free(text);
    free(source);
    free(ini);
    free(folder);
}
/* snx_record_folders: a game of the user's data folder gets <name>.folders naming the config, state and cache folders
   snx_user_path moves its files to, each made, the logs and caches Settings > App > Folders picked in app.ini among
   them; a game elsewhere none, and an older one there is taken away. The user's folders in root for the test, from the
   variables that name them */
static void record_folders(const char* root) {
#ifdef _WIN32
    static const char* const vars[] = {"APPDATA"};
#elif defined(SNX_XDG)
    static const char* const vars[] = {"XDG_DATA_HOME", "XDG_CONFIG_HOME", "XDG_STATE_HOME", "XDG_CACHE_HOME"};
#else
    static const char* const vars[] = {"HOME"};
#endif
    static const char* const keys[] = {"config", "state", "cache"};
    char *old[4], *value, *data, *folder, *record, *text, *expected, *line, *other;
    size_t i, size;
    int kind;
    for (i = 0; i < sizeof vars / sizeof *vars; ++i) {
        old[i] = getenv(vars[i]) ? snx_strdup(getenv(vars[i])) : NULL;
        value = snx_format("%s/user %d", root, (int)i);
        set_env(vars[i], value);
        free(value);
    }
    data = snx_user_data_dir();
    CHECK(data != NULL);
    folder = snx_format("%s/games/G", data);
    record = snx_format("%s/G.folders", folder);
    expected = snx_strdup("");
    CHECK(!snx_mkdirs(folder) && !snx_record_folders(folder, "G"));
    for (kind = SNX_USER_CONFIG; kind <= SNX_USER_CACHE; ++kind) {
        char* moved = snx_user_path(folder, kind);
        CHECK(strcmp(moved, folder) && snx_is_dir(moved));
        line = snx_format("%s%s=%s\n", expected, keys[kind - SNX_USER_CONFIG], moved);
        free(expected);
        free(moved);
        expected = line;
    }
    text = (char*)snx_read_file(record, &size);
    if (!text || strcmp(text, expected))
        fprintf(stderr, "%s has:\n%s\nnot:\n%s", record, text ? text : "(no such file)\n", expected);
    CHECK(text && !strcmp(text, expected));
    free(text);
    { /* Logs and Caches picked, outside the user's folders: named too, so a game started elsewhere finds them */
        char *config = snx_user_dir(SNX_USER_CONFIG), *ini = snx_path_join(config, "app.ini"), *picked[2], *game[2],
             *at;
        picked[0] = snx_path_join(root, "picked logs");
        picked[1] = snx_path_join(root, "picked caches");
        text = snx_format("[folders]\nstate = %s\ncache = %s\n", picked[0], picked[1]);
        CHECK(!snx_mkdirs(config) && !snx_write_file(ini, text, strlen(text)));
        free(text);
        CHECK(!snx_record_folders(folder, "G"));
        for (i = 0; i < 2; ++i) { /* games/G under each picked folder, made */
            game[i] = snx_user_path(folder, i ? SNX_USER_CACHE : SNX_USER_STATE);
            CHECK(strcmp(game[i], folder) && snx_path_within(game[i], picked[i]) && snx_is_dir(game[i]));
        }
        at = snx_user_path(folder, SNX_USER_CONFIG);
        free(expected);
        expected = snx_format("config=%s\nstate=%s\ncache=%s\n", at, game[0], game[1]);
        free(at);
        text = (char*)snx_read_file(record, &size);
        if (!text || strcmp(text, expected))
            fprintf(stderr, "%s has:\n%s\nnot:\n%s", record, text ? text : "(no such file)\n", expected);
        CHECK(text && !strcmp(text, expected));
        free(text);
        for (i = 0; i < 2; ++i) {
            free(picked[i]);
            free(game[i]);
        }
        CHECK(!remove(ini));
        free(ini);
        free(config);
    }
    free(record);
    other = snx_format("%s/elsewhere/G", root);
    record = snx_format("%s/G.folders", other);
    CHECK(!snx_mkdirs(other) && !snx_write_file(record, expected, strlen(expected)));
    CHECK(!snx_record_folders(other, "G") && !snx_path_exists(record));
    for (i = 0; i < sizeof vars / sizeof *vars; ++i) {
#ifdef _WIN32
        CHECK(!_putenv_s(vars[i], old[i] ? old[i] : ""));
#else
        CHECK(old[i] ? !setenv(vars[i], old[i], 1) : !unsetenv(vars[i]));
#endif
        free(old[i]);
    }
    free(data);
    free(folder);
    free(record);
    free(expected);
    free(other);
}
static void reset(const char* repo, const char* dump) {
    CHECK(!snx_remove_tree(repo));
    CHECK(!snx_mkdirs(repo));
    CHECK(!snx_write_file(dump, "synthetic", 9));
    set_env("SNX_WORKFLOW_FAIL", "");
}
int main(int argc, char** argv) {
    char *exe, *root, *repo, *dump, *log, *extracted, *extracting, *kept, *game, *cwd, *expected_cwd, *keys,
        *title_keys;
    const char* args[3];
    char* output;
    int rc;
#ifndef _WIN32
    const char* modes[] = {"interrupt", "terminate", "stop", "parent"};
    int statuses[] = {128 + SIGINT, 128 + SIGTERM, 128 + SIGSTOP, 128 + SIGTERM};
    size_t i;
#endif
    if (argc == 3 && !strcmp(argv[1], "--probe")) {
        args[0] = argv[0];
        args[1] = NULL;
        if (!strcmp(argv[2], "exec")) args[0] = "/nonexistent/scarletnx-test";
        return snx_run_stage(args, !strcmp(argv[2], "cwd") ? "/nonexistent/scarletnx-test" : NULL, "Probe");
    }
    if (argc > 1) { /* the setup's options taken out first, as the program's entry does (runtime/entry.c) */
        char why[160];
        argc = snx_setup_options(argc, argv, why, sizeof why);
        if (argc < 0) {
            fprintf(stderr, "scarletnx: %s\n", why);
            return 2;
        }
        rc = snx_driver(argc, argv);
        if (rc >= 0) return rc;
    }
    if (getenv("SNX_WORKFLOW_CHILD")) {
        check_runtime_keys(&argc, argv);
        const char* flag = getenv("SNX_WORKFLOW_HEADLESS");
        const char* fullscreen = getenv("SNX_WORKFLOW_FLAGS");
        if (fullscreen &&
            (!strcmp(fullscreen, "--fullscreen") || !strcmp(fullscreen, "--fullscreen=1") || !strcmp(fullscreen, "-f")))
            CHECK(argc == 2 && !strcmp(argv[1], "-f"));
        if (fullscreen && !strcmp(fullscreen, "--fullscreen=0")) CHECK(argc == 2 && !strcmp(argv[1], "--fullscreen=0"));
        /* the game's own settings, any --key=value of its .ini, reach it as they were given */
        if (fullscreen && strchr(fullscreen, '=') && strncmp(fullscreen, "--fullscreen=", 13) &&
            strncmp(fullscreen, "--backend=", 10))
            CHECK(argc == 2 && !strcmp(argv[1], fullscreen));
        if (flag && *flag) CHECK(argc == 2 && !strcmp(argv[1], "-H"));
        if (getenv("SNX_WORKFLOW_ARGS") && *getenv("SNX_WORKFLOW_ARGS")) {
            CHECK(argc == 6 && !strcmp(argv[1], "--"));
            CHECK(!strcmp(argv[2], "some arg") && !strcmp(argv[3], ""));
            CHECK(!strcmp(argv[4], "-rx") && !strcmp(argv[5], "--headless=0"));
        }
        /* All game invocations must chdir, including repeat launches. */
        cwd = snx_absolute(".");
        expected_cwd = snx_absolute(getenv("SNX_WORKFLOW_GAMES"));
        CHECK(cwd && expected_cwd && !strcmp(cwd, expected_cwd));
        free(cwd);
        free(expected_cwd);
        /* an update or a DLC runs as its own program, in its game's folder: A */
        return event(addon_mode() && strchr(strrchr(argv[0], '/') ? strrchr(argv[0], '/') : argv[0], '_') ? 'A' : 'G');
    }
    { /* --probe runs in the driver, which is all iOS's menu runs on its thread; alone */
        char flag[] = "--probe", extra[] = "game.nsp";
        char *probe[] = {argv[0], flag, NULL}, *more[] = {argv[0], flag, extra, NULL};
        CHECK(snx_driver(2, probe) == 0 && probes == 1);
        CHECK(snx_driver(3, more) == 1 && probes == 1);
    }
    exe = snx_executable(argv[0]);
    root = snx_temp_dir(NULL);
    CHECK(exe && root);
    keys = snx_path_join(root, "provided prod.keys");
    title_keys = snx_path_join(root, "provided title.keys");
    CHECK(!snx_write_file(keys, "header_key = 00112233\n", strlen("header_key = 00112233\n")) &&
          !snx_write_file(title_keys, "0100000000000000000000000000000a = 0011aabb\n",
                          strlen("0100000000000000000000000000000a = 0011aabb\n")));
    set_env("SNX_WORKFLOW_KEYS", keys);
    set_env("SNX_WORKFLOW_TITLE_KEYS", title_keys);
    record_source(root);
    record_folders(root);
    repo = snx_path_join(root, "checkout with spaces");
    CHECK(!snx_mkdirs(repo));
    dump = snx_path_join(repo, "Test [game];.NSP");
    log = snx_path_join(root, "events");
    /* the extraction, straight into the game's data folder, and the file saying it is not finished */
    extracted = snx_path_join(repo, "recompiled/Test game/Test game_data/exefs");
    extracting = snx_path_join(repo, "recompiled/Test game/Test game.extracting");
    kept = snx_path_join(repo, "work/Test game"); /* one --out names */
#ifdef _WIN32
    game = snx_path_join(repo, "recompiled/Test game/Test game.exe");
#else
    game = snx_path_join(repo, "recompiled/Test game/Test game");
#endif
    cwd = snx_path_join(repo, "recompiled/Test game"); /* the game runs in its folder */
    set_env("SNX_WORKFLOW_GAMES", cwd);
    free(cwd);
    set_env("SNX_WORKFLOW_CHILD", "1");
    set_env("SNX_WORKFLOW_LOG", log);
    set_env("SNX_WORKFLOW_MODE", "exit");
    set_env("SNX_WORKFLOW_HEADLESS", "");
    set_env("SNX_WORKFLOW_PREFIX", "0");
    set_env("SNX_WORKFLOW_NRO", "");
    set_env("SNX_WORKFLOW_SOURCE", "");
    set_env("SNX_WORKFLOW_NAMED", "");
    set_env("SNX_WORKFLOW_OUTDIR", "");
    set_env("SNX_WORKFLOW_OUT", "");
    set_env("SNX_WORKFLOW_DUMP", "");
    set_env("SNX_WORKFLOW_ADDON", "");
    set_env("SNX_WORKFLOW_UPDATE", "");
    /* Extract dump on: the dump extracted into its game's data folder, which the game is made from */
    set_env("SNX_WORKFLOW_EXTRACT", "1");
    reset(repo, dump);
    run(exe, repo, dump, log, 0, "ERG", NULL);
    CHECK(snx_is_dir(extracted) && snx_is_file(game));
    { /* the extraction is the game's data, kept nowhere else */
        char* romfs = snx_path_join(repo, "recompiled/Test game/Test game_data/romfs/data");
        CHECK(snx_is_file(romfs) && !snx_path_exists(kept) && !snx_is_file(extracting));
        free(romfs);
    }
    run(exe, repo, dump, log, 0, "G", NULL);
    CHECK(!snx_remove_tree(game));
    run(exe, repo, dump, log, 0, "RG", NULL);
    set_env("SNX_WORKFLOW_FAIL", "G");
    run(exe, repo, dump, log, 7, "G", "Game failed (exit status 7)");
    set_env("SNX_WORKFLOW_FAIL", "");
    run(exe, repo, dump, log, 0, "G", NULL);
    { /* a game whose runtime another build of this ABI deployed: played as it is, the runtime replaced by this build's
         first; one already this build's is left as it is (a link to it stays the same file) */
        char* kept_runtime = snx_format("%s.kept", game);
        FILE* f = snx_fopen(game, "ab");
        unsigned char *now, *ours;
        size_t a, b;
        CHECK(f && fputs("another build", f) >= 0 && !fclose(f));
        run(exe, repo, dump, log, 0, "G", "ran another build's runtime");
        now = snx_read_file(game, &a);
        ours = snx_read_file(exe, &b);
        CHECK(now && ours && a == b && !memcmp(now, ours, a));
        free(now);
        free(ours);
        CHECK(!snx_link_file(game, kept_runtime));
        run(exe, repo, dump, log, 0, "G", NULL);
        CHECK(snx_same_file(game, kept_runtime));
        CHECK(!snx_remove_tree(kept_runtime));
        free(kept_runtime);
    }

    /* Missing/mismatched/malformed ABI stamps are never trusted, nor one another build wrote with the same ABI: an
       older version's ABI alone, another commit's, or this one's cut short or with more after it */
    {
        const char* const stale[] = {
            "3\nabc1234\n",      "broken", "4", "4garbage\n", "4\n", "4\ndef5678\n", "4\nabc1234", "4\nabc1234\n\n",
            "4\nabc1234-dirty\n"};
        char* stamp = snx_path_join(repo, "recompiled/Test game/Test game.abi");
        size_t j;
        for (j = 0; j < sizeof stale / sizeof *stale; ++j) {
            CHECK(!snx_write_file(stamp, stale[j], strlen(stale[j])));
            run(exe, repo, dump, log, 0, "RG", NULL);
            run(exe, repo, dump, log, 0, "G", NULL); /* the recompile stamped it as this build */
        }
        CHECK(!snx_remove_tree(stamp));
        run(exe, repo, dump, log, 0, "RG", NULL);
        free(stamp);
    }
    {
        const char* flags[] = {"--fullscreen", "-f"};
        size_t j;
        for (j = 0; j < sizeof(flags) / sizeof(flags[0]); ++j) {
            set_env("SNX_WORKFLOW_FLAGS", flags[j]);
            run(exe, repo, dump, log, 0, "G", NULL);
        }
        set_env("SNX_WORKFLOW_FLAGS", "--touch=2");
        run(exe, repo, dump, log, 0, "G", NULL);
        set_env("SNX_WORKFLOW_FLAGS", "--controls=1");
        run(exe, repo, dump, log, 0, "G", NULL);
        set_env("SNX_WORKFLOW_FLAGS", "--resolution=2");
        run(exe, repo, dump, log, 0, "G", NULL);
        set_env("SNX_WORKFLOW_FLAGS", "--seconds=5");
        run(exe, repo, dump, log, 0, "G", NULL);
        /* Debug's SLP vectorizer, which the recompile and the game both take: the game gets it as given, and a
           recompile made for it takes it too */
        set_env("SNX_WORKFLOW_FLAGS", "--slp_vectorizer=1");
        run(exe, repo, dump, log, 0, "G", NULL);
        {
            char* stamp = snx_path_join(repo, "recompiled/Test game/Test game.abi");
            CHECK(!snx_write_file(stamp, "broken", 6));
            run(exe, repo, dump, log, 0, "RG", NULL);
            free(stamp);
        }
        set_env("SNX_WORKFLOW_FLAGS", "--resoltion=2");
        run(exe, repo, dump, log, 1, "", "unknown option: --resoltion=2");
        set_env("SNX_WORKFLOW_FLAGS", "--controls_a=0.5 0.5 100 1");
        run(exe, repo, dump, log, 0, "G", NULL);
        set_env("SNX_WORKFLOW_FLAGS", "--fullscreen=invalid");
        run(exe, repo, dump, log, 1, "", "unknown option");
        set_env("SNX_WORKFLOW_FLAGS", "--fullscreen=0");
        run(exe, repo, dump, log, 0, "G", NULL);
        set_env("SNX_WORKFLOW_FLAGS", "--fullscreen=1");
        run(exe, repo, dump, log, 0, "G", NULL);
        /* LLVM is the only backend: --backend=llvm, which older scripts give, changes nothing */
        set_env("SNX_WORKFLOW_FLAGS", "--backend=gcc");
        run(exe, repo, dump, log, 1, "", "--backend gcc: LLVM is the only backend");
        set_env("SNX_WORKFLOW_FLAGS", "--backend=llvm");
        run(exe, repo, dump, log, 0, "G", NULL);
        set_env("SNX_WORKFLOW_FLAGS", "--backend=invalid");
        run(exe, repo, dump, log, 1, "", "--backend invalid: LLVM is the only backend");
        set_env("SNX_WORKFLOW_FLAGS", "");
    }
    set_env("SNX_WORKFLOW_ARGS", "1");
    run(exe, repo, dump, log, 0, "G", NULL);
    set_env("SNX_WORKFLOW_ARGS", "");
    {
        const char* flags[] = {
            "-r",
            "--recomp",
#ifndef _WIN32
            "-rx",
            "-xr",
#endif
        };
        size_t j;
        for (j = 0; j < sizeof(flags) / sizeof(flags[0]); ++j) {
            set_env("SNX_WORKFLOW_FLAGS", flags[j]);
            reset(repo, dump);
            run(exe, repo, dump, log, 0, "ER", NULL);
            run(exe, repo, dump, log, 0, "R", NULL);
        }
        set_env("SNX_WORKFLOW_FLAGS", "");
    }
    reset(repo, dump);
    set_env("SNX_WORKFLOW_FAIL", "E");
    run(exe, repo, dump, log, 7, "E", "Extraction failed");
    CHECK(snx_is_file(extracting) && !snx_is_file(game));
    set_env("SNX_WORKFLOW_FAIL", "");
    run(exe, repo, dump, log, 0, "ERG", NULL);

    reset(repo, dump);
    set_env("SNX_WORKFLOW_FAIL", "R");
    run(exe, repo, dump, log, 7, "ER", "Recompilation failed");
    CHECK(snx_is_dir(extracted) && !snx_is_file(extracting) && !snx_is_file(game));
    set_env("SNX_WORKFLOW_FAIL", "");
    run(exe, repo, dump, log, 0, "RG", NULL);
#ifndef _WIN32
    for (i = 0; i < sizeof(modes) / sizeof(modes[0]); ++i) {
        reset(repo, dump);
        set_env("SNX_WORKFLOW_MODE", modes[i]);
        set_env("SNX_WORKFLOW_FAIL", "E");
        run(exe, repo, dump, log, statuses[i], "E", "Extraction interrupted by signal");
        CHECK(snx_is_file(extracting) && !snx_is_file(game));
        set_env("SNX_WORKFLOW_FAIL", "R");
        run(exe, repo, dump, log, statuses[i], "ER", "Recompilation interrupted by signal");
        CHECK(snx_is_dir(extracted) && !snx_is_file(extracting) && !snx_is_file(game));
        set_env("SNX_WORKFLOW_FAIL", "");
        run(exe, repo, dump, log, 0, "RG", NULL);
        set_env("SNX_WORKFLOW_FAIL", "G");
        run(exe, repo, dump, log, statuses[i], "G", "Game interrupted by signal");
        set_env("SNX_WORKFLOW_FAIL", "");
        run(exe, repo, dump, log, 0, "G", NULL);
    }
#endif
    reset(repo, dump);
    set_env("SNX_WORKFLOW_MODE", "missing");
    set_env("SNX_WORKFLOW_FAIL", "E");
    run(exe, repo, dump, log, 1, "E", "extraction finished without creating");
    set_env("SNX_WORKFLOW_FAIL", "R");
    run(exe, repo, dump, log, 1, "ER", "recompilation finished without creating");
    set_env("SNX_WORKFLOW_FAIL", "");
    run(exe, repo, dump, log, 0, "RG", NULL);
    run(exe, repo, "/nonexistent/dump.xci", log, 1, "", "/nonexistent/dump.xci is not a file");
    {
        const char* flags[] = {"--headless", "-H"};
        unsigned j;
        for (j = 0; j < 2; ++j) {
            set_env("SNX_WORKFLOW_HEADLESS", flags[j]);
            set_env("SNX_WORKFLOW_PREFIX", "0");
            run(exe, repo, dump, log, 0, "G", NULL);
            set_env("SNX_WORKFLOW_PREFIX", "1");
            reset(repo, dump);
            run(exe, repo, dump, log, 0, "ERG", NULL);
        }
        set_env("SNX_WORKFLOW_HEADLESS", "");
    }
    /* Decrypted containers share the extract/recompile checkpoints, including quoted dump paths. */
    {
        const char* names[] = {"My Switch DXCI's/Test [game];.dnsp", "My Switch DXCI's/Test [game];.dNsP",
                               "My Switch DXCI's/Test [game];.dxci", "My Switch DXCI's/Test [game];.dXcI"};
        unsigned j;
        for (j = 0; j < sizeof names / sizeof names[0]; ++j) {
            char *decrypted = snx_path_join(repo, names[j]), *directory = snx_path_join(repo, "My Switch DXCI's");
            CHECK(!snx_remove_tree(repo) && !snx_mkdirs(directory));
            CHECK(!snx_write_file(decrypted, "synthetic", 9));
            set_env("SNX_WORKFLOW_MODE", "exit");
            set_env("SNX_WORKFLOW_FAIL", "");
            run(exe, repo, decrypted, log, 0, "ERG", NULL);
            CHECK(snx_is_dir(extracted) && snx_is_file(game));
            run(exe, repo, decrypted, log, 0, "G", NULL);
            CHECK(!snx_remove_tree(game));
            run(exe, repo, decrypted, log, 0, "RG", NULL);
            /* -r prepares a decrypted dump too, by its extension */
            CHECK(!snx_remove_tree(extracted) && !snx_remove_tree(game));
            set_env("SNX_WORKFLOW_FLAGS", "-r");
            run(exe, repo, decrypted, log, 0, "ER", NULL);
            set_env("SNX_WORKFLOW_FLAGS", "");
            free(decrypted);
            free(directory);
        }
    }
    /* a game with a title ID of its own is named by it, whatever its dump is called, its recompile given no name; a
       broken NPDM stops the run */
    {
#ifdef _WIN32
        char* named = snx_path_join(repo, "recompiled/0100ABCD12345000/0100ABCD12345000.exe");
#else
        char* named = snx_path_join(repo, "recompiled/0100ABCD12345000/0100ABCD12345000");
#endif
        char* folder = snx_path_join(repo, "recompiled/0100ABCD12345000");
        char* extracted = snx_path_join(folder, "0100ABCD12345000_data");
        reset(repo, dump);
        set_env("SNX_WORKFLOW_MODE", "exit");
        set_env("SNX_WORKFLOW_NPDM", "id");
        set_env("SNX_WORKFLOW_GAMES", folder);
        set_env("SNX_WORKFLOW_SOURCE", extracted); /* given no name: shown by its control.nacp's, not its ID */
        run(exe, repo, dump, log, 0, "ERG", NULL);
        set_env("SNX_WORKFLOW_SOURCE", "");
        free(extracted);
        CHECK(snx_is_file(named) && !snx_is_file(game));
        run(exe, repo, dump, log, 0, "G", NULL);
        { /* made already, recompiled where it is with no name given: shown again by its control.nacp, not its ID */
            char *stamp = snx_format("%s/0100ABCD12345000.abi", folder),
                 *data = snx_format("%s/0100ABCD12345000_data", folder);
            char* icon = snx_format("%s/icon.jpg", data);
            CHECK(!snx_write_file(stamp, "4\n", 2));
            CHECK(!snx_is_file(icon)); /* its extraction kept none: its dump's taken as it plays */
            set_env("SNX_WORKFLOW_SOURCE", data);
            run(exe, repo, named, log, 0, "RG", NULL);
            set_env("SNX_WORKFLOW_SOURCE", "");
            CHECK(snx_is_file(icon));
            free(icon);
            free(data);
            free(stamp);
        }
        reset(repo, dump);
        set_env("SNX_WORKFLOW_NPDM", "bad");
        run(exe, repo, dump, log, 1, "ER", "NPDM: access-control region exceeds file"); /* the recompile reads it */
        CHECK(!snx_is_file(named) && !snx_is_file(game));
        set_env("SNX_WORKFLOW_NPDM", "");
        free(folder);
        folder = snx_path_join(repo, "recompiled/Test game");
        set_env("SNX_WORKFLOW_GAMES", folder);
        free(folder);
        free(named);
    }
    /* Extract to tmpfs: the extraction in TMPDIR/scarletnx/work/<title ID>, the RomFS left there, the game's dump
       recorded in its .ini and its folder marked; a dump played again found by it. Extracted again from the dump once
       tmpfs is emptied, said why not without it, then from the link an older version made, and the RomFS back on disk
       with it off. Windows makes links only with the right to (developer mode), which a runner may not have */
#ifndef _WIN32
    {
        char *folder = snx_path_join(repo, "recompiled/0100ABCD12345000"), *temp = snx_path_join(root, "tmp");
        char *work = snx_path_join(temp, "scarletnx/work/0100ABCD12345000"), *ram = snx_path_join(work, "romfs/data");
        char *disk = snx_format("%s/0100ABCD12345000_data/romfs/data", folder);
        char *link = snx_format("%s/0100ABCD12345000.source", folder);
        char* marker = snx_format("%s/0100ABCD12345000.tmpfs", folder);
        char* ini = snx_game_ini(folder, "0100ABCD12345000");
        CHECK(!snx_mkdirs(temp));
        set_env("TMPDIR", temp);
        reset(repo, dump);
        set_env("SNX_WORKFLOW_MODE", "exit");
        set_env("SNX_WORKFLOW_NPDM", "id");
        set_env("SNX_WORKFLOW_GAMES", folder);
        set_env("SNX_WORKFLOW_FLAGS", "--tmpfs");
        set_env("SNX_WORKFLOW_TMPFS", "1");
        run(exe, repo, dump, log, 0, "ERG", NULL);
        CHECK(snx_is_file(ram) && !snx_is_file(disk) && snx_is_file(marker));
        CHECK(records_dump(folder, "0100ABCD12345000", dump));
        run(exe, repo, dump, log, 0, "G", NULL);
        CHECK(!snx_remove_tree(work)); /* a reboot */
        run(exe, repo, dump, log, 0, "EG", NULL);
        CHECK(snx_is_file(ram));
        run(exe, repo, folder, log, 0, "G", NULL);
        set_env("SNX_WORKFLOW_FLAGS", "");
        run(exe, repo, folder, log, 0, "G", NULL);
        CHECK(snx_is_file(disk) && !snx_is_dir(work) && !snx_is_file(marker));
        set_env("SNX_WORKFLOW_FLAGS", "--tmpfs");
        run(exe, repo, folder, log, 0, "EG", NULL);
        CHECK(snx_is_file(ram) && !snx_is_file(disk) && snx_is_file(marker));
        CHECK(!snx_remove_tree(work) && !snx_remove_tree(ini));
        run(exe, repo, folder, log, 0, "G", "The game cannot be extracted to tmpfs: its .ini names no dump (source=)");
        set_env("SNX_WORKFLOW_FLAGS", "");
        run(exe, repo, folder, log, 0, "G", "names no dump");
        set_env("SNX_WORKFLOW_FLAGS", "--tmpfs");
        CHECK(!snx_symlink(dump, link)); /* an older version's */
        run(exe, repo, folder, log, 0, "EG", NULL);
        CHECK(snx_is_file(ram) && !snx_is_file(ini));
        set_env("SNX_WORKFLOW_OUT", temp);
        run(exe, repo, dump, log, 1, "", "--tmpfs puts the extraction in tmpfs");
        set_env("SNX_WORKFLOW_OUT", "");
        set_env("SNX_WORKFLOW_FLAGS", "");
        set_env("SNX_WORKFLOW_TMPFS", "");
        set_env("SNX_WORKFLOW_NPDM", "");
        free(folder);
        folder = snx_path_join(repo, "recompiled/Test game");
        set_env("SNX_WORKFLOW_GAMES", folder);
        free(folder);
        free(temp);
        free(work);
        free(ram);
        free(disk);
        free(link);
        free(marker);
        free(ini);
    }
#endif
    /* NROs bypass extraction even with a stale same-stem work directory or game executable. */
    {
        const char* names[] = {"Test [game];.nro", "Test [game];.NRO"};
        unsigned j;
        for (j = 0; j < 2; ++j) {
            char* nro = snx_path_join(repo, names[j]);
            reset(repo, nro);
            set_env("SNX_WORKFLOW_MODE", "exit");
            set_env("SNX_WORKFLOW_NRO", nro);
            set_env("SNX_WORKFLOW_HEADLESS", "--headless");
            run(exe, repo, nro, log, 0, "RG", NULL);
            CHECK(!snx_is_dir(kept) && snx_is_file(game));
            set_env("SNX_WORKFLOW_OUT", kept); /* an extraction an NRO does not have */
            run(exe, repo, nro, log, 1, "", "--out cannot go with an NRO: an NRO has no extraction");
            set_env("SNX_WORKFLOW_OUT", "");
            CHECK(!snx_mkdirs(kept));
            run(exe, repo, nro, log, 0, "RG", NULL);
            set_env("SNX_WORKFLOW_FAIL", "R");
            run(exe, repo, nro, log, 7, "R", "Recompilation failed");
            set_env("SNX_WORKFLOW_FAIL", "");
            run(exe, repo, nro, log, 0, "RG", NULL);
            CHECK(!snx_remove_tree(nro));
            {
                char* gone = snx_format("%s is not a file", nro);
                run(exe, repo, nro, log, 1, "", gone);
                free(gone);
            }
            free(nro);
        }
        set_env("SNX_WORKFLOW_NRO", "");
        set_env("SNX_WORKFLOW_HEADLESS", "");
    }
    /* a game made already, named by its program or its folder: played as it is when this build made it, else
       recompiled where it is from its _data (an NRO's from the NRO there), its name kept; -r recompiles it without
       playing. A program an older version laid out beside the others is neither: the driver says what to do */
    {
        char *folder = snx_path_join(repo, "recompiled/Test game"),
             *data = snx_path_join(repo, "recompiled/Test game/Test game_data");
        char *stamp = snx_path_join(repo, "recompiled/Test game/Test game.abi"), *slash = snx_format("%s/", folder);
        char *nro = snx_path_join(data, "exefs/main.nro"), *old = snx_path_join(repo, "recompiled/Old"), *old_data;
#ifdef _WIN32
        char* code = snx_path_join(folder, "Test game_code.dll");
#else
        char* code = snx_path_join(folder, "Test game_code.so");
#endif
        reset(repo, dump);
        run(exe, repo, dump, log, 0, "ERG", NULL);
        set_env("SNX_WORKFLOW_SOURCE", data);
        set_env("SNX_WORKFLOW_NAMED", "Test game"); /* by its extraction's folder: the name kept is given */
        run(exe, repo, game, log, 0, "G", NULL);
        run(exe, repo, folder, log, 0, "G", NULL);
        run(exe, repo, slash, log, 0, "G", NULL);
        CHECK(!snx_write_file(stamp, "4\n", 2)); /* an older version's */
        run(exe, repo, game, log, 0, "RG", NULL);
        run(exe, repo, game, log, 0, "G", NULL);
        CHECK(!snx_write_file(stamp, "4\ndef5678\n", 10)); /* another build's */
        run(exe, repo, folder, log, 0, "RG", NULL);
        CHECK(!snx_remove_tree(code));
        run(exe, repo, game, log, 0, "RG", NULL);
        set_env("SNX_WORKFLOW_FLAGS", "-r");
        run(exe, repo, game, log, 0, "R", NULL);
        run(exe, repo, folder, log, 0, "R", NULL);
#ifndef _WIN32
        set_env("SNX_WORKFLOW_FLAGS", "-rx");
        run(exe, repo, game, log, 1, "", "-rx takes an XCI, NSP, DXCI or DNSP dump");
#endif
        set_env("SNX_WORKFLOW_FLAGS", "--backend=gcc");
        run(exe, repo, game, log, 1, "", "LLVM is the only backend");
        set_env("SNX_WORKFLOW_FLAGS", "");
        set_env("SNX_WORKFLOW_OUTDIR", repo);
        run(exe, repo, game, log, 1, "", "is a game");
        set_env("SNX_WORKFLOW_OUTDIR", "");
        set_env("SNX_WORKFLOW_OUT", kept);
        run(exe, repo, folder, log, 1, "", "is a game");
        set_env("SNX_WORKFLOW_OUT", "");
        CHECK(!snx_mkdirs(data) && !snx_write_file(stamp, "4\n", 2));
        { /* an NRO's game: recompiled from the NRO its data keeps */
            char* exefs = snx_path_join(data, "exefs");
            CHECK(!snx_mkdirs(exefs) && !snx_write_file(nro, "NRO", 3));
            set_env("SNX_WORKFLOW_SOURCE", nro);
            run(exe, repo, game, log, 0, "RG", NULL);
            free(exefs);
        }
        set_env("SNX_WORKFLOW_SOURCE", "");
        set_env("SNX_WORKFLOW_NAMED", "");
        old_data = snx_format("%s_data", old);
        CHECK(!snx_copy_file(game, old) && !snx_mkdirs(old_data));
        run(exe, repo, old, log, 1, "", "laid out by an older version: move its files as docs/DEPRECATIONS.md says");
        set_env("SNX_WORKFLOW_FLAGS", "-r");
        run(exe, repo, old, log, 1, "", "laid out by an older version");
        set_env("SNX_WORKFLOW_FLAGS", "");
        free(old_data);
        free(old);
        free(nro);
        free(code);
        free(slash);
        free(stamp);
        free(data);
        free(folder);
    }
    /* --outdir and --out, as the menu passes the games and the extractions of Settings > App > Folders: the game and
       the extraction go there, not in the data folder, and are used again there */
    {
        char *outdir = snx_path_join(root, "games elsewhere"), *out = snx_path_join(root, "work elsewhere/Test game");
        char *folder = snx_path_join(outdir, "Test game"), *moved;
#ifdef _WIN32
        moved = snx_path_join(folder, "Test game.exe");
#else
        moved = snx_path_join(folder, "Test game");
#endif
        reset(repo, dump);
        set_env("SNX_WORKFLOW_GAMES", folder);
        set_env("SNX_WORKFLOW_OUTDIR", outdir);
        set_env("SNX_WORKFLOW_OUT", out);
        run(exe, repo, dump, log, 0, "ERG", NULL);
        CHECK(snx_is_dir(out) && snx_is_file(moved) && !snx_is_dir(extracted) && !snx_is_file(game));
#ifndef _WIN32
        CHECK(records_dump(folder, "Test game", dump)); /* what an update's RomFS is read over */
#endif
        run(exe, repo, dump, log, 0, "G", NULL);
        set_env("SNX_WORKFLOW_OUT", "");
        set_env("SNX_WORKFLOW_OUTDIR", "none");
        run(exe, repo, dump, log, 1, "", "--outdir requires a directory");
        set_env("SNX_WORKFLOW_OUTDIR", "");
        free(folder);
        folder = snx_path_join(repo, "recompiled/Test game");
        set_env("SNX_WORKFLOW_GAMES", folder);
        free(folder);
        free(moved);
        free(out);
        free(outdir);
    }
    /* Extract dump off, the default: nothing extracted, the game made from its dump read in place and the
       dump recorded in its .ini. Switched on, the dump is extracted into the data folder without a recompile; off again,
       that extraction is taken away. A game made already is recompiled from the dump its .ini names, -r prepares a dump
       without extracting it, and a game whose dump is not there says so */
#ifndef _WIN32
    {
        char* own = snx_path_join(repo, "recompiled/Test game");
        char *stamp = snx_path_join(repo, "recompiled/Test game/Test game.abi"), *moved = snx_format("%s.moved", dump);
        set_env("SNX_WORKFLOW_EXTRACT", "");
        set_env("SNX_WORKFLOW_DUMP", dump);
        set_env("SNX_WORKFLOW_MODE", "exit");
        reset(repo, dump);
        run(exe, repo, dump, log, 0, "RG", NULL);
        CHECK(snx_is_file(game) && !snx_path_exists(extracted) && records_dump(own, "Test game", dump));
        run(exe, repo, dump, log, 0, "G", NULL);
        run(exe, repo, game, log, 0, "G", NULL);
        set_env("SNX_WORKFLOW_EXTRACT", "1");
        run(exe, repo, dump, log, 0, "EG", NULL);
        CHECK(snx_is_dir(extracted) && !snx_is_file(extracting));
        run(exe, repo, game, log, 0, "G", NULL);
        set_env("SNX_WORKFLOW_EXTRACT", "");
        run(exe, repo, dump, log, 0, "G", "the game reads it from its dump");
        CHECK(!snx_path_exists(extracted) && snx_is_file(game));
        CHECK(!snx_write_file(stamp, "4\n", 2)); /* an older version's: recompiled from the dump its .ini names */
        set_env("SNX_WORKFLOW_NAMED", "Test game");
        run(exe, repo, game, log, 0, "RG", NULL);
        set_env("SNX_WORKFLOW_NAMED", "");
        CHECK(!rename(dump, moved));
        run(exe, repo, game, log, 1, "", "is not there");
        CHECK(!rename(moved, dump));
        set_env("SNX_WORKFLOW_FLAGS", "-r");
        reset(repo, dump);
        run(exe, repo, dump, log, 0, "R", NULL);
        CHECK(!snx_path_exists(extracted) && records_dump(own, "Test game", dump));
        run(exe, repo, dump, log, 0, "R", NULL);
        /* -O, played: the recompile's, given when the game is made; a game made already runs as it is */
        set_env("SNX_WORKFLOW_FLAGS", "-O3");
        set_env("SNX_WORKFLOW_OPT", "3");
        reset(repo, dump);
        run(exe, repo, dump, log, 0, "RG", NULL);
        run(exe, repo, dump, log, 0, "G", NULL);
        set_env("SNX_WORKFLOW_OPT", "");
        set_env("SNX_WORKFLOW_FLAGS", "-O9");
        run(exe, repo, dump, log, 1, "", "-O 9: not 0, 1, 2, 3, s or z");
        set_env("SNX_WORKFLOW_FLAGS", "");
        set_env("SNX_WORKFLOW_DUMP", "");
        free(own);
        free(stamp);
        free(moved);
    }
#endif
    /* Updates and DLCs: an update's dump made into its own program beside the game made already, a DLC's recorded with
       no program; each checked in the game's addons=, an update over the one checked, and the game played with them,
       from the checked update's program. Refused before the game is there; given with the game's dump as --update,
       one or several. A DLC program an older version made is taken away. Extract dump extracts the update
       and the DLCs checked into their data folders, and off again takes them away */
#ifndef _WIN32
    {
        char *game_dump = snx_path_join(repo, "Game.nsp"), *update = snx_path_join(repo, "Game Update.nsp");
        char *dlc = snx_path_join(repo, "Game Dlc.nsp"), *folder = snx_path_join(repo, "recompiled/0100ABCD12340000");
        char *image = snx_path_join(folder, "0100ABCD12340000_0100ABCD5555A001_data/system/archives/"
                                            "0100ABCD5555A001.romfs");
        char *own = snx_path_join(folder, "0100ABCD12340000_0100ABCD12340800v65536");
        char *old_dlc = snx_path_join(folder, "0100ABCD12340000_0100ABCD5555A001"), *both, *ini, *checked;
        set_env("SNX_WORKFLOW_ADDON", "1");
        set_env("SNX_WORKFLOW_GAMES", folder);
        set_env("SNX_WORKFLOW_EXTRACT", "");
        reset(repo, dump);
        CHECK(!snx_write_file(game_dump, "game", 4) && !snx_write_file(update, "update", 6) &&
              !snx_write_file(dlc, "dlc", 3));
        run(exe, repo, update, log, 1, "", "which is not recompiled yet: play the game's dump first");
        set_env("SNX_WORKFLOW_UPDATE", update);
        run(exe, repo, game_dump, log, 0, "RRA", NULL);
        set_env("SNX_WORKFLOW_UPDATE", "");
        CHECK(snx_is_file(own) && records_dump(folder, "0100ABCD12340000_0100ABCD12340800v65536", update));
        CHECK(records_dump(folder, "0100ABCD12340000", game_dump));
        ini = snx_game_ini(folder, "0100ABCD12340000");
        checked = snx_ini_value(ini, "addons");
        CHECK(checked && !strcmp(checked, "0100ABCD12340800v65536"));
        free(checked);
        run(exe, repo, update, log, 0, "A", NULL);
        run(exe, repo, own, log, 0, "A", NULL);
        run(exe, repo, game_dump, log, 0, "A", NULL); /* the game, played with its checked update */
        /* a DLC program an older version made, taken away as its dump is played: the game played with it */
        CHECK(!snx_copy_file(exe, old_dlc));
        { /* --out, which the menu passes with Extract dump: a DLC's goes into its own data folder */
            char* out = snx_path_join(root, "work elsewhere/Dlc");
            set_env("SNX_WORKFLOW_OUT", out);
            run(exe, repo, dlc, log, 0, "A", "a DLC has no program, it plays with its game");
            set_env("SNX_WORKFLOW_OUT", "");
            CHECK(!snx_path_exists(out));
            free(out);
        }
        CHECK(!snx_path_exists(old_dlc) && records_dump(folder, "0100ABCD12340000_0100ABCD5555A001", dlc));
        checked = snx_ini_value(ini, "addons");
        CHECK(checked && !strcmp(checked, "0100ABCD12340800v65536,0100ABCD5555A001") && !snx_path_exists(image));
        free(checked);
        set_env("SNX_WORKFLOW_EXTRACT", "1");
        run(exe, repo, game_dump, log, 0, "EEEA", NULL); /* the game's extraction, the update's, then the DLC's */
        CHECK(snx_is_file(image));
        set_env("SNX_WORKFLOW_EXTRACT", "");
        run(exe, repo, own, log, 0, "A", "the DLC reads it from its dump");
        {
            char* game_exefs = snx_path_join(folder, "0100ABCD12340000_data/exefs");
            CHECK(!snx_path_exists(image) && !snx_path_exists(game_exefs));
            free(game_exefs);
        }
        /* with nothing checked, the game's own program plays; both given again, both checked */
        CHECK(!snx_ini_record(ini, "addons", ""));
        run(exe, repo, game_dump, log, 0, "G", NULL);
        both = snx_format("%s|%s", update, dlc);
        set_env("SNX_WORKFLOW_UPDATE", both);
        run(exe, repo, game_dump, log, 0, "A", NULL);
        checked = snx_ini_value(ini, "addons");
        CHECK(checked && !strcmp(checked, "0100ABCD12340800v65536,0100ABCD5555A001"));
        free(checked);
        set_env("SNX_WORKFLOW_UPDATE", "");
        {   /* an older update installed: made into its own program beside the newer one's, and picked in its place,
               the DLCs checked kept; the newer one played again picked back */
            char *old_update = snx_path_join(repo, "Game Update old.nsp");
            char *old_program = snx_path_join(folder, "0100ABCD12340000_0100ABCD12340800v0");
            CHECK(!snx_write_file(old_update, "old update", 10));
            run(exe, repo, old_update, log, 0, "RA", NULL);
            checked = snx_ini_value(ini, "addons");
            CHECK(checked && !strcmp(checked, "0100ABCD12340800v0,0100ABCD5555A001"));
            free(checked);
            CHECK(snx_is_file(old_program) && snx_is_file(own));
            run(exe, repo, update, log, 0, "A", NULL);
            checked = snx_ini_value(ini, "addons");
            CHECK(checked && !strcmp(checked, "0100ABCD12340800v65536,0100ABCD5555A001"));
            free(checked);
            free(old_update);
            free(old_program);
        }
        set_env("SNX_WORKFLOW_UPDATE", dlc); /* given with another game: that game made, the DLC refused */
        set_env("SNX_WORKFLOW_ADDON", "");
        set_env("SNX_WORKFLOW_DUMP", dump);
        set_env("SNX_WORKFLOW_GAMES", "");
        run(exe, repo, dump, log, 1, "R", "is not an update or a DLC of");
        set_env("SNX_WORKFLOW_DUMP", "");
        set_env("SNX_WORKFLOW_UPDATE", "");
        cwd = snx_path_join(repo, "recompiled/Test game");
        set_env("SNX_WORKFLOW_GAMES", cwd);
        free(cwd);
        free(both);
        free(ini);
        free(old_dlc);
        free(game_dump);
        free(update);
        free(dlc);
        free(folder);
        free(image);
        free(own);
    }
#endif
    /* The probe takes a third argument, so use a separate, terminated vector. */
    {
        const char* probe[4];
        probe[0] = exe;
        probe[1] = "--probe";
        probe[2] = "cwd";
        probe[3] = NULL;
        output = NULL;
        rc = snx_run(probe, NULL, NULL, &output);
        CHECK(rc != 0 && strstr(output, "Probe"));
#ifndef _WIN32
        CHECK(strstr(output, "chdir to the child's working directory"));
#endif
        free(output);
        probe[2] = "exec";
        output = NULL;
        rc = snx_run(probe, NULL, NULL, &output);
        CHECK(rc != 0 && strstr(output, "Probe"));
#ifndef _WIN32
        CHECK(strstr(output, "executing child process"));
#endif
        free(output);
    }
#ifndef _WIN32
    /* --setup goes through the setup in the terminal, here a stdin with nothing to read: it stops at the first
       question, writing nothing; --skip alone is refused; --debug runs with no setup, in a home with no setup.ini */
    {
        const char* setup[4];
        char* home = snx_path_join(root, "setup home");
        set_env("XDG_DATA_HOME", home);
        set_env("XDG_STATE_HOME", home);
        set_env("XDG_CACHE_HOME", home);
        CHECK(freopen("/dev/null", "r", stdin) != NULL); /* the children's */
        setup[0] = exe;
        setup[1] = "--setup";
        setup[2] = NULL;
        output = NULL;
        rc = snx_run(setup, NULL, home, &output);
        CHECK(rc == 1 && strstr(output, "Changed in older versions\n") && strstr(output, "no more answers"));
        free(output);
        setup[1] = "--skip";
        setup[2] = dump;
        setup[3] = NULL;
        output = NULL;
        CHECK(snx_run(setup, NULL, home, &output) == 2 && strstr(output, "--skip --skip"));
        free(output);
        setup[1] = "--debug";
        setup[2] = "/nonexistent/dump.nsp";
        output = NULL;
        CHECK(snx_run(setup, NULL, home, &output) == 1 && strstr(output, "/nonexistent/dump.nsp is not a file") &&
              !strstr(output, "Changed"));
        free(output);
        free(home);
    }
#endif
    set_env("SNX_WORKFLOW_KEYS", "");
    set_env("SNX_WORKFLOW_TITLE_KEYS", "");
    CHECK(!snx_remove_tree(root));
    free(exe);
    free(root);
    free(repo);
    free(dump);
    free(log);
    free(extracted);
    free(extracting);
    free(kept);
    free(game);
    free(keys);
    free(title_keys);
    printf("Driver workflow: %u checks passed\n", checks);
    return 0;
}
