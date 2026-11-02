/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "gui/log.h"
#include "common/utilities/common.h"
#include "common/utilities/logscan.h"
#include <errno.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* the folder, which the menu's thread sets and the dumps' icons' thread reads: under a spin lock, a copy at a time */
static struct {
    atomic_flag lock;
    char folder[2048];
} g_gui_log = {ATOMIC_FLAG_INIT, ""};

static void lock(void) {
    while (atomic_flag_test_and_set_explicit(&g_gui_log.lock, memory_order_acquire)) {}
}
static void unlock(void) { atomic_flag_clear_explicit(&g_gui_log.lock, memory_order_release); }
void gui_log_folder(const char* folder) {
    lock();
    snprintf(g_gui_log.folder, sizeof g_gui_log.folder, "%s", folder ? folder : "");
    unlock();
}
void gui_log_console_name(char* out, size_t size) {
    char day[16] = "";
    time_t now = time(NULL);
    struct tm tm;
#ifdef _WIN32
    if (!localtime_s(&tm, &now)) strftime(day, sizeof day, "%Y-%m-%d", &tm);
#else
    if (localtime_r(&now, &tm)) strftime(day, sizeof day, "%Y-%m-%d", &tm);
#endif
    snprintf(out, size, "scarletnx-console-%s.log", day);
}
void gui_log_path(const char* file, char* out, size_t size) {
    char today[64];
    if (!strcmp(file, SNX_CONSOLE_LOG)) {
        gui_log_console_name(today, sizeof today);
        file = today;
    }
    lock();
    if (g_gui_log.folder[0]) {
        char* path = snx_path_join(g_gui_log.folder, file);
        snprintf(out, size, "%s", path);
        free(path);
    } else if (size)
        out[0] = 0;
    unlock();
}
/* a day's launcher log, gui_log_console_name's */
static int console_day(const char* name) {
    static const char shape[] = "scarletnx-console-0000-00-00.log";
    size_t i;
    if (strlen(name) != sizeof shape - 1) return 0;
    for (i = 0; i < sizeof shape - 1; ++i)
        if (shape[i] == '0' ? name[i] < '0' || name[i] > '9' : name[i] != shape[i]) return 0;
    return 1;
}
static int by_name(const void* a, const void* b) { return strcmp(*(char* const*)a, *(char* const*)b); }
/* the launcher's logs of folder down to the GUI_LOG_DAYS newest days; on stderr each that could not go */
static void console_prune(const char* folder) {
    char** names = NULL;
    size_t count = 0, i, days = 0;
    if (snx_list_dir(folder, &names, &count)) return;
    qsort(names, count, sizeof *names, by_name);
    for (i = count; i-- > 0;) {
        char* path;
        if (!console_day(names[i]) || ++days <= GUI_LOG_DAYS) continue;
        path = snx_path_join(folder, names[i]);
        if (remove(path) && errno != ENOENT) { /* another launcher's day may have taken it first */
            char why[256];
            fprintf(stderr, "scarletnx: %s could not be deleted (%s), past the %d days of logs kept\n", path,
                    snx_os_error(why, sizeof why), GUI_LOG_DAYS);
        }
        free(path);
    }
    snx_free_strings(names, count);
}
/* text in the log named file of the folder, when there is one; a day's first line of the launcher's log takes the
   days past GUI_LOG_DAYS away */
static void log_to(const char* file, const char* text) {
    char path[sizeof g_gui_log.folder + 64];
    int day_first;
    gui_log_path(file, path, sizeof path);
    if (!path[0]) return;
    day_first = !strcmp(file, SNX_CONSOLE_LOG) && !snx_path_exists(path);
    if (snx_log_lines(path, text)) {
        char why[256];
        fprintf(stderr, "scarletnx: %s could not be written (%s): %s\n", path, snx_os_error(why, sizeof why), text);
    } else if (day_first) {
        char* folder = snx_dirname(path);
        if (folder) console_prune(folder);
        free(folder);
    }
}
void gui_log(const char* format, ...) {
    va_list args, again;
    char* text;
    int n;
    va_start(args, format);
    va_copy(again, args);
    n = vsnprintf(NULL, 0, format, args);
    text = (char*)snx_alloc(n > 0 ? (size_t)n + 1 : 1);
    vsnprintf(text, n > 0 ? (size_t)n + 1 : 1, format, again);
    va_end(again);
    va_end(args);
#ifndef SNX_IOS /* there stderr is the log's file */
    fprintf(stderr, "scarletnx: %s\n", text);
#endif
    log_to(SNX_CONSOLE_LOG, text);
    free(text);
}
void gui_log_fatal(const char* text) {
    gui_log("%s", text);
    log_to(SNX_ERROR_LOG, text);
}
int gui_log_game_end(const char* path, char* what, size_t size, char* lines, size_t lines_size) {
    SnxLogScan scan;
    char why[256];
    size_t n = 0;
    int outcome;
    FILE* f = snx_fopen(path, "rb");
    if (lines_size) lines[0] = 0;
    if (!f) {
        snprintf(what, size, "%s did not open: %s", path, snx_os_error(why, sizeof why));
        return -1;
    }
    if (snx_logscan_file(&scan, f)) {
        snx_logscan_free(&scan);
        fclose(f);
        snprintf(what, size, "%s could not be read whole", path);
        return -1;
    }
    fclose(f);
    outcome = scan.outcome;
    if (scan.fatal)
        snprintf(what, size, "%s", snx_logscan_text(&scan, scan.fatal));
    else if (scan.signal)
        snprintf(what, size, "%s (signal %s)", snx_logscan_outcome_name(outcome), snx_logscan_text(&scan, scan.signal));
    else
        snprintf(what, size, "%s", snx_logscan_outcome_name(outcome));
    for (int i = 0; i < scan.ntail && n < lines_size; ++i) {
        int w = snprintf(lines + n, lines_size - n, "%s\n", snx_logscan_text(&scan, scan.tail[i]));
        n = w < 0 ? lines_size : n + (size_t)w;
    }
    snx_logscan_free(&scan);
    return outcome;
}
