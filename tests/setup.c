/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* common/utilities/setup.c: setup.ini and what is left to ask, the options that leave the setup out or go through it
   again, the first user's random uid, the fixed uid's saves, the keys found and installed, the .ini and the list of
   folders written in place, and the notices against docs/DEPRECATIONS.md (SNX_DEPRECATIONS) */
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE   700 /* realpath */
#include "common/utilities/setup.h"
#include "common/utilities/common.h"
#include "common/utilities/users.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define FIRST "0123456789abcdeffedcba9876543210"

static char* join(const char* a, const char* b) {
    char* p = snx_path_join(a, b);
    assert(p);
    return p;
}
static void put(const char* path, const char* text) {
    char* dir = snx_dirname(path);
    assert(dir && !snx_mkdirs(dir) && !snx_write_file(path, text, strlen(text)));
    free(dir);
}
/* the file's text, NULL for none */
static char* text_of(const char* path) {
    size_t size;
    return snx_is_file(path) ? (char*)snx_read_file(path, &size) : NULL;
}
static void expect_text(const char* path, const char* expected) {
    char* text = text_of(path);
    if (!text || strcmp(text, expected)) fprintf(stderr, "%s holds:\n%s\nnot:\n%s\n", path, text, expected);
    assert(text && !strcmp(text, expected));
    free(text);
}

/* a draw a thousand times: each a UUID of version 4 and RFC 4122's variant in its users.ini text, none twice */
static void test_uid(void) {
    static char texts[1000][33];
    uint64_t uid[2];
    int i, j, versions = 0;
    for (i = 0; i < 1000; ++i) {
        assert(!snx_setup_uid(uid));
        snx_users_uid_text(uid, texts[i]);
        assert(texts[i][12] == '4' && strchr("89ab", texts[i][16]));
        for (j = 0; j < i; ++j) assert(strcmp(texts[i], texts[j]));
        versions += texts[i][13] != texts[0][13]; /* the bits beside it are random */
    }
    assert(versions > 800);
    puts("PASS a new user's uid: random, of a v4 UUID's text");
}

static void test_file(const char* root) {
    char *path = join(root, "config/setup.ini"), why[512];
    SnxSetup setup;
    int i;
    /* no setup.ini: every step and every notice of this system */
    assert(!snx_setup_read(path, &setup, why, sizeof why) && !setup.version && !setup.notices);
    assert(snx_setup_due(&setup));
    for (i = 0; i < SNX_SETUP_STEPS; ++i) assert(snx_setup_asks(&setup, i));
    for (i = 0; i < SNX_NOTICES; ++i) assert(snx_setup_shows(&setup, i) == snx_notice_here(i));
    /* done: nothing left, read back as written */
    assert(!snx_setup_done(path, &setup, why, sizeof why));
    assert(!snx_setup_read(path, &setup, why, sizeof why) && setup.version == SNX_SETUP_VERSION &&
           setup.notices == SNX_NOTICES && !snx_setup_due(&setup));
    /* a notice added since: it alone */
    {
        char text[64];
        snprintf(text, sizeof text, "[setup]\nversion = 1\nnotices = %d\n", SNX_NOTICES - 1);
        put(path, text);
    }
    assert(!snx_setup_read(path, &setup, why, sizeof why) && snx_setup_due(&setup));
    for (i = 0; i < SNX_SETUP_STEPS; ++i) assert(!snx_setup_asks(&setup, i));
    for (i = 0; i < SNX_NOTICES; ++i) assert(snx_setup_shows(&setup, i) == (i == SNX_NOTICES - 1));
    /* a newer version's: nothing to ask, its own keys left alone, its numbers kept when done */
    put(path, "[setup]\nversion = 3\nnotices = 40\ncolour = red\n[later]\nkey = 1\n");
    assert(!snx_setup_read(path, &setup, why, sizeof why) && setup.version == 3 && setup.notices == 40);
    assert(!snx_setup_due(&setup) && !snx_setup_done(path, &setup, why, sizeof why));
    assert(!snx_setup_read(path, &setup, why, sizeof why) && setup.version == 3 && setup.notices == 40);
    /* this version's with what it does not know, or a number that is none: said, what was read kept */
    put(path, "[setup]\nversion = 1\nnotices = 2\ncolour = red\n");
    assert(snx_setup_read(path, &setup, why, sizeof why) == -1 && strstr(why, "[setup] colour is not the setup's"));
    assert(setup.version == 1 && setup.notices == 2);
    put(path, "[setup]\nversion = one\n");
    assert(snx_setup_read(path, &setup, why, sizeof why) == -1 && strstr(why, "version = one is not a whole number"));
    assert(!setup.version && snx_setup_due(&setup));
    put(path, "[setup]\nnotices = -1\n");
    assert(snx_setup_read(path, &setup, why, sizeof why) == -1 && strstr(why, "notices = -1"));
    free(path);
    puts("PASS setup.ini: the steps and notices left, a newer one's kept");
}

/* argv, count long, through snx_setup_options: what is left, joined by spaces, or "-1" */
static void expect_options(const char* const* args, int count, const char* left, int skip, int again) {
    char *argv[8], out[128], why[256];
    int i, n;
    for (i = 0; i < count; ++i) argv[i] = (char*)args[i];
    argv[count] = NULL;
    n = snx_setup_options(count, argv, why, sizeof why);
    out[0] = 0;
    if (n < 0)
        strcpy(out, "-1");
    else
        for (i = 1; i < n; ++i) sprintf(out + strlen(out), "%s%s", i > 1 ? " " : "", argv[i]);
    if (strcmp(out, left)) fprintf(stderr, "left %s, not %s (%s)\n", out, left, why);
    assert(!strcmp(out, left));
    assert(n < 0 || (!argv[n] && snx_setup_skip == skip && snx_setup_again == again));
}
static void test_options(void) {
    static const char* const debug[] = {"scarletnx", "--debug", "game.nsp"};
    static const char* const skips[] = {"scarletnx", "--skip", "-H", "--skip", "game.nsp"};
    static const char* const one_skip[] = {"scarletnx", "--skip", "game.nsp"};
    static const char* const setup[] = {"scarletnx", "--setup"};
    static const char* const both[] = {"scarletnx", "--setup", "--debug"};
    static const char* const game[] = {"scarletnx", "game.nsp", "--", "--debug", "--skip"};
    static const char* const none[] = {"scarletnx"};
    expect_options(debug, 3, "game.nsp", 1, 0);
    expect_options(skips, 5, "-H game.nsp", 1, 0);
    expect_options(one_skip, 3, "-1", 0, 0);
    expect_options(setup, 2, "", 0, 1);
    expect_options(both, 3, "-1", 0, 0);
    expect_options(game, 5, "game.nsp -- --debug --skip", 0, 0);
    expect_options(none, 1, "", 0, 0);
    puts("PASS --debug, --skip --skip and --setup, the game's own arguments left alone");
}

static void test_saves(const char* root) {
    char *games = join(root, "games"), *path;
    path = join(games, "0100A/0100A_data/save/user." FIRST);
    assert(!snx_mkdirs(path));
    free(path);
    path = join(games, "0100B/0100B_data/save/user.00000000000000020000000000000002");
    assert(!snx_mkdirs(path));
    free(path);
    path = join(games, "0100C/0100C_data/save/user");
    assert(!snx_mkdirs(path));
    free(path);
    path = join(games, "0100D/0100D_data/save/user." FIRST);
    put(path, "a file, not a folder of saves");
    free(path);
    assert(snx_setup_first_saves(games, NULL) == 1);
    path = join(root, "nothing here");
    assert(snx_setup_first_saves(path, path) == 0 && snx_setup_first_saves(NULL, NULL) == 0);
    free(path);
    /* and the data folder's save/, save_layout=global's: a title's folder each, a file there none */
    path = join(root, "global data/save/user." FIRST "/0100000000AB0000");
    assert(!snx_mkdirs(path));
    free(path);
    path = join(root, "global data/save/user." FIRST "/stray");
    put(path, "a file, not a game's saves");
    free(path);
    path = join(root, "global data");
    assert(snx_setup_first_saves(NULL, path) == 1 && snx_setup_first_saves(games, path) == 2);
    free(path);
    free(games);
    puts("PASS the games with saves of the fixed uid");
}

static void test_keys(const char* root, const char* home) {
    static const char prod[] = "header_key = 00112233\n";
    static const char titles[] = "0100000000000000000000000000000a = 0011aabb\n";
    char *data = join(root, "data"), *picked = join(root, "picked"), *found, *dir, *switch_dir = join(home, ".switch");
    char *path, why[512];
    struct stat st;
    int titled;
    assert(snx_keys_text(prod, strlen(prod)) && snx_keys_text(titles, strlen(titles)));
    assert(!snx_keys_text("no keys here\n", 13) && !snx_keys_text("name =\n", 7) && !snx_keys_text("a\0= 00", 6));
    /* none anywhere; ~/.switch's; the data folder's first */
    assert(!snx_keys_found(data, "prod.keys"));
    path = join(switch_dir, "prod.keys");
    put(path, prod);
    found = snx_keys_found(data, "prod.keys");
    assert(found && !strcmp(found, path));
    free(found);
    free(path);
    path = join(data, "prod.keys");
    put(path, prod);
    found = snx_keys_found(data, "prod.keys");
    assert(found && !strcmp(found, path));
    free(found);
    free(path);
    { /* command-line files override the usual lookup, for this driver's run only */
        char *prod_override = join(root, "provided-prod.keys"), *title_override = join(root, "provided-title.keys");
        SnxSetupKeysOverride previous = snx_setup_keys_override_get();
        put(prod_override, prod);
        put(title_override, titles);
        snx_setup_keys_override_set(prod_override, title_override);
        found = snx_keys_found(data, "prod.keys");
        assert(found && !strcmp(found, prod_override));
        free(found);
        found = snx_keys_found(data, "title.keys");
        assert(found && !strcmp(found, title_override));
        free(found);
        found = snx_keys_found(data, "console.keys");
        assert(!found || strcmp(found, title_override));
        free(found);
        assert(!snx_keys_found(data, "other.keys"));
        snx_setup_keys_override_set(previous.prod, previous.title);
        free(prod_override);
        free(title_override);
    }
    /* installed from a folder: prod.keys, title.keys with it when there, each its owner's alone */
    assert(snx_keys_install(picked, data, &titled, why, sizeof why) == -1 && strstr(why, "has no prod.keys"));
    path = join(picked, "prod.keys");
    put(path, "not keys\n");
    assert(snx_keys_install(picked, data, &titled, why, sizeof why) == -1 && strstr(why, "is not a prod.keys"));
    put(path, prod);
    free(path);
    dir = join(root, "installed");
    assert(!snx_keys_install(picked, dir, &titled, why, sizeof why) && !titled);
    path = join(dir, "prod.keys");
    expect_text(path, prod);
    assert(!stat(path, &st) && (st.st_mode & 0777) == 0600);
    free(path);
    path = join(picked, "title.keys");
    put(path, titles);
    free(path);
    assert(!snx_keys_install(picked, dir, &titled, why, sizeof why) && titled);
    path = join(dir, "title.keys");
    expect_text(path, titles);
    free(path);
    /* a folder they cannot go in (a file there): said with the path and the system's reason */
    path = join(root, "a file");
    put(path, "x");
    assert(snx_keys_install(picked, path, &titled, why, sizeof why) == -1 && strstr(why, "could not be written: "));
    assert(strlen(strstr(why, "could not be written: ")) > strlen("could not be written: "));
#ifndef _WIN32
    assert(strstr(why, strerror(EEXIST)) != NULL);
#endif
    free(path);
    /* where they go: the user's data folder itself on an XDG system, else ~/.switch */
    path = snx_user_data_dir();
    found = snx_keys_dir(path);
#ifdef SNX_XDG
    assert(found && !strcmp(found, path));
#else
    assert(found && !strcmp(found, switch_dir));
#endif
    free(found);
    free(path);
    found = snx_keys_dir(data);
    assert(found && !strcmp(found, switch_dir));
    free(found);
    free(dir);
    free(switch_dir);
    free(picked);
    free(data);
    puts("PASS the keys found and installed");
}

static void test_files(const char* root) {
    char *app = join(root, "app.ini"), *list = join(root, "folders.txt"), why[256];
    assert(!snx_ini_put(app, "app", "language", "pt-BR", why, sizeof why));
    expect_text(app, "[app]\nlanguage = pt-BR\n");
    put(app, "; Settings > App\n[app]\ntheme = yorha\n\n[folders]\ndata =\n");
    assert(!snx_ini_put(app, "app", "language", "pt-BR", why, sizeof why));
    expect_text(app, "; Settings > App\n[app]\ntheme = yorha\nlanguage = pt-BR\n\n[folders]\ndata =\n");
    assert(!snx_ini_put(app, "app", "language", "en", why, sizeof why));
    expect_text(app, "; Settings > App\n[app]\ntheme = yorha\nlanguage = en\n\n[folders]\ndata =\n");
    assert(!snx_ini_put(app, "folders", "games", "/games", why, sizeof why));
    expect_text(app, "; Settings > App\n[app]\ntheme = yorha\nlanguage = en\n\n[folders]\ndata =\ngames = /games\n");
    put(app, "[other]\nlanguage = xx\n[app]\nlanguage=fr");
    assert(!snx_ini_put(app, "app", "language", "en", why, sizeof why));
    expect_text(app, "[other]\nlanguage = xx\n[app]\nlanguage = en\n");
    put(app, "[folders]\ndata =");
    assert(!snx_ini_put(app, "app", "language", "en", why, sizeof why));
    expect_text(app, "[folders]\ndata =\n[app]\nlanguage = en\n");
    assert(!snx_lines_add(list, "/dumps", why, sizeof why) && !snx_lines_add(list, "/dumps", why, sizeof why));
    expect_text(list, "/dumps\n");
    put(list, "/a\n/b/**");
    assert(!snx_lines_add(list, "/c", why, sizeof why) && !snx_lines_add(list, "/b/**", why, sizeof why));
    expect_text(list, "/a\n/b/**\n/c\n");
    free(list);
    free(app);
    puts("PASS app.ini's keys and folders.txt's lines written in place");
}

/* every notice names a row of the table of docs/DEPRECATIONS.md's "Now", and every row is some notice's */
static void test_notices(void) {
    size_t size;
    char *doc = (char*)snx_read_file(SNX_DEPRECATIONS, &size), *at, *end;
    int rows = 0, i, seen[SNX_NOTICES] = {0};
    assert(doc);
    at = strstr(doc, "## Now");
    assert(at && (at = strstr(at, "|---|")) != NULL);
    for (at = strchr(at, '\n') + 1; !strncmp(at, "| ", 2); at = end + 1) {
        int named = 0;
        end = strchr(at, '\n');
        *end = 0;
        for (i = 0; i < SNX_NOTICES; ++i)
            if (strstr(at, snx_notices[i].row)) named = seen[i] = 1;
        if (!named) fprintf(stderr, "no notice for: %.80s\n", at);
        assert(named);
        ++rows;
    }
    for (i = 0; i < SNX_NOTICES; ++i) {
        if (!seen[i]) fprintf(stderr, "notice %d's row is not in the table: %s\n", i, snx_notices[i].row);
        assert(seen[i] && snx_notices[i].what[0] && snx_notices[i].before[0] && snx_notices[i].now[0]);
    }
    assert(rows >= SNX_NOTICES);
    free(doc);
    printf("PASS %d notices for the %d rows of DEPRECATIONS.md\n", SNX_NOTICES, rows);
}

static void test_place(const char* root, const char* home) {
    char *data = snx_user_data_dir(), *path = snx_setup_file(data), *expected, *portable, *config;
    config = snx_user_dir(SNX_USER_CONFIG);
    expected = join(config, "setup.ini");
    assert(path && !strcmp(path, expected));
    free(path);
    free(expected);
    portable = join(root, "bin/" SNX_PORTABLE_DIR);
    assert(!snx_mkdirs(portable));
    path = snx_setup_file(portable);
    expected = join(portable, "config/setup.ini");
    assert(path && !strcmp(path, expected));
    free(path);
    free(expected);
    path = join(root, "bin");
    expected = snx_portable_dir(path);
    assert(expected && !strcmp(expected, portable));
    free(expected);
    free(path);
    {
        char shown[256], *in_home = join(home, ".local/share/scarletnx");
        snx_home_short(in_home, shown, sizeof shown);
        assert(!strcmp(shown, "~/.local/share/scarletnx"));
        snx_home_short("/elsewhere/scarletnx", shown, sizeof shown);
        assert(!strcmp(shown, "/elsewhere/scarletnx"));
        free(in_home);
        in_home = snx_format("%s-other/x", home); /* a folder named like it is not in it */
        snx_home_short(in_home, shown, sizeof shown);
        assert(!strcmp(shown, in_home));
        free(in_home);
    }
    free(portable);
    free(config);
    free(data);
    puts("PASS setup.ini in the config folder of the data folder");
}

int main(void) {
    char *root = snx_temp_dir(NULL), *real, *home;
    assert(root);
    real = realpath(root, NULL);
    assert(real);
    home = join(real, "home");
    setenv("HOME", home, 1);
    unsetenv("XDG_DATA_HOME");
    unsetenv("XDG_CONFIG_HOME");
    unsetenv("APPIMAGE");
    test_uid();
    test_file(real);
    test_options();
    test_saves(real);
    test_keys(real, home);
    test_files(real);
    test_notices();
    test_place(real, home);
    assert(!snx_remove_tree(real));
    free(home);
    free(real);
    free(root);
    return 0;
}
