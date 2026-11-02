/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* compile_thread.h: a thread made for LLVM's passes takes 4 MiB of recursion, more than the 1 MiB a thread gets by
   default on Android and Windows. With glibc the test sets that default first (its own is 8 MiB), so without the
   helper's stack the test dies of a stack overflow everywhere */
#ifndef _WIN32
#define _GNU_SOURCE /* pthread_setattr_default_np */
#endif
#include "common/cpu/recompiler/compile_thread.h"
#include <stdio.h>
#include <string.h>

#define FRAME 4096u /* bytes each level keeps on the stack */
#define DEPTH 1024u /* 4 MiB of levels */

/* a level's frame holds FRAME bytes the compiler cannot drop, and the call is no tail call */
static unsigned deep(unsigned n) {
    volatile unsigned char frame[FRAME];
    unsigned i;
    for (i = 0; i < FRAME; i += 512) frame[i] = (unsigned char)(n + i);
    return n ? deep(n - 1) + frame[n % (FRAME / 512) * 512] : frame[0];
}

#ifdef _WIN32
static DWORD WINAPI run(LPVOID arg) {
    *(unsigned*)arg = deep(DEPTH);
    return 0;
}
#else
static void* run(void* arg) {
    *(unsigned*)arg = deep(DEPTH);
    return NULL;
}
#endif

int main(void) {
    unsigned result = 0;
#ifdef _WIN32
    HANDLE t = snx_compile_thread(run, &result);
    if (!t) {
        fprintf(stderr, "compile_thread.c: the thread did not start: error %lu\n", (unsigned long)GetLastError());
        return 1;
    }
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
#else
    pthread_t t;
    int rc;
#ifdef __GLIBC__
    pthread_attr_t fallback;
    if (pthread_attr_init(&fallback) || pthread_attr_setstacksize(&fallback, (size_t)1 << 20) ||
        pthread_setattr_default_np(&fallback)) {
        fprintf(stderr, "compile_thread.c: the 1 MiB default stack was not set\n");
        return 1;
    }
    pthread_attr_destroy(&fallback);
#endif
    rc = snx_compile_thread(&t, run, &result);
    if (rc) {
        fprintf(stderr, "compile_thread.c: the thread did not start: %s\n", strerror(rc));
        return 1;
    }
    pthread_join(t, NULL);
#endif
    printf("compile_thread: %u levels of %u bytes on a compile thread of %zu MiB (sum %u)\n", DEPTH, FRAME,
           SNX_RECOMPILER_STACK >> 20, result);
    return 0;
}
