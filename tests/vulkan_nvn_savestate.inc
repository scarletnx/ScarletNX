/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A save state of a game whose NVN the runtime runs (runtime/state.c, gpu/nvn/nvn_save.c), through state_save and
   state_load as a game's are: a frame drawn and presented, the game saved, the game going on (the command buffer's
   memory added again and recorded over, which gives the first recording's block of the runtime's record memory
   back), the state loaded, a new recording made as a game makes them after a load, and the first recording presented
   again: the same frame as before the save, as g_nvn's record memory, which the state holds, keeps the first
   recording's block in use; and the window's textures come as they did, the pacer's clock going on from the save
   though the saving process had run an hour longer. The load lets go of the buffer cache's entries and of nothing
   else. A save while a queue's work waits on the GPU is refused, and so is the load of a state saved without NVN, or
   whose NVN state indexes past the runtime's tables. Runs alone (SNX_TEST_NVN_CASE=savestate): a load replaces the
   process's guest memory */
#include "nx/runtime/state.h"
#include "nx/runtime/state_io.h"
#include "common/filesystem/tree.h"
#include "nx/hle/hle.h"
#include "nx/hle/services/services_int.h"
#include "nx/runtime/loader.h"
#include "gpu/nvn/nvn.hook.h"

/* the hash renderer.h's screenshot gives an 8x8 frame of one RGBA8 pixel value */
static u64 solid_frame(u32 rgba) {
    u64 h = 1469598103934665603ull;
    for (u32 i = 0; i < 64; i++) h = (h ^ rgba) * 1099511628211ull;
    return h;
}
/* the recording at handle submitted, the window's texture presented and the queue's work done: the frame shown */
static u64 nvn_frame(u64 queue, u64 window, u64 handles, u64 handle, const char* shot) {
    gw64(handles, handle);
    nvn_QueueSubmitCommands(NULL, queue, 1, handles);
    nvn_QueuePresentTexture(NULL, queue, window, 0);
    nvn_QueueFinish(NULL, queue);
    return g_renderer->screenshot(shot);
}
/* a recording into 0x100 bytes of command memory at offset of the pool, which its render target records overrun into
   the runtime's own memory: the target cleared to the color at rgba */
static u64 nvn_clear(u64 cmd, u64 pool, i64 offset, u64 targets, u64 rgba) {
    nvn_CommandBufferAddCommandMemory(NULL, cmd, pool, offset, 0x100);
    nvn_CommandBufferBeginRecording(NULL, cmd);
    nvn_CommandBufferSetRenderTargets(NULL, cmd, 1, targets, 0, 0, 0);
    nvn_CommandBufferClearColor(NULL, cmd, 0, rgba, 15);
    return nvn_CommandBufferEndRecording(NULL, cmd);
}
static void put_rgba(u64 at, f32 r, f32 g, f32 b) {
    const f32 c[4] = {r, g, b, 1.0f};
    memcpy(GP(at), c, sizeof c);
}

/* an NVN section in memory, as state.c's buffers hold one */
typedef struct {
    u8* p;
    u64 n, at;
} NvnSection;
static int section_put(void* ctx, const void* p, u64 n) {
    NvnSection* b = (NvnSection*)ctx;
    u8* q = (u8*)realloc(b->p, (size_t)(b->n + n));
    if (!q) return -1;
    memcpy(q + b->n, p, (size_t)n);
    b->p = q;
    b->n += n;
    return 0;
}
static int section_get(void* ctx, void* p, u64 n) {
    NvnSection* b = (NvnSection*)ctx;
    if (n > b->n - b->at) return -1;
    memcpy(p, b->p + b->at, (size_t)n);
    b->at += n;
    return 0;
}
/* a change of a state's copy of g_nvn: n bytes at off */
typedef struct {
    size_t off;
    const void* bytes;
    u32 n;
} NvnChange;
/* the section with n changes of its copy of g_nvn: whether a load takes it */
static int nvn_parses(const NvnSection* saved, const NvnChange* change, u32 n) {
    NvnSection b = {(u8*)malloc((size_t)saved->n), saved->n, 0};
    if (!b.p) return -1;
    memcpy(b.p, saved->p, (size_t)saved->n);
    for (u32 i = 0; i < n; i++) /* after the version and the pacer's clock */
        memcpy(b.p + 12 + change[i].off, change[i].bytes, change[i].n);
    StateIo io = {&b, section_put, section_get};
    NvnStaged* s = nvn_state_parse(&io);
    int took = s != NULL;
    nvn_state_staged_free(s);
    free(b.p);
    return took;
}
/* A state may come from another player: one whose NVN state indexes past the runtime's tables, or whose chains of
   them loop, is damaged, as is one that holds work for the GPU thread, which a save never writes */
static int check_nvn_state_damage(void) {
    NvnSection saved = {0};
    char why[256];
    StateIo io = {&saved, section_put, section_get};
    CHECK(!nvn_state_save(&io, why, sizeof why));
    CHECK(g_nvn.cmd.slots >= 1 && g_nvn.cmd.slots < NVN_SPILL_PIECES);
    const u32 none = 0, one = 1, many = NVN_SPILL_PIECES + 1, past = g_nvn.cmd.slots + 1, owner = 5;
    CHECK(nvn_parses(&saved, NULL, 0) == 1); /* as it was saved */
    CHECK(nvn_parses(&saved, (NvnChange[]){{offsetof(Nvn, cmd.slots), &many, 4}}, 1) == 0);
    CHECK(nvn_parses(&saved, (NvnChange[]){{offsetof(Nvn, cmd.np), &past, 4}}, 1) == 0);
    CHECK(nvn_parses(&saved, (NvnChange[]){{offsetof(Nvn, cmd.free), &past, 4}}, 1) == 0);
    /* piece 1 links to itself from the first bucket, the free slots none: a loop of a bucket's alone */
    CHECK(nvn_parses(&saved,
                     (NvnChange[]){{offsetof(Nvn, cmd.bucket[0]), &one, 4},
                                   {offsetof(Nvn, cmd.p[1].next), &one, 4},
                                   {offsetof(Nvn, cmd.free), &none, 4}},
                     3) == 0);
    CHECK(nvn_parses(&saved, (NvnChange[]){{offsetof(Nvn, replay.jobs[3].queue), &one, 4}}, 1) == 0);
    CHECK(nvn_parses(&saved, (NvnChange[]){{offsetof(Nvn, replay.q[0].d.ds.owner), &owner, 4}}, 1) == 0);
    /* a function resolved outside the game's code, which a load would take (gpu/nvn/nvn_loader.c) */
    const char name[] = "nvnQueueFinish";
    const u64 inside = g_code_lo, outside = 0x10;
    CHECK(nvn_parses(&saved,
                     (NvnChange[]){{offsetof(Nvn, loader.resolved[5].addr), &inside, 8},
                                   {offsetof(Nvn, loader.resolved[5].name), name, sizeof name}},
                     2) == 1);
    CHECK(nvn_parses(&saved,
                     (NvnChange[]){{offsetof(Nvn, loader.resolved[5].addr), &outside, 8},
                                   {offsetof(Nvn, loader.resolved[5].name), name, sizeof name}},
                     2) == 0);
    free(saved.p);
    return 0;
}

/* the code of the game's NVN driver, which the runtime's functions take the dispatch slots of */
static void driver_code(CPU* c, u64 entry) {
    (void)c;
    (void)entry;
}

static int check_nvn_savestate(const char* shot) {
    const u64 gpu = 0x4f8000000ull, cpu = HEAP_BASE + 0x200000;
    /* the pool's storage: command memory at 0 and 0x400, control memory at 0x2000, an event at 0x3000, the target's
       texels at 0x8000. The objects after it */
    const u64 pool = cpu + 0x10000, tex = cpu + 0x10100, queue = cpu + 0x10200, cmd = cpu + 0x12000;
    const u64 wb = cpu + 0x12100, window = cpu + 0x12200, event = cpu + 0x12400, targets = cpu + 0x12500;
    const u64 rgba = cpu + 0x12600, handles = cpu + 0x12700, sync = cpu + 0x12800, index = cpu + 0x12900;
    const u64 red = solid_frame(0xFF0000FFu);
    char path[1100], data[1100], why[256];
    snprintf(path, sizeof path, "%sstates/nvn.snxstate", g_exedir);
    snprintf(data, sizeof data, "%sdata", g_exedir);
    g_nvn.loader.active = 1; /* as a game with NVN runs, whose states hold g_nvn */
    /* what a game has running that a save and a load go through: the audio clock, which a game pause holds, and the
       file tree, whose save/temp a state holds (an empty folder's here) */
    audio_init();
    CHECK(!plat_thread(audio_clock, NULL, (size_t)8 << 20));
    CHECK(!plat_mkdir(data) && fs_tree_init(&g_hle.fs.tree, data) == FS_TREE_OK);
    gmmu_map(gpu, cpu, 0x10000);
    *NVN_OBJ(NvnPool, pool) =
        (NvnPool){.magic = NVN_MAGIC('P', 'O', 'O', 'L'), .memory = cpu, .size = 0x10000, .gpu = gpu};
    *NVN_OBJ(NvnTexture, tex) = (NvnTexture){.magic = NVN_MAGIC('T', 'E', 'X', 0),
                                             .d = {.format = NVN_FORMAT_RGBA8,
                                                   .target = NVN_TEXTURE_TARGET_2D,
                                                   .levels = 1,
                                                   .width = 8,
                                                   .height = 8,
                                                   .depth = 1},
                                             .gpu = gpu + 0x8000,
                                             .pool = pool,
                                             .offset = 0x8000,
                                             .memory = cpu + 0x8000,
                                             .size = 0x1000};
    gw64(targets, tex);
    /* the saving process's pacer's clock an hour on, as one that had run for an hour: its times in the game's memory
       are ahead of where this process's plat_ns is at the load */
    g_nvn.queue.clock = (i64)3600000000000ll;
    CHECK(nvn_QueueInitialize(NULL, queue, 0));
    nvn_WindowBuilderSetDefaults(NULL, wb);
    nvn_WindowBuilderSetTextures(NULL, wb, 1, targets);
    CHECK(nvn_WindowInitialize(NULL, window, wb));
    nvn_CommandBufferInitialize(NULL, cmd, 0);
    nvn_CommandBufferAddControlMemory(NULL, cmd, cpu + 0x2000, 0x1000);
    /* the game resolves three NVN functions of its driver: one the runtime implements, one it doesn't, and the
       resolver, which it wraps. Their slots are the runtime's then */
    static GuestFn code[16];
    NvnLoaderState* resolved = (NvnLoaderState*)calloc(1, sizeof *resolved);
    CHECK(resolved);
    for (u32 i = 0; i < 16; i++) code[i] = driver_code;
    g_disp = code;
    g_code_lo = 0x7100000000ull;
    g_code_hi = g_code_lo + sizeof code / sizeof *code * 4;
    resolved->active = resolved->decided = 1;
    resolved->resolved[0] = (NvnResolved){g_code_lo, "nvnQueueFinish"};
    resolved->resolved[1] = (NvnResolved){g_code_lo + 4, "nvnNoSuchFunction"};
    resolved->resolved[2] = (NvnResolved){g_code_lo + 8, "nvnDeviceGetProcAddress"};
    nvn_loader_state_install(resolved);
    free(resolved);
    CHECK(code[0] != driver_code && code[1] != driver_code && code[2] != driver_code && code[3] == driver_code);
    CHECK(g_nvn.loader.getproc_original == driver_code);

    /* work waiting on the GPU for an event isn't saved: the state is refused, naming the queue's */
    *NVN_OBJ(NvnEvent, event) =
        (NvnEvent){.magic = NVN_MAGIC('E', 'V', 'N', 'T'), .value = cpu + 0x3000, .gpu = gpu + 0x3000};
    gw32(cpu + 0x3000, 0);
    nvn_CommandBufferAddCommandMemory(NULL, cmd, pool, 0x800, 0x100);
    nvn_CommandBufferBeginRecording(NULL, cmd);
    nvn_CommandBufferWaitEvent(NULL, cmd, event, NVN_EVENT_WAIT_MODE_EQUAL, 1);
    gw64(handles, nvn_CommandBufferEndRecording(NULL, cmd));
    nvn_QueueSubmitCommands(NULL, queue, 1, handles);
    CHECK(state_save(path, why, sizeof why) == -1);
    if (!strstr(why, "NVN queue 1 has work waiting")) fprintf(stderr, "NVN state save refused: %s\n", why);
    CHECK(strstr(why, "NVN queue 1 has work waiting") != NULL);
    gw32(cpu + 0x3000, 1); /* the CPU signals it, and the work goes on */
    nvn_QueueFinish(NULL, queue);

    /* the frame before the save, which the load brings back */
    put_rgba(rgba, 1, 0, 0);
    u64 first = nvn_clear(cmd, pool, 0, targets, rgba);
    CHECK(nvn_frame(queue, window, handles, first, shot) == red);
    CHECK(!state_save(path, why, sizeof why));
    g_nvn.queue.clock = 0; /* from here as a process that started an hour later: on its own plat_ns */

    /* the game goes on: the same command memory recorded over, which hands the first recording's block back to the
       runtime's record memory, where a recording made after the load would take it */
    u32 given = 0;
    for (u32 k = 0; k < NVN_SMALL_CLASSES; k++) given += g_nvn.cmd.small[k].n;
    put_rgba(rgba, 0, 1, 0);
    u64 second = nvn_clear(cmd, pool, 0, targets, rgba);
    CHECK(nvn_frame(queue, window, handles, second, shot) == solid_frame(0xFF00FF00u));
    u32 back = 0;
    for (u32 k = 0; k < NVN_SMALL_CLASSES; k++) back += g_nvn.cmd.small[k].n;
    CHECK(back == given + 1); /* the premise: a block that the state's recording holds is free here */
    CHECK(!check_nvn_state_damage());

    /* a range the game's draws read, in the buffer cache at the load: the load lets go of the cache's entries, and
       entry 0 is none, memory that nothing ever wrote (the test runs with new memory filled, src/nx/CMakeLists.txt) */
    VkBuffer kept;
    u64 kept_at;
    vk_lock();
    int cached = vk_buf_get(gpu + 0x4000, 0x100, &kept, &kept_at);
    vk_unlock();
    CHECK(cached);

    /* the load as in a process that hasn't resolved them yet (a state loaded as the game starts) */
    for (u32 i = 0; i < 16; i++) code[i] = driver_code;
    memset(g_nvn.loader.resolved, 0, sizeof g_nvn.loader.resolved);
    g_nvn.loader.getproc_original = NULL;
    u64 before = ds_generation_next();
    int loaded = state_load(path, why, sizeof why);
    if (loaded) fprintf(stderr, "NVN state not loaded: %s\n", why);
    CHECK(!loaded);
    /* the buffer cache as a load leaves it: none of its entries holds a buffer of its own */
    u32 own = 0;
    u64 own_bytes = 0;
    vk_lock();
    vk_buf_stats(&own, &own_bytes);
    vk_unlock();
    if (own || own_bytes)
        fprintf(stderr, "the buffer cache after the load: %u buffers of its own, %llu bytes (0 and 0 wanted)\n", own,
                (unsigned long long)own_bytes);
    CHECK(!own && !own_bytes);
    /* what the renderer built from the queue's draw state is keyed by generations this process hands out */
    for (u32 g = 0; g < DS_GENS; g++) CHECK(g_nvn.replay.q[0].d.ds.gen[g] > before);
    /* the pacer's clock goes on from the save, and the window's next texture comes at once: its last present's slot
       is behind that clock, not an hour ahead of the process's */
    CHECK(nvn_now() >= NVN_OBJ(const NvnWindow, window)->pace);
    u64 t = plat_ns();
    nvn_WindowAcquireTexture(NULL, window, sync, index);
    CHECK(plat_ns() - t < 500000000ull && gr32(index) == 0);
    /* the state's game's functions are the runtime's again, the resolver wrapped over the driver's own */
    CHECK(code[0] != driver_code && code[1] != driver_code && code[2] != driver_code && code[3] == driver_code);
    CHECK(g_nvn.loader.getproc_original == driver_code);
    put_rgba(rgba, 0, 0, 1);
    u64 third = nvn_clear(cmd, pool, 0x400, targets, rgba);
    u64 again = nvn_frame(queue, window, handles, first, shot);
    if (again != red)
        fprintf(stderr, "NVN frame after the load: hash %016llx, red %016llx\n", (unsigned long long)again,
                (unsigned long long)red);
    CHECK(again == red);
    CHECK(nvn_frame(queue, window, handles, third, shot) == solid_frame(0xFFFF0000u));

    /* a state saved without NVN (a ScarletNX from before states held it) isn't loaded into a game that runs NVN */
    g_nvn.loader.active = 0;
    CHECK(!state_save(path, why, sizeof why));
    g_nvn.loader.active = 1;
    CHECK(state_load(path, why, sizeof why) == -1);
    if (!strstr(why, "without the game's NVN state")) fprintf(stderr, "NVN state load refused: %s\n", why);
    CHECK(strstr(why, "without the game's NVN state") != NULL);
    CHECK(nvn_frame(queue, window, handles, third, shot) == solid_frame(0xFFFF0000u)); /* the game as it was */
    remove(path);
    puts("PASS NVN save state: the frame before the save comes back after the load, and what states can't hold is "
         "refused by name");
    return 0;
}
