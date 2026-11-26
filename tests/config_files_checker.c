/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* tests/config_files.h's runner itself: a reader that follows the rule passes every situation, and each way a reader
   can break it (a key lost, its value said, a key not said or said twice, a line or a repeat not said, the first line
   taken, a bad value not said, a file not there said, one that cannot be read not said or written over, a write in
   place, a secret others read taken) fails the situations it breaks, and only those */
#include "config_files.h"
#include "common/utilities/ini_lines.h"
#include "common/platform/file.h"
#include <assert.h>

enum {
    FINE,
    DROPS_UNKNOWN,
    SAYS_VALUE,
    SILENT_UNKNOWN,
    SAYS_TWICE,
    REFUSES_UNKNOWN,
    SILENT_STRAY,
    FIRST_WINS,
    SILENT_REPEAT,
    SILENT_BAD,
    SAYS_ABSENT,
    SILENT_UNREADABLE,
    WRITES_OVER,
    WRITES_IN_PLACE,
    TAKES_READABLE,
    DEFECTS
};
static int g_defect, g_secret; /* the file it reads holds a secret */

static int owns(void* ctx, const char* section, const char* key) {
    (void)ctx;
    (void)section;
    return !strcmp(key, "a");
}
static void stray(void* ctx, int line, const char* text, const char* does) {
    (void)text;
    (void)does;
    snx_config_other_line((SnxConfigOthers*)ctx, line);
}
/* whether users other than its owner may read path: never on Windows, whose files have no such bits; apart, so
   g_secret is read on every platform (-Wunused-but-set-global) */
static int others_read(const char* path) {
#ifndef _WIN32
    struct stat st;
    return !stat(path, &st) && (st.st_mode & 077);
#else
    (void)path;
    return 0;
#endif
}
/* a=0 to 9, its default 5: the rule's reader */
static int fake_load(const char* dir, char* state, size_t size) {
    char *path = snx_path_join(dir, "fake.ini"), *text, *key, *value, why[1024], said[1024], first[16] = "";
    size_t n;
    int a = 5, rc = 0;
    SnxIni ini;
    SnxConfigOthers others = {0};
    int read = snx_config_read(path, &text, &n, why, sizeof why);
    if (read < 0 && g_defect != SILENT_UNREADABLE) config_say("%s", why);
    if (read == 0 && g_defect == SAYS_ABSENT) config_say("%s is not there", path);
    if (g_secret && read > 0 && others_read(path) && g_defect != TAKES_READABLE) {
        config_say("%s can be read by others: chmod 600 it", path);
        free(text);
        free(path);
        return 1;
    }
    if (read > 0) {
        snx_ini_start(&ini, text);
        ini.left = stray;
        ini.ctx = &others;
        while (snx_ini_next(&ini, &key, &value)) {
            if (strcmp(key, "a")) {
                if (g_defect == REFUSES_UNKNOWN) rc = 1;
                if (g_defect == SAYS_VALUE) config_say("%s=%s is no key", key, value);
                snx_config_other_key(&others, ini.section, key, ini.line);
                continue;
            }
            if (first[0] && g_defect != SILENT_REPEAT) config_say("a is given twice: %s, then %s", first, value);
            if (first[0] && g_defect == FIRST_WINS) continue;
            snprintf(first, sizeof first, "%s", value);
            if (value[0] >= '0' && value[0] <= '9' && !value[1])
                a = value[0] - '0';
            else if (g_defect != SILENT_BAD)
                config_say("a=%s is no digit: it keeps %d", value, a);
        }
        if (g_defect == SILENT_STRAY) others.lines = 0;
        if (g_defect == SILENT_UNKNOWN) others.keys = 0;
        if (g_defect == SAYS_TWICE) snx_config_forget();
        if (snx_config_others_said(&others, path, said, sizeof said)) config_say("%s", said);
    }
    snprintf(state, size, "a %d", a);
    free(text);
    free(path);
    return rc;
}
static int fake_save(const char* dir) {
    char *path = snx_path_join(dir, "fake.ini"), *text, *top, *kept, *out, why[1024];
    size_t n;
    int rc;
    if (snx_config_read(path, &text, &n, why, sizeof why) < 0 && g_defect != WRITES_OVER) {
        config_say("%s", why);
        free(path);
        return 1;
    }
    top = g_defect == DROPS_UNKNOWN ? snx_strdup("") : snx_config_kept(text ? text : "", n, 1, owns, NULL);
    kept = g_defect == DROPS_UNKNOWN ? snx_strdup("") : snx_config_kept(text ? text : "", n, 0, owns, NULL);
    out = snx_format("%sa=7\n%s", top, kept);
    if (g_defect == WRITES_IN_PLACE) {
        rc = snx_write_file(path, out, strlen(out));
        if (rc) config_say("%s could not be written", path);
    } else if (g_defect == WRITES_OVER) { /* beside it and moved over it, whether it was read or not */
        char* staged = snx_format("%s.new", path);
        rc = snx_write_file(staged, out, strlen(out)) || plat_replace_file(staged, path);
        if (rc) config_say("%s could not be written", path);
        free(staged);
    } else {
        rc = snx_config_write(path, ".new", out, strlen(out), why, sizeof why);
        if (rc) config_say("%s", why);
    }
    free(out);
    free(kept);
    free(top);
    free(text);
    free(path);
    return rc;
}

/* what each defect breaks */
static int breaks(int defect, int s) {
    switch (defect) {
        case DROPS_UNKNOWN: return s == CONFIG_UNKNOWN_KEY || s == CONFIG_UNKNOWN_SECTION;
        case SAYS_VALUE: return s == CONFIG_UNKNOWN_KEY || s == CONFIG_UNKNOWN_SECTION;
        case SILENT_UNKNOWN: return s == CONFIG_UNKNOWN_KEY || s == CONFIG_UNKNOWN_SECTION;
        case SAYS_TWICE: return s == CONFIG_UNKNOWN_KEY || s == CONFIG_UNKNOWN_SECTION;
        case REFUSES_UNKNOWN: return s == CONFIG_UNKNOWN_KEY || s == CONFIG_UNKNOWN_SECTION;
        case SILENT_STRAY: return s == CONFIG_NO_EQUALS;
        case FIRST_WINS: return s == CONFIG_REPEATED;
        case SILENT_REPEAT: return s == CONFIG_REPEATED;
        case SILENT_BAD: return s == CONFIG_BAD_VALUE;
        case SAYS_ABSENT: return s == CONFIG_ABSENT;
        case SILENT_UNREADABLE: return s == CONFIG_UNREADABLE || s == CONFIG_CLOSED;
        case WRITES_OVER: return s == CONFIG_CLOSED; /* a folder in its place is never moved over */
        case WRITES_IN_PLACE: return s == CONFIG_WRITE_FAILS;
        case TAKES_READABLE: return s == CONFIG_SECRET_READABLE;
    }
    return 0;
}

int main(void) {
    static const ConfigFile fake = {.id = "fake",
                                    .name = "fake.ini",
                                    .seed = "a=3\n",
                                    .newer = "a=3\nnewer_key=" CONFIG_SECRET "\n",
                                    .newer_line = "newer_key=" CONFIG_SECRET,
                                    .newer_key = "newer_key",
                                    .section = "a=3\n[newer]\nsection_key=" CONFIG_SECRET "\n",
                                    .section_head = "[newer]",
                                    .section_line = "section_key=" CONFIG_SECRET,
                                    .section_key = "section_key",
                                    .stray = "a=3\nstray " CONFIG_SECRET "\n",
                                    .stray_line = 2,
                                    .repeated = "a=2\na=3\n",
                                    .repeated_key = "a is given twice",
                                    .repeated_first = "2",
                                    .repeated_last = "3",
                                    .repeated_as = "a=3\n",
                                    .bad = "a=x\n",
                                    .bad_given = "a=x",
                                    .bad_kept = "keeps 5",
                                    .load = fake_load,
                                    .save = fake_save,
                                    .staged = ".new",
                                    .secret = 0};
    ConfigFile secret = fake;
    char why[2048];
    int here, ran = 0;
    secret.secret = 1;
    secret.id = "fake secret";
    g_config_files.dir = snx_temp_dir(NULL);
    assert(g_config_files.dir);
    snx_error_sink(config_sink);
    for (g_defect = FINE; g_defect < DEFECTS; ++g_defect)
        for (int s = 0; s < CONFIG_SITUATIONS; ++s) {
            const ConfigFile* f = s == CONFIG_SECRET_READABLE ? &secret : &fake;
            if (config_na(f, s, &here)) continue;
            g_secret = f->secret;
            config_check(f, s, why, sizeof why);
            if (!why[0] == !breaks(g_defect, s)) continue;
            fprintf(stderr, "config_files_checker: defect %d, %s: %s\n", g_defect, config_situation_names[s],
                    why[0] ? why : "held, and the defect breaks it");
            return 1;
        }
    for (int s = 0; s < CONFIG_SITUATIONS; ++s) ran += !config_na(&fake, s, &here) || here;
    snx_error_sink(NULL);
    snx_remove_tree(g_config_files.dir);
    free(g_config_files.dir);
    printf(
        "config_files_checker: a reader that follows the rule passes the %d situations, and each of %d defects fails "
        "the ones it breaks alone\n",
        ran, DEFECTS - 1);
    return 0;
}
