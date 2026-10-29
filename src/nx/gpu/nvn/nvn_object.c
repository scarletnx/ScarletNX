/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from nxnative (MIT), Copyright (c) 2026 seedds: dm/gfx/nvn/hle_nvn.c */
/* nvn_direct's NVN objects: the device, memory pools, buffers, descriptor pools, textures and views, samplers,
   programs, syncs and the state objects, with their builders. Each keeps its state in place in the game's object
   (gpu/nvn/nvn_obj.h), and nothing here records commands or touches the GPU: what NVN only stores, it stores, and
   what it computes (a texture's storage size, a handle) it computes as the driver does.

   Where nxnative and the game's own driver disagree, the driver wins: nvnDeviceGetInteger answers from the driver's
   table (its API version is the SDK's, 55.8 in one game) rather than nxnative's 55.15 */
#include "nx/gpu/nvn/nvn_object.h"
#include "nx/gpu/nvn/nvn_state.h"
#include "nx/gpu/nvn/nvn_copy.h"
#include "nx/gpu/nvn/nvn_draw.h"
#include "gpu/nvn/nvn.hook.h"
#include "nx/gpu/nvn/nvn_driver.h"
#include "nx/gpu/nvn/nvn_tex.h"
#include "nx/gpu/nvn/nvn_tic.h"
#include "nx/hle/services/nvdrv.h"
#include "nx/runtime/clock.h"
#include "nx/hle/kernel/kernel.h"
#include "nx/runtime/loader.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"
#include <stdlib.h>
#include <string.h>

u64 nvn_serial(void) { return __atomic_add_fetch(&g_nvn.object.serial, 1, __ATOMIC_RELAXED); }

u64 nvn_pool_memory(u64 pool) { return pool ? NVN_OBJ(NvnPool, pool)->memory : 0; }
u64 nvn_pool_gpu(u64 pool) { return pool ? NVN_OBJ(NvnPool, pool)->gpu : 0; }
u32 nvn_device_flags(void) { return __atomic_load_n(&g_nvn.object.device_flags, __ATOMIC_RELAXED); }
u64 nvn_device(void) { return __atomic_load_n(&g_nvn.object.device, __ATOMIC_RELAXED); }

/* the small state objects are copied by value and may sit anywhere in the game's structures: read and written whole */
static u32 ld32(u64 a) {
    u32 v;
    memcpy(&v, GP(a), sizeof v);
    return v;
}
static u64 ld64(u64 a) {
    u64 v;
    memcpy(&v, GP(a), sizeof v);
    return v;
}
static void st32(u64 a, u32 v) { memcpy(GP(a), &v, sizeof v); }
static void st64(u64 a, u64 v) { memcpy(GP(a), &v, sizeof v); }
#define SET32(a, field, v) st32(a, (u32)nvn_set_field(ld32(a), field, (u64)(v)))
#define SET64(a, field, v) st64(a, nvn_set_field(ld64(a), field, (u64)(v)))

/* ============================================================ device */
/* nvnDeviceGetInteger's answers, items 0 to 0x68. Every item the game's own driver answers with a constant comes from
   that driver (nvn_device_driver, before the game asks): its API version (a game refuses another), the GPU code
   versions its shaders are checked against, and the rest. This table holds the others, which the driver reads from
   its device state: what SDK 12.3.9's driver (NVN 55.8) answers, with nxnative's console measurements for its
   device's fields (0x17, 0x18, 0x2b, 0x2d, 0x37, 0x38, 0x3a, 0x3b, 0x3d-0x41). 0x62 reads device state nobody
   measured: 0, logged */
static const i32 device_info[NVN_DEVICE_INFO_ITEMS] = {
    [0x00] = 55,      [0x01] = 15,      [0x02] = 14,      [0x03] = 65536,   [0x04] = 256,     [0x05] = 8,
    [0x06] = 16,      [0x07] = 4,       [0x08] = 16,      [0x09] = 32,      [0x0a] = 16,      [0x0b] = 4,
    [0x0c] = 4,       [0x0d] = 4,       [0x0e] = 16,      [0x0f] = 32,      [0x10] = 32,      [0x11] = 256,
    [0x12] = 256,     [0x13] = 4,       [0x14] = 8,       [0x15] = 512,     [0x16] = 256,     [0x17] = 2,
    [0x18] = 256,     [0x19] = 4096,    [0x1a] = 131072,  [0x1b] = 16,      [0x1c] = 1536,    [0x1d] = 1024,
    [0x1e] = 64,      [0x1f] = 1536,    [0x20] = 65535,   [0x21] = 65535,   [0x22] = 65535,   [0x23] = 8,
    [0x24] = 1048576, [0x25] = 4096,    [0x26] = 16,      [0x27] = 4096,    [0x28] = 1,       [0x29] = 1,
    [0x2a] = 1,       [0x2b] = 262144,  [0x2c] = 15,      [0x2d] = 2048,    [0x2e] = 1,       [0x2f] = 1,
    [0x30] = 16,      [0x31] = 5,       [0x32] = 1,       [0x33] = 8,       [0x34] = 8,       [0x35] = 4,
    [0x36] = 32,      [0x37] = 128,     [0x38] = 32,      [0x39] = 128,     [0x3a] = 65536,   [0x3b] = 1,
    [0x3c] = 4,       [0x3d] = 16384,   [0x3e] = 134217728, [0x3f] = 2048,  [0x40] = 16384,   [0x41] = 16384,
    [0x42] = 1,       [0x43] = 1,       [0x44] = 1,       [0x45] = 1,       [0x46] = 5,       [0x47] = 1,
    [0x48] = 1,       [0x49] = 1,       [0x4a] = 1,       [0x4b] = 1,       [0x4c] = 1,       [0x4d] = 16,
    [0x4e] = 1024,    [0x4f] = 32,      [0x50] = 4096,    [0x51] = 65536,   [0x52] = 65536,   [0x53] = 256,
    [0x54] = 16384,   [0x55] = 262144,  [0x56] = 4096,    [0x57] = 1,       [0x58] = 4,       [0x59] = 2,
    [0x5a] = 0,       [0x5b] = 4096,    [0x5c] = 16384,   [0x5d] = 4096,    [0x5e] = 128,     [0x5f] = 32,
    [0x60] = 65535,   [0x61] = 1,       [0x62] = 0,       [0x63] = 256,     [0x64] = 16,      [0x65] = 65535,
    [0x66] = 65535,   [0x67] = 256,     [0x68] = 16383,
};

void nvn_DeviceBuilderSetDefaults(CPU* c, u64 b) {
    (void)c;
    memset(GP(b), 0, sizeof(NvnDeviceBuilder));
}
void nvn_DeviceBuilderSetFlags(CPU* c, u64 b, i32 flags) {
    (void)c;
    NVN_OBJ(NvnDeviceBuilder, b)->flags = (u32)flags;
}
int nvn_DeviceInitialize(CPU* c, u64 d, u64 b) {
    NvnDevice o = {0};
    (void)c;
    o.magic = NVN_MAGIC('D', 'E', 'V', 0);
    o.flags = NVN_OBJ(NvnDeviceBuilder, b)->flags;
    o.origin = NVN_WINDOW_ORIGIN_MODE_LOWER_LEFT; /* NVN's defaults: lower-left origin, near = -w */
    memcpy(GP(d), &o, sizeof o);
    __atomic_store_n(&g_nvn.object.device_flags, o.flags, __ATOMIC_RELAXED);
    __atomic_store_n(&g_nvn.object.device, d, __ATOMIC_RELAXED);
    nvn_draw_device(o.origin, o.depth_mode);
    LOG("nvn: device %llx initialized, flags %#x\n", (unsigned long long)d, o.flags);
    return 1;
}
void nvn_DeviceFinalize(CPU* c, u64 d) {
    u64 was = d;
    (void)c;
    NVN_OBJ(NvnDevice, d)->magic = 0;
    __atomic_compare_exchange_n(&g_nvn.object.device, &was, 0, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED);
}
i32 nvn_DeviceBuilderGetFlags(CPU* c, u64 b) {
    (void)c;
    return (i32)NVN_OBJ(const NvnDeviceBuilder, b)->flags;
}
/* The objects the game finalized with DEFERRED_FINALIZE, freed once they are age submissions old: the game's driver
   (NVN 53) returns at once, and the runtime frees nothing at a Finalize */
void nvn_DeviceApplyDeferredFinalizes(CPU* c, u64 d, i32 age) {
    (void)c;
    (void)d;
    (void)age;
}
/* ---- the debug layer's API, which the release driver answers as below (NVN 55.15): it installs no callback, walks
   no object database, has no external debugger, and records no checkpoints (each setter answers 1). A debug domain is
   the next number of a counter from 0 */
void nvn_DeviceInstallDebugCallback(CPU* c, u64 d, u64 fn, u64 data, int enable) {
    (void)c;
    (void)d;
    (void)fn;
    (void)data;
    (void)enable;
}
void nvn_DeviceWalkDebugDatabase(CPU* c, u64 d, i32 type, u64 fn, u64 data) {
    (void)c;
    (void)d;
    (void)type;
    (void)fn;
    (void)data;
}
i32 nvn_DeviceGenerateDebugDomainId(CPU* c, u64 d, u64 name) {
    (void)c;
    (void)d;
    (void)name;
    return (i32)__atomic_fetch_add(&g_nvn.object.debug_domain, 1, __ATOMIC_RELAXED);
}
int nvn_DeviceIsExternalDebuggerAttached(CPU* c, u64 d) {
    (void)c;
    (void)d;
    return 0;
}
int nvn_DeviceSetAutomaticCheckpointsEnable(CPU* c, u64 d, int on) {
    (void)c;
    (void)d;
    (void)on;
    return 1;
}
int nvn_DeviceSetAutomaticCheckpointsByDebugGroup(CPU* c, u64 d, int on) {
    (void)c;
    (void)d;
    (void)on;
    return 1;
}
int nvn_DeviceSetAutomaticCheckpointsMask(CPU* c, u64 d, u32 mask) {
    (void)c;
    (void)d;
    (void)mask;
    return 1;
}
int nvn_DeviceSetAutomaticCheckpointsSamplingInterval(CPU* c, u64 d, u32 interval) {
    (void)c;
    (void)d;
    (void)interval;
    return 1;
}
int nvn_DeviceSetCheckpointCallStacksEnable(CPU* c, u64 d, int on) {
    (void)c;
    (void)d;
    (void)on;
    return 1;
}
int nvn_DeviceSetCheckpointMaxCallStacksDepth(CPU* c, u64 d, i32 depth) {
    (void)c;
    (void)d;
    (void)depth;
    return 1;
}
/* the offline compiler's intermediate shader cache: the release driver returns at once, and the runtime's shaders
   come compiled */
void nvn_DeviceSetIntermediateShaderCache(CPU* c, u64 d, i32 on) {
    (void)c;
    (void)d;
    (void)on;
}
/* the end of the read-only segment that follows func's code: the module's jump tables are there */
typedef struct {
    u64 func, start, end;
} RoAfter;
static int ro_after(void* ctx, const char* name, const u8* bytes, u64 gva, u64 size, int writable) {
    RoAfter* r = (RoAfter*)ctx;
    size_t n = strlen(name);
    (void)bytes;
    if (!writable && n > 3 && !strcmp(name + n - 3, ".ro") && gva > r->func && (!r->start || gva < r->start)) {
        r->start = gva;
        r->end = gva + size;
    }
    return 0;
}
void nvn_device_driver(u64 func) {
    RoAfter r = {0};
    u64 hi;
    if (__atomic_load_n(&g_nvn.object.driver, __ATOMIC_ACQUIRE)) return; /* resolved again: read once */
    r.func = func;
    loader_segments(ro_after, &r);
    hi = r.end ? r.end : g_code_hi;
    u32 i, n = 0;
    memcpy(g_nvn.object.info, device_info, sizeof device_info);
    for (i = 0; i < NVN_DEVICE_INFO_ITEMS; i++)
        n += (u32)nvn_driver_info(g_mem, g_code_lo, hi, func, i, &g_nvn.object.info[i]);
    g_nvn.object.read_api = nvn_driver_info(g_mem, g_code_lo, hi, func, 0, &g_nvn.object.info[0]) &&
                            nvn_driver_info(g_mem, g_code_lo, hi, func, 1, &g_nvn.object.info[1]);
    if (!g_nvn.object.read_api)
        WARN("nvn: the game's nvnDeviceGetInteger at %llx doesn't read as a known switch: the device answers API "
             "%d.%d from its own table, which the game may refuse\n",
             (unsigned long long)func, g_nvn.object.info[0], g_nvn.object.info[1]);
    __atomic_store_n(&g_nvn.object.driver, 1, __ATOMIC_RELEASE);
    LOG("nvn: the game's driver answers API %d.%d and GPU code %d.%d to %d.%d, %u items in all from its code\n",
        g_nvn.object.info[0], g_nvn.object.info[1], g_nvn.object.info[0x2f], g_nvn.object.info[0x31],
        g_nvn.object.info[0x2e], g_nvn.object.info[0x30], n);
}
u32 nvn_device_api(void) {
    if (!__atomic_load_n(&g_nvn.object.driver, __ATOMIC_ACQUIRE) || !g_nvn.object.read_api) return 0;
    return nvn_api_pack(g_nvn.object.info[0], g_nvn.object.info[1]);
}
void nvn_DeviceGetInteger(CPU* c, u64 d, i32 what, u64 out) {
    u32 w = (u32)what;
    const i32* info = __atomic_load_n(&g_nvn.object.driver, __ATOMIC_ACQUIRE) ? g_nvn.object.info : device_info;
    i32 v = w < NVN_DEVICE_INFO_ITEMS ? info[w] : 0;
    (void)c;
    (void)d;
    if (w >= NVN_DEVICE_INFO_ITEMS || w == 0x62)
        LOG_LIMIT(16, "nvn: nvnDeviceGetInteger of item %#x, which the driver reads from state nobody measured: 0\n", w);
    gw32(out, (u32)v);
}
void nvn_DeviceSetWindowOriginMode(CPU* c, u64 d, i32 mode) {
    (void)c;
    NVN_OBJ(NvnDevice, d)->origin = (u32)mode;
    nvn_draw_device((u32)mode, NVN_OBJ(NvnDevice, d)->depth_mode);
    LOG("nvn: window origin %s\n", mode ? "upper left" : "lower left");
}
void nvn_DeviceSetDepthMode(CPU* c, u64 d, i32 mode) {
    (void)c;
    NVN_OBJ(NvnDevice, d)->depth_mode = (u32)mode;
    nvn_draw_device(NVN_OBJ(NvnDevice, d)->origin, (u32)mode);
    LOG("nvn: depth mode %s\n", mode ? "near = 0" : "near = -w");
}
/* a combined handle: texture ID | sampler ID << 20, as the separate handles are laid out */
u64 nvn_DeviceGetTextureHandle(CPU* c, u64 d, i32 texture, i32 sampler) {
    (void)c;
    (void)d;
    return (u64)(u32)texture | (u64)(u32)sampler << 20;
}
u64 nvn_DeviceGetImageHandle(CPU* c, u64 d, i32 id) {
    (void)c;
    (void)d;
    return (u64)(u32)id;
}
u64 nvn_DeviceGetSeparateSamplerHandle(CPU* c, u64 d, i32 id) {
    (void)c;
    (void)d;
    return (u64)(u32)id << 20;
}
/* a texture's separate handle, and a texel fetch's: its ID with bit 32 set, as the game's driver makes both (sxtw, orr
   #0x100000000), which the shader's low word (the ID) combines with a separate sampler's */
u64 nvn_DeviceGetSeparateTextureHandle(CPU* c, u64 d, i32 id) {
    (void)c;
    (void)d;
    return (u64)(i64)id | 1ull << 32;
}
u64 nvn_DeviceGetTexelFetchHandle(CPU* c, u64 d, i32 id) {
    (void)c;
    (void)d;
    return (u64)(i64)id | 1ull << 32;
}
/* a counter's timestamp is in the GPU's 19.2 MHz ticks */
u64 nvn_DeviceGetTimestampInNanoseconds(CPU* c, u64 d, u64 counter) {
    (void)c;
    (void)d;
    return NVN_OBJ(NVNcounterData, counter)->timestamp * 625 / 12;
}
/* fast clear values are hints to the hardware's compression */
int nvn_DeviceRegisterFastClearColor(CPU* c, u64 d, u64 color, i32 format) {
    (void)c;
    (void)d;
    (void)color;
    (void)format;
    return 1;
}
int nvn_DeviceRegisterFastClearColori(CPU* c, u64 d, u64 color, i32 format) {
    return nvn_DeviceRegisterFastClearColor(c, d, color, format);
}
int nvn_DeviceRegisterFastClearColorui(CPU* c, u64 d, u64 color, i32 format) {
    return nvn_DeviceRegisterFastClearColor(c, d, color, format);
}
int nvn_DeviceRegisterFastClearDepth(CPU* c, u64 d, f32 depth) {
    (void)c;
    (void)d;
    (void)depth;
    return 1;
}

/* ============================================================ memory pools */
void nvn_MemoryPoolBuilderSetDefaults(CPU* c, u64 b) {
    (void)c;
    memset(GP(b), 0, sizeof(NvnPoolBuilder));
}
void nvn_MemoryPoolBuilderSetDevice(CPU* c, u64 b, u64 d) {
    (void)c;
    NVN_OBJ(NvnPoolBuilder, b)->device = d;
}
void nvn_MemoryPoolBuilderSetStorage(CPU* c, u64 b, u64 memory, u64 size) {
    NvnPoolBuilder* o = NVN_OBJ(NvnPoolBuilder, b);
    (void)c;
    o->memory = memory;
    o->size = size;
}
void nvn_MemoryPoolBuilderSetFlags(CPU* c, u64 b, i32 flags) {
    (void)c;
    NVN_OBJ(NvnPoolBuilder, b)->flags = (u32)flags;
}
/* A pool takes GPU addresses of its own, as NVIDIA's driver maps it through nvhost-as-gpu: from the same allocator,
   its pages showing its storage. A virtual pool (flags 0x200, no storage) only reserves its range: its mapping
   requests fill it. The storage's page offset carries over, so each byte keeps one GPU address */
#define NVN_POOL_VIRTUAL 0x200u
#define NVN_POOL_ALIGN   0x20000u /* the GPU address alignment: a big page, as compressible kinds want */
int nvn_MemoryPoolInitialize(CPU* c, u64 p, u64 b) {
    const NvnPoolBuilder* s = NVN_OBJ(NvnPoolBuilder, b);
    NvnPool o = {0};
    u64 lead, va;
    (void)c;
    o.magic = NVN_MAGIC('P', 'O', 'O', 'L');
    o.flags = s->flags;
    o.memory = s->memory;
    o.size = s->size;
    if (!o.memory && !(o.flags & NVN_POOL_VIRTUAL))
        LOG_LIMIT(8, "nvn: memory pool %llx has no storage (flags %#x)\n", (unsigned long long)p, o.flags);
    lead = o.memory & 0xfff;
    va = gmmu_space_alloc(lead + o.size, NVN_POOL_ALIGN);
    if (!va)
        fatal("nvn: no GPU address space left for a memory pool of %llx bytes", (unsigned long long)o.size);
    if (o.memory) gmmu_space_map(va, o.memory - lead, lead + o.size);
    o.gpu = va + lead;
    memcpy(GP(p), &o, sizeof o);
    LOGV("nvn: memory pool %llx: %llx, %llx bytes at GPU %llx, flags %#x\n", (unsigned long long)p,
         (unsigned long long)o.memory, (unsigned long long)o.size, (unsigned long long)o.gpu, o.flags);
    return 1;
}
void nvn_MemoryPoolFinalize(CPU* c, u64 p) {
    NvnPool* o = NVN_OBJ(NvnPool, p);
    u64 lead = o->gpu & 0xfff;
    (void)c;
    if (o->magic == NVN_MAGIC('P', 'O', 'O', 'L') && o->gpu) gmmu_space_free(o->gpu - lead, lead + o->size);
    if (o->magic == NVN_MAGIC('P', 'O', 'O', 'L')) nvmap_handle_drop(__atomic_exchange_n(&o->nvmap, 0, __ATOMIC_ACQ_REL));
    o->magic = 0;
    o->gpu = 0;
}
/* The nvmap handle of a pool's storage, made at its first use: the game's driver makes one for every pool, and only an
   NvRm surface (nvnTextureGetNvRmSurfaceNVX) names it, for a decoder of the game's, which maps it through host1x.
   Two threads can ask at once: the first handle stored stays. 0 for a pool without storage of its own */
static u32 pool_nvmap(u64 pool) {
    NvnPool* o = NVN_OBJ(NvnPool, pool);
    u32 h = __atomic_load_n(&o->nvmap, __ATOMIC_ACQUIRE), none = 0;
    if (h || o->magic != NVN_MAGIC('P', 'O', 'O', 'L') || !o->memory) return h;
    h = nvmap_handle_new(o->memory, o->size);
    if (!h) {
        LOG_LIMIT(4, "nvn: memory pool %llx (%llx bytes at %llx) has no nvmap handle for an NvRm surface\n",
                  (unsigned long long)pool, (unsigned long long)o->size, (unsigned long long)o->memory);
        return 0;
    }
    if (__atomic_compare_exchange_n(&o->nvmap, &none, h, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return h;
    nvmap_handle_drop(h);
    return none;
}
/* the CPU and the GPU share guest memory in phase 1: nothing to flush */
void nvn_MemoryPoolFlushMappedRange(CPU* c, u64 p, i64 offset, u64 size) {
    (void)c;
    (void)p;
    (void)offset;
    (void)size;
}
/* debug labels name objects for NVIDIA's tools */
void nvn_MemoryPoolSetDebugLabel(CPU* c, u64 p, u64 label) {
    (void)c;
    (void)p;
    (void)label;
}
void nvn_MemoryPoolInvalidateMappedRange(CPU* c, u64 p, i64 offset, u64 size) {
    (void)c;
    (void)p;
    (void)offset;
    (void)size;
}
/* A virtual pool's pages show what its mapping requests put there, as the GPU's page table does: each request maps
   size bytes of a physical pool's storage from its offset at the virtual pool's offset, or unmaps them (no physical
   pool). Sparse requests at offsets of their own are just more pages */
int nvn_MemoryPoolMapVirtual(CPU* c, u64 p, i32 n, u64 requests) {
    const NvnPool* o = NVN_OBJ(const NvnPool, p);
    i32 i;
    (void)c;
    for (i = 0; i < n; i++) {
        const NvnMapRequest* r = NVN_OBJ(const NvnMapRequest, requests + (u64)i * sizeof(NvnMapRequest));
        u64 phys = nvn_pool_memory(r->physical), at = o->gpu + (u64)r->virtual_offset;
        if (r->virtual_offset < 0 || (u64)r->virtual_offset + r->size > o->size) {
            LOG_LIMIT(8, "nvn: virtual pool %llx: a request at %llx of %llx bytes past its %llx\n",
                      (unsigned long long)p, (unsigned long long)r->virtual_offset, (unsigned long long)r->size,
                      (unsigned long long)o->size);
            return 0;
        }
        if (!r->physical) {
            gmmu_space_unmap(at, r->size);
            continue;
        }
        if (!phys) {
            LOG_LIMIT(8, "nvn: virtual pool %llx: request %d maps pool %llx, which has no storage\n",
                      (unsigned long long)p, i, (unsigned long long)r->physical);
            return 0;
        }
        gmmu_space_map(at, phys + (u64)r->physical_offset, r->size);
    }
    LOG_LIMIT(16, "nvn: virtual pool %llx (%llx bytes at GPU %llx): %d requests mapped\n", (unsigned long long)p,
              (unsigned long long)o->size, (unsigned long long)o->gpu, n);
    return 1;
}
u64 nvn_MemoryPoolGetBufferAddress(CPU* c, u64 p) {
    (void)c;
    return NVN_OBJ(NvnPool, p)->gpu;
}
u64 nvn_MemoryPoolMap(CPU* c, u64 p) {
    (void)c;
    return NVN_OBJ(NvnPool, p)->memory;
}
i32 nvn_MemoryPoolGetFlags(CPU* c, u64 p) {
    (void)c;
    return (i32)NVN_OBJ(NvnPool, p)->flags;
}
u64 nvn_MemoryPoolGetSize(CPU* c, u64 p) {
    (void)c;
    return NVN_OBJ(NvnPool, p)->size;
}

/* ============================================================ buffers: a pool, an offset and a size */
void nvn_BufferBuilderSetDefaults(CPU* c, u64 b) {
    u64 device = NVN_OBJ(NvnBufferBuilder, b)->device;
    (void)c;
    memset(GP(b), 0, sizeof(NvnBufferBuilder));
    NVN_OBJ(NvnBufferBuilder, b)->device = device;
}
void nvn_BufferBuilderSetDevice(CPU* c, u64 b, u64 d) {
    (void)c;
    NVN_OBJ(NvnBufferBuilder, b)->device = d;
}
void nvn_BufferBuilderSetStorage(CPU* c, u64 b, u64 pool, i64 offset, u64 size) {
    NvnBufferBuilder* o = NVN_OBJ(NvnBufferBuilder, b);
    (void)c;
    o->pool = pool;
    o->offset = offset;
    o->size = size;
}
int nvn_BufferInitialize(CPU* c, u64 buf, u64 b) {
    const NvnBufferBuilder* s = NVN_OBJ(NvnBufferBuilder, b);
    u64 gpu = nvn_pool_gpu(s->pool);
    NvnBuffer o = {0};
    (void)c;
    if (!gpu) {
        LOG_LIMIT(8, "nvn: buffer %llx: its pool %llx isn't initialized\n", (unsigned long long)buf,
                  (unsigned long long)s->pool);
        return 0;
    }
    if ((u64)s->offset + s->size > NVN_OBJ(NvnPool, s->pool)->size)
        LOG_LIMIT(8, "nvn: buffer %llx ends past its pool (offset %llx, size %llx)\n", (unsigned long long)buf,
                  (unsigned long long)s->offset, (unsigned long long)s->size);
    o.magic = NVN_MAGIC('B', 'U', 'F', 0);
    o.address = gpu + (u64)s->offset;
    o.size = s->size;
    o.pool = s->pool;
    o.offset = s->offset;
    memcpy(GP(buf), &o, sizeof o);
    return 1;
}
void nvn_BufferFinalize(CPU* c, u64 buf) {
    (void)c;
    NVN_OBJ(NvnBuffer, buf)->magic = 0;
}
u64 nvn_BufferGetAddress(CPU* c, u64 buf) {
    (void)c;
    return NVN_OBJ(NvnBuffer, buf)->address;
}
/* the CPU's view: the pool's storage, 0 in a virtual pool */
u64 nvn_BufferMap(CPU* c, u64 buf) {
    const NvnBuffer* o = NVN_OBJ(const NvnBuffer, buf);
    u64 memory = nvn_pool_memory(o->pool);
    (void)c;
    return memory ? memory + (u64)o->offset : 0;
}
u64 nvn_BufferGetSize(CPU* c, u64 buf) {
    (void)c;
    return NVN_OBJ(NvnBuffer, buf)->size;
}
u64 nvn_BufferGetDebugID(CPU* c, u64 buf) {
    (void)c;
    (void)buf;
    return 0; /* the release driver's: only its debug layer numbers objects */
}
void nvn_BufferSetDebugLabel(CPU* c, u64 buf, u64 label) {
    (void)c;
    (void)buf;
    (void)label;
}
u64 nvn_BufferGetMemoryPool(CPU* c, u64 buf) {
    (void)c;
    return NVN_OBJ(NvnBuffer, buf)->pool;
}
i64 nvn_BufferGetMemoryOffset(CPU* c, u64 buf) {
    (void)c;
    return NVN_OBJ(NvnBuffer, buf)->offset;
}
void nvn_BufferFlushMappedRange(CPU* c, u64 buf, i64 offset, u64 size) {
    (void)c;
    (void)buf;
    (void)offset;
    (void)size;
}
/* A buffer's mapping is the pool's guest memory, where the replay's writes land before the CPU can read them (the
   renderer's ordered releases, and GPU-owned memory landed when the CPU touches it): nothing to invalidate, as for
   a pool's range */
void nvn_BufferInvalidateMappedRange(CPU* c, u64 buf, i64 offset, u64 size) {
    (void)c;
    (void)buf;
    (void)offset;
    (void)size;
}

/* ============================================================ descriptor pools */
/* The descriptors live in pool memory: the hardware's TIC and TSC entries, 32 bytes each, which a registration
   writes at its ID as NVIDIA's driver does (gpu/nvn/nvn_tic.c), and the renderer reads from the pool a command buffer
   set, as it reads nvhost's */
static int desc_pool_init(u64 dp, u64 pool, i64 offset, i32 count, const char* kind) {
    NvnDescPool o = {0};
    if (offset < 0 || (u64)offset > 0xffffffffull) {
        LOG("nvn: a %s pool at offset %llx of its memory pool, which nvn_direct keeps in 32 bits\n", kind,
            (unsigned long long)offset);
        return 0;
    }
    o.pool = pool;
    o.offset = (u32)offset;
    o.count = count;
    memcpy(GP(dp), &o, sizeof o);
    LOG("nvn: %s pool %llx: %d descriptors at GPU %llx\n", kind, (unsigned long long)dp, count,
        (unsigned long long)(nvn_pool_gpu(pool) + (u64)offset));
    return 1;
}
/* the guest address of descriptor id, 0 when it is outside the pool or the pool has no storage */
static u64 desc_slot(u64 dp, i32 id, const char* kind) {
    const NvnDescPool* p = NVN_OBJ(NvnDescPool, dp);
    u64 memory = nvn_pool_memory(p->pool);
    if (id < 0 || id >= p->count || !memory) {
        LOG_LIMIT(8, "nvn: %s %d registered outside its pool %llx (%d descriptors)\n", kind, id,
                  (unsigned long long)dp, p->count);
        return 0;
    }
    return memory + p->offset + (u64)id * NVN_DESCRIPTOR_SIZE;
}
void nvn_TexturePoolFinalize(CPU* c, u64 tp) {
    (void)c;
    (void)tp;
}
void nvn_SamplerPoolFinalize(CPU* c, u64 sp) {
    (void)c;
    (void)sp;
}
int nvn_TexturePoolInitialize(CPU* c, u64 tp, u64 pool, i64 offset, i32 count) {
    (void)c;
    return desc_pool_init(tp, pool, offset, count, "texture");
}
int nvn_SamplerPoolInitialize(CPU* c, u64 sp, u64 pool, i64 offset, i32 count) {
    (void)c;
    return desc_pool_init(sp, pool, offset, count, "sampler");
}
/* what a descriptor pool was initialized with */
u64 nvn_TexturePoolGetMemoryPool(CPU* c, u64 tp) {
    (void)c;
    return NVN_OBJ(const NvnDescPool, tp)->pool;
}
i64 nvn_TexturePoolGetMemoryOffset(CPU* c, u64 tp) {
    (void)c;
    return (i64)NVN_OBJ(const NvnDescPool, tp)->offset;
}
i32 nvn_TexturePoolGetSize(CPU* c, u64 tp) {
    (void)c;
    return NVN_OBJ(const NvnDescPool, tp)->count;
}
u64 nvn_SamplerPoolGetMemoryPool(CPU* c, u64 sp) {
    (void)c;
    return NVN_OBJ(const NvnDescPool, sp)->pool;
}
i64 nvn_SamplerPoolGetMemoryOffset(CPU* c, u64 sp) {
    (void)c;
    return (i64)NVN_OBJ(const NvnDescPool, sp)->offset;
}
i32 nvn_SamplerPoolGetSize(CPU* c, u64 sp) {
    (void)c;
    return NVN_OBJ(const NvnDescPool, sp)->count;
}
/* an image's descriptor is in the texture pool's ID space too (nvnDeviceGetImageHandle is the ID) */
static void register_texture(u64 tp, i32 id, u64 t, u64 v, int image) {
    u64 slot = desc_slot(tp, id, "texture");
    NvnTexture tex;
    NvnView view;
    u32 tic[8];
    if (!slot) return;
    if (!t) { /* no texture: an empty descriptor */
        memset(GP(slot), 0, sizeof tic);
        return;
    }
    memcpy(&tex, GP(t), sizeof tex);
    if (v) memcpy(&view, GP(v), sizeof view);
    if (!nvn_tic_encode(&tex, v ? &view : NULL, image, tic))
        LOG_LIMIT(8, "nvn: a texture of format %#x, which has no texture descriptor: an empty one at %d\n",
                  v && (view.set & NVN_VIEW_FORMAT) ? view.format : tex.d.format, id);
    memcpy(GP(slot), tic, sizeof tic);
}
void nvn_TexturePoolRegisterTexture(CPU* c, u64 tp, i32 id, u64 t, u64 v) {
    (void)c;
    register_texture(tp, id, t, v, 0);
}
void nvn_TexturePoolRegisterImage(CPU* c, u64 tp, i32 id, u64 t, u64 v) {
    (void)c;
    register_texture(tp, id, t, v, 1);
}
void nvn_SamplerPoolRegisterSampler(CPU* c, u64 sp, i32 id, u64 s) {
    u64 slot = desc_slot(sp, id, "sampler");
    u32 tsc[8];
    (void)c;
    if (!slot) return;
    nvn_tsc_encode(&NVN_OBJ(const NvnSampler, s)->p, tsc);
    memcpy(GP(slot), tsc, sizeof tsc);
}
/* a sampler's descriptor straight from a builder's state, as the game's driver writes it with no sampler object */
void nvn_SamplerPoolRegisterSamplerBuilder(CPU* c, u64 sp, i32 id, u64 b) {
    u64 slot = desc_slot(sp, id, "sampler");
    u32 tsc[8];
    (void)c;
    if (!slot) return;
    nvn_tsc_encode(&NVN_OBJ(const NvnSamplerBuilder, b)->p, tsc);
    memcpy(GP(slot), tsc, sizeof tsc);
}

/* ============================================================ textures */
#define TB(b) NVN_OBJ(NvnTexBuilder, b)
void nvn_TextureBuilderSetDefaults(CPU* c, u64 b) {
    u64 device = TB(b)->device;
    NvnTexBuilder* o = TB(b);
    (void)c;
    memset(o, 0, sizeof *o);
    o->device = device;
    o->d.target = NVN_TEXTURE_TARGET_2D;
    o->d.width = o->d.height = o->d.depth = o->d.levels = 1;
}
void nvn_TextureBuilderSetDevice(CPU* c, u64 b, u64 d) {
    (void)c;
    TB(b)->device = d;
}
void nvn_TextureBuilderSetFlags(CPU* c, u64 b, i32 flags) {
    (void)c;
    TB(b)->d.flags = (u32)flags;
}
void nvn_TextureBuilderSetTarget(CPU* c, u64 b, i32 target) {
    (void)c;
    TB(b)->d.target = (u8)target;
}
void nvn_TextureBuilderSetFormat(CPU* c, u64 b, i32 format) {
    (void)c;
    TB(b)->d.format = (u16)format;
}
void nvn_TextureBuilderSetLevels(CPU* c, u64 b, i32 levels) {
    (void)c;
    TB(b)->d.levels = (u8)levels;
}
void nvn_TextureBuilderSetWidth(CPU* c, u64 b, i32 w) {
    (void)c;
    TB(b)->d.width = (u32)w;
}
void nvn_TextureBuilderSetHeight(CPU* c, u64 b, i32 h) {
    (void)c;
    TB(b)->d.height = (u32)h;
}
void nvn_TextureBuilderSetSamples(CPU* c, u64 b, i32 samples) {
    (void)c;
    TB(b)->d.samples = (u8)samples;
}
void nvn_TextureBuilderSetStride(CPU* c, u64 b, i64 stride) {
    (void)c;
    TB(b)->d.stride = (u32)stride;
}
/* a packaged texture's 8 layout bytes: byte 0 is its block height (log2), which its storage size then follows */
void nvn_TextureBuilderSetPackagedTextureLayout(CPU* c, u64 b, u64 layout) {
    NvnTexBuilder* o = TB(b);
    (void)c;
    o->d.bits &= (u8)~(NVN_TEX_HAS_LAYOUT | NVN_TEX_BLOCK_H);
    memset(o->layout, 0, sizeof o->layout);
    if (!layout) return;
    memcpy(o->layout, GP(layout), sizeof o->layout);
    o->d.bits |= (u8)(NVN_TEX_HAS_LAYOUT | ((o->layout[0] + 1u) & NVN_TEX_BLOCK_H));
}
/* the storage class: 0x100 with the GMMU page kind, as the game's driver answers (gpu/nvn/nvn_tex.c nvn_tex_storage) */
i32 nvn_TextureBuilderGetStorageClass(CPU* c, u64 b) {
    NvnTexStorage st;
    (void)c;
    nvn_tex_storage(&TB(b)->d, &st);
    return (i32)(0x100 | st.kind);
}
/* The size of a depth texture's ZCull save area, as the driver answers. The runtime does no ZCull and never touches
   it, but games allocate it from their pools */
u64 nvn_TextureBuilderGetZCullStorageSize(CPU* c, u64 b) {
    NvnTexStorage st;
    (void)c;
    nvn_tex_storage(&TB(b)->d, &st);
    return st.zcull;
}
i32 nvn_TextureBuilderGetTarget(CPU* c, u64 b) {
    (void)c;
    return TB(b)->d.target;
}
i32 nvn_TextureBuilderGetWidth(CPU* c, u64 b) {
    (void)c;
    return (i32)TB(b)->d.width;
}
i32 nvn_TextureBuilderGetHeight(CPU* c, u64 b) {
    (void)c;
    return (i32)TB(b)->d.height;
}
i32 nvn_TextureBuilderGetDepth(CPU* c, u64 b) {
    (void)c;
    return (i32)TB(b)->d.depth;
}
i32 nvn_TextureBuilderGetLevels(CPU* c, u64 b) {
    (void)c;
    return TB(b)->d.levels;
}
i32 nvn_TextureBuilderGetFormat(CPU* c, u64 b) {
    (void)c;
    return TB(b)->d.format;
}
i32 nvn_TextureBuilderGetSamples(CPU* c, u64 b) {
    (void)c;
    return TB(b)->d.samples;
}
i32 nvn_TextureBuilderGetFlags(CPU* c, u64 b) {
    (void)c;
    return (i32)TB(b)->d.flags;
}
i64 nvn_TextureBuilderGetStride(CPU* c, u64 b) {
    (void)c;
    return (i64)TB(b)->d.stride;
}
void nvn_TextureBuilderSetDepth(CPU* c, u64 b, i32 depth) {
    (void)c;
    TB(b)->d.depth = (u32)depth;
}
/* the width, the height and depth 1, as the driver stores the three */
void nvn_TextureBuilderSetSize1D(CPU* c, u64 b, i32 w) {
    NvnTexBuilder* o = TB(b);
    (void)c;
    o->d.width = (u32)w;
    o->d.height = 1;
    o->d.depth = 1;
}
void nvn_TextureBuilderSetSize2D(CPU* c, u64 b, i32 w, i32 h) {
    NvnTexBuilder* o = TB(b);
    (void)c;
    o->d.width = (u32)w;
    o->d.height = (u32)h;
    o->d.depth = 1;
}
void nvn_TextureBuilderSetSize3D(CPU* c, u64 b, i32 w, i32 h, i32 d) {
    NvnTexBuilder* o = TB(b);
    (void)c;
    o->d.width = (u32)w;
    o->d.height = (u32)h;
    o->d.depth = (u32)d;
}
void nvn_TextureBuilderSetStorage(CPU* c, u64 b, u64 pool, i64 offset) {
    NvnTexBuilder* o = TB(b);
    (void)c;
    o->pool = pool;
    o->offset = offset;
}
/* the channels: r | g << 3 | b << 6 | a << 9 */
void nvn_TextureBuilderSetSwizzle(CPU* c, u64 b, i32 r, i32 g, i32 bl, i32 a) {
    (void)c;
    TB(b)->d.swizzle = (u16)(NVN_TEX_SWIZZLE_SET | (r & 7) | (g & 7) << 3 | (bl & 7) << 6 | (a & 7) << 9);
}
void nvn_TextureBuilderSetDepthStencilMode(CPU* c, u64 b, i32 mode) {
    NvnTexBuilder* o = TB(b);
    (void)c;
    if (mode == NVN_TEXTURE_DEPTH_STENCIL_MODE_STENCIL)
        o->d.bits |= NVN_TEX_STENCIL;
    else
        o->d.bits &= (u8)~NVN_TEX_STENCIL;
}
/* packaged texture data in CPU memory: the driver copies it into the storage at TextureInitialize */
void nvn_TextureBuilderSetPackagedTextureData(CPU* c, u64 b, u64 data) {
    (void)c;
    TB(b)->packaged_data = data;
}
u64 nvn_TextureBuilderGetStorageSize(CPU* c, u64 b) {
    NvnTexStorage st;
    (void)c;
    nvn_tex_storage(&TB(b)->d, &st);
    if (!st.size) LOG_LIMIT(8, "nvn: the storage size of a texture of format %#x, which nvn_direct doesn't know\n",
                            TB(b)->d.format);
    return st.size;
}
u64 nvn_TextureBuilderGetStorageAlignment(CPU* c, u64 b) {
    NvnTexStorage st;
    (void)c;
    nvn_tex_storage(&TB(b)->d, &st);
    return st.align;
}
int nvn_TextureInitialize(CPU* c, u64 t, u64 b) {
    const NvnTexBuilder* s = TB(b);
    NvnTexture o = {0};
    NvnTexLayout l;
    u64 memory = nvn_pool_memory(s->pool), gpu = nvn_pool_gpu(s->pool);
    (void)c;
    nvn_tex_layout(&s->d, &l);
    o.magic = NVN_MAGIC('T', 'E', 'X', 0);
    o.gpu = gpu ? gpu + (u64)s->offset : 0;
    o.d = s->d;
    o.pool = s->pool;
    o.offset = s->offset;
    o.memory = memory ? memory + (u64)s->offset : 0;
    o.size = l.size;
    memcpy(o.layout, s->layout, sizeof o.layout);
    o.serial = nvn_serial();
    if (o.d.format < 256 &&
        !(__atomic_fetch_or(&g_nvn.object.formats[o.d.format >> 5], 1u << (o.d.format & 31), __ATOMIC_RELAXED) &
          1u << (o.d.format & 31)))
        LOG("nvn: the first texture of format %#x (target %u, %ux%u)\n", o.d.format, o.d.target, o.d.width,
            o.d.height);
    if (o.d.samples > 1)
        LOG_LIMIT(4, "nvn: a %u-sample texture of format %#x, %ux%u\n", (unsigned)o.d.samples, o.d.format, o.d.width,
                  o.d.height);
    /* a whole range, guest to guest, as the driver's own copy: bytes already in the texture's layout, nothing
       converted, before any submission can read the texture */
    if (s->packaged_data && o.memory && o.size) memmove(GP(o.memory), GP(s->packaged_data), (size_t)o.size);
    memcpy(GP(t), &o, sizeof o);
    return 1;
}
void nvn_TextureFinalize(CPU* c, u64 t) {
    (void)c;
    NVN_OBJ(NvnTexture, t)->magic = 0;
}
#define CT(t) NVN_OBJ(const NvnTexture, t)
i32 nvn_TextureGetFlags(CPU* c, u64 t) {
    (void)c;
    return (i32)CT(t)->d.flags;
}
i32 nvn_TextureGetWidth(CPU* c, u64 t) {
    (void)c;
    return (i32)CT(t)->d.width;
}
i32 nvn_TextureGetHeight(CPU* c, u64 t) {
    (void)c;
    return (i32)CT(t)->d.height;
}
i32 nvn_TextureGetDepth(CPU* c, u64 t) {
    (void)c;
    return (i32)CT(t)->d.depth;
}
i32 nvn_TextureGetLevels(CPU* c, u64 t) {
    (void)c;
    return CT(t)->d.levels;
}
i32 nvn_TextureGetFormat(CPU* c, u64 t) {
    (void)c;
    return CT(t)->d.format;
}
i32 nvn_TextureGetTarget(CPU* c, u64 t) {
    (void)c;
    return CT(t)->d.target;
}
i32 nvn_TextureGetSamples(CPU* c, u64 t) {
    (void)c;
    return CT(t)->d.samples;
}
/* the row stride of a linear texture as built, 0 for a block-linear one */
i64 nvn_TextureGetStride(CPU* c, u64 t) {
    (void)c;
    return (i64)CT(t)->d.stride;
}
i32 nvn_TextureGetStorageSize(CPU* c, u64 t) { /* the builder's answer (nvn_tex_storage), not the extent */
    NvnTexStorage st;
    (void)c;
    nvn_tex_storage(&CT(t)->d, &st);
    return (i32)st.size;
}
u64 nvn_TextureGetMemoryPool(CPU* c, u64 t) {
    (void)c;
    return CT(t)->pool;
}
i64 nvn_TextureGetMemoryOffset(CPU* c, u64 t) {
    (void)c;
    return CT(t)->offset;
}
void nvn_TextureSetDebugLabel(CPU* c, u64 t, u64 label) {
    (void)c;
    (void)t;
    (void)label;
}
/* where a view's first level and layer start in the storage of a texture described by d */
static i64 view_offset(const NvnTexDesc* d, u64 v) {
    const NvnView* w;
    NvnTexLayout l;
    u64 off = 0;
    if (!v) return 0;
    w = NVN_OBJ(const NvnView, v);
    if (w->set & NVN_VIEW_LEVELS) off = nvn_tex_level_offset(d, w->base_level);
    if (w->set & NVN_VIEW_LAYERS) {
        nvn_tex_layout(d, &l);
        off += (u64)w->min_layer * l.layer_size;
    }
    return (i64)off;
}
i64 nvn_TextureGetViewOffset(CPU* c, u64 t, u64 v) {
    (void)c;
    return view_offset(&CT(t)->d, v);
}
/* the CPU and the GPU share guest memory in phase 1: nothing to flush */
void nvn_TextureFlushTexels(CPU* c, u64 t, u64 v, u64 region) {
    (void)c;
    (void)t;
    (void)v;
    (void)region;
}
void nvn_wait_begin(void) {
    KThread* t = kthread_cur();
    if (!t) return;
    core_leave(t);
    kthread_block(t);
}
void nvn_wait_end(void) {
    KThread* t = kthread_cur();
    if (!t) return;
    kthread_unblock(t);
    core_enter(t);
}
/* the texels copied out of the texture by the GPU (gpu/nvn/nvn_copy.c): a deswizzle is the GPU's work, waited for
   off the core */
void nvn_TextureReadTexels(CPU* c, u64 t, u64 v, u64 region, u64 dst) {
    (void)c;
    nvn_wait_begin();
    nvn_read_texels(t, v, region, dst, 0, 0);
    nvn_wait_end();
}
void nvn_TextureWriteTexels(CPU* c, u64 t, u64 v, u64 region, u64 src) {
    (void)c;
    nvn_write_texels(t, v, region, src, 0, 0);
}
/* the channel mapping SetSwizzle gave the builder, R G B A when it gave none */
static u32 tex_swizzle(const NvnTexDesc* d) { return d->swizzle & NVN_TEX_SWIZZLE_SET ? d->swizzle : 2u | 3u << 3 | 4u << 6 | 5u << 9; }
void nvn_TextureGetSwizzle(CPU* c, u64 t, u64 r, u64 g, u64 b, u64 a) {
    u32 w = tex_swizzle(&CT(t)->d);
    (void)c;
    if (r) gw32(r, w & 7);
    if (g) gw32(g, w >> 3 & 7);
    if (b) gw32(b, w >> 6 & 7);
    if (a) gw32(a, w >> 9 & 7);
}
i32 nvn_TextureGetDepthStencilMode(CPU* c, u64 t) {
    (void)c;
    return CT(t)->d.bits & NVN_TEX_STENCIL ? NVN_TEXTURE_DEPTH_STENCIL_MODE_STENCIL : 0;
}
i32 nvn_TextureGetStorageClass(CPU* c, u64 t) {
    NvnTexStorage st;
    (void)c;
    nvn_tex_storage(&CT(t)->d, &st);
    return (i32)(0x100 | st.kind);
}
u64 nvn_TextureGetTextureAddress(CPU* c, u64 t) {
    (void)c;
    return CT(t)->gpu;
}
/* the same texture (games replace a registered descriptor only when the two differ): description, storage and
   channel mapping */
int nvn_TextureCompare(CPU* c, u64 a, u64 b) {
    const NvnTexture *x = CT(a), *y = CT(b);
    (void)c;
    return !memcmp(&x->d, &y->d, sizeof x->d) && x->gpu == y->gpu;
}
u64 nvn_TextureGetZCullStorageSize(CPU* c, u64 t) {
    NvnTexStorage st;
    (void)c;
    nvn_tex_storage(&CT(t)->d, &st);
    return st.zcull;
}
/* the texels written into the storage's layout on the GPU (gpu/nvn/nvn_copy.c): a swizzle is the GPU's work */
void nvn_TextureWriteTexelsStrided(CPU* c, u64 t, u64 v, u64 region, u64 src, i64 row, i64 image) {
    (void)c;
    nvn_write_texels(t, v, region, src, row, image);
}
/* ReadTexels with the destination's rows row bytes apart and its slices image bytes apart (0: tightly packed) */
void nvn_TextureReadTexelsStrided(CPU* c, u64 t, u64 v, u64 region, u64 dst, i64 row, i64 image) {
    (void)c;
    nvn_wait_begin();
    nvn_read_texels(t, v, region, dst, row, image);
    nvn_wait_end();
}
/* The CPU's caches dropped over the texels of a region, for a CPU read of what the GPU wrote: the CPU and the GPU share
   guest memory here, and the GPU's writes land there before the CPU reads them (GPU-owned memory lands when the CPU
   touches it, nx/memory/own.c), so there is nothing to drop, as for FlushTexels */
void nvn_TextureInvalidateTexels(CPU* c, u64 t, u64 v, u64 region) {
    (void)c;
    (void)t;
    (void)v;
    (void)region;
}
u64 nvn_TextureGetDebugID(CPU* c, u64 t) {
    (void)c;
    (void)t;
    return 0; /* the release driver's: only its debug layer numbers objects */
}
/* the raw storage class (NVNrawStorageClass) of a description, by the game's driver's rule: LINEAR (1) for a pitch
   linear texture, else by the format: none (0) for no format, S8 and Z16 (3) for stencil 8 and depth 16, ZF24S8 (4)
   for the 24-bit depths, ZF32_X24S8 (5) for 32-bit float depth with stencil, and color and ZF32 (2) for the rest */
static i32 raw_storage_class(const NvnTexDesc* d) {
    if (d->flags & NVN_TEX_LINEAR) return 1;
    switch (d->format) {
        case 0: return 0;
        case NVN_FORMAT_STENCIL8:
        case NVN_FORMAT_DEPTH16: return 3;
        case NVN_FORMAT_DEPTH24:
        case NVN_FORMAT_DEPTH24_STENCIL8: return 4;
        case NVN_FORMAT_DEPTH32F_STENCIL8: return 5;
        default: return 2;
    }
}
i32 nvn_TextureGetRawStorageClass(CPU* c, u64 t) {
    (void)c;
    return raw_storage_class(&CT(t)->d);
}
/* A sparse texture's tile: the texels of one 64 KiB page of its block-linear layout, as the game's driver answers
   (a 2D tile of 1, 2, 4, 8 or 16 bytes a block is 256x256, 256x128, 128x128, 128x64 or 64x64 blocks, a 3D one
   64x32x32, 32x32x32, 32x32x16, 32x16x16 or 16x16x16), and the levels at least a tile in each dimension, before the
   mip tail. A texture that isn't sparse, or a target or format that can't be, has all zeros */
static void sparse_tile_layout(const NvnTexDesc* d, u64 out) {
    static const u16 tile2d[5][2] = {{256, 256}, {256, 128}, {128, 128}, {128, 64}, {64, 64}};
    static const u16 tile3d[5][3] = {{64, 32, 32}, {32, 32, 32}, {32, 32, 16}, {32, 16, 16}, {16, 16, 16}};
    i32 v[4] = {0, 0, 0, 0}, bw = 1, bh = 1, b = nvn_fmt_bpb(d->format, &bw, &bh), k = 0, level;
    int is3d = d->target == NVN_TEXTURE_TARGET_3D;
    while (k < 5 && (1 << k) != b) k++;
    if ((d->flags & NVN_TEXTURE_FLAGS_SPARSE) && !(d->flags & NVN_TEX_LINEAR) && k < 5 &&
        (is3d || d->target == NVN_TEXTURE_TARGET_2D || d->target == NVN_TEXTURE_TARGET_2D_ARRAY ||
         d->target == NVN_TEXTURE_TARGET_CUBEMAP || d->target == NVN_TEXTURE_TARGET_CUBEMAP_ARRAY)) {
        v[1] = (is3d ? tile3d[k][0] : tile2d[k][0]) * bw;
        v[2] = (is3d ? tile3d[k][1] : tile2d[k][1]) * bh;
        v[3] = is3d ? tile3d[k][2] : 1;
        for (level = 0; level < (i32)d->levels; level++) {
            u32 w = d->width >> level, h = d->height >> level, z = is3d ? d->depth >> level : 1;
            if (w < (u32)v[1] || h < (u32)v[2] || z < (u32)v[3]) break;
        }
        v[0] = level;
    }
    memcpy(GP(out), v, sizeof v);
}
void nvn_TextureGetSparseTileLayout(CPU* c, u64 t, u64 out) {
    (void)c;
    sparse_tile_layout(&CT(t)->d, out);
}
/* The surface description NVIDIA's NvRm library keeps of the texture (NvRmSurface, 0x58 bytes), which one movie
   player takes to have its decoder write into the texture, as the game's driver fills it (SDK 12.3.9, NVN 55):
   0x00 the width and height of level 0, 0x08 the color format (NvColorFormat: the driver knows eight NVN formats,
   others are 0), 0x10 the layout (1 pitch linear, 3 block linear), 0x14 the pitch in bytes, 0x18 the nvmap handle of
   the texture's pool, 0x1c the texture's offset in it, 0x20 the GMMU kind (0 pitch, 0xfe block linear), 0x24 the block
   height (log2 of the GOBs), 0x30 bit 63 once the size is known, 0x38 the size. A block-linear pitch is the row's
   bytes rounded up to the 64-byte GOB (NvRmSurfaceComputePitch), the size the pitch times the rows rounded up to whole
   blocks (NvRmSurfaceComputeSize). The second argument is unused, as in the driver */
void nvn_TextureGetNvRmSurfaceNVX(CPU* c, u64 t, u64 unused, u64 surface) {
    static const struct {
        u16 format;
        u64 color;
    } colors[] = {
        {NVN_FORMAT_R8, 0xf09200408ull},          {NVN_FORMAT_RG8, 0xf08440710ull},
        {NVN_FORMAT_RGBA8, 0x200532120ull},       {NVN_FORMAT_DEPTH32F_STENCIL8, 0x20a532120ull},
        {NVN_FORMAT_RGBX8_SRGB, 0x200532120ull},  {NVN_FORMAT_RGB565, 0x20a881210ull},
        {NVN_FORMAT_RGBX8, 0x20a532120ull},       {NVN_FORMAT_BGRA8, 0x200d12120ull},
        {NVN_FORMAT_BGRA8_SRGB, 0x200d12120ull},
    };
    const NvnTexture* o = CT(t);
    u8 s[0x58];
    u64 color = 0, size = 0, flags = 0;
    u32 width = o->d.width, height = o->d.height, layout, pitch, kind, block_h = 0, bits;
    size_t i;
    (void)c;
    (void)unused;
    for (i = 0; i < sizeof colors / sizeof *colors; i++)
        if (colors[i].format == o->d.format) color = colors[i].color;
    bits = (u32)(color & 0xff); /* the format's bits per pixel */
    if (o->d.flags & NVN_TEX_LINEAR) {
        layout = 1;
        pitch = o->d.stride;
        kind = 0;
        size = (u64)pitch * height;
    } else {
        unsigned th, td;
        nvn_tex_level_tiles(&o->d, 0, &th, &td);
        layout = 3;
        kind = 0xfe;
        block_h = th;
        pitch = (u32)((((u64)width * bits + 7) / 8 + 63) & ~63ull);
        size = (u64)pitch * (((u64)height + (8u << th) - 1) & ~((u64)(8u << th) - 1));
    }
    if (size) flags = 1ull << 63;
    memset(s, 0, sizeof s);
    snx_st_le32(s, width);
    snx_st_le32(s + 0x04, height);
    snx_st_le64(s + 0x08, color);
    snx_st_le32(s + 0x10, layout);
    snx_st_le32(s + 0x14, pitch);
    snx_st_le32(s + 0x18, o->pool ? pool_nvmap(o->pool) : 0);
    snx_st_le32(s + 0x1c, (u32)o->offset);
    snx_st_le32(s + 0x20, kind);
    snx_st_le32(s + 0x24, block_h);
    snx_st_le64(s + 0x30, flags);
    snx_st_le64(s + 0x38, (u32)size);
    memcpy(GP(surface), s, sizeof s);
}

/* ============================================================ texture views */
#define VW(v) NVN_OBJ(NvnView, v)
/* the same view: the same parts set, and each part set the same */
int nvn_TextureViewCompare(CPU* c, u64 a, u64 b) {
    const NvnView *x = NVN_OBJ(const NvnView, a), *y = NVN_OBJ(const NvnView, b);
    (void)c;
    if (x->set != y->set) return 0;
    if ((x->set & NVN_VIEW_LEVELS) && (x->base_level != y->base_level || x->levels != y->levels)) return 0;
    if ((x->set & NVN_VIEW_LAYERS) && (x->min_layer != y->min_layer || x->layers != y->layers)) return 0;
    if ((x->set & NVN_VIEW_FORMAT) && x->format != y->format) return 0;
    if ((x->set & NVN_VIEW_TARGET) && x->target != y->target) return 0;
    if ((x->set & NVN_VIEW_SWIZZLE) && memcmp(x->swizzle, y->swizzle, sizeof x->swizzle)) return 0;
    return 1;
}
void nvn_TextureViewSetDefaults(CPU* c, u64 v) {
    (void)c;
    memset(GP(v), 0, sizeof(NvnView));
}
void nvn_TextureViewSetLevels(CPU* c, u64 v, i32 base, i32 n) {
    NvnView* o = VW(v);
    (void)c;
    o->set |= NVN_VIEW_LEVELS;
    o->base_level = (u8)base;
    o->levels = (u8)n;
}
void nvn_TextureViewSetLayers(CPU* c, u64 v, i32 min, i32 n) {
    NvnView* o = VW(v);
    (void)c;
    o->set |= NVN_VIEW_LAYERS;
    o->min_layer = (u16)min;
    o->layers = (u16)n;
}
void nvn_TextureViewSetFormat(CPU* c, u64 v, i32 format) {
    NvnView* o = VW(v);
    (void)c;
    o->set |= NVN_VIEW_FORMAT;
    o->format = (u16)format;
}
void nvn_TextureViewSetTarget(CPU* c, u64 v, i32 target) {
    NvnView* o = VW(v);
    (void)c;
    o->set |= NVN_VIEW_TARGET;
    o->target = (u8)target;
}
void nvn_TextureViewSetDepthStencilMode(CPU* c, u64 v, i32 mode) {
    NvnView* o = VW(v);
    (void)c;
    o->set = (u8)((o->set & 0x3fu) | NVN_VIEW_DSMODE | ((u32)mode & 3u) << 6);
}
int nvn_TextureViewGetLevels(CPU* c, u64 v, u64 base, u64 n) {
    const NvnView* o = VW(v);
    (void)c;
    gw32(base, o->base_level);
    gw32(n, o->levels);
    return (o->set & NVN_VIEW_LEVELS) != 0;
}
int nvn_TextureViewGetLayers(CPU* c, u64 v, u64 min, u64 n) {
    const NvnView* o = VW(v);
    (void)c;
    gw32(min, o->min_layer);
    gw32(n, o->layers);
    return (o->set & NVN_VIEW_LAYERS) != 0;
}
int nvn_TextureViewGetFormat(CPU* c, u64 v, u64 format) {
    const NvnView* o = VW(v);
    (void)c;
    gw32(format, o->format);
    return (o->set & NVN_VIEW_FORMAT) != 0;
}
int nvn_TextureViewGetTarget(CPU* c, u64 v, u64 target) {
    const NvnView* o = VW(v);
    (void)c;
    gw32(target, o->target);
    return (o->set & NVN_VIEW_TARGET) != 0;
}
int nvn_TextureViewGetSwizzle(CPU* c, u64 v, u64 r, u64 g, u64 b, u64 a) {
    const NvnView* o = VW(v);
    (void)c;
    gw32(r, o->swizzle[0]);
    gw32(g, o->swizzle[1]);
    gw32(b, o->swizzle[2]);
    gw32(a, o->swizzle[3]);
    return (o->set & NVN_VIEW_SWIZZLE) != 0;
}
int nvn_TextureViewGetDepthStencilMode(CPU* c, u64 v, u64 mode) {
    const NvnView* o = VW(v);
    (void)c;
    gw32(mode, (u32)o->set >> 6);
    return (o->set & NVN_VIEW_DSMODE) != 0;
}
void nvn_TextureViewSetSwizzle(CPU* c, u64 v, i32 r, i32 g, i32 b, i32 a) {
    NvnView* o = VW(v);
    (void)c;
    o->set |= NVN_VIEW_SWIZZLE;
    o->swizzle[0] = (u8)r;
    o->swizzle[1] = (u8)g;
    o->swizzle[2] = (u8)b;
    o->swizzle[3] = (u8)a;
}

/* ============================================================ samplers */
#define SB(b) (&NVN_OBJ(NvnSamplerBuilder, b)->p)
void nvn_SamplerBuilderSetDefaults(CPU* c, u64 b) {
    NvnSamplerPar* o = SB(b);
    (void)c;
    memset(GP(b), 0, sizeof(NvnSamplerBuilder));
    o->min_filter = NVN_MIN_FILTER_NEAREST_MIPMAP_LINEAR; /* GL's defaults */
    o->mag_filter = NVN_MAG_FILTER_LINEAR;
    o->wrap_s = o->wrap_t = o->wrap_r = NVN_WRAP_MODE_REPEAT;
    o->compare_func = NVN_COMPARE_FUNC_LESS;
    o->lod_min = -1000.f;
    o->lod_max = 1000.f;
    o->anisotropy = 1.f;
}
/* the runtime has one device, which the builder needn't keep: its bytes hold the sampler's state */
void nvn_SamplerBuilderSetDevice(CPU* c, u64 b, u64 d) {
    (void)c;
    (void)b;
    (void)d;
}
void nvn_SamplerBuilderSetMinMagFilter(CPU* c, u64 b, i32 min, i32 mag) {
    (void)c;
    SB(b)->min_filter = (u8)min;
    SB(b)->mag_filter = (u8)mag;
}
void nvn_SamplerBuilderSetWrapMode(CPU* c, u64 b, i32 s, i32 t, i32 r) {
    NvnSamplerPar* o = SB(b);
    (void)c;
    o->wrap_s = (u8)s;
    o->wrap_t = (u8)t;
    o->wrap_r = (u8)r;
}
void nvn_SamplerBuilderSetLodClamp(CPU* c, u64 b, f32 min, f32 max) {
    (void)c;
    SB(b)->lod_min = min;
    SB(b)->lod_max = max;
}
void nvn_SamplerBuilderSetLodBias(CPU* c, u64 b, f32 bias) {
    (void)c;
    SB(b)->lod_bias = bias;
}
void nvn_SamplerBuilderSetCompare(CPU* c, u64 b, i32 mode, i32 func) {
    (void)c;
    SB(b)->compare_mode = (u8)mode;
    SB(b)->compare_func = (u8)func;
}
void nvn_SamplerBuilderSetBorderColor(CPU* c, u64 b, u64 color) {
    (void)c;
    memcpy(SB(b)->border, GP(color), sizeof SB(b)->border);
}
void nvn_SamplerBuilderSetReductionFilter(CPU* c, u64 b, i32 filter) {
    (void)c;
    SB(b)->reduction = (u8)filter;
}
void nvn_SamplerBuilderSetMaxAnisotropy(CPU* c, u64 b, f32 anisotropy) {
    (void)c;
    SB(b)->anisotropy = anisotropy;
}
/* the LOD snap, kept for its getter: the hardware's trilinear optimization (TSC word 1, 26-30), which Vulkan's
   samplers have no counterpart of */
void nvn_SamplerBuilderSetLodSnap(CPU* c, u64 b, f32 snap) {
    (void)c;
    SB(b)->lod_snap = snap;
}
f32 nvn_SamplerBuilderGetLodSnap(CPU* c, u64 b) {
    (void)c;
    return SB(b)->lod_snap;
}
/* integer border colors: their words as given, in the same place as the floats' (the game's driver keeps all three
   kinds as raw words, and the sampler descriptor takes them so) */
void nvn_SamplerBuilderSetBorderColori(CPU* c, u64 b, u64 color) { nvn_SamplerBuilderSetBorderColor(c, b, color); }
void nvn_SamplerBuilderSetBorderColorui(CPU* c, u64 b, u64 color) { nvn_SamplerBuilderSetBorderColor(c, b, color); }
void nvn_SamplerBuilderGetBorderColor(CPU* c, u64 b, u64 rgba) {
    (void)c;
    memcpy(GP(rgba), SB(b)->border, sizeof SB(b)->border);
}
void nvn_SamplerBuilderGetBorderColori(CPU* c, u64 b, u64 rgba) { nvn_SamplerBuilderGetBorderColor(c, b, rgba); }
void nvn_SamplerBuilderGetBorderColorui(CPU* c, u64 b, u64 rgba) { nvn_SamplerBuilderGetBorderColor(c, b, rgba); }
u64 nvn_SamplerBuilderGetDevice(CPU* c, u64 b) {
    (void)c;
    (void)b;
    return nvn_device();
}
void nvn_SamplerBuilderGetMinMagFilter(CPU* c, u64 b, u64 min, u64 mag) {
    (void)c;
    gw32(min, SB(b)->min_filter);
    gw32(mag, SB(b)->mag_filter);
}
void nvn_SamplerBuilderGetWrapMode(CPU* c, u64 b, u64 ws, u64 wt, u64 wr) {
    (void)c;
    gw32(ws, SB(b)->wrap_s);
    gw32(wt, SB(b)->wrap_t);
    gw32(wr, SB(b)->wrap_r);
}
void nvn_SamplerBuilderGetLodClamp(CPU* c, u64 b, u64 min, u64 max) {
    (void)c;
    memcpy(GP(min), &SB(b)->lod_min, 4);
    memcpy(GP(max), &SB(b)->lod_max, 4);
}
f32 nvn_SamplerBuilderGetLodBias(CPU* c, u64 b) {
    (void)c;
    return SB(b)->lod_bias;
}
void nvn_SamplerBuilderGetCompare(CPU* c, u64 b, u64 mode, u64 func) {
    (void)c;
    gw32(mode, SB(b)->compare_mode);
    gw32(func, SB(b)->compare_func);
}
f32 nvn_SamplerBuilderGetMaxAnisotropy(CPU* c, u64 b) {
    (void)c;
    return SB(b)->anisotropy;
}
i32 nvn_SamplerBuilderGetReductionFilter(CPU* c, u64 b) {
    (void)c;
    return SB(b)->reduction;
}
int nvn_SamplerInitialize(CPU* c, u64 s, u64 b) {
    NvnSampler o = {0};
    (void)c;
    o.magic = NVN_MAGIC('S', 'M', 'P', 0);
    o.p = *SB(b);
    o.serial = nvn_serial();
    memcpy(GP(s), &o, sizeof o);
    return 1;
}
void nvn_SamplerSetDebugLabel(CPU* c, u64 s, u64 label) {
    (void)c;
    (void)s;
    (void)label;
}
#define SP(s) (&NVN_OBJ(const NvnSampler, s)->p)
void nvn_SamplerGetMinMagFilter(CPU* c, u64 s, u64 min, u64 mag) {
    (void)c;
    gw32(min, SP(s)->min_filter);
    gw32(mag, SP(s)->mag_filter);
}
void nvn_SamplerGetWrapMode(CPU* c, u64 s, u64 ws, u64 wt, u64 wr) {
    (void)c;
    gw32(ws, SP(s)->wrap_s);
    gw32(wt, SP(s)->wrap_t);
    gw32(wr, SP(s)->wrap_r);
}
void nvn_SamplerGetLodClamp(CPU* c, u64 s, u64 min, u64 max) {
    (void)c;
    memcpy(GP(min), &SP(s)->lod_min, 4);
    memcpy(GP(max), &SP(s)->lod_max, 4);
}
f32 nvn_SamplerGetLodBias(CPU* c, u64 s) {
    (void)c;
    return SP(s)->lod_bias;
}
void nvn_SamplerGetCompare(CPU* c, u64 s, u64 mode, u64 func) {
    (void)c;
    gw32(mode, SP(s)->compare_mode);
    gw32(func, SP(s)->compare_func);
}
void nvn_SamplerGetBorderColor(CPU* c, u64 s, u64 rgba) {
    (void)c;
    memcpy(GP(rgba), SP(s)->border, 16);
}
void nvn_SamplerGetBorderColori(CPU* c, u64 s, u64 rgba) { nvn_SamplerGetBorderColor(c, s, rgba); }
void nvn_SamplerGetBorderColorui(CPU* c, u64 s, u64 rgba) { nvn_SamplerGetBorderColor(c, s, rgba); }
/* the release driver's debug IDs are all 0: only its debug layer numbers objects */
u64 nvn_SamplerGetDebugID(CPU* c, u64 s) {
    (void)c;
    (void)s;
    return 0;
}
f32 nvn_SamplerGetMaxAnisotropy(CPU* c, u64 s) {
    (void)c;
    return SP(s)->anisotropy;
}
i32 nvn_SamplerGetReductionFilter(CPU* c, u64 s) {
    (void)c;
    return SP(s)->reduction;
}
/* the same sampler state (games deduplicate sampler pool entries by it) */
int nvn_SamplerCompare(CPU* c, u64 a, u64 b) {
    const NvnSamplerPar *x = SP(a), *y = SP(b);
    (void)c;
    return x->min_filter == y->min_filter && x->mag_filter == y->mag_filter && x->wrap_s == y->wrap_s &&
           x->wrap_t == y->wrap_t && x->wrap_r == y->wrap_r && x->compare_mode == y->compare_mode &&
           x->compare_func == y->compare_func && x->reduction == y->reduction && x->lod_min == y->lod_min &&
           x->lod_max == y->lod_max && x->lod_bias == y->lod_bias && x->anisotropy == y->anisotropy &&
           x->lod_snap == y->lod_snap &&
           !memcmp(x->border, y->border, sizeof x->border);
}
void nvn_SamplerFinalize(CPU* c, u64 s) {
    (void)c;
    NVN_OBJ(NvnSampler, s)->magic = 0;
}

/* ============================================================ programs */
int nvn_ProgramInitialize(CPU* c, u64 p, u64 d) {
    (void)c;
    (void)d;
    memset(GP(p), 0, sizeof(NvnProgram));
    NVN_OBJ(NvnProgram, p)->magic = NVN_MAGIC('P', 'R', 'O', 'G');
    return 1;
}
void nvn_ProgramFinalize(CPU* c, u64 p) {
    (void)c;
    NVN_OBJ(NvnProgram, p)->magic = 0;
}
/* A stage's control section, as the game's driver reads it (its accessor for the newest format, which one game's
   format 8 sections share): 0x00 the magic, 0x04 1, 0x08 the format, 0x700 the constants' offset in the code area,
   0x704 the code area's bytes, 0x708 the program's offset (past the driver's 0x30-byte wrapper in a graphics stage),
   0x714 the stage, for a vertex program 0x748/0x74c and 0x754/0x758 VertexA's and VertexB's offsets and sizes,
   both nonzero when it is dual, and for a compute program 0x6fc its constants' bytes, 0x748-0x750 its workgroup size
   and 0x754 its shared memory. data: the code area's GPU address. 0 when the section isn't one */
static int stage_of(NvnProgram* o, u64 data, u64 control) {
    u32 magic = gr32(control), one = gr32(control + 4), format = gr32(control + 8), cb = gr32(control + 0x700),
        size = gr32(control + 0x704), off = gr32(control + 0x708), stage = gr32(control + 0x714);
    NvnStage* st;
    /* a graphics stage's program follows the driver's 0x30-byte wrapper; an older SDK's compute program (format 14)
       starts its code area at 0, with no wrapper (its first words are a scheduling word, 0x001fc000e22007f0): its
       SetShaders failing, one game's effects system went on to read the program's interface table, which the
       failure left out, and crashed */
    if (magic != 0x98761234u || one != 1 || format < 5 || format > 16 || stage > 5 ||
        (off < 0x30 && stage != NVN_SHADER_STAGE_COMPUTE) || cb < off ||
        size < cb || (cb & 0xff) || (size & 0xff) || size >> 24 || !data) {
        LOG_LIMIT(8, "nvn: a shader's control section at %llx isn't one the runtime reads (magic %x, format %u, stage %u, "
                     "program at %#x, constants %#x of %#x)\n",
                  (unsigned long long)control, magic, format, stage, off, cb, size);
        return 0;
    }
    if (format != 8 && format != 16) LOG_LIMIT(1, "nvn: shader control sections of format %u\n", format);
    st = &o->st[stage];
    st->data = data;
    st->off = off;
    st->cb = (u16)(cb >> 8);
    st->cb_size = (u16)((size - cb) >> 8);
    /* a compute program's workgroup size, shared memory and constants' bytes: 0x748-0x750, 0x754 and 0x6fc */
    if (stage == NVN_SHADER_STAGE_COMPUTE) {
        o->block[0] = (u16)gr32(control + 0x748);
        o->block[1] = (u16)gr32(control + 0x74c);
        o->block[2] = (u16)gr32(control + 0x750);
        o->off_a = gr32(control + 0x754);
        st->cb_size = (u16)((gr32(control + 0x6fc) + 0xff) >> 8);
    } else if (stage == NVN_SHADER_STAGE_VERTEX && gr32(control + 0x74c) && gr32(control + 0x758)) {
        o->off_a = gr32(control + 0x748);
        st->off = gr32(control + 0x754);
    }
    return 1;
}
/* ---- transform feedback layouts: the programs whose last stage before rasterization captures, by serial (their
   128-byte layouts don't fit the program object). Programs that capture are few, and their entries stay */
int nvn_program_xfb(u64 serial, ShXfb* x) {
    u32 i;
    int found = 0;
    pm_lock(&g_nvn.object.xfb_lock);
    for (i = g_nvn.object.nxfb; i-- > 0 && !found;)
        if (g_nvn.object.xfb[i].serial == serial) {
            *x = g_nvn.object.xfb[i].x;
            found = 1;
        }
    pm_unlock(&g_nvn.object.xfb_lock);
    return found;
}
/* The capture layout a stage's control section gives, as the game's driver reads it into the 3D engine's
   transform feedback registers: 0x6ec set when it captures; per buffer i, its stream at 0x6e4 + i, its stride
   (u16) at 0x6dc + 2 i and its groups at 0x6e8 + i, each group j an output location (s8) at 0xdc + 0x80 i + j,
   its first component at 0x2dc + 0x80 i + j and its component count at 0x4dc + 0x80 i + j. Each component
   captured is an output attribute word, from the driver's table of locations (the position, the built-ins, the
   clip distances, then generic n at 0x20 + 4 n). The varying count is the stride's words */
static void xfb_of(u64 control, u64 serial) {
    static const u8 word_of[32] = {0x1c, 0xa0, 0xa4, 0xa8, 0xac, 0xba, 0x1b, 0xc0, 0xc4, 0xc8, 0xcc,
                                   0xd0, 0xd4, 0xd8, 0xdc, 0xe0, 0xe4, 0xb0, 0xb1, 0xb2, 0xb3, 0xb4,
                                   0xb5, 0xb6, 0xb7, 0xbf, 0x1a, 0x19, 0xe8, 0xff, 0x18, 0xff};
    const u8* c = GP(control);
    NvnXfbEntry e;
    u32 i, j, k;
    if (!c[0x6ec]) return;
    memset(&e, 0, sizeof e);
    e.serial = serial;
    for (i = 0; i < 4; i++) {
        u32 stride = gr16(control + 0x6dc + 2 * (u64)i), groups = c[0x6e8 + i], n = 0;
        if (c[0x6e4 + i])
            LOG_LIMIT(1, "nvn: transform feedback into stream %u is captured as stream 0\n", c[0x6e4 + i]);
        e.x.stride[i] = (u16)stride;
        e.x.count[i] = (u8)(stride / 4 > 128 ? 128 : stride / 4);
        for (j = 0; j < groups && j < 0x80; j++) {
            i32 loc = (i8)c[0xdc + 0x80 * i + j];
            u32 start = c[0x2dc + 0x80 * i + j], count = c[0x4dc + 0x80 * i + j];
            u32 word = loc < 0 ? 0xff : loc < 32 ? word_of[loc] : 0x20 + 4 * (u32)(loc - 0x20);
            for (k = 0; k < count && n < 128; k++) e.x.layout[i][n++] = (u8)(word == 0xff ? 0xff : word + start + k);
        }
    }
    pm_lock(&g_nvn.object.xfb_lock);
    if (g_nvn.object.nxfb == g_nvn.object.xfb_cap) {
        u32 cap = g_nvn.object.xfb_cap ? 2 * g_nvn.object.xfb_cap : 16;
        NvnXfbEntry* m = (NvnXfbEntry*)realloc(g_nvn.object.xfb, cap * sizeof *m);
        if (!m) fatal("nvn: out of memory for transform feedback layouts");
        g_nvn.object.xfb = m;
        g_nvn.object.xfb_cap = cap;
    }
    g_nvn.object.xfb[g_nvn.object.nxfb++] = e;
    pm_unlock(&g_nvn.object.xfb_lock);
}

/* the stages' {code, control} pairs, read from the control sections while they are the caller's */
static int set_shaders(u64 p, i32 n, u64 sd, u64 stride) {
    NvnProgram* o = NVN_OBJ(NvnProgram, p);
    u64 last = 0; /* the control section of the last stage before rasterization */
    i32 i, last_rank = -1;
    if (n < 0 || n > 6) {
        LOG_LIMIT(8, "nvn: nvnProgramSetShaders with %d stages\n", n);
        return 0;
    }
    memset(o->st, 0, sizeof o->st);
    memset(o->block, 0, sizeof o->block);
    o->off_a = 0;
    for (i = 0; i < n; i++) {
        u64 control = gr64(sd + stride * (u64)i + 8);
        u32 stage;
        i32 rank;
        if (!stage_of(o, gr64(sd + stride * (u64)i), control)) return 0;
        /* geometry, else tessellation evaluation, else control, else vertex: the driver's order */
        stage = gr32(control + 0x714);
        rank = stage == NVN_SHADER_STAGE_GEOMETRY          ? 3
               : stage == NVN_SHADER_STAGE_TESS_EVALUATION ? 2
               : stage == NVN_SHADER_STAGE_TESS_CONTROL    ? 1
               : stage == NVN_SHADER_STAGE_VERTEX          ? 0
                                                           : -1;
        if (rank > last_rank) {
            last_rank = rank;
            last = control;
        }
    }
    o->serial = nvn_serial();
    /* the sample shading SetSampleShading asked for takes effect here, as the game's driver applies it in SetShaders */
    o->sample_shading = (u16)((o->sample_shading & 0xff) | (o->sample_shading & 0xff) << 8);
    if (last) xfb_of(last, o->serial);
    return 1;
}
int nvn_ProgramSetShaders(CPU* c, u64 p, i32 n, u64 sd) {
    (void)c;
    return set_shaders(p, n, sd, sizeof(NVNshaderData));
}

/* NVN 55's SetShaders: 0x18 bytes a stage, NVNshaderData and an 8-byte extension, which nxnative doesn't use either */
int nvn_ProgramSetShadersExt(CPU* c, u64 p, i32 n, u64 sd) {
    (void)c;
    return set_shaders(p, n, sd, 0x18);
}

/* the sample shading of the program's next SetShaders: NVN_SAMPLE_SHADING_* (0 as the program says, 1 every sample, 2
   not every sample), kept in the low byte until SetShaders applies it (the high byte, which the draws read) */
void nvn_ProgramSetSampleShading(CPU* c, u64 p, i32 mode) {
    NvnProgram* o = NVN_OBJ(NvnProgram, p);
    (void)c;
    o->sample_shading = (u16)((o->sample_shading & 0xff00) | ((u32)mode & 0x3f));
}
/* the tiled cache's subtile size a program asks for: the game's driver's SetShaders sets it to 0 and nothing in the
   API sets it otherwise (NVN 55.15). The renderer has no tiled cache */
i32 nvn_ProgramGetSubtileSize(CPU* c, u64 p) {
    (void)c;
    (void)p;
    return 0;
}
/* Subroutine linkage maps the program's subroutine uniforms to their functions. The game's driver answers false for
   a program whose stages have none, which is every program here: the translator runs no subroutines, so a program
   with them couldn't draw as the game asks */
int nvn_ProgramSetSubroutineLinkage(CPU* c, u64 p, i32 n, u64 maps) {
    (void)c;
    if (n > 0 && maps)
        LOG_LIMIT(1, "nvn: program %llx's subroutine linkage (%d stages): the runtime's programs have no subroutines\n",
                  (unsigned long long)p, n);
    return 0;
}

/* ============================================================ syncs */
/* OpenGL interop: there is no OpenGL here. InitializeFromFencedGLSync fails as the game's driver fails without one
   (status 1, false), and CreateGLSync answers 4, its failure, writing nothing */
int nvn_SyncInitializeFromFencedGLSync(CPU* c, u64 s, u64 d, u32 gl_sync, u64 status) {
    (void)c;
    (void)d;
    (void)gl_sync;
    memset(GP(s), 0, sizeof(NvnSync));
    if (status) gw32(status, 1);
    LOG_LIMIT(1, "nvn: the game asked for an OpenGL sync, which the runtime has no OpenGL for\n");
    return 0;
}
i32 nvn_SyncCreateGLSync(CPU* c, u64 s, u64 gl_sync) {
    (void)c;
    (void)s;
    (void)gl_sync;
    LOG_LIMIT(1, "nvn: the game asked for an OpenGL sync, which the runtime has no OpenGL for\n");
    return 4;
}
int nvn_SyncInitialize(CPU* c, u64 s, u64 d) {
    NvnSync o = {0};
    (void)c;
    (void)d;
    o.magic = NVN_MAGIC('S', 'Y', 'N', 'C');
    memcpy(GP(s), &o, sizeof o);
    return 1;
}

void nvn_SyncFinalize(CPU* c, u64 s) {
    (void)c;
    NVN_OBJ(NvnSync, s)->magic = 0;
}

/* ============================================================ events */
void nvn_EventBuilderSetDefaults(CPU* c, u64 b) {
    (void)c;
    memset(GP(b), 0, sizeof(NvnEventBuilder));
}
void nvn_EventBuilderSetStorage(CPU* c, u64 b, u64 pool, i64 offset) {
    NvnEventBuilder* o = NVN_OBJ(NvnEventBuilder, b);
    (void)c;
    o->pool = pool;
    o->offset = offset;
}
int nvn_EventInitialize(CPU* c, u64 e, u64 b) {
    const NvnEventBuilder* s = NVN_OBJ(NvnEventBuilder, b);
    u64 memory = nvn_pool_memory(s->pool);
    NvnEvent o = {0};
    (void)c;
    if (!memory) {
        LOG_LIMIT(8, "nvn: event %llx has no storage\n", (unsigned long long)e);
        return 0;
    }
    o.magic = NVN_MAGIC('E', 'V', 'N', 'T');
    o.value = memory + (u64)s->offset;
    o.gpu = nvn_pool_gpu(s->pool) + (u64)s->offset;
    o.pool = s->pool;
    o.offset = s->offset;
    memcpy(GP(e), &o, sizeof o);
    return 1;
}
void nvn_EventFinalize(CPU* c, u64 e) {
    (void)c;
    NVN_OBJ(NvnEvent, e)->magic = 0;
}
u32 nvn_EventGetValue(CPU* c, u64 e) {
    (void)c;
    return gr32(NVN_OBJ(NvnEvent, e)->value);
}
void nvn_EventSignal(CPU* c, u64 e, i32 mode, u32 value) {
    u64 a = NVN_OBJ(NvnEvent, e)->value;
    (void)c;
    gw32(a, mode == NVN_EVENT_SIGNAL_MODE_ADD ? gr32(a) + value : value);
}
u64 nvn_EventBuilderGetMemoryPool(CPU* c, u64 b) {
    (void)c;
    return NVN_OBJ(const NvnEventBuilder, b)->pool;
}
i64 nvn_EventBuilderGetMemoryOffset(CPU* c, u64 b) {
    (void)c;
    return NVN_OBJ(const NvnEventBuilder, b)->offset;
}
/* the pool, with the offset written at *offset */
u64 nvn_EventBuilderGetStorage(CPU* c, u64 b, u64 offset) {
    const NvnEventBuilder* o = NVN_OBJ(const NvnEventBuilder, b);
    (void)c;
    gw64(offset, (u64)o->offset);
    return o->pool;
}
u64 nvn_EventGetMemoryPool(CPU* c, u64 e) {
    (void)c;
    return NVN_OBJ(const NvnEvent, e)->pool;
}
i64 nvn_EventGetMemoryOffset(CPU* c, u64 e) {
    (void)c;
    return NVN_OBJ(const NvnEvent, e)->offset;
}

/* ============================================================ state objects (nvn_obj.h's packed fields) */
void nvn_BlendStateSetDefaults(CPU* c, u64 s) {
    u64 w = 0;
    (void)c;
    w = nvn_set_field(w, NVN_BLEND_SRC_RGB, NVN_BLEND_FUNC_ONE);
    w = nvn_set_field(w, NVN_BLEND_DST_RGB, NVN_BLEND_FUNC_ZERO);
    w = nvn_set_field(w, NVN_BLEND_SRC_A, NVN_BLEND_FUNC_ONE);
    w = nvn_set_field(w, NVN_BLEND_DST_A, NVN_BLEND_FUNC_ZERO);
    w = nvn_set_field(w, NVN_BLEND_EQ_RGB, NVN_BLEND_EQUATION_ADD);
    w = nvn_set_field(w, NVN_BLEND_EQ_A, NVN_BLEND_EQUATION_ADD);
    /* the advanced mode's: a premultiplied source and a normalized destination, uncorrelated (the driver's) */
    w = nvn_set_field(w, NVN_BLEND_ADV_PMS, 1);
    w = nvn_set_field(w, NVN_BLEND_ADV_NPM, 1);
    st64(s, w);
}
/* The advanced blend modes (NV_blend_equation_advanced): kept in the state, as the game's driver keeps them. The
   renderer draws a target with an advanced mode as plain blending (gpu/nvn/nvn_draw.c make_blend logs it): Vulkan has
   them only with VK_EXT_blend_operation_advanced, which it doesn't use */
void nvn_BlendStateSetAdvancedMode(CPU* c, u64 s, i32 mode) {
    (void)c;
    SET64(s, NVN_BLEND_ADV, mode);
}
i32 nvn_BlendStateGetAdvancedMode(CPU* c, u64 s) {
    (void)c;
    return (i32)nvn_field(ld64(s), NVN_BLEND_ADV);
}
void nvn_BlendStateSetAdvancedOverlap(CPU* c, u64 s, i32 overlap) {
    (void)c;
    SET64(s, NVN_BLEND_ADV_OVL, overlap);
}
i32 nvn_BlendStateGetAdvancedOverlap(CPU* c, u64 s) {
    (void)c;
    return (i32)nvn_field(ld64(s), NVN_BLEND_ADV_OVL);
}
void nvn_BlendStateSetAdvancedPremultipliedSrc(CPU* c, u64 s, int on) {
    (void)c;
    SET64(s, NVN_BLEND_ADV_PMS, on != 0);
}
int nvn_BlendStateGetAdvancedPremultipliedSrc(CPU* c, u64 s) {
    (void)c;
    return (int)nvn_field(ld64(s), NVN_BLEND_ADV_PMS);
}
void nvn_BlendStateSetAdvancedNormalizedDst(CPU* c, u64 s, int on) {
    (void)c;
    SET64(s, NVN_BLEND_ADV_NPM, on != 0);
}
int nvn_BlendStateGetAdvancedNormalizedDst(CPU* c, u64 s) {
    (void)c;
    return (int)nvn_field(ld64(s), NVN_BLEND_ADV_NPM);
}
void nvn_BlendStateSetBlendTarget(CPU* c, u64 s, i32 target) {
    (void)c;
    SET64(s, NVN_BLEND_TARGET, target);
}
void nvn_BlendStateSetBlendFunc(CPU* c, u64 s, i32 src_rgb, i32 dst_rgb, i32 src_a, i32 dst_a) {
    u64 w = ld64(s);
    (void)c;
    w = nvn_set_field(w, NVN_BLEND_SRC_RGB, (u64)src_rgb);
    w = nvn_set_field(w, NVN_BLEND_DST_RGB, (u64)dst_rgb);
    w = nvn_set_field(w, NVN_BLEND_SRC_A, (u64)src_a);
    w = nvn_set_field(w, NVN_BLEND_DST_A, (u64)dst_a);
    st64(s, w);
}
void nvn_BlendStateSetBlendEquation(CPU* c, u64 s, i32 rgb, i32 alpha) {
    u64 w = ld64(s);
    (void)c;
    w = nvn_set_field(w, NVN_BLEND_EQ_RGB, (u64)rgb);
    w = nvn_set_field(w, NVN_BLEND_EQ_A, (u64)alpha);
    st64(s, w);
}

void nvn_ColorStateSetDefaults(CPU* c, u64 s) {
    u64 w = 0;
    (void)c;
    w = nvn_set_field(w, NVN_COLOR_LOGIC_OP, NVN_LOGIC_OP_COPY);
    w = nvn_set_field(w, NVN_COLOR_ALPHA_FUNC, NVN_ALPHA_FUNC_ALWAYS);
    st32(s, (u32)w);
}
void nvn_ColorStateSetBlendEnable(CPU* c, u64 s, i32 i, int enable) {
    u32 w = ld32(s);
    (void)c;
    if (i < 0 || i > 7) return;
    w = enable ? w | 1u << i : w & ~(1u << i); /* NVN_COLOR_BLEND_EN is bits 0-7 */
    st32(s, w);
}
int nvn_ColorStateGetBlendEnable(CPU* c, u64 s, i32 i) {
    (void)c;
    return i >= 0 && i < 8 && (nvn_field(ld32(s), NVN_COLOR_BLEND_EN) >> i & 1);
}
void nvn_ColorStateSetLogicOp(CPU* c, u64 s, i32 op) {
    (void)c;
    SET32(s, NVN_COLOR_LOGIC_OP, op);
}
void nvn_ColorStateSetAlphaTest(CPU* c, u64 s, i32 func) {
    (void)c;
    SET32(s, NVN_COLOR_ALPHA_FUNC, func);
}

void nvn_ChannelMaskStateSetDefaults(CPU* c, u64 s) {
    (void)c;
    st32(s, 0xffffffffu);
}
void nvn_ChannelMaskStateSetChannelMask(CPU* c, u64 s, i32 i, int r, int g, int b, int a) {
    u32 m = (r ? 1u : 0) | (g ? 2u : 0) | (b ? 4u : 0) | (a ? 8u : 0);
    (void)c;
    if (i < 0 || i > 7) return;
    st32(s, (ld32(s) & ~(0xfu << (4 * i))) | m << (4 * i));
}

void nvn_MultisampleStateSetDefaults(CPU* c, u64 s) {
    NvnMultisample o = {0};
    (void)c;
    o.enable = 1;
    memcpy(GP(s), &o, sizeof o);
}
void nvn_MultisampleStateSetMultisampleEnable(CPU* c, u64 s, int enable) {
    (void)c;
    NVN_OBJ(NvnMultisample, s)->enable = enable != 0;
}
void nvn_MultisampleStateSetSamples(CPU* c, u64 s, i32 n) {
    (void)c;
    NVN_OBJ(NvnMultisample, s)->samples = (u8)n;
}
void nvn_MultisampleStateSetAlphaToCoverageEnable(CPU* c, u64 s, int enable) {
    (void)c;
    NVN_OBJ(NvnMultisample, s)->alpha_to_coverage = enable != 0;
}

void nvn_PolygonStateSetDefaults(CPU* c, u64 s) {
    u64 w = 0;
    (void)c;
    w = nvn_set_field(w, NVN_POLY_CULL, NVN_FACE_NONE);
    w = nvn_set_field(w, NVN_POLY_FRONT, NVN_FRONT_FACE_CCW);
    w = nvn_set_field(w, NVN_POLY_MODE, NVN_POLYGON_MODE_FILL);
    st32(s, (u32)w);
}
void nvn_PolygonStateSetCullFace(CPU* c, u64 s, i32 face) {
    (void)c;
    SET32(s, NVN_POLY_CULL, face);
}
void nvn_PolygonStateSetFrontFace(CPU* c, u64 s, i32 face) {
    (void)c;
    SET32(s, NVN_POLY_FRONT, face);
}
void nvn_PolygonStateSetPolygonMode(CPU* c, u64 s, i32 mode) {
    (void)c;
    SET32(s, NVN_POLY_MODE, mode);
}
void nvn_PolygonStateSetPolygonOffsetEnables(CPU* c, u64 s, i32 enables) {
    (void)c;
    SET32(s, NVN_POLY_OFFSET_EN, enables);
}

void nvn_DepthStencilStateSetDefaults(CPU* c, u64 s) {
    u64 w = 0;
    (void)c;
    w = nvn_set_field(w, NVN_DS_WRITE, 1);
    w = nvn_set_field(w, NVN_DS_FUNC, NVN_DEPTH_FUNC_LESS);
    w = nvn_set_field(w, NVN_DS_SFUNC_F, NVN_STENCIL_FUNC_ALWAYS);
    w = nvn_set_field(w, NVN_DS_SFUNC_B, NVN_STENCIL_FUNC_ALWAYS);
    w = nvn_set_field(w, NVN_DS_FAIL_F, NVN_STENCIL_OP_KEEP);
    w = nvn_set_field(w, NVN_DS_ZFAIL_F, NVN_STENCIL_OP_KEEP);
    w = nvn_set_field(w, NVN_DS_ZPASS_F, NVN_STENCIL_OP_KEEP);
    w = nvn_set_field(w, NVN_DS_FAIL_B, NVN_STENCIL_OP_KEEP);
    w = nvn_set_field(w, NVN_DS_ZFAIL_B, NVN_STENCIL_OP_KEEP);
    w = nvn_set_field(w, NVN_DS_ZPASS_B, NVN_STENCIL_OP_KEEP);
    st64(s, w);
}
void nvn_DepthStencilStateSetDepthTestEnable(CPU* c, u64 s, int enable) {
    (void)c;
    SET64(s, NVN_DS_TEST, enable != 0);
}
void nvn_DepthStencilStateSetDepthWriteEnable(CPU* c, u64 s, int enable) {
    (void)c;
    SET64(s, NVN_DS_WRITE, enable != 0);
}
void nvn_DepthStencilStateSetDepthFunc(CPU* c, u64 s, i32 func) {
    (void)c;
    SET64(s, NVN_DS_FUNC, func);
}
void nvn_DepthStencilStateSetStencilTestEnable(CPU* c, u64 s, int enable) {
    (void)c;
    SET64(s, NVN_DS_STENCIL, enable != 0);
}
void nvn_DepthStencilStateSetStencilFunc(CPU* c, u64 s, i32 face, i32 func) {
    u64 w = ld64(s);
    (void)c;
    if (face & NVN_FACE_FRONT) w = nvn_set_field(w, NVN_DS_SFUNC_F, (u64)func);
    if (face & NVN_FACE_BACK) w = nvn_set_field(w, NVN_DS_SFUNC_B, (u64)func);
    st64(s, w);
}
void nvn_DepthStencilStateSetStencilOp(CPU* c, u64 s, i32 face, i32 fail, i32 zfail, i32 zpass) {
    u64 w = ld64(s);
    (void)c;
    if (face & NVN_FACE_FRONT) {
        w = nvn_set_field(w, NVN_DS_FAIL_F, (u64)fail);
        w = nvn_set_field(w, NVN_DS_ZFAIL_F, (u64)zfail);
        w = nvn_set_field(w, NVN_DS_ZPASS_F, (u64)zpass);
    }
    if (face & NVN_FACE_BACK) {
        w = nvn_set_field(w, NVN_DS_FAIL_B, (u64)fail);
        w = nvn_set_field(w, NVN_DS_ZFAIL_B, (u64)zfail);
        w = nvn_set_field(w, NVN_DS_ZPASS_B, (u64)zpass);
    }
    st64(s, w);
}

void nvn_VertexAttribStateSetDefaults(CPU* c, u64 s) {
    (void)c;
    st32(s, 0);
}
void nvn_VertexAttribStateSetFormat(CPU* c, u64 s, i32 format, i64 offset) {
    u64 w = ld32(s);
    (void)c;
    if (offset < 0 || offset >= (1 << 19))
        LOG_LIMIT(8, "nvn: a vertex attribute at offset %lld, past the 19 bits nvn_direct keeps\n", (long long)offset);
    w = nvn_set_field(w, NVN_VA_FORMAT, (u64)format);
    w = nvn_set_field(w, NVN_VA_OFFSET, (u64)offset);
    st32(s, (u32)w);
}
void nvn_VertexAttribStateSetStreamIndex(CPU* c, u64 s, i32 i) {
    (void)c;
    SET32(s, NVN_VA_STREAM, i);
}
void nvn_VertexStreamStateSetDefaults(CPU* c, u64 s) {
    (void)c;
    st64(s, 0);
}
void nvn_VertexStreamStateSetStride(CPU* c, u64 s, i64 stride) {
    (void)c;
    st32(s, (u32)stride); /* NvnVertexStream: stride, then divisor */
}
void nvn_VertexStreamStateSetDivisor(CPU* c, u64 s, i32 divisor) {
    (void)c;
    st32(s + 4, (u32)divisor);
}

/* ============================================================ state getters */
/* each what its setter put in the packed state (gpu/nvn/nvn_obj.h), the enums as the game gave them, as the driver's
   getters read its own packed fields back */
void nvn_BlendStateGetBlendEquation(CPU* c, u64 s, u64 rgb, u64 alpha) {
    u64 w = ld64(s);
    (void)c;
    gw32(rgb, (u32)nvn_field(w, NVN_BLEND_EQ_RGB));
    gw32(alpha, (u32)nvn_field(w, NVN_BLEND_EQ_A));
}
void nvn_BlendStateGetBlendFunc(CPU* c, u64 s, u64 src, u64 dst, u64 src_a, u64 dst_a) {
    u64 w = ld64(s);
    (void)c;
    gw32(src, (u32)nvn_field(w, NVN_BLEND_SRC_RGB));
    gw32(dst, (u32)nvn_field(w, NVN_BLEND_DST_RGB));
    gw32(src_a, (u32)nvn_field(w, NVN_BLEND_SRC_A));
    gw32(dst_a, (u32)nvn_field(w, NVN_BLEND_DST_A));
}
i32 nvn_BlendStateGetBlendTarget(CPU* c, u64 s) {
    (void)c;
    return (i32)nvn_field(ld64(s), NVN_BLEND_TARGET);
}
void nvn_ChannelMaskStateGetChannelMask(CPU* c, u64 s, i32 i, u64 r, u64 g, u64 b, u64 a) {
    u32 m = i >= 0 && i < 8 ? ld32(s) >> (4 * i) : 0;
    (void)c;
    *(u8*)GP(r) = (u8)(m & 1); /* NVNbooleans */
    *(u8*)GP(g) = (u8)(m >> 1 & 1);
    *(u8*)GP(b) = (u8)(m >> 2 & 1);
    *(u8*)GP(a) = (u8)(m >> 3 & 1);
}
i32 nvn_ColorStateGetAlphaTest(CPU* c, u64 s) {
    (void)c;
    return (i32)nvn_field(ld32(s), NVN_COLOR_ALPHA_FUNC);
}
i32 nvn_ColorStateGetLogicOp(CPU* c, u64 s) {
    (void)c;
    return (i32)nvn_field(ld32(s), NVN_COLOR_LOGIC_OP);
}
int nvn_DepthStencilStateGetDepthTestEnable(CPU* c, u64 s) {
    (void)c;
    return (int)nvn_field(ld64(s), NVN_DS_TEST);
}
int nvn_DepthStencilStateGetDepthWriteEnable(CPU* c, u64 s) {
    (void)c;
    return (int)nvn_field(ld64(s), NVN_DS_WRITE);
}
i32 nvn_DepthStencilStateGetDepthFunc(CPU* c, u64 s) {
    (void)c;
    return (i32)nvn_field(ld64(s), NVN_DS_FUNC);
}
int nvn_DepthStencilStateGetStencilTestEnable(CPU* c, u64 s) {
    (void)c;
    return (int)nvn_field(ld64(s), NVN_DS_STENCIL);
}
/* a face's stencil state: the back's for NVN_FACE_BACK, else the front's, as the driver reads it */
i32 nvn_DepthStencilStateGetStencilFunc(CPU* c, u64 s, i32 face) {
    (void)c;
    return (i32)(face == NVN_FACE_BACK ? nvn_field(ld64(s), NVN_DS_SFUNC_B) : nvn_field(ld64(s), NVN_DS_SFUNC_F));
}
void nvn_DepthStencilStateGetStencilOp(CPU* c, u64 s, i32 face, u64 fail, u64 zfail, u64 zpass) {
    u64 w = ld64(s);
    int back = face == NVN_FACE_BACK;
    (void)c;
    gw32(fail, (u32)(back ? nvn_field(w, NVN_DS_FAIL_B) : nvn_field(w, NVN_DS_FAIL_F)));
    gw32(zfail, (u32)(back ? nvn_field(w, NVN_DS_ZFAIL_B) : nvn_field(w, NVN_DS_ZFAIL_F)));
    gw32(zpass, (u32)(back ? nvn_field(w, NVN_DS_ZPASS_B) : nvn_field(w, NVN_DS_ZPASS_F)));
}
i32 nvn_PolygonStateGetCullFace(CPU* c, u64 s) {
    (void)c;
    return (i32)nvn_field(ld32(s), NVN_POLY_CULL);
}
i32 nvn_PolygonStateGetFrontFace(CPU* c, u64 s) {
    (void)c;
    return (i32)nvn_field(ld32(s), NVN_POLY_FRONT);
}
i32 nvn_PolygonStateGetPolygonMode(CPU* c, u64 s) {
    (void)c;
    return (i32)nvn_field(ld32(s), NVN_POLY_MODE);
}
i32 nvn_PolygonStateGetPolygonOffsetEnables(CPU* c, u64 s) {
    (void)c;
    return (i32)nvn_field(ld32(s), NVN_POLY_OFFSET_EN);
}
/* the format, and the offset as a 64-bit ptrdiff_t (the driver stores it with str x) */
void nvn_VertexAttribStateGetFormat(CPU* c, u64 s, u64 format, u64 offset) {
    u32 w = ld32(s);
    (void)c;
    gw32(format, (u32)nvn_field(w, NVN_VA_FORMAT));
    gw64(offset, nvn_field(w, NVN_VA_OFFSET));
}
i32 nvn_VertexAttribStateGetStreamIndex(CPU* c, u64 s) {
    (void)c;
    return (i32)nvn_field(ld32(s), NVN_VA_STREAM);
}
i64 nvn_VertexStreamStateGetStride(CPU* c, u64 s) {
    (void)c;
    return (i64)NVN_OBJ(const NvnVertexStream, s)->stride;
}
i32 nvn_VertexStreamStateGetDivisor(CPU* c, u64 s) {
    (void)c;
    return (i32)NVN_OBJ(const NvnVertexStream, s)->divisor;
}
/* the multisample state's other fields: kept for their getters, the renderer drawing one sample (gpu/nvn/nvn_draw.c) */
#define MS(s) NVN_OBJ(NvnMultisample, s)
int nvn_MultisampleStateGetMultisampleEnable(CPU* c, u64 s) {
    (void)c;
    return MS(s)->enable;
}
i32 nvn_MultisampleStateGetSamples(CPU* c, u64 s) {
    (void)c;
    return MS(s)->samples;
}
int nvn_MultisampleStateGetAlphaToCoverageEnable(CPU* c, u64 s) {
    (void)c;
    return MS(s)->alpha_to_coverage;
}
/* a flag of the state's flags (NVN_MS_*) */
static void ms_flag(u64 s, u8 flag, int on) { MS(s)->flags = (u8)(on ? MS(s)->flags | flag : MS(s)->flags & ~flag); }
void nvn_MultisampleStateSetAlphaToCoverageDither(CPU* c, u64 s, int on) {
    (void)c;
    ms_flag(s, NVN_MS_A2C_DITHER, on);
}
int nvn_MultisampleStateGetAlphaToCoverageDither(CPU* c, u64 s) {
    (void)c;
    return (MS(s)->flags & NVN_MS_A2C_DITHER) != 0;
}
void nvn_MultisampleStateSetRasterSamples(CPU* c, u64 s, i32 n) {
    (void)c;
    MS(s)->raster_samples = (u8)n;
}
i32 nvn_MultisampleStateGetRasterSamples(CPU* c, u64 s) {
    (void)c;
    return MS(s)->raster_samples;
}
void nvn_MultisampleStateSetCoverageModulationMode(CPU* c, u64 s, i32 mode) {
    (void)c;
    MS(s)->coverage_mod = (u8)mode;
}
i32 nvn_MultisampleStateGetCoverageModulationMode(CPU* c, u64 s) {
    (void)c;
    return MS(s)->coverage_mod;
}
void nvn_MultisampleStateSetCoverageToColorEnable(CPU* c, u64 s, int on) {
    (void)c;
    ms_flag(s, NVN_MS_COV_TO_COLOR, on);
}
int nvn_MultisampleStateGetCoverageToColorEnable(CPU* c, u64 s) {
    (void)c;
    return (MS(s)->flags & NVN_MS_COV_TO_COLOR) != 0;
}
void nvn_MultisampleStateSetCoverageToColorOutput(CPU* c, u64 s, i32 output) {
    (void)c;
    MS(s)->cov_to_color_out = (u8)output;
}
i32 nvn_MultisampleStateGetCoverageToColorOutput(CPU* c, u64 s) {
    (void)c;
    return MS(s)->cov_to_color_out;
}
void nvn_MultisampleStateSetSampleLocationsEnable(CPU* c, u64 s, int on) {
    (void)c;
    ms_flag(s, NVN_MS_LOCATIONS, on);
}
int nvn_MultisampleStateGetSampleLocationsEnable(CPU* c, u64 s) {
    (void)c;
    return (MS(s)->flags & NVN_MS_LOCATIONS) != 0;
}
void nvn_MultisampleStateSetSampleLocationsGridEnable(CPU* c, u64 s, int on) {
    (void)c;
    ms_flag(s, NVN_MS_GRID, on);
}
int nvn_MultisampleStateGetSampleLocationsGridEnable(CPU* c, u64 s) {
    (void)c;
    return (MS(s)->flags & NVN_MS_GRID) != 0;
}
/* the pixels the sample locations cover, as the game's driver answers: 16 locations in all, so a 4x4 grid of pixels
   for one sample, 2x4 for two, 2x2 for four, 1x2 for eight and 1x1 for sixteen, by the larger of the samples and the
   raster samples. Other counts write nothing */
void nvn_MultisampleStateGetSampleLocationsGrid(CPU* c, u64 s, u64 w, u64 h) {
    u32 n = MS(s)->samples > MS(s)->raster_samples ? MS(s)->samples : MS(s)->raster_samples, gw, gh;
    (void)c;
    switch (n) {
        case 0:
        case 1: gw = 4, gh = 4; break;
        case 2: gw = 2, gh = 4; break;
        case 4: gw = 2, gh = 2; break;
        case 8: gw = 1, gh = 2; break;
        case 16: gw = 1, gh = 1; break;
        default: return;
    }
    gw32(w, gw);
    gw32(h, gh);
}
/* n locations from first on, x and y each in [0, 1] of a pixel: kept as the game's driver keeps them, in sixteenths
   (clamped to [0, 1], a NaN taken as 0, rounded to the nearest, at most 15). The renderer draws at the standard locations: Vulkan's are programmable only with
   VK_EXT_sample_locations, which it doesn't use, and a pixel's samples are averaged alike by a resolve */
void nvn_MultisampleStateSetSampleLocations(CPU* c, u64 s, i32 first, i32 n, u64 xy) {
    NvnMultisample* o = MS(s);
    i32 i;
    (void)c;
    for (i = 0; i < n; i++) {
        f32 v[2];
        u8 q[2];
        int k;
        if (first + i < 0 || first + i >= 16) continue;
        memcpy(v, GP(xy + 8 * (u64)i), sizeof v);
        for (k = 0; k < 2; k++) {
            f32 t = v[k] >= 0.0f ? v[k] : 0.0f; /* a NaN takes 0 */
            q[k] = (u8)(t < 0.9375f ? (int)(t * 16.0f + 0.5f) : 15);
        }
        o->location[first + i] = (u8)(q[0] | q[1] << 4);
    }
}

/* ============================================================ debug labels, which nothing reads back */
static void no_label(u64 o, u64 label) {
    (void)o;
    (void)label;
}
void nvn_CommandBufferSetDebugLabel(CPU* c, u64 o, u64 label) {
    (void)c;
    no_label(o, label);
}
void nvn_DeviceSetDebugLabel(CPU* c, u64 o, u64 label) {
    (void)c;
    no_label(o, label);
}
void nvn_ProgramSetDebugLabel(CPU* c, u64 o, u64 label) {
    (void)c;
    no_label(o, label);
}
void nvn_QueueSetDebugLabel(CPU* c, u64 o, u64 label) {
    (void)c;
    no_label(o, label);
}
void nvn_SyncSetDebugLabel(CPU* c, u64 o, u64 label) {
    (void)c;
    no_label(o, label);
}
void nvn_SamplerPoolSetDebugLabel(CPU* c, u64 o, u64 label) {
    (void)c;
    no_label(o, label);
}
void nvn_TexturePoolSetDebugLabel(CPU* c, u64 o, u64 label) {
    (void)c;
    no_label(o, label);
}
void nvn_WindowSetDebugLabel(CPU* c, u64 o, u64 label) {
    (void)c;
    no_label(o, label);
}

/* ============================================================ device getters */
i32 nvn_DeviceGetWindowOriginMode(CPU* c, u64 d) {
    (void)c;
    return (i32)NVN_OBJ(const NvnDevice, d)->origin;
}
i32 nvn_DeviceGetDepthMode(CPU* c, u64 d) {
    (void)c;
    return (i32)NVN_OBJ(const NvnDevice, d)->depth_mode;
}
/* the GPU's timestamp now, in nanoseconds: its 19.2 MHz ticks, as GetTimestampInNanoseconds converts a counter's */
u64 nvn_DeviceGetCurrentTimestampInNanoseconds(CPU* c, u64 d) {
    (void)c;
    (void)d;
    return guest_ticks() * 625 / 12;
}

/* ============================================================ builder getters: what was set */
u64 nvn_MemoryPoolBuilderGetDevice(CPU* c, u64 b) {
    (void)c;
    return NVN_OBJ(const NvnPoolBuilder, b)->device;
}
u64 nvn_MemoryPoolBuilderGetMemory(CPU* c, u64 b) {
    (void)c;
    return NVN_OBJ(const NvnPoolBuilder, b)->memory;
}
u64 nvn_MemoryPoolBuilderGetSize(CPU* c, u64 b) {
    (void)c;
    return NVN_OBJ(const NvnPoolBuilder, b)->size;
}
i32 nvn_MemoryPoolBuilderGetFlags(CPU* c, u64 b) {
    (void)c;
    return (i32)NVN_OBJ(const NvnPoolBuilder, b)->flags;
}
u64 nvn_BufferBuilderGetDevice(CPU* c, u64 b) {
    (void)c;
    return NVN_OBJ(const NvnBufferBuilder, b)->device;
}
u64 nvn_BufferBuilderGetMemoryPool(CPU* c, u64 b) {
    (void)c;
    return NVN_OBJ(const NvnBufferBuilder, b)->pool;
}
i64 nvn_BufferBuilderGetMemoryOffset(CPU* c, u64 b) {
    (void)c;
    return NVN_OBJ(const NvnBufferBuilder, b)->offset;
}
u64 nvn_BufferBuilderGetSize(CPU* c, u64 b) {
    (void)c;
    return NVN_OBJ(const NvnBufferBuilder, b)->size;
}
u64 nvn_TextureBuilderGetDevice(CPU* c, u64 b) {
    (void)c;
    return TB(b)->device;
}
u64 nvn_TextureBuilderGetMemoryPool(CPU* c, u64 b) {
    (void)c;
    return TB(b)->pool;
}
u64 nvn_TextureBuilderGetPackagedTextureData(CPU* c, u64 b) {
    (void)c;
    return TB(b)->packaged_data;
}
void nvn_TextureBuilderGetSwizzle(CPU* c, u64 b, u64 r, u64 g, u64 bl, u64 a) {
    u32 w = tex_swizzle(&TB(b)->d);
    (void)c;
    if (r) gw32(r, w & 7);
    if (g) gw32(g, w >> 3 & 7);
    if (bl) gw32(bl, w >> 6 & 7);
    if (a) gw32(a, w >> 9 & 7);
}
i32 nvn_TextureBuilderGetDepthStencilMode(CPU* c, u64 b) {
    (void)c;
    return TB(b)->d.bits & NVN_TEX_STENCIL ? NVN_TEXTURE_DEPTH_STENCIL_MODE_STENCIL : 0;
}
i64 nvn_TextureBuilderGetMemoryOffset(CPU* c, u64 b) {
    (void)c;
    return TB(b)->offset;
}
/* an OpenGL texture's name for interop: kept for its getter, as there is no OpenGL here */
void nvn_TextureBuilderSetGLTextureName(CPU* c, u64 b, u32 name) {
    (void)c;
    TB(b)->gl_name = name;
}
u32 nvn_TextureBuilderGetGLTextureName(CPU* c, u64 b) {
    (void)c;
    return TB(b)->gl_name;
}
/* whether SetPackagedTextureLayout gave one, and the layout's first 4 bytes: its 12 bits the game's driver keeps (the
   rest 0) */
int nvn_TextureBuilderGetPackagedTextureLayout(CPU* c, u64 b, u64 layout) {
    const NvnTexBuilder* o = TB(b);
    u32 w = 0;
    (void)c;
    if (o->d.bits & NVN_TEX_HAS_LAYOUT) w = (u32)(o->layout[0] | o->layout[1] << 8) & 0xfff;
    gw32(layout, w);
    return (o->d.bits & NVN_TEX_HAS_LAYOUT) != 0;
}
i32 nvn_TextureBuilderGetRawStorageClass(CPU* c, u64 b) {
    (void)c;
    return raw_storage_class(&TB(b)->d);
}
void nvn_TextureBuilderGetSparseTileLayout(CPU* c, u64 b, u64 out) {
    (void)c;
    sparse_tile_layout(&TB(b)->d, out);
}
i64 nvn_TextureBuilderGetViewOffset(CPU* c, u64 b, u64 v) {
    (void)c;
    return view_offset(&TB(b)->d, v);
}
/* a recording's handle given up: the game's own driver returns at once (a bare ret), only its debug layer tracks
   handles. The records stay in the game's command memory, which it reuses as it likes */
void nvn_DeviceFinalizeCommandHandle(CPU* c, u64 d, u64 handle) {
    (void)c;
    (void)d;
    (void)handle;
}
