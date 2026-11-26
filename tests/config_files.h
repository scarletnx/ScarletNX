/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Every settings file the project writes and reads, each through the same situations, against the one rule of
   docs/user/settings.md ("How a settings file is read"; src/common/utilities/config_file.h): a key, a section or a line it
   does not know changes nothing it reads, is named once a run by its name alone, and the key stays in the file; a
   repeated key takes its last line's value and says so; a value that does not fit is said with the value in effect; a
   file that is not there says nothing; one that cannot be read is said and never written over; a file with a secret
   that others can read is refused; a write that fails leaves the old file whole.
   Each tests/config_files_<group>.c gives its files (ConfigFile) and calls config_files_main. An adapter only reports
   what its reader did; the rule lives here alone. A situation a file does not have is said with why (na), one the rule
   leaves to the file is said with why (exempt, red once the file passes). tests/architecture.py's check_config_files
   keeps tests/config_files.json, every reader and writer of a settings file in src/, in step */
#ifndef SNX_TEST_CONFIG_FILES_H
#define SNX_TEST_CONFIG_FILES_H
#include "common/utilities/common.h"
#include "common/utilities/config_file.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

enum {
    CONFIG_UNKNOWN_KEY,
    CONFIG_UNKNOWN_SECTION,
    CONFIG_NO_EQUALS,
    CONFIG_REPEATED,
    CONFIG_BAD_VALUE,
    CONFIG_ABSENT,
    CONFIG_UNREADABLE,
    CONFIG_CLOSED,
    CONFIG_SECRET_READABLE,
    CONFIG_WRITE_FAILS,
    CONFIG_SITUATIONS
};
static const char* const config_situation_names[CONFIG_SITUATIONS] = {
    "unknown-key", "unknown-section", "no-equals", "repeated",        "bad-value",
    "absent",      "unreadable",      "closed",    "secret-readable", "write-fails"};

/* the word no message may carry: the value of what a reader does not know, which may be a secret */
#define CONFIG_SECRET "SNXSECRET"

typedef struct {
    const char* id;   /* as tests/config_files.json names it */
    const char* name; /* its path in the folder load and save are given */
    int secret;       /* it holds one: its values and lines are never said, and others may not read it */
    const char* seed; /* a file it takes whole */
    /* the seed with a key another version has where that version would write it, its value CONFIG_SECRET, and that line
       as the file must keep it; the same in a section of its own (both lines kept: the head NULL where a key names its
       setting whatever its section, and only its line is) */
    const char *newer, *newer_line, *newer_key;
    const char *section, *section_head, *section_line, *section_key;
    /* the seed with a line that has no = (CONFIG_SECRET in it), on line stray_line */
    const char* stray;
    int stray_line;
    /* the seed with a key of its own given twice, repeated_first then repeated_last, and the seed with the last alone */
    const char *repeated, *repeated_key, *repeated_first, *repeated_last, *repeated_as;
    /* the seed with a value of its own that does not fit in place of its own, and what the message names: the value
       given and the one in effect (none for a file that refuses it) */
    const char *bad, *bad_given, *bad_kept;
    /* the file in dir read: what it took into state (its own words), 0, or 1 for refused. What it says goes to config_say */
    int (*load)(const char* dir, char* state, size_t size);
    /* a value of its own changed and the file written, as its writer does: 0, or nonzero, said. NULL for a reader */
    int (*save)(const char* dir);
    const char* staged; /* the file save writes beside it first (".new") */
    /* why a key read is not kept by a write: a record taken out once read, its next one written whole. NULL for keeps */
    const char* consumed;
    /* why a situation is not this file's (na) or is the file's own rule (exempt), NULL for neither */
    const char* na[CONFIG_SITUATIONS];
    const char* exempt[CONFIG_SITUATIONS];
} ConfigFile;

static struct {
    char said[16384];
    char* dir;
} g_config_files;
/* what a reader says: its log, its message, its note */
static void config_say(const char* fmt, ...) {
    size_t at = strlen(g_config_files.said);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_config_files.said + at, sizeof g_config_files.said - at, fmt, ap);
    va_end(ap);
    at = strlen(g_config_files.said);
    if (at + 1 < sizeof g_config_files.said && at && g_config_files.said[at - 1] != '\n')
        strcat(g_config_files.said, "\n");
}
static void config_sink(const char* line) { config_say("%s", line); }

static char* config_path(const char* dir, const ConfigFile* f) { return snx_path_join(dir, f->name); }
static void config_put(const char* dir, const ConfigFile* f, const char* text) {
    char *path = config_path(dir, f), *folder = snx_dirname(path);
    if (snx_mkdirs(folder) || snx_write_file(path, text, strlen(text))) {
        fprintf(stderr, "config_files: %s cannot be made\n", path);
        exit(2);
    }
#ifndef _WIN32
    if (f->secret) chmod(path, 0600);
#endif
    free(folder);
    free(path);
}
/* the file as it is now, owned; NULL for none */
static char* config_text(const char* dir, const ConfigFile* f) {
    size_t size;
    char *path = config_path(dir, f), *text = (char*)snx_read_file(path, &size);
    free(path);
    return text;
}
/* how many times line is a whole line of text */
static int config_lines(const char* text, const char* line) {
    size_t n = strlen(line);
    int count = 0;
    for (const char* at = text; at && (at = strstr(at, line)) != NULL; at += n)
        if ((at == text || at[-1] == '\n') && (at[n] == '\n' || at[n] == '\r' || !at[n])) ++count;
    return count;
}
static int config_load(const ConfigFile* f, const char* dir, char* state, size_t size) {
    state[0] = 0;
    return f->load(dir, state, size);
}
/* the state the seed with text in its place gives */
static void config_state_of(const ConfigFile* f, const char* text, char* state, size_t size) {
    char* dir = snx_path_join(g_config_files.dir, "of");
    snx_remove_tree(dir);
    config_put(dir, f, text);
    snx_config_forget();
    config_load(f, dir, state, size);
    snx_remove_tree(dir);
    free(dir);
}

/* one situation of f in a folder of its own: "" when it holds, else what does not */
static void config_check(const ConfigFile* f, int s, char* why, size_t size) {
    char *dir = snx_path_join(g_config_files.dir, config_situation_names[s]), *path, *text, *held;
    char before[4096], state[4096], *said = g_config_files.said;
    int refused, rc;
    why[0] = 0;
    snx_remove_tree(dir);
    snx_mkdirs(dir);
    path = config_path(dir, f);
    snx_config_forget();
    said[0] = 0;
    config_state_of(f, f->seed, before, sizeof before);
    said[0] = 0;
    switch (s) {
        case CONFIG_UNKNOWN_KEY:
        case CONFIG_UNKNOWN_SECTION: {
            const char* key = s == CONFIG_UNKNOWN_KEY ? f->newer_key : f->section_key;
            config_put(dir, f, s == CONFIG_UNKNOWN_KEY ? f->newer : f->section);
            refused = config_load(f, dir, state, sizeof state);
            if (refused)
                snprintf(why, size, "refused: %s", said);
            else if (strcmp(state, before))
                snprintf(why, size, "it read \"%s\", where the file without the key gives \"%s\"", state, before);
            else if (!strstr(said, key))
                snprintf(why, size, "%s is not named: \"%s\"", key, said);
            else if (strstr(said, CONFIG_SECRET))
                snprintf(why, size, "its value is said: \"%s\"", said);
            if (why[0]) break;
            said[0] = 0;
            config_load(f, dir, state, sizeof state);
            if (strstr(said, key)) {
                snprintf(why, size, "%s is named again in the same run: \"%s\"", key, said);
                break;
            }
            if (!f->save || f->consumed) break;
            said[0] = 0;
            rc = f->save(dir);
            text = config_text(dir, f);
            if (rc)
                snprintf(why, size, "the write failed: %s", said);
            else if (strstr(said, CONFIG_SECRET))
                snprintf(why, size, "the write says its value: \"%s\"", said);
            else if (config_lines(text, s == CONFIG_UNKNOWN_KEY ? f->newer_line : f->section_line) != 1 ||
                     (s == CONFIG_UNKNOWN_SECTION && f->section_head && !config_lines(text, f->section_head)))
                snprintf(why, size, "the file written again does not keep it as it was: \"%s\"",
                         text ? text : "(none)");
            free(text);
            break;
        }
        case CONFIG_NO_EQUALS: {
            char line[32];
            snprintf(line, sizeof line, "line %d", f->stray_line);
            config_put(dir, f, f->stray);
            refused = config_load(f, dir, state, sizeof state);
            if (refused)
                snprintf(why, size, "refused: %s", said);
            else if (strcmp(state, before))
                snprintf(why, size, "it read \"%s\", where the file without the line gives \"%s\"", state, before);
            else if (!strstr(said, line))
                snprintf(why, size, "its %s is not said: \"%s\"", line, said);
            else if (f->secret && strstr(said, CONFIG_SECRET))
                snprintf(why, size, "a secret file's line is said: \"%s\"", said);
            break;
        }
        case CONFIG_REPEATED: {
            char last[4096];
            config_state_of(f, f->repeated_as, last, sizeof last);
            said[0] = 0;
            config_put(dir, f, f->repeated);
            refused = config_load(f, dir, state, sizeof state);
            if (refused)
                snprintf(why, size, "refused: %s", said);
            else if (strcmp(state, last))
                snprintf(why, size, "it read \"%s\", not the last line's \"%s\"", state, last);
            else if (!strstr(said, f->repeated_key))
                snprintf(why, size, "%s given twice is not said: \"%s\"", f->repeated_key, said);
            else if (!f->secret && (!strstr(said, f->repeated_first) || !strstr(said, f->repeated_last)))
                snprintf(why, size, "the two values, %s and %s, are not said: \"%s\"", f->repeated_first,
                         f->repeated_last, said);
            else if (f->secret && (strstr(said, f->repeated_first) || strstr(said, f->repeated_last)))
                snprintf(why, size, "a secret file's values are said: \"%s\"", said);
            break;
        }
        case CONFIG_BAD_VALUE:
            config_put(dir, f, f->bad);
            refused = config_load(f, dir, state, sizeof state);
            if (!strstr(said, f->bad_given))
                snprintf(why, size, "the value given, %s, is not said: \"%s\"", f->bad_given, said);
            else if (!refused && !strstr(said, f->bad_kept))
                snprintf(why, size, "the value in effect, %s, is not said: \"%s\"", f->bad_kept, said);
            break;
        case CONFIG_ABSENT:
            refused = config_load(f, dir, state, sizeof state);
            if (refused)
                snprintf(why, size, "refused: %s", said);
            else if (said[0])
                snprintf(why, size, "a file that is not there is said: \"%s\"", said);
            break;
        case CONFIG_UNREADABLE:
            held = snx_path_join(path, "held");
            snx_mkdirs(path);
            snx_write_file(held, "held", 4);
            config_load(f, dir, state, sizeof state);
            if (!strstr(said, "cannot be read") || !strstr(said, snx_path_leaf(f->name)))
                snprintf(why, size, "a folder in its place is not said, by name, as what cannot be read: \"%s\"", said);
            else if (f->save && !f->save(dir))
                snprintf(why, size, "a write over it was taken");
            else if (!snx_is_dir(path) || !snx_is_file(held))
                snprintf(why, size, "a write over it took it away");
            free(held);
            break;
        case CONFIG_CLOSED:
#ifndef _WIN32
            config_put(dir, f, f->seed);
            chmod(path, 0);
            config_load(f, dir, state, sizeof state);
            if (!strstr(said, "cannot be read") || !strstr(said, snx_path_leaf(f->name)))
                snprintf(why, size, "a file it cannot read is not said, by name, as one: \"%s\"", said);
            else if (f->save && !f->save(dir))
                snprintf(why, size, "a write over it was taken");
            chmod(path, 0600);
            text = config_text(dir, f);
            if (!why[0] && (!text || strcmp(text, f->seed)))
                snprintf(why, size, "a write over it changed it: \"%s\"", text ? text : "(none)");
            free(text);
#endif
            break;
        case CONFIG_SECRET_READABLE:
#ifndef _WIN32
            config_put(dir, f, f->seed);
            chmod(path, 0644);
            refused = config_load(f, dir, state, sizeof state);
            if (!refused || !strstr(said, "chmod 600"))
                snprintf(why, size, "a secret others can read is taken: \"%s\"", said);
#endif
            break;
        case CONFIG_WRITE_FAILS: {
            char* staged = snx_format("%s%s", path, f->staged);
            held = snx_path_join(staged, "held");
            config_put(dir, f, f->seed);
            snx_mkdirs(staged);
            snx_write_file(held, "held", 4);
            if (!f->consumed) config_load(f, dir, state, sizeof state); /* a record taken out as it is read is not */
            said[0] = 0;
            rc = f->save(dir);
            text = config_text(dir, f);
            if (!rc)
                snprintf(why, size, "a write that cannot be made is said done");
            else if (!text || strcmp(text, f->seed))
                snprintf(why, size, "the file is not whole as it was: \"%s\"", text ? text : "(none)");
            else if (!strstr(said, snx_path_leaf(f->name)))
                snprintf(why, size, "the failure does not name the file: \"%s\"", said);
            free(text);
            free(held);
            free(staged);
            break;
        }
    }
    for (char* c = why; *c; ++c)
        if (*c == '\n') *c = ' ';
#ifndef _WIN32
    chmod(path, 0700);
#endif
    snx_remove_tree(dir);
    free(path);
    free(dir);
}

/* why the situation is not the file's (f->na, *here 0), or cannot be run on this system (*here 1): NULL for neither */
static const char* config_na(const ConfigFile* f, int s, int* here) {
    *here = 0;
    if (f->na[s]) return f->na[s];
    if ((s == CONFIG_WRITE_FAILS) && !f->save) return "it is only read";
    if (s == CONFIG_SECRET_READABLE && !f->secret) return "it holds no secret";
    *here = 1;
#ifdef _WIN32
    if (s == CONFIG_CLOSED || s == CONFIG_SECRET_READABLE) return "Windows has no file modes";
#else
    if (s == CONFIG_CLOSED && !geteuid()) return "root reads a file nobody may";
#endif
    return NULL;
}
/* each situation of each file: 0 when every one holds, is not the file's or is its own rule; else 1 */
[[maybe_unused]] static int config_files_run(const ConfigFile* files, size_t count) {
    char why[2048];
    int failed = 0;
    g_config_files.dir = snx_temp_dir(NULL);
    if (!g_config_files.dir) return 2;
    snx_error_sink(config_sink);
    for (size_t i = 0; i < count; ++i)
        for (int s = 0; s < CONFIG_SITUATIONS; ++s) {
            const ConfigFile* f = &files[i];
            int here;
            const char *na = config_na(f, s, &here), *name = config_situation_names[s];
            if (na) {
                printf("%s %s: na, %s\n", f->id, name, na);
                continue;
            }
            config_check(f, s, why, sizeof why);
            if (!why[0] && f->exempt[s]) {
                printf("FAIL %s %s: holds now: take its exemption out\n", f->id, name);
                failed = 1;
            } else if (!why[0])
                printf("%s %s: ok\n", f->id, name);
            else if (f->exempt[s])
                printf("%s %s: exempt, %s (%s)\n", f->id, name, f->exempt[s], why);
            else {
                printf("FAIL %s %s: %s\n", f->id, name, why);
                failed = 1;
            }
        }
    snx_error_sink(NULL);
    snx_remove_tree(g_config_files.dir);
    free(g_config_files.dir);
    return failed;
}
#endif
