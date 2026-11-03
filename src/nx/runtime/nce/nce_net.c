/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The net (nce_patch.h's nce_net_filter) under one thread of the host's process, as Linux's seccomp filters are per
   thread: each guest thread takes it as it starts the game's code, and the runtime's own threads run their system
   calls with no filter of ours */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE 1 /* syscall */
#endif
#include "nx/runtime/nce/nce_patch.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__linux__)
#include <errno.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>

int nce_net_filters(void) {
    char line[128];
    int n = -1;
    FILE* f = fopen("/proc/thread-self/status", "r");
    if (!f) return -1;
    while (fgets(line, sizeof line, f))
        if (!strncmp(line, "Seccomp_filters:", 16)) {
            n = atoi(line + 16);
            break;
        }
    fclose(f);
    return n;
}

int nce_net_install(SnxU64 lo, SnxU64 hi, SnxU32 arch, int base, char* why, size_t size) {
    NceBpf prog[NCE_NET_WORDS];
    struct sock_fprog fp;
    int have = nce_net_filters();
    /* a thread inherits the filters of the thread that starts it: a guest thread started by another has the net */
    if (base >= 0 && have > base) return 1;
    fp.len = (unsigned short)nce_net_filter(lo, hi, arch, prog);
    fp.filter = (struct sock_filter*)(void*)prog;
    /* an unprivileged thread takes a filter only with no_new_privs, set on it alone: a setuid program that a guest
       thread starts gains nothing, and the runtime's threads keep theirs */
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0)) {
        snprintf(why, size, "prctl(PR_SET_NO_NEW_PRIVS) failed: %s", strerror(errno));
        return -1;
    }
    if (syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, 0, &fp)) {
        snprintf(why, size, "seccomp(SECCOMP_SET_MODE_FILTER) failed: %s", strerror(errno));
        return -1;
    }
    return 0;
}
#else
int nce_net_filters(void) { return -1; }
int nce_net_install(SnxU64 lo, SnxU64 hi, SnxU32 arch, int base, char* why, size_t size) {
    (void)lo, (void)hi, (void)arch, (void)base;
    snprintf(why, size, "the host is not Linux, whose seccomp filters the net is");
    return -1;
}
#endif
