/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What the JIT loader says when the system refuses its memory (code_loader.h's snx_code_jit_refused): the size it
   asked, the system's reason and what macOS wants of the app, by value; it has a place to end */
#include "common/cpu/recompiler/llvm/code_loader.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    char error[256] = "", small[32];
    char want[64];
    int failed = 0;
    snx_code_jit_refused((size_t)768 << 20, EPERM, error, sizeof error);
    snprintf(want, sizeof want, "768 MiB");
    if (!strstr(error, want) || !strstr(error, strerror(EPERM)) || !strstr(error, "com.apple.security.cs.allow-jit")) {
        fprintf(stderr, "FAIL: %s\n", error);
        failed = 1;
    }
    snx_code_jit_refused((size_t)16 << 20, EINVAL, error, sizeof error);
    if (!strstr(error, "16 MiB") || !strstr(error, strerror(EINVAL))) {
        fprintf(stderr, "FAIL: %s\n", error);
        failed = 1;
    }
    memset(small, 'x', sizeof small);
    snx_code_jit_refused((size_t)16 << 20, EINVAL, small, sizeof small);
    if (memchr(small, 0, sizeof small) == NULL) {
        fprintf(stderr, "FAIL: a message of a small room is not ended\n");
        failed = 1;
    }
    printf("code_loader_refused: %s\n", failed ? "FAILED" : "ok");
    return failed;
}
