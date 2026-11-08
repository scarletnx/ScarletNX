/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Linux: the process's threads of one name, by their nice, to see which of a renderer's pipeline workers took the
   system's low priority (common.h's snx_thread_priority_low) */
#ifndef SNX_TEST_THREAD_NICE_H
#define SNX_TEST_THREAD_NICE_H
#if defined(__linux__)
#include "common/utilities/common.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

/* The threads whose name (/proc's comm, its first 15 bytes) is name: *normal of them at the calling thread's nice,
   *low at SNX_THREAD_LOW_NICE above it (19 at most), *other at any other. 0, or -1 when /proc can't be read */
static int thread_nice_count(const char* name, int* normal, int* low, int* other) {
    char want[16], path[64], comm[32];
    int base, lowered;
    DIR* d = opendir("/proc/self/task");
    struct dirent* e;
    *normal = *low = *other = 0;
    if (!d) return -1;
    snprintf(want, sizeof want, "%s", name);
    errno = 0;
    base = getpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid));
    lowered = base + SNX_THREAD_LOW_NICE < 19 ? base + SNX_THREAD_LOW_NICE : 19;
    while ((e = readdir(d))) {
        FILE* f;
        int nice;
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        snprintf(path, sizeof path, "/proc/self/task/%s/comm", e->d_name);
        if (!(f = fopen(path, "r"))) continue; /* it ended since the listing */
        if (!fgets(comm, sizeof comm, f)) comm[0] = 0;
        fclose(f);
        comm[strcspn(comm, "\n")] = 0;
        if (strcmp(comm, want)) continue;
        errno = 0;
        nice = getpriority(PRIO_PROCESS, (id_t)strtol(e->d_name, NULL, 10));
        if (nice == -1 && errno) continue;
        if (nice == base)
            ++*normal;
        else if (nice == lowered)
            ++*low;
        else
            ++*other;
    }
    closedir(d);
    return 0;
}
/* thread_nice_count until want_normal and want_low of them are at those nices and none at another, for 5 s at most: a
   worker names itself, then lowers itself, as it starts. Whether they came to be */
static int thread_nice_settled(const char* name, int want_normal, int want_low, int* normal, int* low, int* other) {
    for (int i = 0; i < 500; i++) {
        if (thread_nice_count(name, normal, low, other)) return 0;
        if (*normal == want_normal && *low == want_low && !*other) return 1;
        usleep(10000);
    }
    return 0;
}
#endif
#endif
