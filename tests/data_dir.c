/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* snx_data_dir, the folder the driver and the menu share: a portable copy's (.scarletnx beside the AppImage or the
   program), else the user's data folder, for a build tree's programs too. snx_checkout, the checkout of a build tree's
   program, and what an older version kept in it, said with where it goes now. */
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE   700 /* realpath */
#include "common/utilities/common.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

static char* join(const char* a, const char* b) {
    char* p = snx_path_join(a, b);
    assert(p);
    return p;
}

/* what snx_checkout_left says on stderr, through a file */
static char* left_said(const char* checkout, const char* data, const char* file) {
    char* text;
    size_t size;
    fflush(stderr);
    assert(freopen(file, "w", stderr));
    snx_checkout_left(checkout, data);
    fflush(stderr);
    text = (char*)snx_read_file(file, &size);
    assert(text);
    return text;
}
/* what snx_recompiled_left says on stderr for data, read back from file */
static char* recompiled_said(const char* data, const char* file) {
    char* text;
    size_t size;
    fflush(stderr);
    assert(freopen(file, "w", stderr));
    snx_recompiled_left(data);
    fflush(stderr);
    text = (char*)snx_read_file(file, &size);
    assert(text);
    return text;
}

int main(void) {
    char *root = snx_temp_dir(NULL), *checkout, *build, *built, *program, *mount, *copy, *data, *xdg, *expected, *file;
    char *portable, *beside, *said, *line, *cache, *old, *now;
    int i;
    assert(root);
    /* the folder as the functions name it, links resolved: macOS's temporary folder is under /var, a link to
       /private/var */
    {
        char* real = realpath(root, NULL);
        assert(real);
        free(root);
        root = real;
    }
    checkout = join(root, "checkout");
    build = join(root, "build");
    built = join(build, "src/gui");
    program = join(root, "program");
    mount = join(root, "mount");
    copy = join(root, "copy");
    xdg = join(root, "xdg");
    cache = join(root, "xdg-cache");
    assert(!snx_mkdirs(checkout) && !snx_mkdirs(built) && !snx_mkdirs(program) && !snx_mkdirs(mount) &&
           !snx_mkdirs(copy));
    file = join(checkout, "CMakeLists.txt");
    assert(!snx_write_file(file, "", 0));
    free(file);
    unsetenv("APPIMAGE");
    unsetenv("APPDIR");
    setenv("XDG_DATA_HOME", xdg, 1);
    setenv("XDG_CACHE_HOME", cache, 1);
#ifdef SNX_XDG
    expected = join(xdg, "scarletnx");
#else
    /* no XDG spec (macOS): the user's data folder of its own, under a HOME of the test's (tests/user_dirs.c has it) */
    file = join(root, "home");
    setenv("HOME", file, 1);
    free(file);
    expected = snx_user_data_dir();
    assert(expected);
#endif

    /* a build tree's program works in the user's data folder too, its checkout there or not */
    data = snx_data_dir(built);
    assert(data && !strcmp(data, expected));
    free(data);
    data = snx_data_dir(program);
    assert(data && !strcmp(data, expected));
    free(data);
    data = snx_data_dir(NULL);
    assert(data && !strcmp(data, expected));
    free(data);
    /* a games/ beside the program is an older version's portable copy: not one now */
    file = join(program, "games");
    assert(!snx_mkdirs(file));
    free(file);
    data = snx_data_dir(program);
    assert(data && !strcmp(data, expected));
    free(data);
    /* .scarletnx beside the program (the Windows zip, a portable install) is the folder */
    beside = join(program, ".scarletnx");
    assert(!snx_mkdirs(beside));
    data = snx_data_dir(program);
    assert(data && !strcmp(data, beside));
    free(data);

    /* an AppImage: its programs run from the mount, the copy is the AppImage file's folder */
    file = join(copy, "ScarletNX.AppImage");
    assert(!snx_write_file(file, "", 0));
    setenv("APPIMAGE", file, 1);
    setenv("APPDIR", mount, 1);
    free(file);
    file = join(mount, "bin");
    data = snx_data_dir(file);
    assert(data && !strcmp(data, expected)); /* no .scarletnx beside the AppImage yet */
    free(data);
    portable = join(copy, ".scarletnx");
    assert(!snx_mkdirs(portable));
    data = snx_data_dir(file);
    assert(data && !strcmp(data, portable));
    free(data);
    free(file);
    /* APPIMAGE inherited by a program outside the mount: that program's own folder decides */
    data = snx_data_dir(program);
    assert(data && !strcmp(data, beside));
    free(data);
    free(portable);
    free(beside);
    unsetenv("APPIMAGE");
    unsetenv("APPDIR");

    /* the checkout: for a program of its build tree, or any program when no build tree is named */
    data = snx_checkout(checkout, build, built);
    assert(data && !strcmp(data, checkout));
    free(data);
    data = snx_checkout(checkout, NULL, program);
    assert(data && !strcmp(data, checkout));
    free(data);
    /* a copy installed from that build (cmake --install) has none, the checkout still there or not (#251) */
    assert(!snx_checkout(checkout, build, program));
    /* a folder without CMakeLists.txt is no checkout; none named, none */
    assert(!snx_checkout(program, NULL, program));
    assert(!snx_checkout(NULL, NULL, program));

    /* where this version keeps what an older one kept in the checkout: games/ and the keys in the data folder, work/
       in its cache folder (the user's on Linux, cache/ of the data folder without XDG) */
    for (i = 0; i < SNX_CHECKOUT_FILES; ++i) {
        now = snx_checkout_now(expected, i);
#ifdef SNX_XDG
        line = !strcmp(snx_checkout_files[i], "work")    ? snx_format("%s/scarletnx/work", cache)
               : !strcmp(snx_checkout_files[i], "games") ? join(expected, "recompiled")
                                                         : join(expected, snx_checkout_files[i]);
#else
        line = !strcmp(snx_checkout_files[i], "work")    ? join(expected, "cache/work")
               : !strcmp(snx_checkout_files[i], "games") ? join(expected, "recompiled")
                                                         : join(expected, snx_checkout_files[i]);
#endif
        assert(now && !strcmp(now, line));
        free(line);
        free(now);
    }
    /* said on stderr for each one still there, nothing for the others; nothing is moved */
    file = join(root, "stderr.txt");
    said = left_said(checkout, expected, file);
    assert(!said[0]);
    free(said);
    old = join(checkout, "games");
    assert(!snx_mkdirs(old));
    free(old);
    old = join(checkout, "prod.keys");
    assert(!snx_write_file(old, "", 0));
    free(old);
    said = left_said(checkout, expected, file);
    line = snx_format("scarletnx: %s/games was left by an older version: this one keeps it in %s/recompiled "
                      "(docs/user/deprecations.md)\n"
                      "scarletnx: %s/prod.keys was left by an older version: this one keeps it in %s/prod.keys "
                      "(docs/user/deprecations.md)\n",
                      checkout, expected, checkout, expected);
    assert(!strcmp(said, line));
    free(line);
    free(said);
    old = join(checkout, "games");
    assert(snx_is_dir(old));
    free(old);
    /* the checkout named as the data folder (--repo) left nothing behind */
    said = left_said(checkout, checkout, file);
    assert(!said[0]);
    free(said);
    /* recompiled/ of a data folder, and the games/ an older version made its games in: where a game it made is still
       found until the player moves it, said on stderr while it is there; nothing is moved */
    {
        char *dir = join(root, "made"), *old_games = join(dir, "games"), *new_games = join(dir, "recompiled");
        char *made = join(old_games, "0100ABCD12345000"), *fresh = join(new_games, "0100ABCD12345000"), *found;
        found = snx_recompiled_game(dir, "0100ABCD12345000"); /* neither: the new one */
        assert(found && !strcmp(found, fresh));
        free(found);
        said = recompiled_said(dir, file);
        assert(!said[0]);
        free(said);
        assert(!snx_mkdirs(made));
        found = snx_recompiled_game(dir, "0100ABCD12345000"); /* only the old one */
        assert(found && !strcmp(found, made));
        free(found);
        said = recompiled_said(dir, file);
        line = snx_format("scarletnx: %s was left by an older version: new games go to %s, and the ones in it still "
                          "run until they are moved there (docs/user/deprecations.md)\n",
                          old_games, new_games);
        assert(!strcmp(said, line) && snx_is_dir(made));
        free(line);
        free(said);
        assert(!snx_mkdirs(fresh));
        found = snx_recompiled_game(dir, "0100ABCD12345000"); /* both: the new one */
        assert(found && !strcmp(found, fresh));
        free(found);
        free(fresh);
        free(made);
        free(new_games);
        free(old_games);
        free(dir);
    }
    free(file);

    /* snx_folder_unfit: how the menu's Folders and each run check a folder picked for the drivers */
    {
        char *dir = snx_path_join(root, "fit"), *file = snx_path_join(root, "fit.txt"), **left = NULL;
        size_t n = 0;
        assert(!strcmp(snx_folder_unfit(dir, 0), "is not there"));
        assert(!snx_mkdirs(dir) && !snx_folder_unfit(dir, 1) && !snx_write_file(file, "", 0));
        assert(!strcmp(snx_folder_unfit(file, 0), "is not a folder"));
#ifndef _WIN32
        if (geteuid()) { /* root writes anywhere */
            assert(!chmod(dir, 0555) && !strcmp(snx_folder_unfit(dir, 0), "cannot be written"));
            assert(!chmod(dir, 0755));
        }
#endif
        assert(!snx_list_dir(dir, &left, &n) && !n); /* the check leaves nothing in it */
        snx_free_strings(left, n);
        free(file);
        free(dir);
    }
    /* snx_dump_name: the extraction's folder the drivers and the menu give a dump */
    {
        static const char* const names[][2] = {{"/dumps/My Game [v1.0].nsp", "My Game v10"},
                                               {"C:\\dumps\\two.dots.xci", "twodots"},
                                               {"/dumps/.nsp", "nsp"},
                                               {"/dumps/[] .xci", "Game"},
                                               {"caf\xc3\xa9 -_1.nsp", "caf\xc3\xa9 -_1"}};
        size_t i;
        for (i = 0; i < sizeof names / sizeof names[0]; ++i) {
            char* name = snx_dump_name(names[i][0]);
            assert(!strcmp(name, names[i][1]));
            free(name);
        }
    }
    /* one disk: two folders of the temporary one, and one still to be made there; another file system is another */
    assert(snx_same_disk(checkout, program));
    file = join(root, "not/made/yet");
    assert(snx_same_disk(file, checkout));
    free(file);
#ifdef __linux__
    assert(!snx_same_disk(root, "/proc"));
#endif

    puts("snx_data_dir: .scarletnx beside the program or the AppImage, else the user's data folder, for a build "
         "tree's programs too; snx_checkout: a build tree's program's checkout, what an older version kept in it said");
    free(expected);
    free(cache);
    free(xdg);
    free(copy);
    free(mount);
    free(built);
    free(build);
    free(program);
    free(checkout);
    free(root);
    return 0;
}
