/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The driver's setup in a terminal (src/nx/runtime/driver/setup.c), its answers from a file: every step at its
   default, each choice that writes a file, answers that are no answer asked again, an end of input before the end,
   a portable copy, the fixed uid kept for its saves, and a notice alone after an update. The launcher's languages are
   this file's three, as po/ has them */
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE   700 /* realpath */
#include "nx/runtime/driver/setup.h"
#include "common/utilities/common.h"
#include "common/utilities/setup.h"
#include "common/utilities/users.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define FIRST "0123456789abcdeffedcba9876543210"

const char* snx_menu_language(int i, const char** name) {
    static const char *const codes[] = {"en", "pt-BR", "zh-Hans-CN"},
                             *const names[] = {"English", "Portugu\xc3\xaas (Brasil)",
                                               "\xe7\xae\x80\xe4\xbd\x93\xe4\xb8\xad\xe6\x96\x87"};
    if (i < 0 || i > 2) return NULL;
    *name = names[i];
    return codes[i];
}

static char *root, *bin, *home, *data, *config;
static char output[16384];

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
static char* text_of(const char* folder, const char* name) {
    char* path = join(folder, name);
    size_t size;
    char* text = snx_is_file(path) ? (char*)snx_read_file(path, &size) : NULL;
    free(path);
    return text;
}
/* the setup run with answers, its output in output: its status */
static int run(const char* answers, int all) {
    char *path = join(root, "answers"), *said = join(root, "said");
    FILE *in, *out;
    size_t n;
    int rc;
    put(path, answers);
    in = fopen(path, "r");
    out = fopen(said, "w+");
    assert(in && out);
    rc = snx_setup_terminal(in, out, bin, all);
    rewind(out);
    n = fread(output, 1, sizeof output - 1, out);
    output[n] = 0;
    fclose(in);
    fclose(out);
    free(said);
    free(path);
    return rc;
}
static void said(const char* words) {
    if (!strstr(output, words)) fprintf(stderr, "the setup said:\n%s\nnot: %s\n", output, words);
    assert(strstr(output, words));
}
/* words holding a folder as the setup shows it, ~ for the home: the folders are each system's */
static void said_with(const char* format, const char* first, const char* second) {
    char a[1024], b[1024], *words;
    snx_home_short(first, a, sizeof a);
    snx_home_short(second ? second : first, b, sizeof b);
    words = snx_format(format, a, b);
    said(words);
    free(words);
}
static void expect(const char* folder, const char* name, const char* words) {
    char* text = text_of(folder, name);
    if (!text || !strstr(text, words)) fprintf(stderr, "%s/%s holds:\n%s\nnot: %s\n", folder, name, text, words);
    assert(text && strstr(text, words));
    free(text);
}
/* a fresh home: no setup.ini, no data folder */
static void fresh(void) {
    snx_remove_tree(home);
    assert(!snx_mkdirs(home));
}

/* every step at its default: the notices' OK, English, installed, folders and keys as they are, Player */
static void test_defaults(void) {
    char *users = join(data, "users"), *text, uid[33];
    fresh();
    assert(!run("\n\n\n\n\n\n", 0));
    said("Changed in older versions\n  Game settings\n    before  removed or renamed keys");
    {
        char last[256]; /* the last notice, then the steps */
        snprintf(last, sizeof last, "    now     %s\n  Not moved. See docs/DEPRECATIONS.md\n[Enter] OK ",
                 snx_notices[SNX_NOTICES - 1].now);
        said(last);
    }
    said("[1/5] Language\n  1) English 2) Portugu\xc3\xaas (Brasil) 3) "
         "\xe7\xae\x80\xe4\xbd\x93\xe4\xb8\xad\xe6\x96\x87\n  [1] ");
    said_with("[2/5] Install Type\n  Installed: settings %s, data %s", config, data);
    said_with("[3/5] Folders\n  Recompiled: %s/recompiled\n  Dumps: none yet\n", data, NULL);
    said("[4/5] Keys\n  prod.keys missing\n  title.keys missing: only some .nsp need it\n  1) Choose folder  2) Later\n"
         "  [2] ");
    said("[5/5] First User\n  uid ");
    said_with("Done: %s/setup.ini\n", config, NULL);
    {
        char seen[64]; /* every notice there is, seen */
        snprintf(seen, sizeof seen, "[setup]\nversion = 1\nnotices = %d\n", SNX_NOTICES);
        expect(config, "setup.ini", seen);
    }
    assert(!text_of(config, "app.ini")); /* the language it had: app.ini as it was */
    text = text_of(users, "users.ini");
    assert(text && sscanf(strstr(text, "uid=") + 4, "%32s", uid) == 1 && strstr(text, "\nnickname=Player\n"));
    assert(strlen(uid) == 32 && uid[12] == '4' && strchr("89ab", uid[16]) && strcmp(uid, FIRST));
    free(text);
    /* done: nothing to ask, nothing said */
    assert(!run("", 0) && !output[0]);
    free(users);
    puts("PASS every step at its default");
}

/* again with --setup: users.ini kept (n), Portuguese, games and dumps folders, keys installed, answers that are none */
static void test_again(void) {
    char *users = join(data, "users"), *games = join(root, "picked games"), *dumps = join(root, "dumps");
    char *keys = join(root, "keys"), *path, answers[4096], *before, *keys_dir = snx_keys_dir(data);
    assert(keys_dir && !snx_mkdirs(games) && !snx_mkdirs(dumps));
    path = join(keys, "prod.keys");
    put(path, "header_key = 00112233\n");
    free(path);
    before = text_of(users, "users.ini");
    sprintf(answers, "\n9\n2\n1\n2\n%s\n3\n/no/such/folder\n%s\n3\n%s\n1\n1\n%s\n1\nmaybe\nn\n", games, dumps, dumps,
            keys);
    assert(!run(answers, 1));
    said("9: answer 1 to 3, or Enter for 1");
    said("/no/such/folder is not a folder");
    said("  Dumps:\n");
    said_with("  Installed prod.keys in %s\n", keys_dir, NULL);
    said_with("prod.keys found, %s/prod.keys", keys_dir, NULL);
    said("[5/5] users.ini has 1 user: Player\n  Overwrite? (users.ini.old keeps it) [y/N] ");
    said("maybe: answer y or n");
    expect(config, "app.ini", "[folders]\nrecompiled = ");
    expect(config, "app.ini", "\n[app]\nlanguage = pt-BR\n");
    path = text_of(config, "folders.txt");
    assert(path && !strcmp(strstr(path, "dumps"), "dumps\n")); /* once */
    free(path);
    path = text_of(users, "users.ini");
    assert(path && before && !strcmp(path, before));
    free(path);
    free(before);
    free(keys_dir);
    free(keys);
    free(dumps);
    free(games);
    free(users);
    puts("PASS --setup again: each choice written, users.ini kept, answers that are none asked again");
}

/* a portable copy: .scarletnx beside the program, every file in it; then installed refused while it is there */
static void test_portable(void) {
    char *portable = join(bin, SNX_PORTABLE_DIR), *pconfig = join(portable, "config"), *users = join(portable, "users");
    fresh();
    assert(run("\n\n2\n", 0) == 1 && snx_is_dir(portable)); /* made as it is picked, though nothing else was */
    assert(!snx_remove_tree(portable));
    assert(!run("\n\n2\n\n\nAbcdefghijklmnopqrstuvwxyz0123456\nAogo\n", 0));
    said("Portable: everything in ");
    said("Abcdefghijklmnopqrstuvwxyz0123456 is not 1 to 32 bytes with no control character nor a space at either end");
    expect(pconfig, "setup.ini", "version = 1");
    expect(users, "users.ini", "nickname=Aogo");
    assert(!text_of(config, "setup.ini"));
    assert(!run("\n\n1\n2\n\n\nn\n", 1));
    said("is there, so this copy is portable: move it away first, then answer 1");
    assert(!snx_remove_tree(portable));
    free(users);
    free(pconfig);
    free(portable);
    puts("PASS a portable copy, its files beside the program");
}

/* the answers end before the setup does: it says so, and setup.ini stays as it was */
static void test_ended(void) {
    fresh();
    assert(run("\n\n", 0) == 1);
    said("The setup stops here: no more answers. It asks again the next time.");
    assert(!text_of(config, "setup.ini"));
    puts("PASS answers that end too soon");
}

/* games with saves of the fixed uid, in recompiled/ where the launcher puts them or in the games/ an older version
   left: user 1 keeps it */
static void test_first_saves(void) {
    static const char* const saves[] = {"recompiled/0100A/0100A_data/save/user." FIRST,
                                        "games/0100A/0100A_data/save/user." FIRST};
    size_t i;
    for (i = 0; i < sizeof saves / sizeof *saves; ++i) {
        char *games = join(data, saves[i]), *users = join(data, "users");
        fresh();
        assert(!snx_mkdirs(games));
        assert(!run("\n\n\n\n\n\n", 0));
        said("uid " FIRST ", the uid of the saves of 1 game\n");
        expect(users, "users.ini", "uid=" FIRST "\nnickname=Player\n");
        free(users);
        free(games);
    }
    puts("PASS the fixed uid kept for its saves");
}

/* the language it starts on: the system's (LANG), the same language of another country, else English, and never one
   of another script; app.ini's once it names one, in an older form of its tag too, and English picked over the
   system's written */
static void test_language(void) {
    char* app = join(config, "app.ini");
    static const char* const simplified[] = {"zh_CN.UTF-8", "zh_SG.UTF-8", "zh"};
    static const char* const traditional[] = {"zh_TW.UTF-8", "zh_HK.UTF-8", "zh-Hant"};
    int i;
    fresh();
    setenv("LANG", "pt_BR.UTF-8", 1);
    assert(!run("\n\n\n\n\n\n", 0));
    said("\xe7\xae\x80\xe4\xbd\x93\xe4\xb8\xad\xe6\x96\x87\n  [2] ");
    assert(!text_of(config, "app.ini"));
    for (i = 0; i < 3; ++i) {
        setenv("LANG", simplified[i], 1);
        assert(!run("\n\n\n\n\n\n", 1));
        said("\xe7\xae\x80\xe4\xbd\x93\xe4\xb8\xad\xe6\x96\x87\n  [3] ");
        setenv("LANG", traditional[i], 1);
        assert(!run("\n\n\n\n\n\n", 1));
        said("\xe7\xae\x80\xe4\xbd\x93\xe4\xb8\xad\xe6\x96\x87\n  [1] ");
    }
    setenv("LANG", "pt_PT.UTF-8", 1);
    assert(!run("\n1\n\n\n\n\n", 1) && strstr(output, "\xe7\xae\x80\xe4\xbd\x93\xe4\xb8\xad\xe6\x96\x87\n  [2] "));
    expect(config, "app.ini", "[app]\nlanguage = en\n");
    assert(!run("\n\n\n\n\n\n", 1));
    said("\xe7\xae\x80\xe4\xbd\x93\xe4\xb8\xad\xe6\x96\x87\n  [1] ");
    put(app, "[app]\nlanguage = pt_BR\n"); /* as a build before the tags wrote it */
    assert(!run("\n\n\n\n\n\n", 1));
    said("\xe7\xae\x80\xe4\xbd\x93\xe4\xb8\xad\xe6\x96\x87\n  [2] ");
    expect(config, "app.ini", "[app]\nlanguage = pt_BR\n"); /* kept: the launcher writes its tag */
    unsetenv("LANG");
    free(app);
    puts("PASS the language it starts on");
}

/* after an update: the notice it brings, alone, and OK */
static void test_notice(void) {
    char *file = join(config, "setup.ini"), before[64], after[64], newest[160];
    /* the newest notice unseen, all the others seen */
    snprintf(before, sizeof before, "[setup]\nversion = 1\nnotices = %d\n", SNX_NOTICES - 1);
    snprintf(after, sizeof after, "notices = %d\n", SNX_NOTICES);
    snprintf(newest, sizeof newest, "Changed since the last version\n  %s\n", snx_notices[SNX_NOTICES - 1].what);
    fresh();
    put(file, before);
    assert(!run("\n", 0));
    said(newest);
    assert(!strstr(output, "Game settings") && !strstr(output, "[1/"));
    expect(config, "setup.ini", after);
    free(file);
    puts("PASS a notice alone after an update");
}

int main(void) {
    char* real;
    root = snx_temp_dir(NULL);
    assert(root);
    real = realpath(root, NULL);
    assert(real);
    free(root);
    root = real;
    bin = join(root, "bin");
    home = join(root, "home");
    assert(!snx_mkdirs(bin));
    setenv("HOME", home, 1);
    unsetenv("XDG_DATA_HOME");
    unsetenv("XDG_CONFIG_HOME");
    unsetenv("XDG_STATE_HOME");
    unsetenv("XDG_CACHE_HOME");
    unsetenv("APPIMAGE");
    unsetenv("LANG");
    data = snx_user_data_dir();
    config = snx_user_dir(SNX_USER_CONFIG);
    test_defaults();
    test_again();
    test_portable();
    test_ended();
    test_first_saves();
    test_language();
    test_notice();
    assert(!snx_remove_tree(root));
    free(config);
    free(data);
    free(home);
    free(bin);
    free(root);
    return 0;
}
