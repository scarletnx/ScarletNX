/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "gui/probe.h"
#include "common/utilities/common.h"
#include <yyjson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char* gui_probe_data(const char* folder) { return snx_path_join(folder, SNX_PORTABLE_DIR); }
char* gui_probe_nro(const char* folder) { return snx_path_join(folder, "edenprobe.nro"); }
/* the card of the game made in folder: sdmc:/edenprobe */
static char* card(const char* folder) {
    char* path = snx_format("%s/%s/recompiled/edenprobe/edenprobe_data/sdmc/edenprobe", folder, SNX_PORTABLE_DIR);
    snx_path_native(path);
    return path;
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
int gui_probe_crashes(const char* folder) {
    char *sdmc = card(folder), *path = snx_path_join(sdmc, "crashes"), *text, *line;
    size_t n = 0;
    int cases = 0;
    text = (char*)snx_read_file(path, &n);
    for (line = text; line && *line; line = strchr(line, '\n') ? strchr(line, '\n') + 1 : line + strlen(line))
        cases += *line != '\n';
    free(text);
    free(path);
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
int gui_probe_start(const char* folder, int full, int* cleared, char* why, size_t size) {
    char *sdmc = card(folder), *mode = snx_path_join(sdmc, "mode"), reason[256];
    const char* words = full ? GUI_PROBE_FULL "\n" : GUI_PROBE_QUICK "\n";
    int rc = gui_probe_clear(folder, cleared, why, size) || gone(sdmc, "runs", why, size) ||
             gone(sdmc, "last-run", why, size);
    if (!rc && (snx_mkdirs(sdmc) || snx_write_file(mode, words, strlen(words)))) {
        snprintf(why, size, "the mode file %s cannot be written: %s", mode, snx_os_error(reason, sizeof reason));
        rc = -1;
    }
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
        if (type && !strcmp(type, "note") && note && !r->build[0] && !strncmp(note, "edenprobe ", 10) &&
            strstr(note, ") starting"))
            snprintf(r->build, sizeof r->build, "%.*s", (int)(strstr(note, ") starting") + 1 - note - 10), note + 10);
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
    size_t at = (size_t)snprintf(out, size, "edenprobe %s: ", r->build[0] ? r->build : "?");
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
