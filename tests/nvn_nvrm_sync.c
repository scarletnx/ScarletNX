/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* nvnSyncGetNvRmSyncNVX and nvnSyncInitializeFromNvRmSyncNVX against the game's driver, which reads and writes a sync
   in its own layout (kind at 0x08, fence count at 0x0c, fences {syncpoint, value} from 0x10, 0x40 bytes). The driver's
   code is a stub that sees the sync nvn_object.c built in guest memory */
#include "nx/gpu/nvn/nvn_object.h"
#include "nx/gpu/nvn/nvn_loader.h"
#include "nx/gpu/nvn/nvn_replay.h"
#include "nx/gpu/nvn/nvn_draw.h"
#include "nx/gpu/nvn/nvn_tex.h"
#include "nx/gpu/nvn/nvn_tic.h"
#include "nx/hle/kernel/kernel.h"
#include "nx/runtime/arm.h"
#include "nx/runtime/loader.h"
#include "nx/gpu/nvn/nvn_state.h"
#include "gpu/nvn/nvn.hook.h"
#include "nx/hle/services/nvdrv.h"
#include "nx/runtime/platform/log.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static u8 memory[1 << 16];
u8* g_mem = memory; /* guest address a is memory + a */

/* ---- the driver stub: what it was called with, the sync it saw, and what it answers */
static struct {
    int calls;
    GuestFn fn;
    u64 at, a0, a1, a2, sp;
    u8 saw[0x40];
    u64 ret;   /* its x0 */
    int write; /* write `out` into the sync, as the init does */
    u8 out[0x40];
} drv;

u64 nvn_loader_run_driver(CPU* c, GuestFn fn, u64 at, u64 a0, u64 a1, u64 a2) {
    drv.calls++;
    drv.fn = fn;
    drv.at = at;
    drv.a0 = a0;
    drv.a1 = a1;
    drv.a2 = a2;
    drv.sp = c->x[31];
    memcpy(drv.saw, GP(a0), sizeof drv.saw);
    if (drv.write) memcpy(GP(a0), drv.out, sizeof drv.out);
    return drv.ret;
}
u32 nvn_replay_shown_syncpt(void) { return 9; }

/* ---- what the file needs of the runtime besides: stubs, none reached by the cases below */
void logf_(int lvl, const char* fmt, ...) {
    (void)lvl;
    (void)fmt;
}
void fatal(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "FATAL: ");
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    exit(1);
}

u64 g_code_lo, g_code_hi;
void pm_init(PMutex* m) { (void)m; }
void pm_lock(PMutex* m) { (void)m; }
void pm_unlock(PMutex* m) { (void)m; }
KThread* kthread_cur(void) { return NULL; }
void kthread_block(KThread* t) { (void)t; }
void kthread_unblock(KThread* t) { (void)t; }
void core_enter(KThread* t) { (void)t; }
void core_leave(KThread* t) { (void)t; }
u64 guest_ticks(void) { return 0; }
u64 guest_gpu_ticks(void) { return 0; }
void loader_segments(LoaderSegEach each, void* ctx) {
    (void)each;
    (void)ctx;
}
u32 nvn_api_pack(i32 major, i32 minor) { return (u32)major << 16 | (u32)minor; }
int nvn_driver_info(const u8* mem, u64 lo, u64 hi, u64 func, u32 item, i32* out) {
    (void)mem;
    (void)lo;
    (void)hi;
    (void)func;
    (void)item;
    (void)out;
    return 0;
}
int nvn_driver_info32(const u8* mem, u64 lo, u64 hi, u64 func, u32 item, i32* out) {
    return nvn_driver_info(mem, lo, hi, func, item, out);
}
int arm_code_range(u64* lo, u64* hi) {
    *lo = *hi = 0;
    return 0;
}
u64 gmmu_space_alloc(u64 size, u64 align) {
    (void)size;
    (void)align;
    return 0;
}
void gmem_read_code(u64 gva, void* dst, u64 size) {
    (void)gva;
    memset(dst, 0, size);
}
void gmmu_space_map(u64 gva, u64 cpu, u64 size) {
    (void)gva;
    (void)cpu;
    (void)size;
}
void gmmu_space_unmap(u64 gva, u64 size) {
    (void)gva;
    (void)size;
}
void gmmu_space_free(u64 gva, u64 size) {
    (void)gva;
    (void)size;
}
u32 nvmap_handle_new(u64 addr, u64 size) {
    (void)addr;
    (void)size;
    return 0;
}
void nvmap_handle_drop(u32 h) { (void)h; }
void nvn_cmd_memory_back(u64 memory, u64 size) {
    (void)memory;
    (void)size;
}
void nvn_draw_device(u32 origin, u32 depth_mode) {
    (void)origin;
    (void)depth_mode;
}
int nvn_fmt_bpb(int format, int* bw, int* bh) {
    (void)format;
    *bw = *bh = 1;
    return 0;
}
void nvn_read_texels(u64 t, u64 v, u64 region, u64 dst, i64 row, i64 image) {
    (void)t;
    (void)v;
    (void)region;
    (void)dst;
    (void)row;
    (void)image;
}
void nvn_write_texels(u64 t, u64 v, u64 region, u64 src, i64 row, i64 image) {
    (void)t;
    (void)v;
    (void)region;
    (void)src;
    (void)row;
    (void)image;
}
void nvn_tex_layout(const NvnTexDesc* d, NvnTexLayout* o) {
    (void)d;
    memset(o, 0, sizeof *o);
}
u64 nvn_tex_level_offset(const NvnTexDesc* d, unsigned level) {
    (void)d;
    (void)level;
    return 0;
}
void nvn_tex_level_tiles(const NvnTexDesc* d, unsigned level, unsigned* tile_h, unsigned* tile_d) {
    (void)d;
    (void)level;
    *tile_h = *tile_d = 0;
}
int nvn_tic_encode(const NvnTexture* t, const NvnView* v, int image, u32 api, u32 out[8]) {
    (void)t;
    (void)v;
    (void)image;
    (void)api;
    memset(out, 0, 8 * sizeof *out);
    return 0;
}
void nvn_tsc_encode(const NvnSamplerPar* p, u32 out[8]) {
    (void)p;
    memset(out, 0, 8 * sizeof *out);
}
void nvn_tex_storage(const NvnTexDesc* d, NvnTexStorage* s) {
    (void)d;
    memset(s, 0, sizeof *s);
}
u64 nvn_tex_zcull(const NvnTexDesc* d) {
    (void)d;
    return 0;
}

static int failures;
static void check(int ok, const char* what, ...) {
    if (!ok) {
        va_list ap;
        va_start(ap, what);
        fprintf(stderr, "FAIL: ");
        vfprintf(stderr, what, ap);
        fprintf(stderr, "\n");
        va_end(ap);
        failures++;
    }
}

#define SYNC   0x100u /* the game's NvnSync */
#define NVRM   0x300u /* the game's NvRm sync, which is only passed through */
#define DEVICE 0x500u
#define CPU_SP 0x8000u

static void GetFn(CPU* c, u64 e) {
    (void)c;
    (void)e;
}
static void InitFn(CPU* c, u64 e) {
    (void)c;
    (void)e;
}

static CPU cpu;

static void setup(void) {
    memset(memory, 0, sizeof memory);
    memset(&g_nvn, 0, sizeof g_nvn);
    memset(&drv, 0, sizeof drv);
    memset(&cpu, 0, sizeof cpu);
    cpu.x[31] = CPU_SP;
    g_nvn.loader.nvrm_get = GetFn;
    g_nvn.loader.nvrm_init = InitFn;
    g_nvn.loader.nvrm_get_at = 0x1234;
    g_nvn.loader.nvrm_init_at = 0x5678;
}

static void sync_make(u32 fenced, u32 syncpt, u32 value, u32 shown) {
    NvnSync* s = NVN_OBJ(NvnSync, SYNC);
    memset(s, 0, sizeof *s);
    s->magic = NVN_MAGIC('S', 'Y', 'N', 'C');
    s->fenced = fenced;
    s->syncpt = syncpt;
    s->value = value;
    s->shown = shown;
}

static u32 saw(unsigned off) { return snx_ld_le32(drv.saw + off); }

/* the Get call of one sync, its checks of the kind, the fence count and the arguments */
static int get(const char* what, u32 kind, u32 nfences) {
    int r = nvn_SyncGetNvRmSyncNVX(&cpu, SYNC, NVRM);
    check(drv.calls == 1, "%s: expected 1 driver call, found %d", what, drv.calls);
    check(drv.fn == GetFn && drv.at == 0x1234, "%s: expected the get function at 0x1234, found %p at %#llx", what,
          (void*)drv.fn, (unsigned long long)drv.at);
    check(drv.a1 == NVRM, "%s: expected a1 the NvRm pointer %#x, found %#llx", what, NVRM, (unsigned long long)drv.a1);
    check(saw(8) == kind, "%s: expected kind %u, found %u", what, kind, saw(8));
    check(saw(12) == nfences, "%s: expected %u fences, found %u", what, nfences, saw(12));
    return r;
}

/* 1: an initialized sync that was never fenced has no fences */
static void test_get_unfenced(void) {
    int r;
    setup();
    sync_make(0, 0, 0, 0);
    drv.ret = 1;
    r = get("unfenced", 0, 0);
    check(r == 1, "unfenced: expected the return 1 (bit 0 of the driver's x0), found %d", r);
    drv.calls = 0;
    drv.ret = 0xfffffffe; /* only bit 0 counts */
    r = nvn_SyncGetNvRmSyncNVX(&cpu, SYNC, NVRM);
    check(r == 0, "unfenced, x0 %#x: expected the return 0, found %d", 0xfffffffe, r);
}

/* 2: a fence of the queue's syncpoint */
static void test_get_fenced(void) {
    setup();
    sync_make(1, 5, 100, 0);
    drv.ret = 1;
    get("fenced", 5, 1);
    check(saw(0x10) == 5 && saw(0x14) == 100, "fenced: expected fence 0 {5, 100}, found {%u, %u}", saw(0x10),
          saw(0x14));
}

/* 3: and the presents' syncpoint when the sync waits for a present on the display */
static void test_get_shown(void) {
    setup();
    sync_make(1, 5, 100, 7);
    drv.ret = 1;
    get("fenced and shown", 5, 2);
    check(saw(0x10) == 5 && saw(0x14) == 100, "fenced and shown: expected fence 0 {5, 100}, found {%u, %u}", saw(0x10),
          saw(0x14));
    check(saw(0x18) == 9 && saw(0x1c) == 7, "fenced and shown: expected fence 1 {9, 7}, found {%u, %u}", saw(0x18),
          saw(0x1c));
}

/* 4: the driver's sync lives below the CPU's stack pointer, 16-aligned, and the pointer comes back */
static void test_get_stack(void) {
    setup();
    sync_make(1, 5, 100, 0);
    get("stack", 5, 1);
    check(drv.a0 + 0x40 <= CPU_SP,
          "stack: expected the driver's sync (%#llx, 0x40 bytes) below the SP %#x, found it above",
          (unsigned long long)drv.a0, CPU_SP);
    check((drv.a0 & 15) == 0, "stack: expected the sync 16-aligned, found %#llx", (unsigned long long)drv.a0);
    check(drv.sp == drv.a0, "stack: expected the SP in the call at the sync %#llx, found %#llx",
          (unsigned long long)drv.a0, (unsigned long long)drv.sp);
    check(cpu.x[31] == CPU_SP, "stack: expected the SP back at %#x, found %#llx", CPU_SP,
          (unsigned long long)cpu.x[31]);
}

static void init_out(u32 kind, u32 n, u32 sp0, u32 v0) {
    memset(drv.out, 0, sizeof drv.out);
    snx_st_le32(drv.out + 8, kind);
    snx_st_le32(drv.out + 12, n);
    snx_st_le32(drv.out + 0x10, sp0);
    snx_st_le32(drv.out + 0x14, v0);
    drv.write = 1;
}

/* 5: a sync from NvRm fences: the first fence becomes the NvnSync's */
static void test_init(void) {
    const NvnSync* s = NVN_OBJ(const NvnSync, SYNC);
    int r;
    setup();
    memset(memory + SYNC, 0xee, sizeof(NvnSync)); /* what the game had there is not read */
    init_out(5, 1, 11, 200);
    drv.ret = 1;
    r = nvn_SyncInitializeFromNvRmSyncNVX(&cpu, SYNC, DEVICE, NVRM);
    check(r == 1, "init: expected the return 1, found %d", r);
    check(drv.calls == 1 && drv.fn == InitFn && drv.at == 0x5678,
          "init: expected 1 call of the init function at 0x5678, found %d calls of %p at %#llx", drv.calls,
          (void*)drv.fn, (unsigned long long)drv.at);
    check(drv.a1 == DEVICE, "init: expected a1 the device %#x, found %#llx", DEVICE, (unsigned long long)drv.a1);
    check(drv.a2 == NVRM, "init: expected a2 the NvRm pointer %#x, found %#llx", NVRM, (unsigned long long)drv.a2);
    check(s->magic == NVN_MAGIC('S', 'Y', 'N', 'C'), "init: expected the magic SYNC, found %#x", s->magic);
    check(s->fenced == 1, "init: expected fenced 1, found %u", s->fenced);
    check(s->syncpt == 11, "init: expected syncpt 11, found %u", s->syncpt);
    check(s->value == 200, "init: expected value 200, found %u", s->value);
    check(cpu.x[31] == CPU_SP, "init: expected the SP back at %#x, found %#llx", CPU_SP, (unsigned long long)cpu.x[31]);
}

/* 6: nothing to wait for: an initialized, unfenced sync */
static void test_init_none(void) {
    const NvnSync* s = NVN_OBJ(const NvnSync, SYNC);
    int r;
    setup();
    memset(memory + SYNC, 0xee, sizeof(NvnSync));
    init_out(5, 1, 11, 200);
    drv.ret = 0; /* the driver failed: its fences are not taken */
    r = nvn_SyncInitializeFromNvRmSyncNVX(&cpu, SYNC, DEVICE, NVRM);
    check(r == 0, "init, driver returned 0: expected the return 0, found %d", r);
    check(s->magic == NVN_MAGIC('S', 'Y', 'N', 'C'), "init, driver returned 0: expected the magic SYNC, found %#x",
          s->magic);
    check(s->fenced == 0, "init, driver returned 0: expected fenced 0, found %u", s->fenced);
    setup();
    memset(memory + SYNC, 0xee, sizeof(NvnSync));
    init_out(5, 0, 11, 200);
    drv.ret = 1;
    r = nvn_SyncInitializeFromNvRmSyncNVX(&cpu, SYNC, DEVICE, NVRM);
    check(r == 1, "init, 0 fences: expected the return 1, found %d", r);
    check(s->magic == NVN_MAGIC('S', 'Y', 'N', 'C'), "init, 0 fences: expected the magic SYNC, found %#x", s->magic);
    check(s->fenced == 0, "init, 0 fences: expected fenced 0, found %u", s->fenced);
}

int main(void) {
    test_get_unfenced();
    test_get_fenced();
    test_get_shown();
    test_get_stack();
    test_init();
    test_init_none();
    if (failures) {
        fprintf(stderr, "%d failures\n", failures);
        return 1;
    }
    puts("ok");
    return 0;
}
