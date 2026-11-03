/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* nn::diag's abort hook (src/nx/hle/hook/diag.c) on a fake abort: the generated adapter called as the SDK's own
   AbortImpl calls VAbortImpl, with its strings and Result in guest memory. The log line holds each value as passed,
   the error code the Result makes, "none" for a null string and "no result" for a null Result; the function's own
   code then runs once, with the registers the call left */
#include "nx/hle/hook/hook.h"
#include "nx/runtime/platform/log.h"
#include "hle/hook/hooks.hook.h"
#include "hle/hook/hooks.hook.inc"
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#define VABORT "_ZN2nn4diag6detail10VAbortImplEPKcS3_S3_iPKNS_6ResultEPKNS_2os17UserExceptionInfoES3_RSt9__va_list"
#define AT     0x8100040u /* where the sdk module exports it */

/* the log's lines, what the function's own code was called with, and the guest's memory */
static struct {
    char log[4096];
    unsigned own_runs;
    u64 own_entry, own_x[8];
    u8 mem[0x10000];
} g_diag_abort;

void logf_(int level, const char* format, ...) {
    size_t n = strlen(g_diag_abort.log);
    va_list args;
    assert(level == 0); /* a warning: the game is about to stop */
    va_start(args, format);
    vsnprintf(g_diag_abort.log + n, sizeof g_diag_abort.log - n, format, args);
    va_end(args);
}
void fatal(const char* format, ...) {
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    exit(1);
}
u64 kernel_find_symbol_exact(const char* name) { return strcmp(name, VABORT) ? 0 : AT; }
/* the function's own code: what it was called with */
static void own(CPU* RESTRICT c, u64 entry) {
    g_diag_abort.own_runs++;
    g_diag_abort.own_entry = entry;
    memcpy(g_diag_abort.own_x, c->x, sizeof g_diag_abort.own_x);
    c->pc = c->x[30];
}
GuestFn hook_original(u64 addr) { return addr == AT ? own : NULL; }
/* hooks.hook's other handlers, which the table names */
u64 hook_memmove(CPU* c, u64 dst, u64 src, u64 size) {
    (void)c, (void)src, (void)size;
    return dst;
}
u64 hook_memset(CPU* c, u64 dst, i32 value, u64 size) {
    (void)c, (void)value, (void)size;
    return dst;
}

static void put(CPU* c, u64 at, const char* s) { memcpy(c->mem + at, s, strlen(s) + 1); }

/* the adapter of the diag hook, from the generated table */
static GuestFn adapter(void) {
    u32 i;
    for (i = 0; i < hook_table.count; i++)
        if (!strcmp(hook_table.symbol[i], VABORT)) return hook_fn[i];
    return NULL;
}

int main(void) {
    CPU c = {0};
    GuestFn fn = adapter();
    assert(fn != NULL);
    c.mem = g_diag_abort.mem;
    put(&c, 0x1000, "result.IsSuccess()");
    put(&c, 0x1100, "ReadFile");
    put(&c, 0x1200, "fs_File.cpp");
    put(&c, 0x1300, "Failed: %s");
    snx_st_le32(g_diag_abort.mem + 0x1400, 0x2EF02); /* module 0x102 (258, its 9th bit set), description 0x177 (375) */
    c.x[0] = 0x1000, c.x[1] = 0x1100, c.x[2] = 0x1200, c.x[3] = 117, c.x[4] = 0x1400, c.x[5] = 0x1500;
    c.x[6] = 0x1300, c.x[7] = 0x1600, c.x[30] = 0x8005123;
    fn(&c, AT);
    printf("%s", g_diag_abort.log);
    assert(strstr(g_diag_abort.log,
                  "nn::diag abort: \"result.IsSuccess()\" failed in ReadFile (fs_File.cpp, line 117) with "
                  "result 0x2ef02 (error 2258-0375), message format \"Failed: %s\""));
    assert(g_diag_abort.own_runs == 1 && g_diag_abort.own_entry == AT && g_diag_abort.own_x[0] == 0x1000 &&
           g_diag_abort.own_x[3] == 117 && g_diag_abort.own_x[4] == 0x1400 && g_diag_abort.own_x[7] == 0x1600);
    assert(c.pc == 0x8005123);
    puts("PASS a fake abort logged with its values, then its own code run with the call's registers");

    g_diag_abort.log[0] = 0;
    c.x[0] = 0, c.x[2] = 0, c.x[3] = 0, c.x[4] = 0, c.x[6] = 0;
    fn(&c, AT);
    printf("%s", g_diag_abort.log);
    assert(strstr(g_diag_abort.log,
                  "nn::diag abort: \"none\" failed in ReadFile (none, line 0) with no result, message format "
                  "\"none\""));
    assert(g_diag_abort.own_runs == 2);
    puts("PASS null strings and no Result said so");
    return 0;
}
