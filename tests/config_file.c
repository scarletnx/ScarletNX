/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* src/common/utilities/config_file.c, which every settings file is read and written through: a file that is not there,
   one that cannot be read, one put in place whole; the keys and lines a reader does not take, named once a run and never
   with their value; and the lines of them kept byte for byte, with the sections they are of */
#include "common/utilities/common.h"
#include "common/utilities/config_file.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

static char* dir;
static char* in(const char* name) { return snx_path_join(dir, name); }
static void put(const char* path, const char* text) { assert(!snx_write_file(path, text, strlen(text))); }
static int is(const char* path, const char* text) {
    size_t size;
    char* got = (char*)snx_read_file(path, &size);
    int same = got && size == strlen(text) && !memcmp(got, text, size);
    free(got);
    return same;
}

static void test_files(void) {
    char *path = in("a.ini"), *sub = in("folder/b.ini"), *text, why[512];
    size_t size;
    assert(snx_config_read(path, &text, &size, why, sizeof why) == 0 && !text && !size);
    put(path, "k=1\n");
    assert(snx_config_read(path, &text, &size, why, sizeof why) == 1 && size == 4 && !strcmp(text, "k=1\n"));
    free(text);
    /* the folder made, the old file in place until the new one is whole, and nothing of the staged one left */
    assert(!snx_config_write(sub, ".new", "x\n", 2, why, sizeof why) && is(sub, "x\n"));
    assert(!snx_config_write(sub, ".new", "y\n", 2, why, sizeof why) && is(sub, "y\n"));
    {
        char* staged = in("folder/b.ini.new");
        assert(!snx_path_exists(staged));
        free(staged);
    }
    /* a staged file that cannot be made (a folder with something in it there): the old one stays, said with the reason */
    {
        char *staged = in("folder/b.ini.tmp"), *held = in("folder/b.ini.tmp/held");
        assert(!snx_mkdirs(staged));
        put(held, "z");
        assert(snx_config_write(sub, ".tmp", "w\n", 2, why, sizeof why) == -1 &&
               strstr(why, "b.ini could not be written ("));
        assert(is(sub, "y\n") && is(held, "z"));
        free(held);
        free(staged);
    }
    /* a folder where the file should be: there, not readable, not written over, said with its path */
    {
        char *there = in("c.ini"), *held = in("c.ini/held");
        assert(!snx_mkdirs(there));
        put(held, "z");
        assert(snx_config_read(there, &text, &size, why, sizeof why) == -1 && !text &&
               strstr(why, "c.ini cannot be read ("));
        assert(snx_config_write(there, ".new", "w\n", 2, why, sizeof why) == -1 &&
               strstr(why, "c.ini cannot be read (") && strstr(why, "): nothing is written over it"));
        assert(snx_is_dir(there) && is(held, "z"));
        free(held);
        free(there);
    }
#ifndef _WIN32
    if (geteuid()) { /* root reads a file nobody may */
        char* closed = in("d.ini");
        put(closed, "secret\n");
        assert(!chmod(closed, 0));
        assert(snx_config_read(closed, &text, &size, why, sizeof why) == -1 && strstr(why, "Permission denied"));
        assert(snx_config_write(closed, ".new", "w\n", 2, why, sizeof why) == -1 &&
               strstr(why, "): nothing is written"));
        assert(!chmod(closed, 0600) && is(closed, "secret\n"));
        free(closed);
    }
#endif
    free(sub);
    free(path);
    puts("PASS a settings file read, refused when it cannot be, put in place whole");
}

static void test_said(void) {
    SnxConfigOthers o = {0};
    char out[1024], other[1024], name[24];
    int i;
    snx_config_forget();
    assert(!snx_config_others_said(&o, "x.ini", out, sizeof out) && !out[0]);
    snx_config_other_key(&o, "", "colour", 2);
    snx_config_other_key(&o, "newer", "rule", 5);
    snx_config_other_key(&o, "", "hunter2 here", 7); /* what is no key's name is a secret's, not said */
    snx_config_other_key(&o, "s\001x", "key", 9);
    snx_config_other_line(&o, 11);
    assert(snx_config_others_said(&o, "x.ini", out, sizeof out) == 5);
    assert(!strcmp(
        out, "x.ini: this version does not read colour, [newer] rule, line 7, key of line 9's section, which stay "
             "as they are; line 11 has no = and sets nothing"));
    assert(!strstr(out, "hunter2"));
    /* once a run for each name of a file; another file's, or the next run's, again */
    assert(!snx_config_others_said(&o, "x.ini", out, sizeof out) && !out[0]);
    assert(snx_config_others_said(&o, "y.ini", other, sizeof other) == 5 && strstr(other, "y.ini: this version"));
    snx_config_forget();
    assert(snx_config_others_said(&o, "x.ini", out, sizeof out) == 5);
    /* one key */
    memset(&o, 0, sizeof o);
    snx_config_forget();
    snx_config_other_key(&o, "", "one", 1);
    assert(snx_config_others_said(&o, "x.ini", out, sizeof out) == 1 &&
           !strcmp(out, "x.ini: this version does not read one, which stays as it is"));
    /* two lines, and no key */
    memset(&o, 0, sizeof o);
    snx_config_forget();
    snx_config_other_line(&o, 4);
    snx_config_other_line(&o, 9);
    assert(snx_config_others_said(&o, "x.ini", out, sizeof out) == 2 &&
           !strcmp(out, "x.ini: lines 4, 9 have no = and set nothing"));
    /* in a file of secrets, a key named only in small letters: another could be a token pasted as a line */
    memset(&o, 0, sizeof o);
    snx_config_forget();
    o.secret = 1;
    snx_config_other_key(&o, "", "FAKEtoken", 1);
    snx_config_other_key(&o, "", "sign_in", 2);
    assert(snx_config_others_said(&o, "x.ini", out, sizeof out) == 2 && strstr(out, "read line 1, sign_in") &&
           !strstr(out, "FAKE"));
    /* a key given twice: both lines, with their values but for a secret's */
    memset(&o, 0, sizeof o);
    snx_config_forget();
    snx_config_twice(&o, "app", "side", "on", 2, "off", 5);
    snx_config_twice(&o, "", "room_password", NULL, 1, NULL, 3);
    assert(snx_config_others_said(&o, "x.ini", out, sizeof out) == 2 &&
           !strcmp(out, "x.ini: [app] side is given twice, side=on on line 2 and side=off on line 5; room_password is "
                        "given twice, on lines 1 and 3: the last line's is taken"));
    assert(!snx_config_others_said(&o, "x.ini", out, sizeof out));
    /* more than it names, counted */
    memset(&o, 0, sizeof o);
    snx_config_forget();
    for (i = 0; i < SNX_CONFIG_NAMED + 3; ++i) {
        snprintf(name, sizeof name, "key%d", i);
        snx_config_other_key(&o, "", name, i + 1);
    }
    assert(snx_config_others_said(&o, "x.ini", out, sizeof out) == SNX_CONFIG_NAMED && strstr(out, "key0, key1") &&
           strstr(out, "key7 and 3 more, which stay as they are") && !strstr(out, "key8"));
    /* it fits where it is told to */
    snx_config_forget();
    assert(snx_config_others_said(&o, "x.ini", out, 20) == SNX_CONFIG_NAMED && strlen(out) < 20);
    puts("PASS the keys and lines a reader does not take, named once a run, never with a value");
}

static int owns(void* ctx, const char* section, const char* key) {
    (void)ctx;
    return (!strcmp(section, "") && !strcmp(key, "top")) || (!strcmp(section, "app") && !strcmp(key, "theme"));
}
static void test_kept(void) {
    static const char text[] =
        "; a comment\r\ntop = 1\r\nNewTop = a=b\r\n[app]\r\ntheme = mine\r\n  Later  =  SNX  \r\n"
        "stray\r\n[other]\r\nk=v\r\n[app]\r\ntheme=two\r\nAgain=1\n[]\nnone=2";
    /* a key after [] is of no section, as the reader reads it */
    char* out = snx_config_kept(text, sizeof text - 1, 1, owns, NULL);
    assert(out && !strcmp(out, "NewTop = a=b\nnone=2\n"));
    free(out);
    out = snx_config_kept(text, sizeof text - 1, 0, owns, NULL);
    assert(out && !strcmp(out, "[app]\n  Later  =  SNX  \n[other]\nk=v\n[app]\nAgain=1\n"));
    free(out);
    out = snx_config_kept(text, sizeof text - 1, -1, owns, NULL);
    assert(out && !strcmp(out, "NewTop = a=b\n  Later  =  SNX  \nk=v\nAgain=1\nnone=2\n"));
    free(out);
    /* nothing to keep, an empty file, one key with no line end */
    out = snx_config_kept("[app]\ntheme=1\n", 14, 0, owns, NULL);
    assert(out && !out[0]);
    free(out);
    out = snx_config_kept("", 0, 0, owns, NULL);
    assert(out && !out[0]);
    free(out);
    out = snx_config_kept("k=1", 3, 1, owns, NULL);
    assert(out && !strcmp(out, "k=1\n"));
    free(out);
    puts("PASS the lines of the keys a reader does not take, as they were and under their sections");
}

int main(void) {
    dir = snx_temp_dir(NULL);
    assert(dir);
    test_files();
    test_said();
    test_kept();
    assert(!snx_remove_tree(dir));
    free(dir);
    return 0;
}
