/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The crash report's lines debug.c writes about a thread: its registers, its backtrace and the last SVCs, each with
   the id of the thread that made it. Guest memory and the log are stood in for here */
#include <stdarg.h>
#include <stddef.h>

#include "nx/hle/kernel/debug.c"

#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        if (!(x)) {                                                                                                    \
            fprintf(stderr, "kernel_dump.c:%d: check failed: %s\n", __LINE__, #x);                                     \
            exit(1);                                                                                                   \
        }                                                                                                              \
    } while (0)

/* ============================================================ what debug.c calls */
Hle g_hle;
HleHost g_host;
/* guest memory: [GUEST_LO, GUEST_LO + GUEST_SIZE) mapped as normal memory, the rest of the address space free */
#define GUEST_LO   0x8000000ull
#define GUEST_SIZE 0x10000ull
MemLayout g_layout = {.base = GUEST_LO, .end = AS_MAX};
u8* g_mem;
/* the test's own state: guest memory, and the log of what the dumps write, all of it, for the checks */
static struct {
    u8 guest[GUEST_SIZE];
    char log[16384];
    size_t len;
} g_test;
int mem_reserved(void) { return 1; }
void mem_query(u64 addr, MemBlock* out) {
    memset(out, 0, sizeof *out);
    if (addr < GUEST_LO) {
        out->size = GUEST_LO;
    } else if (addr < GUEST_LO + GUEST_SIZE) {
        out->addr = GUEST_LO;
        out->size = GUEST_SIZE;
        out->state = MS_NORMAL;
        out->perm = PERM_RW;
    } else {
        out->addr = GUEST_LO + GUEST_SIZE;
        out->size = AS_MAX - out->addr;
    }
}
const SnxModule* g_mod;
const SnxArmModule* g_arm_mod;
void pm_lock(PMutex* m) { (void)m; }
void pm_unlock(PMutex* m) { (void)m; }
u64 plat_ns(void) { return 0; }
u64 g_t0_ns(void) { return 0; }
u32 ipc_ring_count(void) { return 0; }
void ipc_ring_text(u32 seq, char* out, size_t n) {
    (void)seq;
    snprintf(out, n, "?");
}
void logf_(int level, const char* format, ...) {
    va_list args;
    int n;
    (void)level;
    va_start(args, format);
    n = vsnprintf(g_test.log + g_test.len, sizeof g_test.log - g_test.len, format, args);
    va_end(args);
    if (n > 0)
        g_test.len += (size_t)n < sizeof g_test.log - g_test.len ? (size_t)n : sizeof g_test.log - g_test.len - 1;
}
static void log_clear(void) {
    g_test.len = 0;
    g_test.log[0] = 0;
}

/* ============================================================ the checks */
#define CALLER 0x8001000ull /* a game function's call */

/* each of the last SVCs with the id of the thread that made it: two threads' calls, as the ring mixes them */
static void test_svc_ring(void) {
    CPU c = {0};
    SvcRec* r = &g_hle.kernel.svc_ring[0];
    r->imm = 0x2d, r->res = 0, r->o1 = 0x40000, r->lr = 0x1071c294, r->tid = 102;
    r->a[0] = 0xaa2d70000ull, r->a[1] = 0x40000, r->a[2] = 0x40000, r->a[3] = 0x17ea5678;
    r = &g_hle.kernel.svc_ring[1];
    r->imm = 0x2c, r->res = 0, r->o1 = 0x23000, r->lr = 0x1071bfc4, r->tid = 87;
    r->a[0] = 0xaa2d70000ull, r->a[1] = 0x23000, r->a[2] = 0x23000, r->a[3] = 0x8ca81c0;
    g_hle.kernel.svc_ring_n = 2;
    c.x[30] = CALLER + 4;
    log_clear();
    kernel_dump_regs(&c);
    CHECK(strstr(g_test.log, "  last SVCs:\n"
                             "    svc 2d (aa2d70000 40000 40000 17ea5678) -> 0, 40000  lr=1071c294  thread 102\n"
                             "    svc 2c (aa2d70000 23000 23000 8ca81c0) -> 0, 23000  lr=1071bfc4  thread 87\n"
                             "  last IPC requests:\n"));
}

int main(void) {
    g_mem = (u8*)((uintptr_t)g_test.guest - GUEST_LO); /* guest address a is g_test.guest[a - GUEST_LO] */
    test_svc_ring();
    printf("kernel_dump: every check passed\n");
    return 0;
}
