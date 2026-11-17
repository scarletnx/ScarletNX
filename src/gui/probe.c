/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "gui/probe.h"
#include "common/utilities/common.h"
#include <yyjson.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char* gui_probe_data(const char* folder) { return snx_path_join(folder, SNX_PORTABLE_DIR); }
/* the program's name: its NRO's, which names the game the driver makes of it */
static const char* game(const char* folder) {
    char *amberlith = snx_path_join(folder, GUI_PROBE_NRO), *edenprobe = snx_path_join(folder, GUI_PROBE_OLD_NRO),
         *made = snx_format("%s/%s/recompiled/amberlith", folder, SNX_PORTABLE_DIR),
         *made_old = snx_format("%s/%s/recompiled/edenprobe", folder, SNX_PORTABLE_DIR);
    const char* name;
    snx_path_native(made);
    snx_path_native(made_old);
    /* neither NRO there: the game made of one before, whose report Continue and Last Run read, Amberlith's first */
    if (snx_is_file(amberlith))
        name = "amberlith";
    else if (snx_is_file(edenprobe))
        name = "edenprobe";
    else
        name = snx_is_dir(made) || !snx_is_dir(made_old) ? "amberlith" : "edenprobe";
    free(made_old);
    free(made);
    free(edenprobe);
    free(amberlith);
    return name;
}
char* gui_probe_nro(const char* folder) {
    char* path = snx_path_join(folder, GUI_PROBE_NRO);
    if (!snx_is_file(path)) {
        char* old = snx_path_join(folder, GUI_PROBE_OLD_NRO);
        if (snx_is_file(old)) {
            free(path);
            return old;
        }
        free(old);
    }
    return path;
}
/* the folder of the game's card under the probe's name: sdmc:/amberlith (new 1) or sdmc:/edenprobe */
static char* root(const char* folder, int new) {
    const char* name = game(folder);
    char* path = snx_format("%s/%s/recompiled/%s/%s_data/sdmc/%s", folder, SNX_PORTABLE_DIR, name, name,
                            new ? "amberlith" : "edenprobe");
    snx_path_native(path);
    return path;
}
/* whether root holds a mode file or runs, a run's or a press's */
static int used(const char* root) {
    char *mode = snx_path_join(root, "mode"), *runs = snx_path_join(root, "runs");
    int is = snx_path_exists(mode) || snx_path_exists(runs);
    free(runs);
    free(mode);
    return is;
}
/* the card the probe runs from, as Amberlith 1.0.0 chooses it: sdmc:/amberlith, but sdmc:/edenprobe while only that
   one holds a mode file or runs, so an edenprobe build's run goes on and is read where it is */
static char* card(const char* folder) {
    char *amberlith = root(folder, 1), *edenprobe = root(folder, 0);
    if (!used(amberlith) && used(edenprobe)) {
        free(amberlith);
        return edenprobe;
    }
    free(edenprobe);
    return amberlith;
}
char* gui_probe_report(const char* folder) {
    char *sdmc = card(folder), *last = snx_path_join(sdmc, "last-run"), *report = NULL;
    size_t n = 0;
    char* name = (char*)snx_read_file(last, &n);
    while (name && n && (name[n - 1] == '\n' || name[n - 1] == '\r' || name[n - 1] == ' ')) name[--n] = 0;
    if (name && n) {
        char* run = snx_path_join(sdmc, name);
        report = snx_path_join(run, "report.jsonl");
        free(run);
        if (!snx_is_file(report)) {
            free(report);
            report = NULL;
        }
    }
    free(name);
    free(last);
    free(sdmc);
    return report;
}
/* the file or folder name of the card gone when it is there: 0, or -1 with why */
static int gone(const char* sdmc, const char* name, char* why, size_t size) {
    char *path = snx_path_join(sdmc, name), reason[256];
    int rc = snx_path_exists(path) && snx_remove_tree(path) ? -1 : 0;
    if (rc) snprintf(why, size, "%s cannot be removed: %s", path, snx_os_error(reason, sizeof reason));
    free(path);
    return rc;
}
/* the cases sdmc's crash list holds */
static int crash_count(const char* sdmc) {
    char *path = snx_path_join(sdmc, "crashes"), *text, *line;
    size_t n = 0;
    int cases = 0;
    text = (char*)snx_read_file(path, &n);
    for (line = text; line && *line; line = strchr(line, '\n') ? strchr(line, '\n') + 1 : line + strlen(line))
        cases += *line != '\n';
    free(text);
    free(path);
    return cases;
}
int gui_probe_crashes(const char* folder) {
    char* sdmc = card(folder);
    int cases = crash_count(sdmc);
    free(sdmc);
    return cases;
}
int gui_probe_clear(const char* folder, int* cleared, char* why, size_t size) {
    char* sdmc = card(folder);
    int rc;
    *cleared = gui_probe_crashes(folder);
    rc = gone(sdmc, "crashes", why, size);
    free(sdmc);
    return rc;
}
/* a version the probe's machine= takes: 1 to 32 letters, digits, dots, dashes and underscores */
static int version_word(const char* commit) {
    size_t n = strlen(commit), i;
    if (!n || n > 32 || !strcmp(commit, "unknown")) return 0;
    for (i = 0; i < n; i++)
        if (!isalnum((unsigned char)commit[i]) && !strchr("._-", commit[i])) return 0;
    return 1;
}
/* whether folder's NRO carries text (1, 0), or -1 with why when it cannot be read */
static int nro_knows(const char* folder, const char* text, char* why, size_t why_size) {
    char *nro = gui_probe_nro(folder), *bytes, reason[256];
    size_t n = 0, i, length = strlen(text);
    int knows = 0;
    bytes = (char*)snx_read_file(nro, &n);
    if (!bytes) {
        snprintf(why, why_size, "%s cannot be read: %s", nro, snx_os_error(reason, sizeof reason));
        free(nro);
        return -1;
    }
    for (i = 0; !knows && i + length <= n; i++) knows = !memcmp(bytes + i, text, length);
    free(bytes);
    free(nro);
    return knows;
}
int gui_probe_words(const char* folder, int full, const char* commit, char* words, size_t size, char* why,
                    size_t why_size) {
    int knows = nro_knows(folder, "machine=", why, why_size);
    if (knows < 0) return -1;
    if (!knows)
        snprintf(words, size, "%s", full ? GUI_PROBE_FULL : GUI_PROBE_QUICK);
    else if (version_word(commit))
        snprintf(words, size, "%s machine=scarletnx@%s", full ? GUI_PROBE_FULL : GUI_PROBE_QUICK, commit);
    else
        snprintf(words, size, "%s machine=scarletnx", full ? GUI_PROBE_FULL : GUI_PROBE_QUICK);
    return 0;
}
int gui_probe_start(const char* folder, const char* words, int* cleared, char* why, size_t size) {
    /* the probe's own folder: an edenprobe build reads no other, and Amberlith runs from its own once it holds the
       mode file, whatever sdmc:/edenprobe holds */
    int amberlith = nro_knows(folder, "sdmc:/amberlith", why, size), rc;
    char *sdmc, *mode, *line, reason[256];
    if (amberlith < 0) return -1;
    sdmc = root(folder, amberlith);
    mode = snx_path_join(sdmc, "mode");
    line = snx_format("%s\n", words);
    *cleared = crash_count(sdmc);
    rc = gone(sdmc, "crashes", why, size) || gone(sdmc, "runs", why, size) || gone(sdmc, "last-run", why, size);
    if (!rc && (snx_mkdirs(sdmc) || snx_write_file(mode, line, strlen(line)))) {
        snprintf(why, size, "the mode file %s cannot be written: %s", mode, snx_os_error(reason, sizeof reason));
        rc = -1;
    }
    free(line);
    free(mode);
    free(sdmc);
    return rc ? -1 : 0;
}
static void counted(GuiProbeReport* r, const char* suite, int crashed) {
    int i;
    for (i = 0; i < r->suites && strcmp(r->suite[i].name, suite); i++) {}
    if (i == GUI_PROBE_SUITES) return;
    if (i == r->suites) snprintf(r->suite[r->suites++].name, sizeof r->suite[0].name, "%s", suite);
    if (crashed)
        r->suite[i].crashed++;
    else
        r->suite[i].failed++;
}
int gui_probe_read(const char* folder, GuiProbeReport* r) {
    char *path = gui_probe_report(folder), *text, *line, *end;
    size_t n = 0;
    memset(r, 0, sizeof *r);
    text = path ? (char*)snx_read_file(path, &n) : NULL;
    free(path);
    if (!text) return 0;
    for (line = text; *line; line = end + 1) {
        yyjson_doc* doc;
        yyjson_val* o;
        const char *type, *note;
        if (!(end = strchr(line, '\n'))) break; /* a line still being written */
        doc = yyjson_read(line, (size_t)(end - line), 0);
        o = yyjson_doc_get_root(doc);
        type = yyjson_get_str(yyjson_obj_get(o, "type"));
        note = yyjson_get_str(yyjson_obj_get(o, "text"));
        if (type && !strcmp(type, "note") && note && !r->build[0] && strstr(note, ") starting")) {
            /* the name the probe went by when it was built: edenprobe before 1.0.0 */
            static const char* const names[] = {"Amberlith ", "edenprobe "};
            for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
                size_t length = strlen(names[i]);
                if (strncmp(note, names[i], length)) continue;
                snprintf(r->name, sizeof r->name, "%.*s", (int)length - 1, names[i]);
                snprintf(r->build, sizeof r->build, "%.*s", (int)(strstr(note, ") starting") + 1 - note - length),
                         note + length);
            }
        }
        if (type && !strcmp(type, "case")) {
            const char *suite = yyjson_get_str(yyjson_obj_get(o, "suite")),
                       *result = yyjson_get_str(yyjson_obj_get(o, "result")),
                       *reason = yyjson_get_str(yyjson_obj_get(o, "reason"));
            r->cases++;
            if (suite && result && !strcmp(result, "fail")) counted(r, suite, 0);
            if (suite && result && !strcmp(result, "skip") && reason && !strcmp(reason, "crashed"))
                counted(r, suite, 1);
        }
        if (type && !strcmp(type, "summary")) {
            r->summary = 1;
            r->passed = (long)yyjson_get_sint(yyjson_obj_get(o, "passed"));
            r->failed = (long)yyjson_get_sint(yyjson_obj_get(o, "failed"));
            r->skipped = (long)yyjson_get_sint(yyjson_obj_get(o, "skipped"));
            r->crashed = (long)yyjson_get_sint(yyjson_obj_get(o, "crashed"));
        }
        yyjson_doc_free(doc);
    }
    free(text);
    return 1;
}
/* the suites with failures (crashed 0) or crashes after at in out: " (astc 15, tex8k 2)", the most first, then by name */
static size_t suites(const GuiProbeReport* r, int crashed, char* out, size_t at, size_t size) {
    int listed = 0, last = -1, i;
    const char* prev = NULL;
    for (;;) {
        int best = -1, most = 0;
        for (i = 0; i < r->suites; i++) {
            int n = crashed ? r->suite[i].crashed : r->suite[i].failed;
            /* after the last listed: fewer, or as many and later by name */
            if (!n || (prev && (n > last || (n == last && strcmp(r->suite[i].name, prev) <= 0)))) continue;
            if (best < 0 || n > most || (n == most && strcmp(r->suite[i].name, r->suite[best].name) < 0)) {
                best = i;
                most = n;
            }
        }
        if (best < 0 || at >= size) break;
        at += (size_t)snprintf(out + at, size - at, "%s%s %d", listed++ ? ", " : " (", r->suite[best].name, most);
        last = most;
        prev = r->suite[best].name;
    }
    if (listed && at < size) at += (size_t)snprintf(out + at, size - at, ")");
    return at;
}
void gui_probe_said(const GuiProbeReport* r, char* out, size_t size) {
    size_t at =
        (size_t)snprintf(out, size, "%s %s: ", r->name[0] ? r->name : "Amberlith", r->build[0] ? r->build : "?");
    if (!r->summary) {
        if (at < size)
            snprintf(out + at, size - at, "no summary, %d case%s reported", r->cases, r->cases == 1 ? "" : "s");
        return;
    }
    if (at < size) at += (size_t)snprintf(out + at, size - at, "%ld passed, %ld failed", r->passed, r->failed);
    at = suites(r, 0, out, at, size);
    if (at < size) at += (size_t)snprintf(out + at, size - at, ", %ld skipped, %ld crashed", r->skipped, r->crashed);
    suites(r, 1, out, at, size);
}
