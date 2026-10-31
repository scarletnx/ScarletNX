/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* compile_thread.h: a thread made for LLVM's passes takes 16 MiB of recursion, more than a thread gets by default
   (1 MiB on Android and Windows, 8 MiB from glibc): without its stack the test dies of a stack overflow */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "common/cpu/recompiler/compile_thread.h"
#include <stdio.h>
#include <string.h>

#define FRAME 4096u /* bytes each level keeps on the stack */
#define DEPTH 4096u /* 16 MiB of levels */

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
    int rc = snx_compile_thread(&t, run, &result);
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
