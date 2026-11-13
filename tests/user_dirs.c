/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* snx_user_dir and snx_user_path: an installed build's config, state and cache folders (the XDG Base Directory spec),
   a portable copy's (config/, state/ and cache/ of its .scarletnx), and where a file of a data folder goes in them;
   snx_menu_dir: the launcher's own, for it and for a game of its; the logs and caches Settings > App > Folders picks
   in app.ini. tests/data_dir.c has the data folder itself. */
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE   700 /* realpath */
#include "common/utilities/common.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char* join(const char* a, const char* b) {
    char* p = snx_path_join(a, b);
    assert(p);
    return p;
}
static void expect_dir(int kind, const char* expected) {
    char* dir = snx_user_dir(kind);
    assert(expected ? dir && !strcmp(dir, expected) : !dir);
    free(dir);
}
static void expect_path(const char* path, int kind, const char* expected) {
    char* moved = snx_user_path(path, kind);
    assert(moved && !strcmp(moved, expected));
    free(moved);
}
static void expect_menu(const char* folder, int kind, const char* expected) {
    char* dir = snx_menu_dir(folder, kind);
    assert(dir && !strcmp(dir, expected));
    free(dir);
}
/* what snx_error said last, and how many times */
static struct {
    char said[2048];
    int says;
} g_test;
static void error_line(const char* line) {
    snprintf(g_test.said, sizeof g_test.said, "%s", line);
    ++g_test.says;
}
static void write_text(const char* path, const char* text) { assert(!snx_write_file(path, text, strlen(text))); }

int main(void) {
    static const char* const vars[] = {"XDG_DATA_HOME", "XDG_CONFIG_HOME", "XDG_STATE_HOME", "XDG_CACHE_HOME"};
    char *root = snx_temp_dir(NULL), *home, *dir[4], *base, *games, *path;
    int kind;
    assert(root);
    /* the folder as the functions name it, links resolved: macOS's temporary folder is under /var, a link to
       /private/var */
    {
        char* real = realpath(root, NULL);
        assert(real);
        free(root);
        root = real;
    }
    home = join(root, "home");
    setenv("HOME", home, 1);
    for (kind = SNX_USER_DATA; kind <= SNX_USER_CACHE; ++kind) unsetenv(vars[kind]);
#ifdef SNX_XDG
    {
        static const char* const defaults[] = {".local/share", ".config", ".local/state", ".cache"};
        char* expected;
        for (kind = SNX_USER_DATA; kind <= SNX_USER_CACHE; ++kind) {
            /* unset, empty or relative: the spec's default under HOME */
            base = join(home, defaults[kind]);
            expected = join(base, "scarletnx");
            expect_dir(kind, expected);
            setenv(vars[kind], "", 1);
            expect_dir(kind, expected);
            setenv(vars[kind], "relative/folder", 1);
            expect_dir(kind, expected);
            free(expected);
            free(base);
            /* without a home only an absolute variable names the folder */
            unsetenv("HOME");
            expect_dir(kind, NULL);
            base = join(root, vars[kind]);
            expected = join(base, "scarletnx");
            setenv(vars[kind], base, 1);
            expect_dir(kind, expected);
            setenv("HOME", home, 1);
            expect_dir(kind, expected);
            free(expected);
            free(base);
        }
    }
#else
    /* config/, state/ and cache/ of the data folder where the spec does not apply */
    {
        static const char* const names[] = {NULL, "config", "state", "cache"};
        base = snx_user_data_dir();
        assert(base);
        expect_dir(SNX_USER_DATA, base);
        for (kind = SNX_USER_CONFIG; kind <= SNX_USER_CACHE; ++kind) {
            path = join(base, names[kind]);
            expect_dir(kind, path);
            free(path);
        }
        free(base);
    }
#endif

    for (kind = SNX_USER_DATA; kind <= SNX_USER_CACHE; ++kind) {
        base = join(root, vars[kind]);
        setenv(vars[kind], base, 1);
        dir[kind] = snx_user_dir(kind);
        assert(dir[kind]);
        free(base);
    }
    /* one separator in a path made here, the system's: on Windows a / given becomes \, as every folder shown is; a
       path made in a buffer it does not fit leaves it empty, said with its length and the room */
    {
        char *joined = snx_path_join("top/a", "b/c"), formatted[16], small[8];
        assert(!snx_path_format(formatted, sizeof formatted, "%s/%s.ini", "top/a", "G"));
#ifdef _WIN32
        assert(!strcmp(joined, "top\\a\\b\\c") && !strcmp(formatted, "top\\a\\G.ini"));
        for (kind = SNX_USER_DATA; kind <= SNX_USER_CACHE; ++kind) assert(!strchr(dir[kind], '/'));
#else
        assert(!strcmp(joined, "top/a/b/c") && !strcmp(formatted, "top/a/G.ini"));
#endif
        free(joined);
        snx_error_sink(error_line);
        g_test.says = 0;
        assert(snx_path_format(small, sizeof small, "%s/%s", "top", "name") == -1 && !small[0]);
        assert(g_test.says == 1 && !strcmp(g_test.said, "the path top/nam... is 8 bytes, over the 7 it has room for"));
        g_test.says = 0;
        snx_error_sink(NULL);
    }
    games = join(dir[SNX_USER_DATA], "games");
    assert(!snx_mkdirs(games));
#ifdef SNX_XDG
    {
        char *expected, *link, *cwd;
        /* the installed games/: each kind in its folder, under the same name */
        path = join(dir[SNX_USER_CONFIG], "games");
        expect_path(games, SNX_USER_CONFIG, path);
        free(path);
        path = join(games, "Game/Game.ini"); /* a game's folder */
        expected = join(dir[SNX_USER_CONFIG], "games/Game/Game.ini");
        expect_path(path, SNX_USER_CONFIG, expected);
        free(expected);
        free(path);
        path = join(dir[SNX_USER_STATE], "games");
        expect_path(games, SNX_USER_STATE, path);
        free(path);
        path = join(dir[SNX_USER_DATA], "work");
        expected = join(dir[SNX_USER_CACHE], "work");
        expect_path(path, SNX_USER_CACHE, expected);
        free(expected);
        free(path);
        expect_path(dir[SNX_USER_DATA], SNX_USER_CACHE, dir[SNX_USER_CACHE]);
        /* a trailing separator, and the folder reached through a link */
        path = snx_format("%s/", games);
        expected = join(dir[SNX_USER_STATE], "games");
        expect_path(path, SNX_USER_STATE, expected);
        free(path);
        link = join(root, "link");
        assert(!symlink(dir[SNX_USER_DATA], link));
        path = join(link, "games");
        expect_path(path, SNX_USER_STATE, expected);
        free(path);
        free(link);
        /* the runner started in games/ as ./Game */
        cwd = getcwd(NULL, 0);
        assert(cwd && !chdir(games));
        expect_path("./", SNX_USER_STATE, expected);
        expect_path(".", SNX_USER_STATE, expected);
        assert(!chdir(cwd));
        free(cwd);
        free(expected);
    }
#endif
    /* data stays; a checkout's or an --outdir's games/, or an older version's portable copy's, keep everything beside
       the game */
    expect_path(games, SNX_USER_DATA, games);
    path = join(root, "portable/games");
    expect_path(path, SNX_USER_CONFIG, path);
    expect_path(path, SNX_USER_CACHE, path);
    free(path);
    /* a portable copy's .scarletnx: its config/, state/ and cache/, on every system */
    {
        char *expected, *copy = join(root, "portable/.scarletnx");
        path = join(copy, "games/Game");
        expected = join(copy, "config/games/Game");
        expect_path(path, SNX_USER_CONFIG, expected);
        free(expected);
        expected = join(copy, "cache/work");
        free(path);
        path = join(copy, "work");
        expect_path(path, SNX_USER_CACHE, expected);
        expect_path(path, SNX_USER_DATA, path);
        free(expected);
        free(path);
        expected = join(copy, "state");
        expect_path(copy, SNX_USER_STATE, expected);
        assert(snx_app_dir(copy) && !snx_app_dir(root));
        free(expected);
        path = join(root, "portable/.scarletnx-other/games"); /* only that name */
        expect_path(path, SNX_USER_STATE, path);
        free(path);
        /* the launcher's settings.ini and gamecontrollerdb.txt: its config/, for it and for a game in its games/ */
        expected = join(copy, "config");
        expect_menu(copy, SNX_USER_CONFIG, expected);
        path = join(copy, "games/0100ABCD12345000/");
        expect_menu(path, SNX_USER_CONFIG, expected);
        free(path);
        free(expected);
        free(copy);
    }
    /* the user's data folder's, and any other folder's (a checkout, an --outdir, a folder of games the player added):
       the user's config folder */
    {
        char* user = snx_user_dir(SNX_USER_CONFIG);
        assert(user);
        expect_menu(games, SNX_USER_CONFIG, user);
        path = join(root, "portable/games/0100ABCD12345000");
        expect_menu(path, SNX_USER_CONFIG, user);
        free(path);
        expect_menu(NULL, SNX_USER_CONFIG, user);
        free(user);
    }
    path = snx_format("%s-other/games", dir[SNX_USER_DATA]);
    expect_path(path, SNX_USER_STATE, path);
    free(path);
    expect_path("games", SNX_USER_CONFIG, "games");
    /* Logs and Caches picked: [folders] state = and cache = of the user's app.ini, for the launcher's own files and its
       games' files of those kinds; the config folder, which holds app.ini, stays */
    {
        char *config = snx_user_dir(SNX_USER_CONFIG), *ini, *logs = join(root, "picked logs"),
             *caches = join(root, "picked caches"), *text, *expected, *fallback;
        assert(config && !snx_mkdirs(config));
        ini = join(config, "app.ini");
        text = snx_format("[app]\ntheme = yorha\n[folders]\ndata = \nstate = %s\ncache = %s\n", logs, caches);
        write_text(ini, text);
        free(text);
        expect_menu(NULL, SNX_USER_STATE, logs);
        expect_menu(games, SNX_USER_CACHE, caches);
        expect_menu(games, SNX_USER_CONFIG, config);
        expect_menu(NULL, SNX_USER_DATA, dir[SNX_USER_DATA]);
        path = join(games, "Game/Game.log");
        expected = join(logs, "games/Game/Game.log");
        expect_path(path, SNX_USER_STATE, expected);
        free(expected);
        free(path);
        path = join(dir[SNX_USER_DATA], "work"); /* the extractions' default goes with them */
        expected = join(caches, "work");
        expect_path(path, SNX_USER_CACHE, expected);
        free(expected);
        free(path);
        /* the folder before Folders picked one, which the launcher shows as the default */
        fallback = snx_menu_kind_default(games, SNX_USER_STATE);
        assert(fallback && !strcmp(fallback, dir[SNX_USER_STATE]));
        free(fallback);
        fallback = snx_app_kind_default(dir[SNX_USER_DATA], SNX_USER_CACHE);
        assert(fallback && !strcmp(fallback, dir[SNX_USER_CACHE]));
        free(fallback);
        /* none, or an empty one: the default; one that is not a full path too, said once with the folder in use */
        write_text(ini, "[folders]\nstate = \n");
        expect_menu(NULL, SNX_USER_STATE, dir[SNX_USER_STATE]);
        expect_menu(NULL, SNX_USER_CACHE, dir[SNX_USER_CACHE]);
        snx_error_sink(error_line);
        write_text(ini, "[folders]\ncache = relative/caches\n");
        expect_menu(NULL, SNX_USER_CACHE, dir[SNX_USER_CACHE]);
        expect_menu(games, SNX_USER_CACHE, dir[SNX_USER_CACHE]);
        expected = snx_format("%s: [folders] cache = relative/caches is not a full path: the caches go in %s", ini,
                              dir[SNX_USER_CACHE]);
        assert(g_test.says == 1 && !strcmp(g_test.said, expected));
        free(expected);
        snx_error_sink(NULL);
        /* the last one, as the launcher reads it */
        text = snx_format("[folders]\nstate = /an older pick\nstate = %s\n", logs);
        write_text(ini, text);
        free(text);
        expect_menu(NULL, SNX_USER_STATE, logs);
        assert(!remove(ini));
        /* a portable copy's own app.ini, in its config/ */
        {
            char *copy = join(root, "portable/.scarletnx"), *own = join(copy, "config"),
                 *own_ini = join(own, "app.ini");
            assert(!snx_mkdirs(own));
            text = snx_format("[folders]\ncache = %s\n", caches);
            write_text(own_ini, text);
            free(text);
            path = join(copy, "games/Game");
            expect_menu(path, SNX_USER_CACHE, caches);
            expected = join(caches, "games/Game");
            expect_path(path, SNX_USER_CACHE, expected);
            free(expected);
            expected = join(copy, "state/games/Game");
            expect_path(path, SNX_USER_STATE, expected);
            free(expected);
            free(path);
            expect_menu(NULL, SNX_USER_CACHE, dir[SNX_USER_CACHE]); /* the user's own: none picked */
            assert(!remove(own_ini));
            free(own_ini);
            free(own);
            free(copy);
        }
        free(ini);
        free(logs);
        free(caches);
        free(config);
    }
#ifndef _WIN32
    /* a user data folder this process cannot resolve (here through a link to itself, which realpath refuses with
       ELOOP even as root; to a shell run as the Android app, /sdcard) is no folder's: snx_app_dir holds for no other
       folder, and a portable copy's game still finds its .ini in the copy's config/ */
    {
        char *loop = join(root, "loop"), *unresolved = join(loop, "home"), *copy = join(root, "unresolved/.scarletnx");
        char *game = join(copy, "recompiled/0100ABCD12345000"),
             *expected = join(copy, "config/recompiled/0100ABCD12345000");
        char *was_home = snx_strdup(getenv("HOME")),
             *was_data = getenv("XDG_DATA_HOME") ? snx_strdup(getenv("XDG_DATA_HOME")) : NULL;
        assert(!symlink(loop, loop) && !snx_mkdirs(game));
        setenv("HOME", unresolved, 1);
        unsetenv("XDG_DATA_HOME");
        path = snx_user_data_dir();
        assert(path && !snx_absolute(path)); /* the case: a data folder named, not resolved */
        free(path);
        assert(!snx_app_dir(root) && !snx_app_dir(game) && snx_app_dir(copy));
        expect_path(game, SNX_USER_CONFIG, expected);
        setenv("HOME", was_home, 1);
        if (was_data) setenv("XDG_DATA_HOME", was_data, 1);
        assert(!unlink(loop));
        free(was_home);
        free(was_data);
        free(expected);
        free(game);
        free(copy);
        free(unresolved);
        free(loop);
    }
#endif
#ifdef SNX_XDG /* elsewhere the config folder is always the data folder's config/ */
    /* a config folder that is the data folder divides nothing */
    setenv("XDG_CONFIG_HOME", getenv("XDG_DATA_HOME"), 1);
    expect_path(games, SNX_USER_CONFIG, games);
#endif

    puts("snx_user_dir: the XDG variables, their defaults, a relative path ignored; snx_user_path: the installed "
         "and a portable copy's games/ divided, any other folder kept; snx_menu_dir: the launcher's for its games; "
         "Logs and Caches picked in app.ini, a wrong one said once");
    for (kind = SNX_USER_DATA; kind <= SNX_USER_CACHE; ++kind) free(dir[kind]);
    free(games);
    free(home);
    assert(!snx_remove_tree(root));
    free(root);
    return 0;
}
