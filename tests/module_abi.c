/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The runtime's check of a game module's version (runtime/main.c), on modules that are a descriptor alone
   (module_abi_fixture.c): one of an ABI from SNX_ABI_OLDEST to SNX_ABI is taken, and the run goes on to load the game,
   whose program the fixture's data folder doesn't hold; one outside them, or a 32-bit one of another SNX_ARM_MODULE_ABI,
   is refused before the game's settings or log are written, the message saying its ABI and the ones this runtime runs.
     runtime_module_abi SCARLETNX MODULE ARM_MODULE */
#define _POSIX_C_SOURCE 200809L
#include "common/utilities/common.h"
#include "nx/loader/module_abi.h"
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define MODULE_SUFFIX "_code.dll"
#else
#define MODULE_SUFFIX "_code.so"
#endif

static int failures;
static void check(int ok, const char* what, const char* output) {
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n%s\n", what, output ? output : "");
        ++failures;
    }
}
static void set_env(const char* name, const char* value) {
#ifdef _WIN32
    check(!_putenv_s(name, value), name, NULL);
#else
    check(!setenv(name, value, 1), name, NULL);
#endif
}

/* the game name, in its folder of root (root/name/name, as the driver lays games out): the path of its file of that
   suffix ("_code.so", ".folders", "_data"), owned */
static char* game_file(const char* root, const char* name, const char* suffix) {
    char *folder = snx_path_join(root, name), *game = snx_path_join(folder, name), *path;
    path = snx_format("%s%s", game, suffix);
    free(game);
    free(folder);
    return path;
}
/* 1 when the run whose output is given was refused with expected in its FATAL line, and the game's code named in a
   line of its own: a fatal's text, which reports are grouped by, has no path */
static int refused(const char* output, const char* expected, const char* root, const char* name) {
    const char* fatal = strstr(output, "FATAL: ");
    char line[512] = {0}, *code = game_file(root, name, MODULE_SUFFIX), *named = snx_format("its code is %s", code);
    int good;
    if (fatal) snprintf(line, sizeof line, "%.*s", (int)strcspn(fatal, "\r\n"), fatal);
    good = fatal && strstr(line, expected) && !strstr(line, root) && strstr(output, named);
    free(named);
    free(code);
    return good;
}
/* the game name in its folder of root, with module as its code, an empty data folder, and its settings, log and
   caches in folders of its own (name.folders): the runtime's exit status playing it with the module's ABI abi, and its
   output. The folder is left for the caller to look in */
static int play(const char* runtime, const char* root, const char* name, const char* module, unsigned long abi,
                char** output) {
    static const char* const kinds[] = {"config", "state", "cache"};
    char *folder = snx_path_join(root, name), *game = game_file(root, name, ""),
         *code = game_file(root, name, MODULE_SUFFIX);
    char *data = game_file(root, name, "_data"), *record = game_file(root, name, ".folders"), *text = snx_strdup("");
    char *option = snx_format("--game=%s", game), *value = snx_format("%lu", abi);
    const char* args[4];
    size_t i;
    int rc;
    check(!snx_mkdirs(data) && !snx_copy_file(module, code), "the game's code and data folder", NULL);
    for (i = 0; i < sizeof kinds / sizeof *kinds; ++i) {
        char *kind = snx_path_join(folder, kinds[i]), *line = snx_format("%s%s=%s\n", text, kinds[i], kind);
        check(!snx_mkdirs(kind), kinds[i], NULL);
        free(text);
        text = line;
        free(kind);
    }
    check(!snx_write_file(record, text, strlen(text)), "the game's folders recorded", NULL);
    set_env("SNX_TEST_MODULE_ABI", value);
    args[0] = runtime;
    args[1] = option;
    args[2] = "-H"; /* no window, and no box for the error: it is on stderr */
    args[3] = NULL;
    rc = snx_run(args, folder, NULL, output);
    free(value);
    free(option);
    free(text);
    free(record);
    free(data);
    free(code);
    free(game);
    free(folder);
    return rc;
}
/* 1 when the game name played in its folder of root wrote its file of that kind there: its settings ("config", ".ini")
   or its log ("state", ".log") */
static int wrote(const char* root, const char* name, const char* kind, const char* extension) {
    char* path = snx_format("%s/%s/%s/%s%s", root, name, kind, name, extension);
    int found = snx_is_file(path);
    free(path);
    return found;
}

int main(int argc, char** argv) {
    static const char* const homes[] = {"HOME",           "XDG_DATA_HOME", "XDG_CONFIG_HOME", "XDG_STATE_HOME",
                                        "XDG_CACHE_HOME", "APPDATA",       "LOCALAPPDATA"};
    char *root, *output = NULL, *expected;
    size_t i;
    int rc;
    if (argc != 4) {
        fprintf(stderr, "usage: %s SCARLETNX MODULE ARM_MODULE\n", argv[0]);
        return 2;
    }
    root = snx_temp_dir(NULL);
    if (!root) {
        fprintf(stderr, "FAIL: no temporary folder\n");
        return 1;
    }
    /* the user's folders, the runtime's data among them, in the temporary one: nothing of the person running it */
    for (i = 0; i < sizeof homes / sizeof *homes; ++i) {
        char* home = snx_format("%s/%s", root, homes[i]);
        check(!snx_mkdirs(home), homes[i], NULL);
        set_env(homes[i], home);
        free(home);
    }

    /* the oldest ABI this runtime runs, and its own: taken, the run stopping where the game's program is missing */
    rc = play(argv[1], root, "oldest", argv[2], SNX_ABI_OLDEST, &output);
    check(rc != 0 && strstr(output, "The game's module main is missing") &&
              strstr(output, "Could not load the game executables") && !strstr(output, "another ScarletNX version"),
          "a module of SNX_ABI_OLDEST is taken", output);
    check(wrote(root, "oldest", "config", ".ini") && wrote(root, "oldest", "state", ".log"),
          "a module taken runs the game, which writes its settings and log", output);
    free(output);
    rc = play(argv[1], root, "current", argv[2], SNX_ABI, &output);
    check(rc != 0 && strstr(output, "The game's module main is missing") &&
              !strstr(output, "another ScarletNX version"),
          "a module of SNX_ABI is taken", output);
    free(output);

    /* older than the oldest, newer than this runtime: refused, saying the module's ABI and the ones it runs */
    rc = play(argv[1], root, "older", argv[2], SNX_ABI_OLDEST - 1, &output);
    expected = snx_format("from another ScarletNX version (ABI %u, where this one runs %u to %u): recompile the game",
                          (unsigned)SNX_ABI_OLDEST - 1, (unsigned)SNX_ABI_OLDEST, (unsigned)SNX_ABI);
    check(rc != 0 && refused(output, expected, root, "older") && !strstr(output, "The game's module main is missing"),
          "a module older than SNX_ABI_OLDEST is refused, saying its ABI", output);
    check(!wrote(root, "older", "config", ".ini") && !wrote(root, "older", "state", ".log"),
          "a module refused leaves no settings and no log", output);
    free(expected);
    free(output);
    rc = play(argv[1], root, "newer", argv[2], SNX_ABI + 1, &output);
    expected = snx_format("(ABI %u, where this one runs %u to %u)", (unsigned)SNX_ABI + 1, (unsigned)SNX_ABI_OLDEST,
                          (unsigned)SNX_ABI);
    check(rc != 0 && refused(output, expected, root, "newer"), "a module newer than SNX_ABI is refused, saying its ABI",
          output);
    free(expected);
    free(output);

    /* a 32-bit module has its own version, SNX_ABI's range left alone: another one is refused, saying both */
    rc = play(argv[1], root, "arm", argv[3], SNX_ARM_MODULE_ABI + 1, &output);
    expected = snx_format("from another ScarletNX version (32-bit ABI %08x, where this one runs %08x)",
                          (unsigned)SNX_ARM_MODULE_ABI + 1, (unsigned)SNX_ARM_MODULE_ABI);
    check(rc != 0 && refused(output, expected, root, "arm"),
          "a 32-bit module of another version is refused, saying its ABI", output);
    free(expected);
    free(output);

    check(!snx_remove_tree(root), "temporary folder removed", NULL);
    free(root);
    if (failures) return 1;
    printf("runtime_module_abi: ok\n");
    return 0;
}
