/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "gui/last_run.h"
#include "common/utilities/common.h"
#include "common/utilities/ini_lines.h"
#include "common/platform/file.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char* gui_last_run_path(const char* run_log) {
    size_t n = run_log ? strlen(run_log) : 0;
    if (n < 4 || strcmp(run_log + n - 4, ".log")) return NULL;
    return snx_format("%.*s.ini", (int)(n - 4), run_log);
}
/* a value as the record keeps it: a line, with no line break of its own */
static int one_line(const char* value) { return !value || !strpbrk(value, "\r\n"); }
int gui_last_run_write(const char* path, const GuiLastRun* run, char* why, size_t size) {
    char reason[256], *fresh, *text;
    int failed;
    if (!one_line(run->program) || !one_line(run->name) || !one_line(run->probe)) {
        snprintf(why, size, "%s is not written: a path or a name of the run holds a line break", path);
        return -1;
    }
    text = snx_format("; the run the menu started last, until it sees it end (docs/user/gui.md, Last Run)\n"
                      "started = %lld\nname = %s\n%s%s%s%s%s%s",
                      run->started, run->name ? run->name : "", run->program ? "program = " : "",
                      run->program ? run->program : "", run->program ? "\n" : "", run->probe ? "probe = " : "",
                      run->probe ? run->probe : "", run->probe ? "\n" : "");
    if (run->probe) {
        char* more = snx_format("%scases = %d\nlaunch = %d\n", text, run->cases, run->launch);
        free(text);
        text = more;
    }
    fresh = snx_format("%s.new", path);
    failed = snx_write_file(fresh, text, strlen(text));
    if (failed)
        snprintf(why, size, "%s cannot be written (%s)", fresh, snx_os_error(reason, sizeof reason));
    else if (plat_replace_file(fresh, path)) {
        failed = 1;
        snprintf(why, size, "%s cannot be named %s (%s)", fresh, path, snx_os_error(reason, sizeof reason));
        remove(fresh);
    }
    free(fresh);
    free(text);
    return failed ? -1 : 0;
}
int gui_last_run_take(const char* path, GuiLastRun* run, char* why, size_t size) {
    char reason[256], *text, *key, *value;
    size_t bytes = 0;
    int started = 0;
    SnxIni ini;
    memset(run, 0, sizeof *run);
    run->cases = -1;
    if (!snx_path_exists(path)) return 0;
    text = (char*)snx_read_file(path, &bytes);
    if (!text) {
        snprintf(why, size, "%s cannot be read (%s)", path, snx_os_error(reason, sizeof reason));
        return -1;
    }
    snx_ini_start(&ini, text);
    while (snx_ini_next(&ini, &key, &value)) {
        if (!strcmp(key, "started")) {
            char* end = NULL;
            run->started = strtoll(value, &end, 10);
            started = end && end != value && !*end && run->started > 0;
        } else if (!strcmp(key, "name"))
            run->name = snx_strdup(value);
        else if (!strcmp(key, "program"))
            run->program = snx_strdup(value);
        else if (!strcmp(key, "probe"))
            run->probe = snx_strdup(value);
        else if (!strcmp(key, "cases"))
            run->cases = atoi(value);
        else if (!strcmp(key, "launch"))
            run->launch = atoi(value);
    }
    free(text);
    if (remove(path)) {
        snprintf(why, size, "%s cannot be deleted (%s): the next start says this run again", path,
                 snx_os_error(reason, sizeof reason));
        return -1;
    }
    if (!started) {
        snprintf(why, size, "%s holds no started = with the run's start in seconds since 1970", path);
        return -1;
    }
    return 1;
}
void gui_last_run_free(GuiLastRun* run) {
    free(run->program);
    free(run->name);
    free(run->probe);
    memset(run, 0, sizeof *run);
}

void gui_system_end_read(const char* text, GuiSystemEnd* end) {
    char *copy = text ? snx_strdup(text) : NULL, *line, *next;
    memset(end, 0, sizeof *end);
    end->reason = -1;
    end->pss = end->rss = -1;
    for (line = copy; line && *line; line = next) {
        char* eq;
        next = strchr(line, '\n');
        if (next)
            *next++ = 0;
        else
            next = line + strlen(line);
        eq = strchr(line, '=');
        if (!eq) continue;
        *eq++ = 0;
        if (!strcmp(line, "none"))
            snprintf(end->none, sizeof end->none, "%s", eq);
        else if (!strcmp(line, "reason")) {
            end->reason = atoi(eq);
            end->known = 1;
        } else if (!strcmp(line, "said"))
            snprintf(end->said, sizeof end->said, "%s", eq);
        else if (!strcmp(line, "process"))
            snprintf(end->process, sizeof end->process, "%s", eq);
        else if (!strcmp(line, "status"))
            end->status = atoi(eq);
        else if (!strcmp(line, "importance"))
            snprintf(end->importance, sizeof end->importance, "%s", eq);
        else if (!strcmp(line, "pss"))
            end->pss = strtoll(eq, NULL, 10);
        else if (!strcmp(line, "rss"))
            end->rss = strtoll(eq, NULL, 10);
        else if (!strcmp(line, "description"))
            snprintf(end->description, sizeof end->description, "%s", eq);
    }
    free(copy);
    if (!end->known && !end->none[0])
        snprintf(end->none, sizeof end->none, "%s",
                 text ? "the system's answer names no reason" : "the system does not say why a process ended");
}
int gui_system_stopped(int reason) {
    switch (reason) {
        case 3:  /* low memory */
        case 6:  /* ANR */
        case 8:  /* permission change */
        case 9:  /* excessive resource usage */
        case 12: /* dependency died */
        case 13: /* other kills by the system */
        case 14: /* freezer */
        case 15: /* package state change */
        case 16: /* package updated */ return 1;
        default: return 0;
    }
}

long long gui_oom_kills_in(const char* text) {
    const char* at = text;
    size_t n = strlen("oom_kill ");
    while (at && *at) {
        if (!strncmp(at, "oom_kill ", n)) return strtoll(at + n, NULL, 10);
        at = strchr(at, '\n');
        if (at) ++at;
    }
    return -1;
}
#ifdef __linux__
/* the count in the file at path, -1 when it cannot be read or names none */
static long long kills_of(const char* path) {
    size_t size = 0;
    char* text = (char*)snx_read_file(path, &size);
    long long kills = text ? gui_oom_kills_in(text) : -1;
    free(text);
    return kills;
}
#endif
long long gui_oom_kills(const char* given, char* file, size_t size) {
#ifdef __linux__
    size_t bytes = 0;
    char *cgroup = given ? NULL : (char*)snx_read_file("/proc/self/cgroup", &bytes), *at, *events = NULL;
    long long kills = -1;
    /* cgroup v2: the line "0::/user.slice/...", the unified hierarchy's path */
    for (at = cgroup; at && *at && !events; at = strchr(at, '\n') ? strchr(at, '\n') + 1 : NULL)
        if (!strncmp(at, "0::/", 4)) {
            const char* end = strchr(at, '\n');
            int n = end ? (int)(end - at - 3) : (int)strlen(at + 3);
            events = snx_format("/sys/fs/cgroup%.*s%smemory.events", n, at + 3, n > 1 ? "/" : "");
        }
    free(cgroup);
    if (given) events = snx_strdup(given);
    if (events) kills = kills_of(events);
    if (kills >= 0)
        snprintf(file, size, "%s", events);
    else {
        kills = kills_of("/proc/vmstat");
        snprintf(file, size, "%s", kills >= 0 ? "/proc/vmstat" : "");
    }
    free(events);
    return kills;
#else
    (void)given;
    snprintf(file, size, "%s", "");
    return -1;
#endif
}
