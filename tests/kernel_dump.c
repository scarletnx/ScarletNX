/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The crash report's lines debug.c writes about a thread: its registers, its backtrace and the last SVCs, each with
   the id of the thread that made it, and its last call when that went to a hooked memcpy, memmove or memset and the
   fault lies in the range the call copies or fills. That call is the one before the thread's lr: a BL to the
   function, to a PLT stub or a branch that leads to it, or a BLR. Guest memory, the hooks' sites and the log are
   stood in for here */
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
/* the hooks' sites: memcpy runs hook_memmove, memset hook_memset */
#define SITE_MEMCPY 0x8004000ull
#define SITE_MEMSET 0x8004100ull
#define NOT_HOOKED  0x8004200ull
const char* hook_at(u64 addr, const char** handler) {
    if (addr == SITE_MEMCPY) return *handler = "hook_memmove", "memcpy";
    if (addr == SITE_MEMSET) return *handler = "hook_memset", "memset";
    return NULL;
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

/* ============================================================ guest code */
static void put32(u64 a, u32 v) {
    CHECK(a >= GUEST_LO && a + 4 <= GUEST_LO + GUEST_SIZE);
    snx_st_le32(GP(a), v);
}
static void put64(u64 a, u64 v) {
    CHECK(a >= GUEST_LO && a + 8 <= GUEST_LO + GUEST_SIZE);
    snx_st_le64(GP(a), v);
}
static u32 bl(u64 at, u64 to) { return 0x94000000u | (u32)(((to - at) >> 2) & 0x3FFFFFFu); }
static u32 b(u64 at, u64 to) { return 0x14000000u | (u32)(((to - at) >> 2) & 0x3FFFFFFu); }
/* the static linker's PLT stub at at for the GOT slot at got: adrp x16; ldr x17, [x16, #off]; add x16, x16, #off;
   br x17 */
static void plt(u64 at, u64 got) {
    u64 pages = (got >> 12) - (at >> 12), off = got & 0xFFF;
    put32(at, 0x90000010u | (u32)(pages & 3) << 29 | (u32)((pages >> 2) & 0x7FFFF) << 5);
    put32(at + 4, 0xF9400211u | (u32)(off / 8) << 10);
    put32(at + 8, 0x91000210u | (u32)off << 10);
    put32(at + 12, 0xD61F0220u);
}

/* ============================================================ the checks */
#define CALLER 0x8001000ull /* a game function's call */
#define STUB   0x8002000ull /* its PLT stub */
#define GOT    0x8003008ull /* the stub's GOT slot */
#define BRANCH 0x8002100ull /* a plain branch to memcpy */
#define BUF    0xaa2d70000ull

/* the copy #1585's reports point at: 0x29fe0 bytes from a block that has 0x23000 mapped, faulting at +0x29fc0 */
static void test_copy(void) {
    CPU c = {0};
    put32(CALLER, bl(CALLER, STUB));
    plt(STUB, GOT);
    put64(GOT, SITE_MEMCPY);
    c.x[0] = 0x1c0000000ull, c.x[1] = BUF, c.x[2] = 0x29fe0, c.x[30] = CALLER + 4;
    log_clear();
    kernel_dump_libc_fault(&c, BUF + 0x29fc0);
    CHECK(!strcmp(g_test.log, "  its last call: memcpy (the runtime's hook_memmove) at 8001000, 0x29fe0 bytes from "
                              "aa2d70000 to 1c0000000; the fault is in the source, 0x29fc0 bytes in\n"));
    /* a fault past both ranges is another access */
    log_clear();
    kernel_dump_libc_fault(&c, BUF + 0x29fe0);
    kernel_dump_libc_fault(&c, BUF - 1);
    kernel_dump_libc_fault(&c, 0x1c0000000ull + 0x29fe0);
    CHECK(!g_test.len);
    /* the destination, by a BL to the function itself */
    put32(CALLER, bl(CALLER, SITE_MEMCPY));
    log_clear();
    kernel_dump_libc_fault(&c, 0x1c0000000ull + 0x23000);
    CHECK(strstr(g_test.log,
                 "0x29fe0 bytes from aa2d70000 to 1c0000000; the fault is in the destination, 0x23000 bytes "
                 "in\n"));
    /* through a plain branch */
    put32(CALLER, bl(CALLER, BRANCH));
    put32(BRANCH, b(BRANCH, SITE_MEMCPY));
    log_clear();
    kernel_dump_libc_fault(&c, BUF);
    CHECK(strstr(g_test.log, "; the fault is in the source, 0x0 bytes in\n"));
    /* a copy between overlapping ranges, as memmove makes */
    c.x[0] = BUF + 0x1000;
    log_clear();
    kernel_dump_libc_fault(&c, BUF + 0x2000);
    CHECK(strstr(g_test.log,
                 "0x29fe0 bytes from aa2d70000 to aa2d71000; the fault is in the source and the destination, "
                 "0x2000 bytes in\n"));
    /* a PLT stub whose GOT slot holds a function no hook runs */
    put32(CALLER, bl(CALLER, STUB));
    put64(GOT, NOT_HOOKED);
    log_clear();
    kernel_dump_libc_fault(&c, BUF);
    CHECK(!g_test.len);
}

/* memset by a function pointer (BLR x8), and the calls that name nothing */
static void test_fill_and_others(void) {
    CPU c = {0};
    put32(CALLER, 0xD63F0100u); /* blr x8 */
    c.x[0] = BUF, c.x[1] = 0, c.x[2] = 0x40000, c.x[8] = SITE_MEMSET, c.x[30] = CALLER + 4;
    log_clear();
    kernel_dump_libc_fault(&c, BUF + 0x23000);
    CHECK(!strcmp(g_test.log, "  its last call: memset (the runtime's hook_memset) at 8001000, 0x40000 bytes at "
                              "aa2d70000; the fault is 0x23000 bytes in\n"));
    /* a fill has no source: x1 is the value it writes */
    c.x[1] = BUF + 0x100000;
    log_clear();
    kernel_dump_libc_fault(&c, BUF + 0x100000);
    CHECK(!g_test.len);
    /* a function no hook runs */
    c.x[8] = NOT_HOOKED;
    log_clear();
    kernel_dump_libc_fault(&c, BUF + 0x23000);
    CHECK(!g_test.len);
    /* lr after another instruction than a call, and lr where nothing is mapped */
    put32(CALLER, 0xD503201Fu); /* nop */
    c.x[8] = SITE_MEMSET;
    log_clear();
    kernel_dump_libc_fault(&c, BUF + 0x23000);
    c.x[30] = GUEST_LO + GUEST_SIZE + 0x1000;
    kernel_dump_libc_fault(&c, BUF + 0x23000);
    CHECK(!g_test.len);
}

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
    test_copy();
    test_fill_and_others();
    test_svc_ring();
    printf("kernel_dump: every check passed\n");
    return 0;
}
