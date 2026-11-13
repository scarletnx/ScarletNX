/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The page cache's reads (gpu/vk/pagespace.c) over a model of what it uses, with no device: guest memory, a write
   tracking that knows which pages the CPU wrote and when, the ring a piece of which may be refused, and the region's
   sparse buffer as the bytes its copies put there. The CPU writes between the epochs of a stream, ranges are asked
   for, and each range the cache serves holds guest memory's bytes: in a granule first read, on a page written since,
   in a long range and a short one, after a piece of the ring was refused in the middle of a range, after a granule
   gave its memory back, and after every page was forgotten. CI's driver has too small a sparse address space for the
   cache, so no other test of it runs there. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#pragma GCC diagnostic ignored "-Wmissing-field-initializers" /* pagespace.c builds without -Wextra */
#include "nx/gpu/vk/pagespace.c"

#define CHECK(c)                                                                                                       \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                               \
            exit(1);                                                                                                   \
        }                                                                                                              \
    } while (0)

/* ------------------------------------------------ guest memory, the write tracking, the ring, the region */
#define SPACE   (8u << 20) /* the guest memory there is, at the start of region 0 */
#define NPAGES  (SPACE >> 12)
#define GVA     0x7000000000ull /* its GPU address: one run */
#define STAGE   (16u << 20)
#define REGION0 ((VkBuffer)(uintptr_t)0x9A6E)
#define RING    ((VkBuffer)(uintptr_t)0x5157)
static struct {
    u8 mem[SPACE];         /* guest memory */
    u8 region[SPACE];      /* the region's buffer, where a granule has memory */
    u8 bound[SPACE >> 16]; /* the granules with memory */
    u32 wrote[NPAGES];     /* the clock at a page's last write by the CPU */
    u32 clock, epoch;
    u64 sub;
    u8 stage[STAGE];
    u64 stage_used;
    u32 stage_refuse; /* the piece of the ring refused, counted down: 1 refuses the next */
    u32 before;       /* vk_present_before's answer */
    u32 copies, binds, unbinds;
} g_t;

VkState g_vk;
OwnState g_own;
u8* g_mem = g_t.mem;
u64 g_as_size = 1ull << 39;
MemLayout g_layout;
u32 g_vk_present = 1;
u64 g_vk_drawn, g_vk_dispatches;
PFN_vkCreateBuffer vkCreateBuffer;
PFN_vkGetBufferMemoryRequirements vkGetBufferMemoryRequirements;
PFN_vkDestroyBuffer vkDestroyBuffer;

void logf_(int lvl, const char* fmt, ...) { (void)lvl, (void)fmt; }
void fatal(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    exit(2);
}
int vk_ok_(VkResult r, const char* what, const char* file, int line) {
    (void)what, (void)file, (void)line;
    return r == VK_SUCCESS;
}
uint64_t plat_ns(void) { return 0; }
int vk_uma(void) { return 0; }
u64 gmmu_run(u64 gva, u64 max, u64* cpu) {
    if (gva < GVA || gva - GVA >= SPACE) return 0;
    *cpu = gva - GVA;
    return max < SPACE - *cpu ? max : SPACE - *cpu;
}
/* the tracking's clock moves at each reading and at each write, so "since" is never the moment of a write */
u32 vk_ww_now(void) { return ++g_t.clock; }
u32 vk_ww_epoch(void) { return g_t.epoch; }
int vk_written_since(const u8* host, u64 len, u32 since) {
    for (u64 p = (u64)(host - g_t.mem) >> 12, e = (u64)(host - g_t.mem + len - 1) >> 12; p <= e; p++)
        if (g_t.wrote[p] > since) return 1;
    return 0;
}
void vk_written_runs(const u8* host, u64 len, u32 since, void (*fn)(void*, u64, u64), void* user) {
    u64 first = (u64)(host - g_t.mem) >> 12, n = (len + 4095) >> 12;
    for (u64 p = 0; p < n; p++) {
        u64 from = p;
        while (p < n && g_t.wrote[first + p] > since) p++;
        if (p > from) fn(user, from << 12, (p - from) << 12);
    }
}
/* no GPU writes guest memory here, and nothing is owned */
u64 gpu_mem_generation(void) { return 1; }
int gpu_mem_dirty_since(u64 since, u64 a, u64 sz) {
    (void)since, (void)a, (void)sz;
    return 0;
}
void gpu_mem_dirty_runs(u64 since, u64 a, u64 sz, void (*fn)(void* ctx, u64 at, u64 n), void* ctx) {
    (void)since, (void)a, (void)sz, (void)fn, (void)ctx;
}
void gpu_mem_written_shader(u64 gva, u64 size) { (void)gva, (void)size; }
u32 own_lookup(u64 addr, u64 size, OwnRange* out, u32 max) {
    (void)addr, (void)size, (void)out, (void)max;
    return 0;
}
int own_trapped(u64 addr, u64 size) {
    (void)addr, (void)size;
    return 0;
}
void own_acquire(u64 addr, u64 size) { (void)addr, (void)size; }
VkOwnRes* vk_own_res_borrow(VkBuffer buf) {
    (void)buf;
    return NULL;
}
void vk_own_res_unref(VkOwnRes* r) { (void)r; }
void vk_own_res_wrote(VkOwnRes* r) { (void)r; }
void vk_own_publish(u64 cpu, u64 size, VkOwnRes* r, u64 off) { (void)cpu, (void)size, (void)r, (void)off; }
void vk_mem_track(DevMem a, u32 owner) { (void)a, (void)owner; }
u64 vk_submit_number(void) { return g_t.sub; }
u32 vk_present_before(u64 ago_ns, u32 presents) {
    (void)ago_ns, (void)presents;
    return g_t.before;
}
VkCommandBuffer vk_cmd(void) { return (VkCommandBuffer)(uintptr_t)0xC0; }
VkCommandBuffer vk_cmd_upload(int gpu) {
    (void)gpu;
    return (VkCommandBuffer)(uintptr_t)0xC1;
}
void vk_render_end_at(const char* file, int line) { (void)file, (void)line; }
void vk_all_barrier(VkCommandBuffer cb) { (void)cb; }
void rec_CmdPipelineBarrier2(VkCommandBuffer cb, const VkDependencyInfo* d) { (void)cb, (void)d; }
/* a copy from the ring into the region lands at once: a granule without memory takes nothing */
void rec_CmdCopyBuffer(VkCommandBuffer cb, VkBuffer src, VkBuffer dst, u32 n, const VkBufferCopy* r) {
    (void)cb;
    CHECK(src == RING && dst == REGION0);
    for (u32 i = 0; i < n; i++) {
        CHECK(r[i].size && r[i].srcOffset + r[i].size <= g_t.stage_used && r[i].dstOffset + r[i].size <= SPACE);
        for (u64 g = r[i].dstOffset >> 16; g <= (r[i].dstOffset + r[i].size - 1) >> 16; g++) CHECK(g_t.bound[g]);
        memcpy(g_t.region + r[i].dstOffset, g_t.stage + r[i].srcOffset, r[i].size);
        g_t.copies++;
    }
}
u64 vk_stage_align(void) { return 64; }
u8* vk_stage_upload(u64 size, u64 align, VkBuffer* buf, u64* off) {
    u64 at = (g_t.stage_used + align - 1) & ~(align - 1);
    if (g_t.stage_refuse && !--g_t.stage_refuse) return NULL;
    CHECK(size && at + size <= STAGE);
    g_t.stage_used = at + size;
    *buf = RING;
    *off = at;
    return g_t.stage + at;
}
VkResult vk_devmem_alloc(const VkMemoryRequirements* req, const DevMemWant* want, DevMem* mem, DevMemInfo* info) {
    static DevMem last;
    (void)want;
    CHECK(req->size == 65536);
    *mem = ++last;
    *info = (DevMemInfo){.memory = (VkDeviceMemory)(uintptr_t)*mem, .size = req->size};
    return VK_SUCCESS;
}
void vk_devmem_free(DevMem mem) { (void)mem; }
/* the slabs' calls: the model's device has sparse buffers, so the cache makes no slab here */
VkResult vk_buffer_create(const VkBufferCreateInfo* info, const DevMemWant* allocation, VkBuffer* buffer, DevMem* memory,
                          DevMemInfo* allocation_info) {
    (void)info, (void)allocation, (void)buffer, (void)memory, (void)allocation_info;
    CHECK(!"a slab made on a device with sparse buffers");
    return VK_ERROR_FEATURE_NOT_PRESENT;
}
void vk_defer_buffer(VkBuffer b, DevMem a) {
    (void)b, (void)a;
    CHECK(!"a slab retired on a device with sparse buffers");
}
/* a granule's memory bound, or (gone) given back: what it held is lost */
u64 vk_buffer_sparse_bind(VkBuffer buf, const VkSparseMemoryBind* bind, DevMem gone, u64 size, int readers) {
    (void)size, (void)readers;
    u64 g = bind->resourceOffset >> 16;
    CHECK(buf == REGION0 && bind->size == 65536 && !(bind->resourceOffset & 65535) && g < SPACE >> 16);
    CHECK(g_t.bound[g] == (gone != 0) && (bind->memory != VK_NULL_HANDLE) == !gone);
    g_t.bound[g] = !gone;
    if (gone) {
        memset(g_t.region + (g << 16), 0xEE, 65536);
        g_t.unbinds++;
    } else
        g_t.binds++;
    return 1;
}
static VkResult VKAPI_CALL create_buffer(VkDevice dev, const VkBufferCreateInfo* ci, const VkAllocationCallbacks* cb,
                                         VkBuffer* out) {
    (void)dev, (void)cb;
    CHECK(ci->size == 1ull << 30 && (ci->flags & VK_BUFFER_CREATE_SPARSE_BINDING_BIT));
    *out = REGION0;
    return VK_SUCCESS;
}
static void VKAPI_CALL buffer_requirements(VkDevice dev, VkBuffer buf, VkMemoryRequirements* req) {
    (void)dev, (void)buf;
    *req = (VkMemoryRequirements){1ull << 30, 65536, 1};
}
static void VKAPI_CALL destroy_buffer(VkDevice dev, VkBuffer buf, const VkAllocationCallbacks* cb) {
    (void)dev, (void)buf, (void)cb;
}

/* ------------------------------------------------ the checks */
static u32 g_seed = 0x2545f491u;
static u32 rnd(void) {
    g_seed ^= g_seed << 13;
    g_seed ^= g_seed >> 17;
    g_seed ^= g_seed << 5;
    return g_seed;
}
/* the CPU writes [at, at + n): new bytes, and its pages written now */
static void cpu_write(u64 at, u64 n) {
    for (u64 i = 0; i < n; i++) g_t.mem[at + i] = (u8)rnd();
    for (u64 p = at >> 12; p <= (at + n - 1) >> 12; p++) g_t.wrote[p] = ++g_t.clock;
}
/* the next submission of a stream's job, and its next job: the CPU's writes so far are what its checks look for */
static void next_submission(void) {
    g_t.sub++;
    g_t.stage_used = 0;
}
static void next_epoch(void) {
    g_t.epoch++;
    next_submission();
}
/* [at, at + n) asked for: 1 when the cache served it, with guest memory's bytes */
static int get(u64 at, u64 n) {
    VkBuffer buf = VK_NULL_HANDLE;
    u64 off = ~0ull;
    int got = vk_pages_get(GVA + at, n, 1, &buf, &off);
    if (got <= 0) return 0;
    CHECK(buf == REGION0 && off == at);
    for (u64 g = at >> 16; g <= (at + n - 1) >> 16; g++) CHECK(g_t.bound[g]);
    CHECK(!memcmp(g_t.region + at, g_t.mem + at, n));
    return 1;
}

int main(void) {
    vkCreateBuffer = create_buffer;
    vkGetBufferMemoryRequirements = buffer_requirements;
    vkDestroyBuffer = destroy_buffer;
    g_settings.buffer_pages = 1;
    g_vk.sparse = 1;
    g_vk.feat.sparseResidencyBuffer = 1;
    g_vk.max_buffer_size = 1ull << 32;
    g_vk.props.limits.sparseAddressSpaceSize = 1ull << 40;
    for (u32 i = 0; i < SPACE; i++) g_t.mem[i] = (u8)rnd();
    next_epoch();
    CHECK(vk_pages_on());

    /* a range first read takes its pages; read again with nothing written, it takes none */
    CHECK(get(0x1234, 0x5000) && g_t.binds == 1 && g_t.copies);
    u32 copies = g_t.copies;
    CHECK(get(0x1234, 0x5000) && get(0x2000, 0x100) && g_t.copies == copies);
    /* the two pages written since are taken again at the next epoch, and no other */
    cpu_write(0x3ff0, 0x20);
    next_epoch();
    CHECK(get(0x1234, 0x5000) && g_t.copies == copies + 1 && g_t.stage_used == 0x2000);
    /* a range that is not one run of guest memory is the caller's to copy */
    CHECK(!vk_pages_get(GVA + SPACE - 0x1000, 0x2000, 1, &(VkBuffer){0}, &(u64){0}));

    /* A piece of the ring is refused for a range whose pages the CPU wrote: the range is the caller's then, and at
       the next submission of the same epoch the cache serves it with memory's bytes. The page the refusal fell on
       was not taken, so the tracking is asked for it again */
    next_epoch();
    CHECK(get(0x100000, 0x8000));
    cpu_write(0x100000, 0x8000);
    next_epoch();
    g_t.stage_refuse = 1;
    CHECK(!get(0x100000, 0x8000));
    next_submission();
    CHECK(get(0x100000, 0x8000));
    /* and in a long range, which takes the ring in pieces of PG_STAGE pages: the second piece is refused */
    next_epoch();
    CHECK(get(0x200000, 0x500000));
    cpu_write(0x200000, 0x500000);
    next_epoch();
    g_t.stage_refuse = 2;
    CHECK(!get(0x200000, 0x500000));
    next_submission();
    CHECK(get(0x200000 + PG_STAGE * 4096 - 0x800, 0x1000)); /* the pages on both sides of the refusal */
    CHECK(get(0x200000, 0x500000));

    /* ranges of every size over the memory, the CPU writing between the epochs, a piece of the ring refused now and
       then: what the cache serves is memory's bytes */
    u32 served = 0, refused = 0;
    for (u32 r = 0; r < 4000; r++) {
        for (u32 w = rnd() % 4; w; w--) {
            u64 at = rnd() % SPACE, n = 1 + rnd() % (rnd() % 8 ? 0x3000 : 0x40000);
            cpu_write(at, n < SPACE - at ? n : SPACE - at);
        }
        next_epoch();
        for (u32 k = 1 + rnd() % 3; k; k--) {
            u64 at = rnd() % SPACE, n = 1 + rnd() % (rnd() % 6 ? 0x20000 : 0x300000);
            if (n > SPACE - at) n = SPACE - at;
            if (rnd() % 2) next_submission();
            if (rnd() % 8 == 0) g_t.stage_refuse = 1 + rnd() % 3;
            if (get(at, n))
                served++;
            else
                refused++;
            g_t.stage_refuse = 0;
        }
        /* now and then the granules nothing read lately give their memory back, and every page is forgotten (a save
           state's load changes memory with no word from the tracking) */
        if (r % 512 == 511) {
            g_vk_present = 4 * (r + 1);
            g_t.before = g_vk_present;
            for (u32 i = 0; i < 16; i++) vk_pages_frame();
            g_t.before = 0;
            CHECK(g_t.unbinds);
        }
        if (r % 700 == 699) {
            for (u32 i = 0; i < SPACE; i += 64) g_t.mem[i] = (u8)rnd();
            vk_pages_forget_all();
        }
    }
    CHECK(served > 4000 && refused);
    puts("vk_pagespace: ok");
    return 0;
}
