/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L /* getpid and localtime_r, which strict C99 leaves undeclared */
#endif
#include "common/utilities/run_mark.h"
#include "common/utilities/common.h"
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <process.h> /* _getpid */
#else
#include <unistd.h>
#endif

#define RUN_MARK ".running"

/* this process's mark, from snx_run_mark_start to snx_run_mark_end: its path, NULL while there is none (taken at once
   by the end, so that a second end on another thread finds none), its lock, and whether atexit has the end */
static struct {
    char* path;
    intptr_t lock;
    int at_exit;
} g_run_mark;

static long long own_pid(void) {
#ifdef _WIN32
    return (long long)_getpid();
#else
    return (long long)getpid();
#endif
}
/* the pid a mark's name says, .running-<pid>; 0 for .running, an older iOS app's mark, whose text says it; -1 for a
   name no mark has, .running-<pid>.new among them, a mark being made */
static long long name_pid(const char* name) {
    size_t n = sizeof RUN_MARK - 1;
    long long pid = 0;
    if (strncmp(name, RUN_MARK, n)) return -1;
    if (!name[n]) return 0;
    if (name[n] != '-' || !name[n + 1]) return -1;
    for (name += n + 1; *name; ++name) {
        if (*name < '0' || *name > '9' || pid > 999999999ll) return -1;
        pid = pid * 10 + (*name - '0');
    }
    return pid ? pid : -1;
}
/* the mark name of folder, when no program holds it: given to seen, then deleted. One held, and one gone since the
   folder was read (its program ended), are left */
static void stale(const char* folder, const char* name, void (*seen)(void* data, const SnxRunMark* mark), void* data) {
    long long pid = name_pid(name);
    SnxRunMark mark = {0};
    char reason[256], *path, *text = NULL, *left = NULL, *end;
    unsigned char* bytes = NULL;
    size_t size = 0;
    intptr_t lock;
    int held;
    if (pid < 0) return;
    path = snx_path_join(folder, name);
    held = snx_lock_existing(path, &lock);
    if (held > 0 || (held < 0 && errno == ENOENT)) {
        free(path);
        return;
    }
    if (held < 0)
        left = snx_format("%s cannot be opened (%s)", path, snx_os_error(reason, sizeof reason));
    else {
        bytes = snx_read_file(path, &size);
        if (!bytes) text = snx_format("(its text cannot be read: %s)", snx_os_error(reason, sizeof reason));
        if (remove(path)) left = snx_format("%s cannot be deleted (%s)", path, snx_os_error(reason, sizeof reason));
        snx_unlock_file(lock); /* after the delete: until then, no other start takes it too */
    }
    mark.text = held < 0 ? NULL : text ? text : bytes ? (const char*)bytes : "";
    if (bytes) { /* snx_read_file ends what it reads with a 0: its first line, then the time */
        end = strchr((char*)bytes, '\n');
        if (end) {
            *end = 0;
            mark.started = strtoll(end + 1, NULL, 10);
        }
        if (!pid && strncmp((const char*)bytes, "pid ", 4) == 0) pid = strtoll((const char*)bytes + 4, NULL, 10);
    }
    mark.pid = pid;
    mark.left = left;
    seen(data, &mark);
    free(left);
    free(text);
    free(bytes);
    free(path);
}
int snx_run_mark_start(const char* folder, void (*seen)(void* data, const SnxRunMark* mark), void* data, char* why,
                       size_t why_size) {
    char reason[256], when[48] = "at a time the system does not give", **names = NULL, *name, *path, *fresh, *text;
    size_t count = 0, i;
    time_t now = time(NULL);
    struct tm tm;
    intptr_t lock;
    FILE* f;
    int failed;
    if (why_size) why[0] = 0;
    if (snx_run_mark_held()) return 1;
    if (snx_list_dir(folder, &names, &count)) {
        snprintf(why, why_size, "%s cannot be read (%s)", folder, snx_os_error(reason, sizeof reason));
        return -1;
    }
    for (i = 0; i < count; ++i) stale(folder, names[i], seen, data);
    snx_free_strings(names, count);
    name = snx_format(RUN_MARK "-%lld", own_pid());
    path = snx_path_join(folder, name);
    fresh = snx_format("%s.new", path); /* held and written before it takes its name: no start finds it unheld */
    free(name);
    failed = snx_lock_file(fresh, &lock);
    if (failed) {
        snprintf(why, why_size, "%s cannot be made (%s)", fresh,
                 failed > 0 ? "another program holds it" : snx_os_error(reason, sizeof reason));
        free(fresh);
        free(path);
        return -1;
    }
#ifdef _WIN32
    if (!localtime_s(&tm, &now)) strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", &tm);
#else
    if (localtime_r(&now, &tm)) strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", &tm);
#endif
    text = snx_format("pid %lld, started %s\n%lld\n", own_pid(), when, (long long)now);
    f = snx_fopen(fresh, "wb");
    failed = !f || fputs(text, f) < 0;
    if (f && fclose(f)) failed = 1;
    if (failed)
        snprintf(why, why_size, "%s cannot be written (%s)", fresh, snx_os_error(reason, sizeof reason));
    else if (snx_replace(fresh, path)) {
        failed = 1;
        snprintf(why, why_size, "%s cannot be named %s (%s)", fresh, path, snx_os_error(reason, sizeof reason));
    }
    free(text);
    if (failed) {
        remove(fresh);
        snx_unlock_file(lock);
        free(fresh);
        free(path);
        return -1;
    }
    free(fresh);
    g_run_mark.lock = lock;
    __atomic_store_n(&g_run_mark.path, path, __ATOMIC_RELEASE);
    if (!g_run_mark.at_exit) g_run_mark.at_exit = !atexit(snx_run_mark_end);
    return 0;
}
int snx_run_mark_held(void) { return __atomic_load_n(&g_run_mark.path, __ATOMIC_ACQUIRE) != NULL; }
void snx_run_mark_end(void) {
    char reason[256], *path = __atomic_exchange_n(&g_run_mark.path, NULL, __ATOMIC_ACQ_REL);
    if (!path) return;
    /* before the lock goes: a start in between would find it held by no one */
    if (remove(path) && errno != ENOENT)
        fprintf(stderr, "scarletnx: %s cannot be deleted (%s): the next start says this one did not end with an exit\n",
                path, snx_os_error(reason, sizeof reason));
    snx_unlock_file(g_run_mark.lock);
    free(path);
}
