/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The graphics command buffers are recorded on a thread of their own. In one game a fifth of the GPU
   thread's time was the driver encoding commands, which doesn't depend on anything the GPU thread does next: the
   rec_Cmd* functions copy a command's arguments into a ring, and the recording thread makes the Vulkan call. A
   command buffer is begun, ended and submitted from the ring too, in order with its commands.

   Only the frame's command buffers (vk_cmd, and vk_cmd_upload's that runs before it) go through the ring. A command for
   any other one (compute's, VIC's) is made at once, as are commands while vk_record=0. Whoever adds to the ring holds
   vk_lock, so there is one producer at a time. A present goes through the ring too, after the submission whose
   semaphore it waits on. What else has to happen after the queued commands are recorded and submitted waits for the
   ring to drain (vk_rec_sync): a new command buffer from the frame's pool, a device wait. Waits on the graphics
   timeline need none, as a timeline value can be waited for before its submission. */
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include "common/utilities/common.h"
#include "common/gpu/vk/gpu_pstate.h"
#include "nx/runtime/options.h"
#include "nx/runtime/platform/log.h"
#include "nx/gpu/vk/vk.h"
#include "nx/gpu/vk/descriptors.h"
#include "nx/gpu/vk/descriptor_heaps.h"
#include "nx/runtime/platform/plat.h"
#include "nx/runtime/platform/tracy.h"

typedef void (*RecFn)(const void* args);
typedef struct {
    RecFn fn; /* NULL: skip to the start of the ring */
    u64 size; /* of the packet, header included, a multiple of 16 */
} RecHdr;
static void barrier_run(const void* p);
static void barrier_flush(void);
static void desc_batch_flush(void);

/* The ring holds about a frame of one game's commands (3.8 MB a frame in its underground scene). At 16 MB every packet
   went to memory last written four frames before, long out of the caches, and the stream through the shared cache
   likely pushed out what the GPU thread's draws look up: at 4 MB that thread's load misses dropped a tenth and its
   frames took 5% fewer cycles. The producer waits for room only when the recording thread is a whole frame behind */
#define REC_CAP     (4ull << 20)
#define REC_PUBLISH (32ull << 10)

static u8* g_ring;
static int g_rec = -1; /* recording on the thread: 1, at once: 0, not decided yet: -1 */
static VkCommandBuffer g_rec_cb;
/* written by the producer: the end of what it has added, and the consumer: the end of what it has made */
static _Atomic u64 g_head;
static char g_pad0[64];
static _Atomic u64 g_tail;
static char g_pad1[64];
static _Atomic u32 g_rec_tid; /* the recording thread's plat_tid: a vk_rec_sync from it waits for no one */
static _Atomic int g_sleep;   /* the consumer waits on g_cv for more */
static PMutex g_m;
static PCond g_cv;
/* the producer waits on g_room for the consumer to move g_tail (room in a full ring, or a drain), counted in
   g_room_waits. It spun on sched_yield before: with the recording thread held 184 s in a read of a slow disk, the GPU
   thread took a whole core all that while, 60% of it in the kernel (#2487) */
static PCond g_room;
static _Atomic int g_room_waits;
/* the most a room wait sleeps before it looks again, should a wake-up be missed */
#define REC_ROOM_WAIT_NS 100000000ll
/* how long a room wait spins on sched_yield before it sleeps: a drain the recording thread ends in microseconds (a
   landing's, owned.c) took 5.1 to 6.4 us asleep and woken, 2.4 to 3.4 us spun (vulkan_recorder, 20,000 drains) */
#define REC_ROOM_SPIN_NS 200000ull

/* the command kinds the recording thread drained, by their run function: where its time goes. Reset by
   vk_rec_command_report */
#define REC_KINDS 64
typedef struct {
    RecFn fn;
    u64 n;
} RecKind;
static RecKind g_rec_kind[REC_KINDS];
static u64 g_rec_kind_total;
static int g_rec_kind_count; /* the GPU profile is on (vk_rec_command_report's count): the count runs on every command */
static void rec_kind(RecFn fn) {
    u64 h = (uintptr_t)fn >> 4;
    g_rec_kind_total++;
    for (u32 i = 0; i < REC_KINDS; i++) {
        RecKind* k = &g_rec_kind[(h + i) & (REC_KINDS - 1)];
        if (k->fn == fn) {
            k->n++;
            return;
        }
        if (!k->fn) {
            k->fn = fn;
            k->n = 1;
            return;
        }
    }
}

static void rec_thread(void* arg) {
    u64 tail = 0;
    (void)arg;
    atomic_store_explicit(&g_rec_tid, plat_tid(), memory_order_release); /* other threads read it in vk_rec_sync */
    (void)g_pad0;
    (void)g_pad1;
    SNX_THREAD_NAME("Vulkan recorder");
    for (;;) {
        u64 head = atomic_load_explicit(&g_head, memory_order_acquire);
        /* nothing more yet: sleep until the producer publishes (REC_PUBLISH). It spun 4000 pauses first, which with
           a publish every 4 KiB kept it from sleeping at all: a host core spent on waiting (0.8 of one in Tears of
           the Kingdom, 0.26 with this) */
        if (head == tail) {
            pm_lock(&g_m);
            atomic_store_explicit(&g_sleep, 1, memory_order_seq_cst);
            while ((head = atomic_load_explicit(&g_head, memory_order_seq_cst)) == tail) pc_wait(&g_cv, &g_m, -1);
            atomic_store_explicit(&g_sleep, 0, memory_order_relaxed);
            pm_unlock(&g_m);
        }
        [[maybe_unused]] u64 records = 0; /* the zone's value; only the profiler reads it */
        SNX_ZONE(z_rec, "vk record");
        while (tail != head) {
            const RecHdr* h = (const RecHdr*)(g_ring + (tail & (REC_CAP - 1)));
            if (h->fn) {
                if (g_rec_kind_count) rec_kind(h->fn);
                h->fn(h + 1);
            }
            tail += h->size;
            records++;
        }
        SNX_ZONE_VALUE(z_rec, records);
        SNX_ZONE_END(z_rec);
        /* seq_cst with the load after it, as the producer's store of g_room_waits and its look at g_tail: one of the
           two sees the other, so a producer about to sleep is woken */
        atomic_store_explicit(&g_tail, tail, memory_order_seq_cst);
        if (atomic_load_explicit(&g_room_waits, memory_order_seq_cst)) {
            pm_lock(&g_m);
            pc_broadcast(&g_room);
            pm_unlock(&g_m);
        }
    }
}

static int rec_enabled(void) {
    if (g_rec < 0) {
        g_rec = g_settings.vulkan_recording_thread;
        if (g_rec) {
            g_ring = malloc(REC_CAP);
            pm_init(&g_m);
            pc_init(&g_cv);
            pc_init(&g_room);
            if (!g_ring || plat_thread(rec_thread, NULL, (size_t)1 << 20)) {
                char why[256];
                WARN("vulkan: could not start the command recording thread (%s): recording on the GPU thread\n",
                     snx_os_error(why, sizeof why));
                free(g_ring);
                g_ring = NULL;
                g_rec = 0;
            }
        }
    }
    return g_rec;
}

void vk_rec_init(void) { rec_enabled(); }
int vk_rec_threaded(void) { return g_rec > 0; }
u64 vk_rec_behind(void) {
    return g_rec > 0 ? atomic_load_explicit(&g_head, memory_order_acquire) -
                           atomic_load_explicit(&g_tail, memory_order_acquire)
                     : 0;
}

/* a command for cb goes through the ring */
static int rec_on(VkCommandBuffer cb) {
    return g_rec > 0 && (cb == g_rec_cb || (cb && cb == vk_cmd_upload_recording()));
}

int vk_rec_ring(VkCommandBuffer cb) { return rec_on(cb); }

static void wake(void) {
    if (atomic_load_explicit(&g_sleep, memory_order_seq_cst)) {
        pm_lock(&g_m);
        pc_signal(&g_cv);
        pm_unlock(&g_m);
    }
}

/* What the producer has added (g_pending) is published to the consumer (g_head) REC_PUBLISH bytes at a time, and at
   once for a submission or a drain: until something waits for the commands, it doesn't matter when they are
   recorded. Each publish is a locked store (with the wake-up check it has to be ordered with), and wakes a sleeping
   consumer: a batch of 32 KiB lets it record a while and sleep again, not wake for every few draws */
static u64 g_pending, g_published;
static void publish(void) {
    g_published = g_pending;
    atomic_store_explicit(&g_head, g_pending, memory_order_seq_cst);
    wake();
}
/* the producer asleep until the consumer has made more than `more` bytes of room past g_pending, or until (plat_ns,
   VK_REC_FOREVER: none): 0 when until came first. What is pending goes to the consumer first, or it would never make
   the room */
static int room_wait(u64 more, u64 until) {
    const u64 spin = plat_ns() + REC_ROOM_SPIN_NS;
    for (;;) {
        if (g_pending + more - atomic_load_explicit(&g_tail, memory_order_seq_cst) <= REC_CAP) return 1;
        const u64 now = plat_ns();
        if (now < spin && (until == VK_REC_FOREVER || now < until)) {
            publish();
            plat_yield();
            continue;
        }
        if (until != VK_REC_FOREVER && now >= until) return 0;
        const i64 left =
            until == VK_REC_FOREVER || until - now > (u64)REC_ROOM_WAIT_NS ? REC_ROOM_WAIT_NS : (i64)(until - now);
        publish();
        pm_lock(&g_m);
        atomic_fetch_add_explicit(&g_room_waits, 1, memory_order_seq_cst);
        if (g_pending + more - atomic_load_explicit(&g_tail, memory_order_seq_cst) > REC_CAP)
            pc_wait(&g_room, &g_m, left);
        atomic_fetch_sub_explicit(&g_room_waits, 1, memory_order_relaxed);
        pm_unlock(&g_m);
    }
}
/* room for a packet of size bytes of arguments, filled in and then made known by rec_commit */
static void* rec_push(RecFn fn, u64 size) {
    if (fn != barrier_run) barrier_flush(); /* a pending batch of barriers ends before any other command */
    u64 need = (sizeof(RecHdr) + size + 15) & ~15ull, at = g_pending & (REC_CAP - 1);
    /* one that could never fit would wait for room forever, with vk_lock held and every thread that takes it behind */
    if (need > REC_CAP / 2)
        fatal("vulkan: a command of %llu bytes, past what the recording ring takes", (unsigned long long)need);
    if (at + need > REC_CAP) { /* the packet doesn't fit before the end: the rest is skipped */
        u64 skip = REC_CAP - at;
        room_wait(skip, VK_REC_FOREVER);
        RecHdr* s = (RecHdr*)(g_ring + at);
        s->fn = NULL;
        s->size = skip;
        g_pending += skip;
        at = 0;
    }
    room_wait(need, VK_REC_FOREVER);
    RecHdr* h = (RecHdr*)(g_ring + at);
    h->fn = fn;
    h->size = need;
    g_pending += need;
    return h + 1;
}
static void rec_commit(void) {
    if (g_pending - g_published >= REC_PUBLISH) publish();
}

/* Runs of draws (vk_draw_runs=1): whether one Vulkan call could take a direct draw and the one before it, from what
   the ring took between the two. Nothing at all: a multi-draw as they are. The index buffer bound again at another
   offset a whole number of indices away: the same once the offset goes in the draw's first index. Descriptor sets,
   push constants or the offsets of the same vertex buffers too: only once the draws read those by their number. Any
   other command, another buffer, another kind of draw or other instances ends the run. A binding is compared by
   value with the last one of its slot, since the renderer binds some again that did not change; the commands that
   are not bindings are told by their bytes in the ring, which the bindings' bytes do not add up to. Commands for the
   upload command buffer go through the ring too and end a run here as any other does */
enum { RUN_INDEX_AT = 1, RUN_BOUND = 2, RUN_OTHER = 4 };
enum { RUN_SETS = 1, RUN_PUSH, RUN_PUSH_DATA, RUN_OFFSETS, RUN_VERTEX_BUFFERS, RUN_VERTEX_AT };
static struct {
    u64 at, known;                       /* g_pending after the last direct draw, and the bindings' bytes since */
    u32 since;                           /* RUN_ bits: what changed since that draw */
    u32 kind, instances, first_instance; /* that draw: 1 of vertices, 2 of indices, and its instances */
    struct {
        u64 key, value;
    } last[32]; /* the last value of each kind of binding at each slot */
    VkBuffer index_buffer;
    u64 index_at;
    VkIndexType index_type;
    u64 length[VK_REC_RUN_KEYS]; /* the draws of the run each key is in */
    VkRecRuns n, logged; /* since the start, and at the last log */
    int said;            /* that nothing is counted without the recording thread */
} g_runs;
static u64 run_hash(u64 h, const void* p, u64 size) {
    const u8* b = p;
    for (u64 i = 0; i < size; i++) h = (h ^ b[i]) * 0x100000001B3ull;
    return h;
}
/* a binding of kind at slot whose value hashes to value, of bytes bytes in the ring; other: not what it bound last */
static void run_bound(u32 kind, u32 slot, u64 value, u64 bytes, u32 other) {
    u64 key = (u64)kind << 32 | slot;
    u32 i = (kind * 5 + slot) & 31;
    if (g_runs.last[i].key != key || g_runs.last[i].value != value) g_runs.since |= other;
    g_runs.last[i].key = key;
    g_runs.last[i].value = value;
    g_runs.known += bytes;
}
static void run_index(VkBuffer buffer, u64 at, VkIndexType type, u64 bytes) {
    u64 size = type == VK_INDEX_TYPE_UINT16 ? 2 : type == VK_INDEX_TYPE_UINT32 ? 4 : 1;
    u64 apart = at > g_runs.index_at ? at - g_runs.index_at : g_runs.index_at - at;
    if (buffer != g_runs.index_buffer || type != g_runs.index_type || apart % size)
        g_runs.since |= RUN_OTHER;
    else if (apart)
        g_runs.since |= RUN_INDEX_AT;
    g_runs.index_buffer = buffer;
    g_runs.index_at = at;
    g_runs.index_type = type;
    g_runs.known += bytes;
}
/* a direct draw whose packet starts at was in the ring */
static void run_draw(u32 kind, u32 instances, u32 first_instance, u64 was) {
    u32 since = g_runs.since;
    if (was - g_runs.at != g_runs.known || kind != g_runs.kind || instances != g_runs.instances ||
        first_instance != g_runs.first_instance)
        since |= RUN_OTHER;
    const u32 ends[VK_REC_RUN_KEYS] = {RUN_INDEX_AT | RUN_BOUND | RUN_OTHER, RUN_BOUND | RUN_OTHER, RUN_OTHER};
    g_runs.n.draws++;
    for (u32 k = 0; k < VK_REC_RUN_KEYS; k++) {
        if (!g_runs.length[k] || (since & ends[k])) {
            g_runs.n.runs[k]++;
            g_runs.length[k] = 0;
        }
        if (++g_runs.length[k] > g_runs.n.longest[k]) g_runs.n.longest[k] = g_runs.length[k];
    }
    g_runs.at = g_pending;
    g_runs.known = 0;
    g_runs.since = 0;
    g_runs.kind = kind;
    g_runs.instances = instances;
    g_runs.first_instance = first_instance;
}
void vk_rec_runs(VkRecRuns* runs) { *runs = g_runs.n; }
void vk_rec_runs_log(void) {
    const VkRecRuns* n = &g_runs.n;
    if (g_rec <= 0) {
        if (!g_runs.said++)
            LOG("vulkan: vk_draw_runs=1 counts what the recording thread's ring takes: vk_record is 0, and no run "
                "is counted\n");
        return;
    }
    u64 draws = n->draws - g_runs.logged.draws, runs[VK_REC_RUN_KEYS];
    double each[VK_REC_RUN_KEYS];
    for (u32 k = 0; k < VK_REC_RUN_KEYS; k++) {
        runs[k] = n->runs[k] - g_runs.logged.runs[k];
        each[k] = runs[k] ? (double)draws / (double)runs[k] : 0;
    }
    LOG("vulkan: runs of direct draws in the last 600 presents (vk_draw_runs=1): %llu draws. With nothing between "
        "two draws: %llu runs, %.2f draws each, the longest %llu. With the index buffer's offset alone: %llu runs, "
        "%.2f each, the longest %llu. With descriptors, push constants and vertex buffer offsets too: %llu runs, "
        "%.2f each, the longest %llu\n",
        (unsigned long long)draws, (unsigned long long)runs[0], each[0], (unsigned long long)n->longest[0],
        (unsigned long long)runs[1], each[1], (unsigned long long)n->longest[1], (unsigned long long)runs[2], each[2],
        (unsigned long long)n->longest[2]);
    for (u32 k = 0; k < VK_REC_RUN_KEYS; k++) g_runs.n.longest[k] = 0;
    g_runs.logged = g_runs.n;
}

int vk_rec_sync_until(u64 until) {
    int drained = 1;
    if (g_rec <= 0) return 1;
    /* The recording thread moves g_tail itself, after the packet it runs returns. A wait from one of its own packets
       would wait for itself: a submit that found the device lost calls fatal(), which releases the window through
       here, and the process then spins at full speed instead of showing the error (record.c's submit_run). Every
       packet before this one is already recorded, and the packet after it is not part of what the caller asks for */
    u32 self = plat_tid();
    if (atomic_load_explicit(&g_rec_tid, memory_order_acquire) == self) return 1;
    vk_lock(); /* no command is added meanwhile */
    barrier_flush(); /* a pending batch goes in the ring before the drain waits for it */
    publish();
    drained = room_wait(REC_CAP, until); /* all of the ring's room: everything pending made */
    vk_unlock();
    return drained;
}
void vk_rec_sync(void) { (void)vk_rec_sync_until(VK_REC_FOREVER); }

typedef struct {
    void (*fn)(void*);
    void* arg;
} RecCall;
static void rec_call_run(const void* p) {
    const RecCall* c = p;
    c->fn(c->arg);
}
void vk_rec_run(void (*fn)(void*), void* arg) {
    RecCall* c;
    if (!rec_enabled()) {
        fn(arg);
        return;
    }
    vk_lock(); /* the ring has one producer at a time */
    c = rec_push(rec_call_run, sizeof *c);
    c->fn = fn;
    c->arg = arg;
    publish();
    vk_unlock();
    vk_rec_sync(); /* the packet, and the commands queued before it */
}

void vk_rec_queue(VkQueue q) {
    if (q == g_vk.gfx) vk_rec_sync();
}

/* The graphics and the compute queue's locks (vk_queue_lock): this thread submits, presents and binds sparse memory
   (memory.c) on the graphics queue while the GPU thread may submit compute on it. The compute queue's is the graphics
   queue's when they are one queue */
static PMutex g_gfx_qlock = PMUTEX_INIT, g_compute_qlock = PMUTEX_INIT;
static PMutex* queue_mutex(VkQueue q) { return q == g_vk.gfx ? &g_gfx_qlock : &g_compute_qlock; }
void vk_queue_lock(VkQueue q) { pm_lock(queue_mutex(q)); }
int vk_queue_trylock(VkQueue q) { return pm_trylock(queue_mutex(q)); }
void vk_queue_unlock(VkQueue q) { pm_unlock(queue_mutex(q)); }

/* ------------------------------------------------------------ beginning and submitting */
static void begin_run(const void* p) {
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    VK_OK(vkBeginCommandBuffer(*(const VkCommandBuffer*)p, &bi));
}
void vk_rec_begin(VkCommandBuffer cb) {
    g_rec_cb = cb;
    if (!rec_enabled()) {
        begin_run(&cb);
        return;
    }
    *(VkCommandBuffer*)rec_push(begin_run, sizeof cb) = cb;
}

void vk_rec_begin_upload(VkCommandBuffer cb) {
    if (!rec_enabled()) {
        begin_run(&cb);
        return;
    }
    *(VkCommandBuffer*)rec_push(begin_run, sizeof cb) = cb;
}

typedef struct {
    VkCommandBuffer up, cb;
    VkSemaphoreSubmitInfo w[VK_SUBMIT_WAITS], s[2];
    u32 nw, ns;
    VkSemaphore timeline;
    u64 value;
    u64 queued; /* plat_ns when vk_rec_submit put it in the ring: its dwell before the recording thread runs it */
} Submit;
/* the graphics timeline value of the last submission vkQueueSubmit2 took (atomic): the GPU has that work. A value
   handed to the ring after it is not on the GPU yet (vk_wait_semaphore) */
static _Atomic u64 g_gfx_queued;
u64 vk_rec_gfx_queued(void) { return atomic_load_explicit(&g_gfx_queued, memory_order_acquire); }
/* where a submission's time goes, on the recording thread: the vkEndCommandBuffer's, the vkQueueSubmit2's, and how
   many submissions carry an upload command buffer. Reset by gpuprof.c's report */
u64 g_submit_end_ns, g_submit_queue_ns, g_submit_uploads, g_submit_count;
/* the submission's dwell in the ring before the recording thread reaches it (the backlog the pacer's batch left), and
   the backlog bytes (vk_rec_behind) when the GPU thread made it */
u64 g_submit_dwell_ns, g_submit_backlog, g_submit_backlog_max;
static void submit_run(const void* p) {
    const Submit* x = p;
    desc_batch_flush(); /* any set's writes precede the command buffer that binds them being ended */
    g_submit_dwell_ns += plat_ns() - x->queued;
    VkCommandBufferSubmitInfo c[2] = {{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = x->up},
                                      {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = x->cb}};
    u64 t = plat_ns();
    if (x->up) VK_OK(vkEndCommandBuffer(x->up));
    VK_OK(vkEndCommandBuffer(x->cb));
    g_submit_end_ns += plat_ns() - t;
    VkSubmitInfo2 si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
                        .waitSemaphoreInfoCount = x->nw,
                        .pWaitSemaphoreInfos = x->w,
                        .commandBufferInfoCount = x->up ? 2u : 1u,
                        .pCommandBufferInfos = x->up ? c : c + 1,
                        .signalSemaphoreInfoCount = x->ns,
                        .pSignalSemaphoreInfos = x->s};
    vk_wait_foreground();
    vk_queue_lock(g_vk.gfx);
    t = plat_ns();
    VkResult r = vkQueueSubmit2(g_vk.gfx, 1, &si, VK_NULL_HANDLE);
    g_submit_queue_ns += plat_ns() - t;
    vk_queue_unlock(g_vk.gfx);
    g_submit_uploads += x->up != VK_NULL_HANDLE;
    g_submit_count++;
    if (r == VK_SUCCESS && x->timeline == vk_gfx_timeline())
        atomic_store_explicit(&g_gfx_queued, x->value, memory_order_release);
    if (r == VK_ERROR_DEVICE_LOST) {
        vk_ck_report();
        fatal("vulkan: the GPU device was lost (driver reset or crash)");
    }
    VK_OK(r);
    /* a timeline of FFmpeg's never reaching its value would hang its decoder for good */
    if (r != VK_SUCCESS && x->ns > 1 && x->s[1].value)
        fatal("vulkan: a submission FFmpeg's Vulkan Video decoder waits for failed");
    if (r != VK_SUCCESS) { /* never signaled by the GPU: the host signals it, so waits for it and later ones end */
        VkSemaphoreSignalInfo ss = {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO, .semaphore = x->timeline, .value = x->value};
        VK_OK(vkSignalSemaphore(g_vk.dev, &ss));
    }
}
void vk_rec_submit(VkCommandBuffer up, VkCommandBuffer cb, const VkSemaphoreSubmitInfo* w, u32 nw,
                   const VkSemaphoreSubmitInfo* s, u32 ns) {
    Submit x = {.up = up, .cb = cb, .nw = nw, .ns = ns, .timeline = s[0].semaphore, .value = s[0].value,
                 .queued = plat_ns()};
    g_submit_backlog += vk_rec_behind();
    if (vk_rec_behind() > g_submit_backlog_max) g_submit_backlog_max = vk_rec_behind();
    memcpy(x.w, w, nw * sizeof *w);
    memcpy(x.s, s, ns * sizeof *s);
    g_rec_cb = VK_NULL_HANDLE;
    if (!rec_enabled()) {
        submit_run(&x);
        return;
    }
    *(Submit*)rec_push(submit_run, sizeof x) = x;
    publish(); /* the timeline value is waited for by whoever needs the work done */
}

void vk_rec_call(void (*fn)(const void* arg), const void* arg, u64 size) {
    if (!rec_enabled()) {
        fn(arg);
        return;
    }
    memcpy(rec_push(fn, size), arg, size);
    rec_commit();
}

/* A present, after the frame's submission. The GPU thread waited for the ring to drain before presenting, and so
   for the recording of its whole frame: a sixth of its time in one game, where it could have gone on with the next
   frame. Its result comes back at a later present (vk_rec_present) */
typedef struct {
    VkSwapchainKHR sc;
    VkSemaphore wait;
    u32 idx;
    u64 id;     /* display.c's, 0 none */
    u64 target; /* not shown before then (with VK_CLOCK_WAIT2, in the time domain domain), 0 none */
    u64 domain; /* the time domain's id */
    u32 stage;  /* the present stage the domain is local to, 0 none */
    int clock;  /* the swapchain's VkClock: how the id and the time are given */
} Present;
static _Atomic VkResult
    g_present_result; /* atomic: the last present's result that wasn't VK_SUCCESS, until it is taken */
static VkResult present_run(const Present* x) {
    VkPresentInfoKHR pi = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                           .waitSemaphoreCount = 1,
                           .pWaitSemaphores = &x->wait,
                           .swapchainCount = 1,
                           .pSwapchains = &x->sc,
                           .pImageIndices = &x->idx};
    VkPresentIdKHR pid = {.sType = VK_STRUCTURE_TYPE_PRESENT_ID_KHR, .swapchainCount = 1, .pPresentIds = &x->id};
    VkPresentId2KHR pid2 = {.sType = VK_STRUCTURE_TYPE_PRESENT_ID_2_KHR, .swapchainCount = 1, .pPresentIds = &x->id};
    VkPresentTimingInfoEXT ti = {.sType = VK_STRUCTURE_TYPE_PRESENT_TIMING_INFO_EXT,
                                 .targetTime = x->target,
                                 .timeDomainId = x->domain,
                                 .targetTimeDomainPresentStage = x->stage};
    VkPresentTimingsInfoEXT tis = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_TIMINGS_INFO_EXT, .swapchainCount = 1, .pTimingInfos = &ti};
    VkPresentTimeGOOGLE gt = {.presentID = (u32)x->id, .desiredPresentTime = x->target};
    VkPresentTimesInfoGOOGLE gtis = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_TIMES_INFO_GOOGLE, .swapchainCount = 1, .pTimes = &gt};
    if (x->id && x->clock == VK_CLOCK_WAIT2) {
        pi.pNext = &pid2;
        if (x->target) pid2.pNext = &tis;
    } else if (x->id && x->clock == VK_CLOCK_GOOGLE)
        pi.pNext = &gtis;
    else if (x->id)
        pi.pNext = &pid;
    SNX_ZONE(z_queue_present, "vkQueuePresentKHR");
    pm_lock(&g_vk.sc_lock); /* the swapchain, which the GPU thread acquires from and display.c waits on meanwhile */
    vk_queue_lock(g_vk.gfx);
    VkResult r = vkQueuePresentKHR(g_vk.gfx, &pi);
    vk_queue_unlock(g_vk.gfx);
    pm_unlock(&g_vk.sc_lock);
    vk_display_made(x->id, r);
    snx_gpu_pstate_present(); /* a kgsl hold lasts while frames are presented */
    return r;
}
static void present_queued(const void* p) {
    VkResult r = present_run(p);
    if (r != VK_SUCCESS) atomic_store_explicit(&g_present_result, r, memory_order_release);
}
VkResult vk_rec_present(VkSwapchainKHR sc, u32 idx, VkSemaphore wait, u64 id, u64 target) {
    Present x = {sc, wait, idx, id, g_vk.sc_hold ? target : 0, g_vk.sc_time_domain, g_vk.sc_time_stage, g_vk.sc_clock};
    if (g_rec <= 0) return present_run(&x);
    *(Present*)rec_push(present_queued, sizeof x) = x;
    publish();
    return vk_rec_present_result();
}
VkResult vk_rec_present_result(void) {
    return g_rec > 0 ? atomic_exchange_explicit(&g_present_result, VK_SUCCESS, memory_order_acq_rel) : VK_SUCCESS;
}

/* ------------------------------------------------------------ commands of scalar arguments */
#define REC_FN(Name, PARAMS, FIELDS, SET, ARGS, XARGS)                                                                 \
    typedef struct {                                                                                                   \
        VkCommandBuffer cb;                                                                                            \
        FIELDS                                                                                                         \
    } Name##Args;                                                                                                      \
    static void Name##_run(const void* p) {                                                                            \
        const Name##Args* x = p;                                                                                       \
        vk##Name XARGS;                                                                                                \
    }                                                                                                                  \
    void rec_##Name PARAMS {                                                                                           \
        if (!rec_on(cb)) {                                                                                             \
            vk##Name ARGS;                                                                                             \
            return;                                                                                                    \
        }                                                                                                              \
        Name##Args* x = rec_push(Name##_run, sizeof *x);                                                               \
        x->cb = cb;                                                                                                    \
        SET rec_commit();                                                                                              \
    }
#define REC0(Name) REC_FN(Name, (VkCommandBuffer cb), , , (cb), (x->cb))
#define REC1(Name, T1)                                                                                                 \
    REC_FN(Name, (VkCommandBuffer cb, T1 a1), T1 a1;, x->a1 = a1;, (cb, a1), (x->cb, x->a1))
#define REC2(Name, T1, T2)                                                                                             \
    REC_FN(Name, (VkCommandBuffer cb, T1 a1, T2 a2), T1 a1; T2 a2;, x->a1 = a1; x->a2 = a2;, (cb, a1, a2),            \
           (x->cb, x->a1, x->a2))
#define REC3(Name, T1, T2, T3)                                                                                         \
    REC_FN(Name, (VkCommandBuffer cb, T1 a1, T2 a2, T3 a3), T1 a1; T2 a2; T3 a3;, x->a1 = a1; x->a2 = a2;           \
           x->a3 = a3;, (cb, a1, a2, a3), (x->cb, x->a1, x->a2, x->a3))
#define REC4(Name, T1, T2, T3, T4)                                                                                     \
    REC_FN(Name, (VkCommandBuffer cb, T1 a1, T2 a2, T3 a3, T4 a4), T1 a1; T2 a2; T3 a3; T4 a4;, x->a1 = a1;          \
           x->a2 = a2; x->a3 = a3; x->a4 = a4;, (cb, a1, a2, a3, a4), (x->cb, x->a1, x->a2, x->a3, x->a4))
#define REC5(Name, T1, T2, T3, T4, T5)                                                                                 \
    REC_FN(Name, (VkCommandBuffer cb, T1 a1, T2 a2, T3 a3, T4 a4, T5 a5), T1 a1; T2 a2; T3 a3; T4 a4; T5 a5;,        \
           x->a1 = a1; x->a2 = a2; x->a3 = a3; x->a4 = a4; x->a5 = a5;, (cb, a1, a2, a3, a4, a5),                    \
           (x->cb, x->a1, x->a2, x->a3, x->a4, x->a5))
#define REC7(Name, T1, T2, T3, T4, T5, T6, T7)                                                                         \
    REC_FN(Name, (VkCommandBuffer cb, T1 a1, T2 a2, T3 a3, T4 a4, T5 a5, T6 a6, T7 a7),                              \
           T1 a1; T2 a2; T3 a3; T4 a4; T5 a5; T6 a6; T7 a7;,                                                         \
           x->a1 = a1; x->a2 = a2; x->a3 = a3; x->a4 = a4; x->a5 = a5; x->a6 = a6; x->a7 = a7;,                      \
           (cb, a1, a2, a3, a4, a5, a6, a7), (x->cb, x->a1, x->a2, x->a3, x->a4, x->a5, x->a6, x->a7))

/* A graphics pipeline may be a stand-in for one a worker is still making (vk_pipeline_get_deferred): it is waited for
   here, at its bind. One that couldn't be made binds nothing, and the draws after it until the next graphics bind are
   dropped, as the GPU thread drops a draw without its pipeline (the recording thread's own flag: 1). With
   async_pipelines one still being made isn't waited for: its draws are dropped the same way (2) until a later frame
   binds it made. A new shader's compile, 60 to 400 ms on one driver, stalled the frame it first appeared in */
static int g_rec_unbound;
static u64 g_rec_dropped, g_rec_skipped;
typedef struct {
    VkCommandBuffer cb;
    VkPipelineBindPoint bp;
    VkPipeline p;
} BindPipelineArgs;
static void bind_pipeline_run(const void* p) {
    const BindPipelineArgs* x = p;
    int graphics = x->bp == VK_PIPELINE_BIND_POINT_GRAPHICS, pending = 0;
    VkPipeline pipe =
        graphics && g_settings.async_pipelines ? vk_pipeline_now(x->p, &pending) : vk_pipeline_resolve(x->p);
    if (graphics) g_rec_unbound = pipe ? 0 : pending ? 2 : 1;
    if (pipe) vk_pipeline_bind(x->cb, x->bp, pipe);
}
void rec_CmdBindPipeline(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipeline p) {
    if (bp == VK_PIPELINE_BIND_POINT_GRAPHICS) vk_descriptor_heaps_pipeline_state(0);
    if (!rec_on(cb)) {
        VkPipeline pipe = vk_pipeline_resolve(p);
        if (pipe) vk_pipeline_bind(cb, bp, pipe);
        return;
    }
    BindPipelineArgs* x = rec_push(bind_pipeline_run, sizeof *x);
    *x = (BindPipelineArgs){cb, bp, p};
    rec_commit();
}
static int rec_drop_draw(void) {
    if (!g_rec_unbound) return 0;
    if (g_rec_unbound == 2) {
        if (g_rec_skipped++ % 1000 == 0)
            LOG("vulkan: %llu draws skipped while their pipelines were made (async_pipelines)\n",
                (unsigned long long)g_rec_skipped);
    } else if (!g_rec_dropped++)
        WARN("vulkan: a pipeline couldn't be made: its draws are dropped\n");
    return 1;
}
/* the programs of the draws recorded next (rec_draw_tag), which their checkpoints carry: the GPU thread's */
static u64 g_rec_tag[2];
static u32 g_rec_src; /* where the draw's vertices, indices and constants were bound (vk_buf_sources) */
void rec_draw_tag(u64 first, u64 second, u32 sources) {
    g_rec_tag[0] = first;
    g_rec_tag[1] = second;
    g_rec_src = sources;
}
typedef struct {
    VkCommandBuffer cb;
    u32 a1, a2, a3, a4;
    u64 tag[2];
    u32 src;
} DrawArgs;
/* NV checkpoints name the draw or dispatch that faults. The marker is a small index into a ring of descriptions the
   recording thread fills, not a string pointer: a string ring of the same size would be overwritten before the GPU (which
   lags the recorder by up to the 4 MiB command ring) reads it. The ring covers that whole lag, so an entry is intact
   when the checkpoint dump reads it */
#define CP_LOG (1u << 20)
typedef struct {
    const char* what;
    u64 key;
    u64 addr;
    u32 gx, gy, gz;
    u64 after; /* the marker of checkpoints.c set last before it: which piece of work the draw came after */
} CpDesc;
static CpDesc g_cp_desc[CP_LOG];
static _Atomic u64 g_cp_seq;
static u64 g_cp_after; /* checkpoints.c's last marker in the commands recorded so far */
/* the top bit tells these markers from checkpoints.c's (vk_checkpoints=1), which count up from 1 in the same queues */
#define CP_MARK ((uintptr_t)1 << (sizeof(uintptr_t) * 8 - 1))
static void vk_checkpoint(VkCommandBuffer cb, const char* what, u64 key, u64 addr, u32 gx, u32 gy, u32 gz) {
    if (!g_vk.has_nv_checkpoints) return;
    u64 seq = atomic_fetch_add_explicit(&g_cp_seq, 1, memory_order_relaxed) & (CP_LOG - 1);
    g_cp_desc[seq] = (CpDesc){what, key, addr, gx, gy, gz, g_cp_after};
    vkCmdSetCheckpointNV(cb, (const void*)(CP_MARK | (uintptr_t)(seq + 1)));
}
/* checkpoints.c's markers (vk_checkpoints=1) */
typedef struct {
    VkCommandBuffer cb;
    const void* marker;
} CkArgs;
static void ck_run(const void* p) {
    const CkArgs* x = p;
    g_cp_after = (u64)(uintptr_t)x->marker;
    vkCmdSetCheckpointNV(x->cb, x->marker);
}
void rec_CmdSetCheckpointNV(VkCommandBuffer cb, const void* marker) {
    if (!rec_on(cb)) {
        g_cp_after = (u64)(uintptr_t)marker;
        vkCmdSetCheckpointNV(cb, marker);
        return;
    }
    CkArgs* x = rec_push(ck_run, sizeof *x);
    *x = (CkArgs){cb, marker};
    rec_commit();
}
/* A hang or device-lost is usually a GPU fault: read the queues' NV checkpoints (each a draw or dispatch's marker set
   above) so the log names the one that hung. Dumped once: after a device-lost every later VK call fails too */
/* The GPU's own report of what it faulted on (VK_EXT_device_fault): its description, the addresses it names (the
   instruction's, a read's or a write's) and the driver's codes. A hang names no address, but says it was one */
static void fault_dump(const char* why) {
    static const char* const kinds[] = {"none", "read invalid", "write invalid", "execute invalid",
                                        "instruction pointer unknown", "instruction pointer invalid",
                                        "instruction pointer fault"};
    static int dumped;
    if (dumped || !g_vk.has_fault) return;
    dumped = 1;
    VkDeviceFaultCountsEXT n = {.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT};
    if (vkGetDeviceFaultInfoEXT(g_vk.dev, &n, NULL) < 0) return;
    VkDeviceFaultAddressInfoEXT* a = n.addressInfoCount ? calloc(n.addressInfoCount, sizeof *a) : NULL;
    VkDeviceFaultVendorInfoEXT* v = n.vendorInfoCount ? calloc(n.vendorInfoCount, sizeof *v) : NULL;
    VkDeviceFaultInfoEXT info = {.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT, .pAddressInfos = a, .pVendorInfos = v};
    n.vendorBinarySize = 0; /* not asked for */
    if ((!n.addressInfoCount || a) && (!n.vendorInfoCount || v) && vkGetDeviceFaultInfoEXT(g_vk.dev, &n, &info) >= 0) {
        LOG("vulkan: %s: the GPU reports %s\n", why, info.description);
        for (u32 i = 0; i < n.addressInfoCount; i++)
            LOG("vulkan: %s: fault address %#llx (%s, within %#llx)\n", why, (unsigned long long)a[i].reportedAddress,
                (u32)a[i].addressType < sizeof kinds / sizeof *kinds ? kinds[a[i].addressType] : "another kind",
                (unsigned long long)a[i].addressPrecision);
        for (u32 i = 0; i < n.vendorInfoCount; i++)
            LOG("vulkan: %s: driver fault %s, code %#llx data %#llx\n", why, v[i].description,
                (unsigned long long)v[i].vendorFaultCode, (unsigned long long)v[i].vendorFaultData);
    }
    free(a);
    free(v);
}
/* how full each heap of the device was then, and devmem.c's blocks: a GPU that stops for want of memory says so
   nowhere else */
static void memory_dump(const char* why) {
    static int dumped;
    DevMemBudget b[VK_MAX_MEMORY_HEAPS];
    if (dumped) return;
    dumped = 1;
    vk_devmem_budgets(b);
    for (u32 h = 0; h < vk_devmem_props()->memoryHeapCount; h++)
        LOG("vulkan: %s: heap %u in use %llu of %llu MiB budgeted, %llu MiB of it this renderer's blocks with %llu MiB "
            "handed out\n",
            why, h, (unsigned long long)(b[h].usage >> 20), (unsigned long long)(b[h].budget >> 20),
            (unsigned long long)(b[h].block_bytes >> 20), (unsigned long long)(b[h].allocated_bytes >> 20));
    vk_devmem_log();
}
void vk_checkpoint_dump(const char* why) {
    static int dumped;
    fault_dump(why);
    memory_dump(why);
    if (dumped || !g_vk.has_nv_checkpoints) return;
    dumped = 1;
    VkQueue qs[2] = {g_vk.gfx, g_vk.compute};
    const char* names[2] = {"graphics", "compute"};
    u32 qi;
    if (g_vk.compute == g_vk.gfx || !g_vk.compute) { /* one queue: dump it once */
        qs[1] = VK_NULL_HANDLE;
    }
    for (qi = 0; qi < 2 && qs[qi]; qi++) {
        u32 n = 0, i;
        vkGetQueueCheckpointDataNV(qs[qi], &n, NULL);
        if (!n) continue;
        VkCheckpointDataNV* c = (VkCheckpointDataNV*)calloc(n, sizeof *c);
        if (!c) continue;
        vkGetQueueCheckpointDataNV(qs[qi], &n, c);
        for (i = 0; i < n; i++) {
            uintptr_t mark = (uintptr_t)c[i].pCheckpointMarker;
            u64 idx = (mark & CP_MARK) ? (u64)(mark & ~CP_MARK) : 0; /* 0: checkpoints.c's, which vk_ck_report names */
            if (idx >= 1 && idx <= CP_LOG) {
                CpDesc d = g_cp_desc[idx - 1];
                /* the three numbers: a dispatch's grid; a draw's vertices or indices, its instances and where its
                   inputs were bound (vk_buf_sources' bits); an indirect draw's most draws, stride and kind */
                LOG("vulkan: %s (%s) checkpoint[%u] stage %#x: %s %016llx code %llx grid %ux%ux%u, after marker "
                    "%llu\n",
                    why, names[qi], i, c[i].stage, d.what, (unsigned long long)d.key, (unsigned long long)d.addr, d.gx,
                    d.gy, d.gz, (unsigned long long)d.after);
            } else {
                LOG("vulkan: %s (%s) checkpoint[%u] stage %#x: marker %p\n", why, names[qi], i, c[i].stage,
                    c[i].pCheckpointMarker);
            }
        }
        free(c);
    }
}
static void draw_run(const void* p) {
    const DrawArgs* x = p;
    vk_checkpoint(x->cb, "draw", x->tag[0], x->tag[1], x->a1, x->a2, x->src);
    if (!rec_drop_draw()) vkCmdDraw(x->cb, x->a1, x->a2, x->a3, x->a4);
    vk_checkpoint(x->cb, "the end of draw", x->tag[0], x->tag[1], x->a1, x->a2, x->src);
}
void rec_CmdDraw(VkCommandBuffer cb, u32 vertices, u32 instances, u32 first_vertex, u32 first_instance) {
    if (!rec_on(cb)) {
        vk_checkpoint(cb, "draw", g_rec_tag[0], g_rec_tag[1], vertices, instances, first_vertex);
        vkCmdDraw(cb, vertices, instances, first_vertex, first_instance);
        return;
    }
    const u64 was = g_pending;
    DrawArgs* x = rec_push(draw_run, sizeof *x);
    *x = (DrawArgs){cb, vertices, instances, first_vertex, first_instance, {g_rec_tag[0], g_rec_tag[1]}, g_rec_src};
    if (g_settings.draw_run_stats) run_draw(1, instances, first_instance, was);
    rec_commit();
}
typedef struct {
    VkCommandBuffer cb;
    u32 a1, a2, a3;
    i32 a4;
    u32 a5;
    u64 tag[2];
    u32 src;
} DrawIndexedArgs;
static void draw_indexed_run(const void* p) {
    const DrawIndexedArgs* x = p;
    vk_checkpoint(x->cb, "draw_indexed", x->tag[0], x->tag[1], x->a1, x->a2, x->src);
    if (!rec_drop_draw()) vkCmdDrawIndexed(x->cb, x->a1, x->a2, x->a3, x->a4, x->a5);
    vk_checkpoint(x->cb, "the end of draw_indexed", x->tag[0], x->tag[1], x->a1, x->a2, x->src);
}
void rec_CmdDrawIndexed(VkCommandBuffer cb, u32 indices, u32 instances, u32 first_index, i32 vertex_offset,
                        u32 first_instance) {
    if (!rec_on(cb)) {
        vk_checkpoint(cb, "draw_indexed", g_rec_tag[0], g_rec_tag[1], indices, instances, first_index);
        vkCmdDrawIndexed(cb, indices, instances, first_index, vertex_offset, first_instance);
        return;
    }
    const u64 was = g_pending;
    DrawIndexedArgs* x = rec_push(draw_indexed_run, sizeof *x);
    *x = (DrawIndexedArgs){cb, indices, instances, first_index, vertex_offset, first_instance,
                           {g_rec_tag[0], g_rec_tag[1]}, g_rec_src};
    if (g_settings.draw_run_stats) run_draw(2, instances, first_instance, was);
    rec_commit();
}
/* an Ounce's mesh draw: groups workgroups in a row (render.c) */
/* a mesh draw of groups workgroups, or with buf the one record {x, y, z} at off there (vkCmdDrawMeshTasksIndirectEXT) */
typedef struct {
    VkCommandBuffer cb;
    VkBuffer buf;
    VkDeviceSize off;
    u32 groups;
    u64 tag[2];
} DrawMeshArgs;
static void draw_mesh_now(const DrawMeshArgs* x) {
    vk_checkpoint(x->cb, "mesh", x->tag[0], x->tag[1], 0, 0, 0);
    if (x->buf)
        vkCmdDrawMeshTasksIndirectEXT(x->cb, x->buf, x->off, 1, 16);
    else
        vkCmdDrawMeshTasksEXT(x->cb, x->groups, 1, 1);
}
static void draw_mesh_run(const void* p) {
    if (!rec_drop_draw()) draw_mesh_now((const DrawMeshArgs*)p);
}
static void rec_draw_mesh(const DrawMeshArgs* a) {
    if (!rec_on(a->cb)) {
        draw_mesh_now(a);
        return;
    }
    DrawMeshArgs* x = rec_push(draw_mesh_run, sizeof *x);
    *x = *a;
    rec_commit();
}
void rec_CmdDrawMeshTasks(VkCommandBuffer cb, u32 groups) {
    DrawMeshArgs a = {cb, VK_NULL_HANDLE, 0, groups, {g_rec_tag[0], g_rec_tag[1]}};
    rec_draw_mesh(&a);
}
void rec_CmdDrawMeshTasksIndirect(VkCommandBuffer cb, VkBuffer buf, VkDeviceSize off) {
    DrawMeshArgs a = {cb, buf, off, 0, {g_rec_tag[0], g_rec_tag[1]}};
    rec_draw_mesh(&a);
}
/* the indirect draws: kind bit 0 indexed, bit 1 limited by the count at count_buf */
typedef struct {
    VkCommandBuffer cb;
    VkBuffer buf, count_buf;
    VkDeviceSize off, count_off;
    u32 kind, max, stride;
    u64 tag[2];
} DrawIndirectArgs;
static void draw_indirect_now(const DrawIndirectArgs* x) {
    vk_checkpoint(x->cb, "draw_indirect", x->tag[0], x->tag[1], x->max, x->stride, x->kind);
    switch (x->kind) {
        case 0: vkCmdDrawIndirect(x->cb, x->buf, x->off, x->max, x->stride); break;
        case 1: vkCmdDrawIndexedIndirect(x->cb, x->buf, x->off, x->max, x->stride); break;
        case 2: vkCmdDrawIndirectCount(x->cb, x->buf, x->off, x->count_buf, x->count_off, x->max, x->stride); break;
        default:
            vkCmdDrawIndexedIndirectCount(x->cb, x->buf, x->off, x->count_buf, x->count_off, x->max, x->stride);
            break;
    }
}
static void draw_indirect_run(const void* p) {
    if (!rec_drop_draw()) draw_indirect_now((const DrawIndirectArgs*)p);
}
void rec_CmdDrawIndirect(VkCommandBuffer cb, int indexed, VkBuffer buf, VkDeviceSize off, VkBuffer count_buf,
                         VkDeviceSize count_off, u32 max, u32 stride) {
    DrawIndirectArgs a = {cb, buf, count_buf, off, count_off, (indexed ? 1u : 0u) | (count_buf ? 2u : 0u), max,
                          stride, {g_rec_tag[0], g_rec_tag[1]}};
    if (!rec_on(cb)) {
        draw_indirect_now(&a);
        return;
    }
    DrawIndirectArgs* x = rec_push(draw_indirect_run, sizeof *x);
    *x = a;
    rec_commit();
}
typedef struct {
    VkCommandBuffer cb;
    VkBuffer buffer;
    VkDeviceSize at;
    VkIndexType type;
} IndexBufferArgs;
static void index_buffer_run(const void* p) {
    const IndexBufferArgs* x = p;
    vkCmdBindIndexBuffer(x->cb, x->buffer, x->at, x->type);
}
void rec_CmdBindIndexBuffer(VkCommandBuffer cb, VkBuffer buffer, VkDeviceSize at, VkIndexType type) {
    if (!rec_on(cb)) {
        vkCmdBindIndexBuffer(cb, buffer, at, type);
        return;
    }
    const u64 was = g_pending;
    IndexBufferArgs* x = rec_push(index_buffer_run, sizeof *x);
    *x = (IndexBufferArgs){cb, buffer, at, type};
    if (g_settings.draw_run_stats) run_index(buffer, at, type, g_pending - was);
    rec_commit();
}
/* generated under other names: the calls go through vk_gpuprof's timing of each dispatch first, and each carries a
   checkpoint so a hanging dispatch (a Xid 109 CTX SWITCH TIMEOUT) is named in vk/owned.c's queue checkpoint dump */
typedef struct {
    VkCommandBuffer cb;
    u32 a1, a2, a3;
    u64 key, addr;
} DispatchArgs;
static void dispatch_run(const void* p) {
    const DispatchArgs* x = p;
    vk_checkpoint(x->cb, "cs", x->key, x->addr, x->a1, x->a2, x->a3);
    vkCmdDispatch(x->cb, x->a1, x->a2, x->a3);
}
static void rec_CmdDispatch_raw(VkCommandBuffer cb, u32 a1, u32 a2, u32 a3, u64 key, u64 addr) {
    if (!rec_on(cb)) {
        vk_checkpoint(cb, "cs", key, addr, a1, a2, a3);
        vkCmdDispatch(cb, a1, a2, a3);
        return;
    }
    DispatchArgs* x = rec_push(dispatch_run, sizeof *x);
    *x = (DispatchArgs){cb, a1, a2, a3, key, addr};
    rec_commit();
}
typedef struct {
    VkCommandBuffer cb;
    VkBuffer a1;
    VkDeviceSize a2;
    u64 key, addr;
} DispatchIndirectArgs;
static void dispatch_indirect_run(const void* p) {
    const DispatchIndirectArgs* x = p;
    vk_checkpoint(x->cb, "cs", x->key, x->addr, 1, 1, 1); /* the grid is in the buffer */
    vkCmdDispatchIndirect(x->cb, x->a1, x->a2);
}
static void rec_CmdDispatchIndirect_raw(VkCommandBuffer cb, VkBuffer a1, VkDeviceSize a2, u64 key, u64 addr) {
    if (!rec_on(cb)) {
        vk_checkpoint(cb, "cs", key, addr, 1, 1, 1);
        vkCmdDispatchIndirect(cb, a1, a2);
        return;
    }
    DispatchIndirectArgs* x = rec_push(dispatch_indirect_run, sizeof *x);
    *x = (DispatchIndirectArgs){cb, a1, a2, key, addr};
    rec_commit();
}
void rec_CmdDispatch(VkCommandBuffer cb, u32 x, u32 y, u32 z) {
    int timed = vk_gpuprof_dispatch_begin(cb);
    rec_CmdDispatch_raw(cb, x, y, z, 0, 0);
    if (timed) vk_gpuprof_dispatch_end(cb);
}
void rec_CmdDispatchIndirect(VkCommandBuffer cb, VkBuffer buf, VkDeviceSize off) {
    int timed = vk_gpuprof_dispatch_begin(cb);
    rec_CmdDispatchIndirect_raw(cb, buf, off, 0, 0);
    if (timed) vk_gpuprof_dispatch_end(cb);
}
/* the game's compute dispatches (compute.c) carry the program's key and code address, so a fault names its shader, and
   vk_gpu_profile sums their time by program */
void rec_CmdDispatchKey(VkCommandBuffer cb, u32 x, u32 y, u32 z, u64 key, u64 addr) {
    vk_gpuprof_tag(key);
    int timed = vk_gpuprof_dispatch_begin(cb);
    rec_CmdDispatch_raw(cb, x, y, z, key, addr);
    if (timed) vk_gpuprof_dispatch_end(cb);
}
void rec_CmdDispatchIndirectKey(VkCommandBuffer cb, VkBuffer buf, VkDeviceSize off, u64 key, u64 addr) {
    vk_gpuprof_tag(key);
    int timed = vk_gpuprof_dispatch_begin(cb);
    rec_CmdDispatchIndirect_raw(cb, buf, off, key, addr);
    if (timed) vk_gpuprof_dispatch_end(cb);
}
REC0(CmdEndRendering)
REC1(CmdSetCullMode, VkCullModeFlags)
REC1(CmdSetFrontFace, VkFrontFace)
REC1(CmdSetPrimitiveTopology, VkPrimitiveTopology)
REC1(CmdSetPrimitiveRestartEnable, VkBool32)
REC1(CmdSetRasterizerDiscardEnable, VkBool32)
REC1(CmdSetLineWidth, float)
REC1(CmdSetDepthBiasEnable, VkBool32)
REC3(CmdSetDepthBias, float, float, float)
REC1(CmdSetDepthTestEnable, VkBool32)
REC1(CmdSetDepthWriteEnable, VkBool32)
REC1(CmdSetDepthCompareOp, VkCompareOp)
REC1(CmdSetDepthBoundsTestEnable, VkBool32)
REC2(CmdSetDepthBounds, float, float)
REC1(CmdSetStencilTestEnable, VkBool32)
REC5(CmdSetStencilOp, VkStencilFaceFlags, VkStencilOp, VkStencilOp, VkStencilOp, VkCompareOp)
REC2(CmdSetStencilReference, VkStencilFaceFlags, u32)
REC2(CmdSetStencilCompareMask, VkStencilFaceFlags, u32)
REC2(CmdSetStencilWriteMask, VkStencilFaceFlags, u32)
REC1(CmdSetAttachmentFeedbackLoopEnableEXT, VkImageAspectFlags)
REC1(CmdSetPolygonModeEXT, VkPolygonMode)
REC1(CmdSetDepthClampEnableEXT, VkBool32)
REC1(CmdSetDepthClipEnableEXT, VkBool32)
REC1(CmdSetProvokingVertexModeEXT, VkProvokingVertexModeEXT)
REC1(CmdSetConservativeRasterizationModeEXT, VkConservativeRasterizationModeEXT)
REC1(CmdSetExtraPrimitiveOverestimationSizeEXT, float)
REC1(CmdSetPatchControlPointsEXT, u32)
REC1(CmdSetDepthClipNegativeOneToOneEXT, VkBool32)
REC3(CmdResetQueryPool, VkQueryPool, u32, u32)
REC3(CmdWriteTimestamp2, VkPipelineStageFlags2, VkQueryPool, u32)
REC3(CmdBeginQuery, VkQueryPool, u32, VkQueryControlFlags)
REC2(CmdEndQuery, VkQueryPool, u32)
REC7(CmdCopyQueryPoolResults, VkQueryPool, u32, u32, VkBuffer, VkDeviceSize, VkDeviceSize, VkQueryResultFlags)
REC0(CmdEndConditionalRenderingEXT)

/* conditional rendering on the word at buf + off, its begin info made from the arguments */
typedef struct {
    VkCommandBuffer cb;
    VkBuffer buf;
    VkDeviceSize off;
    VkConditionalRenderingFlagsEXT flags;
} CondArgs;
static void cond_call(VkCommandBuffer cb, VkBuffer buf, VkDeviceSize off, VkConditionalRenderingFlagsEXT flags) {
    VkConditionalRenderingBeginInfoEXT ci = {.sType = VK_STRUCTURE_TYPE_CONDITIONAL_RENDERING_BEGIN_INFO_EXT,
                                             .buffer = buf,
                                             .offset = off,
                                             .flags = flags};
    vkCmdBeginConditionalRenderingEXT(cb, &ci);
}
static void cond_run(const void* p) {
    const CondArgs* x = p;
    cond_call(x->cb, x->buf, x->off, x->flags);
}
void rec_CmdBeginConditionalRenderingEXT(VkCommandBuffer cb, VkBuffer buf, VkDeviceSize off,
                                         VkConditionalRenderingFlagsEXT flags) {
    if (!rec_on(cb)) {
        cond_call(cb, buf, off, flags);
        return;
    }
    CondArgs* x = rec_push(cond_run, sizeof *x);
    x->cb = cb;
    x->buf = buf;
    x->off = off;
    x->flags = flags;
    rec_commit();
}

/* many copies from a list the GPU reads (VK_KHR_copy_memory_indirect) */
typedef struct {
    VkCommandBuffer cb;
    VkAddressCopyFlagsKHR src, dst;
    u32 count;
    VkStridedDeviceAddressRangeKHR range;
} CopyIndirectArgs;
static void copy_indirect_call(const CopyIndirectArgs* x) {
    VkCopyMemoryIndirectInfoKHR i = {.sType = VK_STRUCTURE_TYPE_COPY_MEMORY_INDIRECT_INFO_KHR,
                                     .srcCopyFlags = x->src,
                                     .dstCopyFlags = x->dst,
                                     .copyCount = x->count,
                                     .copyAddressRange = x->range};
    vkCmdCopyMemoryIndirectKHR(x->cb, &i);
}
static void copy_indirect_run(const void* p) { copy_indirect_call(p); }
void rec_CmdCopyMemoryIndirectKHR(VkCommandBuffer cb, const VkCopyMemoryIndirectInfoKHR* info) {
    CopyIndirectArgs a = {cb, info->srcCopyFlags, info->dstCopyFlags, info->copyCount, info->copyAddressRange};
    /* timed for vk_gpu_profile as the copies it takes the place of are (rec_CmdCopyBuffer), its bytes in the GPU's
       list. Each ring's copy counts, as they do (rec_xfer) */
    int timed = vk_gpuprof_profiled(cb) &&
                vk_gpuprof_op_begin(cb, VK_OP_COPY,
                                    (u64)((const u8*)__builtin_return_address(0) - (const u8*)vk_gpuprof_base()), 0);
    if (!rec_on(cb))
        copy_indirect_call(&a);
    else {
        g_rec_xfers++;
        g_rec_xfer_kind[0]++;
        *(CopyIndirectArgs*)rec_push(copy_indirect_run, sizeof a) = a;
        rec_commit();
    }
    if (timed) vk_gpuprof_op_end(cb);
}
/* ------------------------------------------------------------ commands with arrays, copied after their arguments */
/* the packet's arrays follow its arguments, each at a multiple of 16 */
#define REC_AL(n) (((n) + 15) & ~(u64)15)

typedef struct {
    VkCommandBuffer cb;
    float c[4];
} BlendArgs;
static void blend_run(const void* p) {
    const BlendArgs* x = p;
    vkCmdSetBlendConstants(x->cb, x->c);
}
typedef struct {
    VkCommandBuffer cb;
    VkDepthClampModeEXT mode;
    VkDepthClampRangeEXT range;
} ClampRangeArgs;
static void clamp_range_run(const void* p) {
    const ClampRangeArgs* x = p;
    vkCmdSetDepthClampRangeEXT(x->cb, x->mode, &x->range);
}
void rec_CmdSetDepthClampRangeEXT(VkCommandBuffer cb, VkDepthClampModeEXT mode, const VkDepthClampRangeEXT* range) {
    if (!rec_on(cb)) {
        vkCmdSetDepthClampRangeEXT(cb, mode, range);
        return;
    }
    ClampRangeArgs* x = rec_push(clamp_range_run, sizeof *x);
    *x = (ClampRangeArgs){cb, mode, *range};
    rec_commit();
}
void rec_CmdSetBlendConstants(VkCommandBuffer cb, const float c[4]) {
    if (!rec_on(cb)) {
        vkCmdSetBlendConstants(cb, c);
        return;
    }
    BlendArgs* x = rec_push(blend_run, sizeof *x);
    x->cb = cb;
    memcpy(x->c, c, sizeof x->c);
    rec_commit();
}

typedef struct {
    VkCommandBuffer cb;
    VkPipelineLayout layout;
    VkShaderStageFlags stages;
    u32 offset, size;
} PushArgs;
static void push_run(const void* p) {
    const PushArgs* x = p;
    vkCmdPushConstants(x->cb, x->layout, x->stages, x->offset, x->size, (const u8*)p + REC_AL(sizeof *x));
}
void rec_CmdPushConstants(VkCommandBuffer cb, VkPipelineLayout layout, VkShaderStageFlags stages, u32 offset, u32 size,
                          const void* data) {
    if (vk_descriptor_heaps_active() && vk_graphics_playout_is(layout)) {
        rec_CmdPushDataEXT(cb, offset, size, data);
        return;
    }
    vk_descriptor_heaps_invalidate();
    /* the device reads push constants as it reads memory, little-endian: a big-endian host swaps the words */
    if (!rec_on(cb)) {
        u32 le[64];
        if (SNX_HOST_BE && size <= sizeof le) {
            memcpy(le, data, size);
            snx_le32_array(le, size / 4);
            data = le;
        }
        vkCmdPushConstants(cb, layout, stages, offset, size, data);
        return;
    }
    const u64 was = g_pending;
    PushArgs* x = rec_push(push_run, REC_AL(sizeof *x) + size);
    *x = (PushArgs){cb, layout, stages, offset, size};
    memcpy((u8*)x + REC_AL(sizeof *x), data, size);
    snx_le32_array((u8*)x + REC_AL(sizeof *x), size / 4);
    if (g_settings.draw_run_stats)
        run_bound(RUN_PUSH, offset, run_hash(run_hash(stages, &layout, sizeof layout), data, size), g_pending - was,
                  RUN_BOUND);
    rec_commit();
}

typedef struct {
    VkCommandBuffer cb;
    u32 offset, size;
} HeapPushArgs;
static void heap_push_run(const void* p) {
    const HeapPushArgs* x = p;
    VkPushDataInfoEXT info = {.sType = VK_STRUCTURE_TYPE_PUSH_DATA_INFO_EXT, .offset = x->offset,
        .data = {.address = (const u8*)p + REC_AL(sizeof *x), .size = x->size}};
    vkCmdPushDataEXT(x->cb, &info);
}
void rec_CmdPushDataEXT(VkCommandBuffer cb, u32 offset, u32 size, const void* data) {
    vk_descriptors_invalidate();
    if (!rec_on(cb)) {
        u32 le[64];
        if (SNX_HOST_BE && size <= sizeof le) {
            memcpy(le, data, size);
            snx_le32_array(le, size / 4);
            data = le;
        }
        VkPushDataInfoEXT info = {.sType = VK_STRUCTURE_TYPE_PUSH_DATA_INFO_EXT, .offset = offset,
                                  .data = {.address = data, .size = size}};
        vkCmdPushDataEXT(cb, &info);
        return;
    }
    const u64 was = g_pending;
    HeapPushArgs* x = rec_push(heap_push_run, REC_AL(sizeof *x) + size);
    *x = (HeapPushArgs){cb, offset, size};
    memcpy((u8*)x + REC_AL(sizeof *x), data, size);
    snx_le32_array((u8*)x + REC_AL(sizeof *x), size / 4);
    if (g_settings.draw_run_stats)
        run_bound(RUN_PUSH_DATA, offset, run_hash(size, data, size), g_pending - was, RUN_BOUND);
    rec_commit();
}
typedef struct {
    VkCommandBuffer cb;
    VkBindHeapInfoEXT resource, sampler;
} HeapBindArgs;
static void heap_bind_run(const void* p) {
    const HeapBindArgs* x = p;
    vkCmdBindResourceHeapEXT(x->cb, &x->resource);
    vkCmdBindSamplerHeapEXT(x->cb, &x->sampler);
}
void rec_CmdBindDescriptorHeapsEXT(VkCommandBuffer cb, const VkBindHeapInfoEXT* resource,
                                  const VkBindHeapInfoEXT* sampler) {
    vk_descriptors_invalidate();
    HeapBindArgs args = {cb, *resource, *sampler};
    if (!rec_on(cb)) {
        heap_bind_run(&args);
        return;
    }
    HeapBindArgs* x = rec_push(heap_bind_run, sizeof *x);
    *x = args;
    rec_commit();
}
typedef struct {
    VkCommandBuffer cb;
    VkPipelineBindPoint bp;
    VkPipelineLayout layout;
    u32 first, n, ndyn;
} SetsArgs;
static VkDescriptorSet rec_set_real(VkDescriptorSet s);
static void sets_run(const void* p) {
    const SetsArgs* x = p;
    const VkDescriptorSet* sets = (const void*)((const u8*)p + REC_AL(sizeof *x));
    const u32* dyn = (const void*)((const u8*)sets + REC_AL(x->n * sizeof *sets));
    VkDescriptorSet real[8];
    desc_batch_flush(); /* a set's writes precede the bind that names it */
    for (u32 i = 0; i < x->n && i < 8; i++)
        if (!(real[i] = rec_set_real(sets[i]))) return; /* a set the pools had no room for: not bound */
    vkCmdBindDescriptorSets(x->cb, x->bp, x->layout, x->first, x->n, real, x->ndyn, x->ndyn ? dyn : NULL);
}
static _Atomic u64 g_set_binds;
u64 vk_rec_set_binds(void) { return atomic_load_explicit(&g_set_binds, memory_order_relaxed); }
void rec_CmdBindDescriptorSets(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout layout, u32 first, u32 n,
                               const VkDescriptorSet* sets, u32 ndyn, const u32* dyn) {
    vk_descriptor_heaps_invalidate();
    atomic_fetch_add_explicit(&g_set_binds, 1, memory_order_relaxed);
    if (!rec_on(cb)) {
        vkCmdBindDescriptorSets(cb, bp, layout, first, n, sets, ndyn, dyn);
        return;
    }
    u64 so = REC_AL(sizeof(SetsArgs)), dof = so + REC_AL(n * sizeof *sets);
    const u64 was = g_pending;
    SetsArgs* x = rec_push(sets_run, dof + ndyn * sizeof *dyn);
    *x = (SetsArgs){cb, bp, layout, first, n, ndyn};
    memcpy((u8*)x + so, sets, n * sizeof *sets);
    if (ndyn) memcpy((u8*)x + dof, dyn, ndyn * sizeof *dyn);
    if (g_settings.draw_run_stats && bp == VK_PIPELINE_BIND_POINT_GRAPHICS)
        run_bound(
            RUN_SETS, first,
            run_hash(run_hash(run_hash(n, &layout, sizeof layout), sets, n * sizeof *sets), dyn, ndyn * sizeof *dyn),
            g_pending - was, RUN_BOUND);
    rec_commit();
}

typedef struct {
    VkCommandBuffer cb;
    VkDeviceAddress address;
} DescriptorBufferArgs;
static void descriptor_buffer_run(const void* p) {
    const DescriptorBufferArgs* x = p;
    VkDescriptorBufferBindingInfoEXT info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_BUFFER_BINDING_INFO_EXT,
        .address = x->address,
        .usage = VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT | VK_BUFFER_USAGE_SAMPLER_DESCRIPTOR_BUFFER_BIT_EXT};
    vkCmdBindDescriptorBuffersEXT(x->cb, 1, &info);
}
void rec_CmdBindDescriptorBuffersEXT(VkCommandBuffer cb, VkDeviceAddress address) {
    vk_descriptor_heaps_invalidate();
    DescriptorBufferArgs args = {cb, address};
    if (!rec_on(cb)) {
        descriptor_buffer_run(&args);
        return;
    }
    DescriptorBufferArgs* x = rec_push(descriptor_buffer_run, sizeof *x);
    *x = args;
    rec_commit();
}
typedef struct {
    VkCommandBuffer cb;
    VkPipelineLayout layout;
    u32 first, count;
    VkPipelineBindPoint bp;
    VkDeviceSize offsets[5];
} DescriptorOffsetsArgs;
static void descriptor_offsets_run(const void* p) {
    const DescriptorOffsetsArgs* x = p;
    const u32 indices[5] = {0};
    vkCmdSetDescriptorBufferOffsetsEXT(x->cb, x->bp, x->layout,
                                        x->first, x->count, indices, x->offsets);
}
void rec_CmdSetDescriptorBufferOffsetsEXT(VkCommandBuffer cb, VkPipelineLayout layout, u32 first, u32 count,
                                         const VkDeviceSize* offsets) {
    rec_CmdSetDescriptorBufferOffsetsBP(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, first, count, offsets);
}
/* the offsets of a set bound at another bind point: a compute pipeline's (vk_descriptors_bind_compute) */
void rec_CmdSetDescriptorBufferOffsetsBP(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout layout, u32 first,
                                         u32 count, const VkDeviceSize* offsets) {
    vk_descriptor_heaps_invalidate();
    DescriptorOffsetsArgs args = {.cb = cb, .layout = layout, .first = first, .count = count, .bp = bp};
    for (u32 i = 0; i < count && i < 5; i++) args.offsets[i] = offsets[i];
    if (!rec_on(cb)) {
        descriptor_offsets_run(&args);
        return;
    }
    const u64 was = g_pending;
    DescriptorOffsetsArgs* x = rec_push(descriptor_offsets_run, sizeof *x);
    *x = args;
    if (g_settings.draw_run_stats)
        run_bound(RUN_OFFSETS, first,
                  run_hash(run_hash(count, &layout, sizeof layout), args.offsets, sizeof args.offsets), g_pending - was,
                  RUN_BOUND);
    rec_commit();
}

typedef struct {
    VkCommandBuffer cb;
    u32 first, n;
    int sizes, strides;
} VbArgs;
static void vb_run(const void* p) {
    const VbArgs* x = p;
    const u8* q = (const u8*)p + REC_AL(sizeof *x);
    u64 bn = REC_AL(x->n * sizeof(VkBuffer)), dn = REC_AL(x->n * sizeof(VkDeviceSize));
    const VkDeviceSize* offs = (const void*)(q + bn);
    const VkDeviceSize* sizes = x->sizes ? (const void*)(q + bn + dn) : NULL;
    const VkDeviceSize* strides = x->strides ? (const void*)(q + bn + dn * (1 + x->sizes)) : NULL;
    vkCmdBindVertexBuffers2(x->cb, x->first, x->n, (const VkBuffer*)q, offs, sizes, strides);
}
void rec_CmdBindVertexBuffers2(VkCommandBuffer cb, u32 first, u32 n, const VkBuffer* bufs, const VkDeviceSize* offs,
                               const VkDeviceSize* sizes, const VkDeviceSize* strides) {
    if (!rec_on(cb)) {
        vkCmdBindVertexBuffers2(cb, first, n, bufs, offs, sizes, strides);
        return;
    }
    u64 h = REC_AL(sizeof(VbArgs)), bn = REC_AL(n * sizeof *bufs), dn = REC_AL(n * sizeof *offs);
    const u64 was = g_pending;
    VbArgs* x = rec_push(vb_run, h + bn + dn * (1 + !!sizes + !!strides));
    u8* q = (u8*)x + h;
    *x = (VbArgs){cb, first, n, !!sizes, !!strides};
    memcpy(q, bufs, n * sizeof *bufs);
    memcpy(q + bn, offs, n * sizeof *offs);
    if (sizes) memcpy(q + bn + dn, sizes, n * sizeof *sizes);
    if (strides) memcpy(q + bn + dn * (1 + !!sizes), strides, n * sizeof *strides);
    if (g_settings.draw_run_stats) {
        u64 where = run_hash(n, offs, n * sizeof *offs);
        if (sizes) where = run_hash(where, sizes, n * sizeof *sizes);
        if (strides) where = run_hash(where ^ 1, strides, n * sizeof *strides);
        run_bound(RUN_VERTEX_BUFFERS, first, run_hash(n, bufs, n * sizeof *bufs), 0, RUN_OTHER);
        run_bound(RUN_VERTEX_AT, first, where, g_pending - was, RUN_BOUND);
    }
    rec_commit();
}
void rec_CmdBindVertexBuffers(VkCommandBuffer cb, u32 first, u32 n, const VkBuffer* bufs, const VkDeviceSize* offs) {
    rec_CmdBindVertexBuffers2(cb, first, n, bufs, offs, NULL, NULL);
}

/* a draw's vertex layout as dynamic state: the bindings, then the attributes */
typedef struct {
    VkCommandBuffer cb;
    u32 nb, na;
} VertexInputArgs;
static void vertex_input_run(const void* p) {
    const VertexInputArgs* x = p;
    const u8* q = (const u8*)p + REC_AL(sizeof *x);
    vkCmdSetVertexInputEXT(x->cb, x->nb, (const void*)q, x->na,
                           (const void*)(q + REC_AL(x->nb * sizeof(VkVertexInputBindingDescription2EXT))));
}
void rec_CmdSetVertexInputEXT(VkCommandBuffer cb, u32 nb, const VkVertexInputBindingDescription2EXT* b, u32 na,
                              const VkVertexInputAttributeDescription2EXT* a) {
    if (!rec_on(cb)) {
        vkCmdSetVertexInputEXT(cb, nb, b, na, a);
        return;
    }
    u64 h = REC_AL(sizeof(VertexInputArgs)), bn = REC_AL(nb * sizeof *b);
    VertexInputArgs* x = rec_push(vertex_input_run, h + bn + na * sizeof *a);
    *x = (VertexInputArgs){cb, nb, na};
    if (nb) memcpy((u8*)x + h, b, nb * sizeof *b);
    if (na) memcpy((u8*)x + h + bn, a, na * sizeof *a);
    rec_commit();
}

/* transform feedback (VK_EXT_transform_feedback): the buffers with their offsets and sizes (bind), or the counter
   buffers begin resumes from and end writes (NULL: none), each an array of n after the arguments */
typedef struct {
    VkCommandBuffer cb;
    u32 first, n;
    int what; /* 0 bind, 1 begin, 2 end */
    int counters;
} XfbArgs;
static void xfb_run(const void* p) {
    const XfbArgs* x = p;
    const u8* q = (const u8*)p + REC_AL(sizeof *x);
    u64 bn = REC_AL(x->n * sizeof(VkBuffer)), dn = REC_AL(x->n * sizeof(VkDeviceSize));
    const VkBuffer* bufs = (const VkBuffer*)q;
    const VkDeviceSize* offs = (const void*)(q + bn);
    if (x->what == 0)
        vkCmdBindTransformFeedbackBuffersEXT(x->cb, x->first, x->n, bufs, offs, (const void*)(q + bn + dn));
    else if (x->what == 1)
        vkCmdBeginTransformFeedbackEXT(x->cb, x->first, x->n, x->counters ? bufs : NULL, x->counters ? offs : NULL);
    else
        vkCmdEndTransformFeedbackEXT(x->cb, x->first, x->n, x->counters ? bufs : NULL, x->counters ? offs : NULL);
}
static void xfb_rec(VkCommandBuffer cb, int what, u32 first, u32 n, const VkBuffer* bufs, const VkDeviceSize* offs,
                    const VkDeviceSize* sizes) {
    int counters = bufs != NULL;
    u64 h = REC_AL(sizeof(XfbArgs)), bn = REC_AL(n * sizeof(VkBuffer)), dn = REC_AL(n * sizeof(VkDeviceSize));
    XfbArgs* x;
    u8* q;
    if (!rec_on(cb)) {
        if (what == 0)
            vkCmdBindTransformFeedbackBuffersEXT(cb, first, n, bufs, offs, sizes);
        else if (what == 1)
            vkCmdBeginTransformFeedbackEXT(cb, first, n, bufs, offs);
        else
            vkCmdEndTransformFeedbackEXT(cb, first, n, bufs, offs);
        return;
    }
    x = rec_push(xfb_run, h + bn + dn * 2);
    q = (u8*)x + h;
    *x = (XfbArgs){cb, first, n, what, counters};
    if (counters) {
        memcpy(q, bufs, n * sizeof *bufs);
        memcpy(q + bn, offs, n * sizeof *offs);
    }
    if (sizes) memcpy(q + bn + dn, sizes, n * sizeof *sizes);
    rec_commit();
}
void rec_CmdBindTransformFeedbackBuffersEXT(VkCommandBuffer cb, u32 first, u32 n, const VkBuffer* bufs,
                                            const VkDeviceSize* offs, const VkDeviceSize* sizes) {
    xfb_rec(cb, 0, first, n, bufs, offs, sizes);
}
void rec_CmdBeginTransformFeedbackEXT(VkCommandBuffer cb, u32 first, u32 n, const VkBuffer* counters,
                                      const VkDeviceSize* offs) {
    xfb_rec(cb, 1, first, n, counters, offs, NULL);
}
void rec_CmdEndTransformFeedbackEXT(VkCommandBuffer cb, u32 first, u32 n, const VkBuffer* counters,
                                    const VkDeviceSize* offs) {
    xfb_rec(cb, 2, first, n, counters, offs, NULL);
}

typedef struct {
    VkCommandBuffer cb;
    u32 first, n;
} ViewportArgs;
static void viewport_run(const void* p) {
    const ViewportArgs* x = p;
    const void* a = (const u8*)p + REC_AL(sizeof *x);
    if (x->first == ~0u)
        vkCmdSetViewportWithCount(x->cb, x->n, a);
    else
        vkCmdSetViewport(x->cb, x->first, x->n, a);
}
static void scissor_run(const void* p) {
    const ViewportArgs* x = p;
    const void* a = (const u8*)p + REC_AL(sizeof *x);
    if (x->first == ~0u)
        vkCmdSetScissorWithCount(x->cb, x->n, a);
    else
        vkCmdSetScissor(x->cb, x->first, x->n, a);
}
static void rec_array(RecFn fn, VkCommandBuffer cb, u32 first, u32 n, const void* a, u64 size) {
    ViewportArgs* x = rec_push(fn, REC_AL(sizeof *x) + size);
    *x = (ViewportArgs){cb, first, n};
    memcpy((u8*)x + REC_AL(sizeof *x), a, size);
    rec_commit();
}
void rec_CmdSetViewportWithCount(VkCommandBuffer cb, u32 n, const VkViewport* v) {
    if (!rec_on(cb))
        vkCmdSetViewportWithCount(cb, n, v);
    else
        rec_array(viewport_run, cb, ~0u, n, v, n * sizeof *v);
}
void rec_CmdSetViewport(VkCommandBuffer cb, u32 first, u32 n, const VkViewport* v) {
    if (!rec_on(cb))
        vkCmdSetViewport(cb, first, n, v);
    else
        rec_array(viewport_run, cb, first, n, v, n * sizeof *v);
}
void rec_CmdSetScissorWithCount(VkCommandBuffer cb, u32 n, const VkRect2D* r) {
    if (!rec_on(cb))
        vkCmdSetScissorWithCount(cb, n, r);
    else
        rec_array(scissor_run, cb, ~0u, n, r, n * sizeof *r);
}
void rec_CmdSetScissor(VkCommandBuffer cb, u32 first, u32 n, const VkRect2D* r) {
    if (!rec_on(cb))
        vkCmdSetScissor(cb, first, n, r);
    else
        rec_array(scissor_run, cb, first, n, r, n * sizeof *r);
}

/* copies, blits and clears: two handles, two layouts, a count of regions and one more scalar */
typedef struct {
    VkCommandBuffer cb;
    u64 h1, h2; /* handles */
    VkImageLayout l1, l2;
    u32 n;
    u32 e; /* a filter */
    VkClearValue v; /* a clear value */
} XferArgs;
#define XFER_REGIONS(x) ((const void*)((const u8*)(x) + REC_AL(sizeof *(x))))
static void copy_buffer_run(const void* p) {
    const XferArgs* x = p;
    vkCmdCopyBuffer(x->cb, (VkBuffer)x->h1, (VkBuffer)x->h2, x->n, XFER_REGIONS(x));
}
static void copy_image_run(const void* p) {
    const XferArgs* x = p;
    vkCmdCopyImage(x->cb, (VkImage)x->h1, x->l1, (VkImage)x->h2, x->l2, x->n, XFER_REGIONS(x));
}
static void blit_run(const void* p) {
    const XferArgs* x = p;
    vkCmdBlitImage(x->cb, (VkImage)x->h1, x->l1, (VkImage)x->h2, x->l2, x->n, XFER_REGIONS(x), (VkFilter)x->e);
}
static void resolve_run(const void* p) {
    const XferArgs* x = p;
    vkCmdResolveImage(x->cb, (VkImage)x->h1, x->l1, (VkImage)x->h2, x->l2, x->n, XFER_REGIONS(x));
}
static void buffer_to_image_run(const void* p) {
    const XferArgs* x = p;
    vkCmdCopyBufferToImage(x->cb, (VkBuffer)x->h1, (VkImage)x->h2, x->l2, x->n, XFER_REGIONS(x));
}
static void image_to_buffer_run(const void* p) {
    const XferArgs* x = p;
    vkCmdCopyImageToBuffer(x->cb, (VkImage)x->h1, x->l1, (VkBuffer)x->h2, x->n, XFER_REGIONS(x));
}
static void clear_color_run(const void* p) {
    const XferArgs* x = p;
    vkCmdClearColorImage(x->cb, (VkImage)x->h1, x->l1, &x->v.color, x->n, XFER_REGIONS(x));
}
static void clear_depth_run(const void* p) {
    const XferArgs* x = p;
    vkCmdClearDepthStencilImage(x->cb, (VkImage)x->h1, x->l1, &x->v.depthStencil, x->n,
                                XFER_REGIONS(x));
}
u64 g_rec_xfers, g_rec_barriers; /* copies and blits, and barriers, put in the frame's ring (gpuprof.c's gaps) */
u64 g_rec_barriers_synced;       /* barriers with a pNext chain, recorded after the ring drained */
u64 g_rec_xfer_kind[3], g_rec_buf_bytes; /* buffer copies, buffer-image copies, image copies and blits; buffer bytes */
static void copy_buffer_run(const void* p);
static u64 copy_bytes(u32 n, const VkBufferCopy* r) {
    u64 b = 0;
    for (u32 i = 0; i < n; i++) b += r[i].size;
    return b;
}
static void buffer_to_image_run(const void* p);
static void image_to_buffer_run(const void* p);
static XferArgs* rec_xfer(RecFn fn, VkCommandBuffer cb, u64 h1, VkImageLayout l1, u64 h2, VkImageLayout l2, u32 n,
                          const void* regions, u64 size) {
    g_rec_xfers++;
    if (fn == copy_buffer_run) {
        g_rec_xfer_kind[0]++;
        for (u32 i = 0; i < n; i++) g_rec_buf_bytes += ((const VkBufferCopy*)regions)[i].size;
    } else
        g_rec_xfer_kind[fn == buffer_to_image_run || fn == image_to_buffer_run ? 1 : 2]++;
    XferArgs* x = rec_push(fn, REC_AL(sizeof *x) + size);
    memset(x, 0, sizeof *x);
    x->cb = cb;
    x->h1 = h1;
    x->h2 = h2;
    x->l1 = l1;
    x->l2 = l2;
    x->n = n;
    memcpy((u8*)x + REC_AL(sizeof *x), regions, size);
    return x;
}
static void rec_CmdCopyBuffer_raw(VkCommandBuffer cb, VkBuffer src, VkBuffer dst, u32 n, const VkBufferCopy* r) {
    if (!rec_on(cb)) {
        vkCmdCopyBuffer(cb, src, dst, n, r);
        return;
    }
    rec_xfer(copy_buffer_run, cb, (u64)src, 0, (u64)dst, 0, n, r, n * sizeof *r);
    rec_commit();
}
void rec_CmdCopyBuffer(VkCommandBuffer cb, VkBuffer src, VkBuffer dst, u32 n, const VkBufferCopy* r) {
    /* timed for vk_gpu_profile by where it was recorded */
    int timed = vk_gpuprof_profiled(cb) &&
                vk_gpuprof_op_begin(cb, VK_OP_COPY,
                                    (u64)((const u8*)__builtin_return_address(0) - (const u8*)vk_gpuprof_base()),
                                    copy_bytes(n, r));
    rec_CmdCopyBuffer_raw(cb, src, dst, n, r);
    if (timed) vk_gpuprof_op_end(cb);
}
static void rec_CmdCopyImage_raw(VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkImage dst, VkImageLayout dl,
                                 u32 n, const VkImageCopy* r) {
    if (!rec_on(cb)) {
        vkCmdCopyImage(cb, src, sl, dst, dl, n, r);
        return;
    }
    rec_xfer(copy_image_run, cb, (u64)src, sl, (u64)dst, dl, n, r, n * sizeof *r);
    rec_commit();
}
void rec_CmdCopyImage(VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkImage dst, VkImageLayout dl, u32 n,
                      const VkImageCopy* r) {
    /* timed for vk_gpu_profile by where it was recorded */
    int timed = vk_gpuprof_profiled(cb) &&
                vk_gpuprof_op_begin(cb, VK_OP_IMAGE,
                                    (u64)((const u8*)__builtin_return_address(0) - (const u8*)vk_gpuprof_base()), 0);
    rec_CmdCopyImage_raw(cb, src, sl, dst, dl, n, r);
    if (timed) vk_gpuprof_op_end(cb);
}
/* a transfer of kind (VK_OP_*) timed for vk_gpu_profile by where it was recorded, site its caller's return address: 1
   when it is, for xfer_end */
static int xfer_begin(VkCommandBuffer cb, u32 kind, const void* site, u64 bytes) {
    return vk_gpuprof_profiled(cb) &&
           vk_gpuprof_op_begin(cb, kind, (u64)((const u8*)site - (const u8*)vk_gpuprof_base()), bytes);
}
static void xfer_end(VkCommandBuffer cb, int timed) {
    if (timed) vk_gpuprof_op_end(cb);
}
void rec_CmdBlitImage(VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkImage dst, VkImageLayout dl, u32 n,
                      const VkImageBlit* r, VkFilter filter) {
    int timed = xfer_begin(cb, VK_OP_BLIT, __builtin_return_address(0), 0);
    if (!rec_on(cb))
        vkCmdBlitImage(cb, src, sl, dst, dl, n, r, filter);
    else {
        rec_xfer(blit_run, cb, (u64)src, sl, (u64)dst, dl, n, r, n * sizeof *r)->e = filter;
        rec_commit();
    }
    xfer_end(cb, timed);
}
void rec_CmdResolveImage(VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkImage dst, VkImageLayout dl, u32 n,
                         const VkImageResolve* r) {
    int timed = xfer_begin(cb, VK_OP_RESOLVE, __builtin_return_address(0), 0);
    if (!rec_on(cb))
        vkCmdResolveImage(cb, src, sl, dst, dl, n, r);
    else {
        rec_xfer(resolve_run, cb, (u64)src, sl, (u64)dst, dl, n, r, n * sizeof *r);
        rec_commit();
    }
    xfer_end(cb, timed);
}
static void rec_CmdCopyBufferToImage_raw(VkCommandBuffer cb, VkBuffer src, VkImage dst, VkImageLayout dl, u32 n,
                                         const VkBufferImageCopy* r) {
    if (!rec_on(cb)) {
        vkCmdCopyBufferToImage(cb, src, dst, dl, n, r);
        return;
    }
    rec_xfer(buffer_to_image_run, cb, (u64)src, 0, (u64)dst, dl, n, r, n * sizeof *r);
    rec_commit();
}
void rec_CmdCopyBufferToImage(VkCommandBuffer cb, VkBuffer src, VkImage dst, VkImageLayout dl, u32 n,
                              const VkBufferImageCopy* r) {
    /* timed for vk_gpu_profile by where it was recorded */
    int timed = vk_gpuprof_profiled(cb) &&
                vk_gpuprof_op_begin(cb, VK_OP_TO_IMAGE,
                                    (u64)((const u8*)__builtin_return_address(0) - (const u8*)vk_gpuprof_base()), 0);
    rec_CmdCopyBufferToImage_raw(cb, src, dst, dl, n, r);
    if (timed) vk_gpuprof_op_end(cb);
}
static void rec_CmdCopyImageToBuffer_raw(VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkBuffer dst, u32 n,
                                         const VkBufferImageCopy* r) {
    if (!rec_on(cb)) {
        vkCmdCopyImageToBuffer(cb, src, sl, dst, n, r);
        return;
    }
    rec_xfer(image_to_buffer_run, cb, (u64)src, sl, (u64)dst, 0, n, r, n * sizeof *r);
    rec_commit();
}
void rec_CmdCopyImageToBuffer(VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkBuffer dst, u32 n,
                              const VkBufferImageCopy* r) {
    /* timed for vk_gpu_profile by where it was recorded */
    int timed = vk_gpuprof_profiled(cb) &&
                vk_gpuprof_op_begin(cb, VK_OP_TO_BUFFER,
                                    (u64)((const u8*)__builtin_return_address(0) - (const u8*)vk_gpuprof_base()), 0);
    rec_CmdCopyImageToBuffer_raw(cb, src, sl, dst, n, r);
    if (timed) vk_gpuprof_op_end(cb);
}
void rec_CmdClearColorImage(VkCommandBuffer cb, VkImage img, VkImageLayout l, const VkClearColorValue* c, u32 n,
                            const VkImageSubresourceRange* r) {
    int timed = xfer_begin(cb, VK_OP_CLEAR, __builtin_return_address(0), 0);
    if (!rec_on(cb))
        vkCmdClearColorImage(cb, img, l, c, n, r);
    else {
        rec_xfer(clear_color_run, cb, (u64)img, l, 0, 0, n, r, n * sizeof *r)->v.color = *c;
        rec_commit();
    }
    xfer_end(cb, timed);
}
void rec_CmdClearDepthStencilImage(VkCommandBuffer cb, VkImage img, VkImageLayout l, const VkClearDepthStencilValue* c,
                                   u32 n, const VkImageSubresourceRange* r) {
    int timed = xfer_begin(cb, VK_OP_CLEAR, __builtin_return_address(0), 0);
    if (!rec_on(cb))
        vkCmdClearDepthStencilImage(cb, img, l, c, n, r);
    else {
        rec_xfer(clear_depth_run, cb, (u64)img, l, 0, 0, n, r, n * sizeof *r)->v.depthStencil = *c;
        rec_commit();
    }
    xfer_end(cb, timed);
}
typedef struct {
    VkCommandBuffer cb;
    VkBuffer buf;
    VkDeviceSize off, size;
    u32 data;
} FillArgs;
static void fill_run(const void* p) {
    const FillArgs* x = p;
    vkCmdFillBuffer(x->cb, x->buf, x->off, x->size, x->data);
}
void rec_CmdFillBuffer(VkCommandBuffer cb, VkBuffer buf, VkDeviceSize off, VkDeviceSize size, u32 data) {
    /* its bytes where they are known: VK_WHOLE_SIZE counts none */
    int timed = xfer_begin(cb, VK_OP_FILL, __builtin_return_address(0), size == VK_WHOLE_SIZE ? 0 : size);
    if (!rec_on(cb))
        vkCmdFillBuffer(cb, buf, off, size, data);
    else {
        FillArgs* x = rec_push(fill_run, sizeof *x);
        *x = (FillArgs){cb, buf, off, size, data};
        rec_commit();
    }
    xfer_end(cb, timed);
}

typedef struct {
    VkCommandBuffer cb;
    u32 na, nr;
} ClearArgs;
static void clear_att_run(const void* p) {
    const ClearArgs* x = p;
    const u8* a = (const u8*)p + REC_AL(sizeof *x);
    vkCmdClearAttachments(x->cb, x->na, (const void*)a, x->nr,
                          (const void*)(a + REC_AL(x->na * sizeof(VkClearAttachment))));
}
void rec_CmdClearAttachments(VkCommandBuffer cb, u32 na, const VkClearAttachment* a, u32 nr, const VkClearRect* r) {
    if (!rec_on(cb)) {
        vkCmdClearAttachments(cb, na, a, nr, r);
        return;
    }
    u64 h = REC_AL(sizeof(ClearArgs)), an = REC_AL(na * sizeof *a);
    ClearArgs* x = rec_push(clear_att_run, h + an + nr * sizeof *r);
    *x = (ClearArgs){cb, na, nr};
    memcpy((u8*)x + h, a, na * sizeof *a);
    memcpy((u8*)x + h + an, r, nr * sizeof *r);
    rec_commit();
}

/* the barriers' structures have no extensions chained (pNext): a dependency or pass with one is recorded at once,
   after the ring drains */
typedef struct {
    VkCommandBuffer cb;
    VkDependencyFlags flags;
    u32 nm, nb, ni;
} BarrierArgs;
static void barrier_run(const void* p) {
    const BarrierArgs* x = p;
    const u8* q = (const u8*)p + REC_AL(sizeof *x);
    u64 mn = REC_AL(x->nm * sizeof(VkMemoryBarrier2)), bn = REC_AL(x->nb * sizeof(VkBufferMemoryBarrier2));
    VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                          .dependencyFlags = x->flags,
                          .memoryBarrierCount = x->nm,
                          .pMemoryBarriers = (const void*)q,
                          .bufferMemoryBarrierCount = x->nb,
                          .pBufferMemoryBarriers = (const void*)(q + mn),
                          .imageMemoryBarrierCount = x->ni,
                          .pImageMemoryBarriers = (const void*)(q + mn + bn)};
    vkCmdPipelineBarrier2(x->cb, &d);
}
static int barrier_chained(const VkDependencyInfo* d) {
    if (d->pNext) return 1;
    for (u32 i = 0; i < d->memoryBarrierCount; i++)
        if (d->pMemoryBarriers[i].pNext) return 1;
    for (u32 i = 0; i < d->bufferMemoryBarrierCount; i++)
        if (d->pBufferMemoryBarriers[i].pNext) return 1;
    for (u32 i = 0; i < d->imageMemoryBarrierCount; i++)
        if (d->pImageMemoryBarriers[i].pNext) return 1;
    return 0;
}
/* Consecutive barriers merge into one vkCmdPipelineBarrier2: the driver call costs the same for one barrier as for
   many, and one game's passes cluster barriers (gpuprof.c's "gaps with barriers only"). Two barriers in one call are
   not ordered against each other, so a barrier stays apart from the batch where the order matters:
   - two image barriers on one image: a layout transition's old layout would be wrong.
   - a barrier whose first scope takes stages that the batch's second scope has (dst). In two calls the second
     barrier's dependency chains after the first one's: a copy, a barrier from the copy to compute, then a barrier
     from compute to the fragment stage orders the copy before the fragment stage too. One call loses that chain. */
#define BAR_BATCH_CAP 64
static struct {
    VkCommandBuffer cb;
    VkDependencyFlags flags;
    u32 nm, nb, ni;
    VkMemoryBarrier2 mem[BAR_BATCH_CAP];
    VkBufferMemoryBarrier2 buf[BAR_BATCH_CAP];
    VkImageMemoryBarrier2 img[BAR_BATCH_CAP];
    VkImage images[BAR_BATCH_CAP]; /* the image of img[i], to keep two barriers on one image apart */
    VkPipelineStageFlags2 dst;     /* the stages of the batch's second scope, all its barriers' */
} g_bar_batch;
u64 g_barrier_merged; /* barriers that shared a call with an earlier one (vk_rec_command_report) */
static void barrier_flush(void) {
    if (!g_bar_batch.nm && !g_bar_batch.nb && !g_bar_batch.ni) return;
    u32 nm = g_bar_batch.nm, nb = g_bar_batch.nb, ni = g_bar_batch.ni;
    g_bar_batch.nm = g_bar_batch.nb = g_bar_batch.ni = 0;
    g_bar_batch.dst = 0;
    u64 h = REC_AL(sizeof(BarrierArgs)), mn = REC_AL(nm * sizeof(VkMemoryBarrier2)),
        bn = REC_AL(nb * sizeof(VkBufferMemoryBarrier2));
    BarrierArgs* x = rec_push(barrier_run, h + mn + bn + ni * sizeof(VkImageMemoryBarrier2));
    u8* q = (u8*)x + h;
    *x = (BarrierArgs){g_bar_batch.cb, g_bar_batch.flags, nm, nb, ni};
    memcpy(q, g_bar_batch.mem, nm * sizeof(VkMemoryBarrier2));
    memcpy(q + mn, g_bar_batch.buf, nb * sizeof(VkBufferMemoryBarrier2));
    memcpy(q + mn + bn, g_bar_batch.img, ni * sizeof(VkImageMemoryBarrier2));
    rec_commit();
}
/* whether a scope's stages src take any of dst's: ALL_COMMANDS and ALL_GRAPHICS take every stage (for this test) */
static int stages_meet(VkPipelineStageFlags2 src, VkPipelineStageFlags2 dst) {
    const VkPipelineStageFlags2 all = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT;
    if (!src || !dst) return 0;
    return (src & all) || (dst & all) || (src & dst);
}
/* the first scope's stages of d's barriers, and the second scope's */
static VkPipelineStageFlags2 barrier_stages(const VkDependencyInfo* d, int second) {
    VkPipelineStageFlags2 st = 0;
    for (u32 i = 0; i < d->memoryBarrierCount; i++)
        st |= second ? d->pMemoryBarriers[i].dstStageMask : d->pMemoryBarriers[i].srcStageMask;
    for (u32 i = 0; i < d->bufferMemoryBarrierCount; i++)
        st |= second ? d->pBufferMemoryBarriers[i].dstStageMask : d->pBufferMemoryBarriers[i].srcStageMask;
    for (u32 i = 0; i < d->imageMemoryBarrierCount; i++)
        st |= second ? d->pImageMemoryBarriers[i].dstStageMask : d->pImageMemoryBarriers[i].srcStageMask;
    return st;
}
void rec_CmdPipelineBarrier2(VkCommandBuffer cb, const VkDependencyInfo* d) {
    if (rec_on(cb)) g_rec_barriers++;
    if (!rec_on(cb) || barrier_chained(d)) {
        if (rec_on(cb)) {
            g_rec_barriers_synced++;
            barrier_flush(); /* what the drain waits for goes first */
            vk_rec_sync();
        }
        vkCmdPipelineBarrier2(cb, d);
        return;
    }
    int apart = g_bar_batch.cb != cb || g_bar_batch.flags != d->dependencyFlags ||
                g_bar_batch.nm + d->memoryBarrierCount > BAR_BATCH_CAP ||
                g_bar_batch.nb + d->bufferMemoryBarrierCount > BAR_BATCH_CAP ||
                g_bar_batch.ni + d->imageMemoryBarrierCount > BAR_BATCH_CAP ||
                stages_meet(barrier_stages(d, 0), g_bar_batch.dst);
    for (u32 i = 0; i < d->imageMemoryBarrierCount && !apart; i++)
        for (u32 j = 0; j < g_bar_batch.ni && !apart; j++)
            if (d->pImageMemoryBarriers[i].image == g_bar_batch.images[j]) apart = 1;
    if (apart) barrier_flush();
    else if (g_bar_batch.nm || g_bar_batch.nb || g_bar_batch.ni) g_barrier_merged++;
    g_bar_batch.cb = cb;
    g_bar_batch.flags = d->dependencyFlags;
    memcpy(g_bar_batch.mem + g_bar_batch.nm, d->pMemoryBarriers, d->memoryBarrierCount * sizeof(VkMemoryBarrier2));
    memcpy(g_bar_batch.buf + g_bar_batch.nb, d->pBufferMemoryBarriers,
           d->bufferMemoryBarrierCount * sizeof(VkBufferMemoryBarrier2));
    for (u32 i = 0; i < d->imageMemoryBarrierCount; i++) {
        g_bar_batch.img[g_bar_batch.ni + i] = d->pImageMemoryBarriers[i];
        g_bar_batch.images[g_bar_batch.ni + i] = d->pImageMemoryBarriers[i].image;
    }
    g_bar_batch.nm += d->memoryBarrierCount;
    g_bar_batch.nb += d->bufferMemoryBarrierCount;
    g_bar_batch.ni += d->imageMemoryBarrierCount;
    g_bar_batch.dst |= barrier_stages(d, 1);
}

typedef struct {
    VkCommandBuffer cb;
    VkRenderingFlags flags;
    VkRect2D area;
    u32 layers, view_mask, n;
    int depth, stencil;
    /* an Ounce's shading rate image as the pass's attachment (shading_rate.c), VK_NULL_HANDLE for none */
    VkImageView rate;
    VkExtent2D rate_texel;
} RenderArgs;
static void rendering_run(const void* p) {
    const RenderArgs* x = p;
    const VkRenderingAttachmentInfo* a = (const void*)((const u8*)p + REC_AL(sizeof *x));
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                          .flags = x->flags,
                          .renderArea = x->area,
                          .layerCount = x->layers,
                          .viewMask = x->view_mask,
                          .colorAttachmentCount = x->n,
                          .pColorAttachments = a,
                          .pDepthAttachment = x->depth ? &a[x->n] : NULL,
                          .pStencilAttachment = x->stencil ? &a[x->n + 1] : NULL};
    VkRenderingFragmentShadingRateAttachmentInfoKHR rate = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_FRAGMENT_SHADING_RATE_ATTACHMENT_INFO_KHR,
        .imageView = x->rate,
        .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        .shadingRateAttachmentTexelSize = x->rate_texel};
    if (x->rate) ri.pNext = &rate;
    vkCmdBeginRendering(x->cb, &ri);
}
void rec_CmdBeginRendering(VkCommandBuffer cb, const VkRenderingInfo* ri) {
    vk_ck(cb, VK_CK_PASS, ri->renderArea.extent.width, ri->renderArea.extent.height, ri->colorAttachmentCount, 0);
    /* a shading rate attachment alone goes in the packet */
    const VkRenderingFragmentShadingRateAttachmentInfoKHR* rate = ri->pNext;
    if (rate && (rate->sType != VK_STRUCTURE_TYPE_RENDERING_FRAGMENT_SHADING_RATE_ATTACHMENT_INFO_KHR || rate->pNext ||
                 rate->imageLayout != VK_IMAGE_LAYOUT_GENERAL))
        rate = NULL;
    int chained = (ri->pNext && !rate) || (ri->pDepthAttachment && ri->pDepthAttachment->pNext) ||
                  (ri->pStencilAttachment && ri->pStencilAttachment->pNext);
    for (u32 i = 0; i < ri->colorAttachmentCount && !chained; i++) chained = ri->pColorAttachments[i].pNext != NULL;
    if (!rec_on(cb) || chained) {
        if (rec_on(cb)) vk_rec_sync();
        vkCmdBeginRendering(cb, ri);
        return;
    }
    u32 n = ri->colorAttachmentCount;
    RenderArgs* x = rec_push(rendering_run, REC_AL(sizeof *x) + (n + 2) * sizeof(VkRenderingAttachmentInfo));
    VkRenderingAttachmentInfo* a = (void*)((u8*)x + REC_AL(sizeof *x));
    *x = (RenderArgs){cb,
                      ri->flags,
                      ri->renderArea,
                      ri->layerCount,
                      ri->viewMask,
                      n,
                      !!ri->pDepthAttachment,
                      !!ri->pStencilAttachment,
                      rate ? rate->imageView : VK_NULL_HANDLE,
                      rate ? rate->shadingRateAttachmentTexelSize : (VkExtent2D){0, 0}};
    memcpy(a, ri->pColorAttachments, n * sizeof *a);
    if (ri->pDepthAttachment) a[n] = *ri->pDepthAttachment;
    if (ri->pStencilAttachment) a[n + 1] = *ri->pStencilAttachment;
    rec_commit();
}

/* ------------------------------------------------------------ descriptor sets */
/* A draw's new descriptor set is allocated and written on the recording thread too, ahead of the commands that bind
   it: the GPU thread gets a stand-in handle at once (REC_STANDIN in the top bits, which no handle has: a driver's is a
   pointer or a small offset, and the validation layer's are counters), and the recording thread keeps which set it
   stands for until the commands binding it are recorded. The sets come from the recording thread's own pools for
   each frame slot, reset in order with the commands when the slot is reused (vk_rec_frame_reset). Stand-ins count up
   and are reused after REC_SETS, more sets than two frames ever make before vk_frame_check ends one */
#define REC_SETS        (1u << 17)
/* not merely odd: half of the validation layer's handles are odd, and a real set taken for a stand-in went unbound,
   its dispatch reading no descriptors until the GPU hung */
#define REC_STANDIN     0x5EC5000000000000ull
#define REC_SPARE_SETS  32
#define REC_SPARE_KINDS 16
static VkDescriptorSet g_rset[REC_SETS];
static u32 g_rset_next;
typedef struct {
    VkDescriptorPool pool[64];
    int npool, ipool;
    int took, peak; /* the pools this use allocated from, the most of this window's uses (vk_desc_pools_keep) */
    u32 uses;
    struct {
        VkDescriptorSetLayout layout;
        VkDescriptorSet set[REC_SPARE_SETS];
        u32 n;
    } spare[REC_SPARE_KINDS];
} RecPools;
static RecPools g_rpools[VK_FRAMES];
static u32 g_frame_sets[VK_FRAMES]; /* stand-ins handed out in each slot since its reset (the GPU thread's count) */

static int rec_set_standin(VkDescriptorSet s) { return ((u64)(uintptr_t)s & ~(u64)(REC_SETS - 1)) == REC_STANDIN; }
static VkDescriptorSet rec_set_real(VkDescriptorSet s) {
    return rec_set_standin(s) ? g_rset[(u64)(uintptr_t)s & (REC_SETS - 1)] : s;
}
/* count sets of layout from slot f's pools into sets: count, or 1 when a fresh pool can't hold that many, 0 when none */
static u32 rec_alloc_n(RecPools* f, VkDescriptorSetLayout layout, VkDescriptorSet* sets, u32 count) {
    VkDescriptorSetLayout layouts[REC_SPARE_SETS];
    for (u32 i = 0; i < count; i++) layouts[i] = layout;
    for (int tries = 0; tries < 3; tries++) {
        if (tries) f->ipool++;
        if (tries == 2) count = 1;
        if (f->ipool == f->npool) {
            if (f->npool == (int)(sizeof f->pool / sizeof f->pool[0])) return 0;
            if (!(f->pool[f->npool] = vk_desc_pool_new(VK_POOL_ARRAY_SETS_FOR(layout)))) return 0;
            f->npool++;
        }
        VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                          .descriptorPool = f->pool[f->ipool],
                                          .descriptorSetCount = count,
                                          .pSetLayouts = layouts};
        VkResult r = vkAllocateDescriptorSets(g_vk.dev, &ai, sets);
        if (r == VK_SUCCESS) {
            vk_desc_taken(layout, count);
            if (f->ipool + 1 > f->took) f->took = f->ipool + 1;
            return count;
        }
        if (r != VK_ERROR_OUT_OF_POOL_MEMORY && r != VK_ERROR_FRAGMENTED_POOL) {
            VK_OK(r);
            return 0;
        }
    }
    return 0;
}
static VkDescriptorSet rec_alloc(RecPools* f, VkDescriptorSetLayout layout) {
    VkDescriptorSet set = VK_NULL_HANDLE;
    int i = 0;
    while (i < REC_SPARE_KINDS && f->spare[i].layout && f->spare[i].layout != layout) i++;
    if (i == REC_SPARE_KINDS) return rec_alloc_n(f, layout, &set, 1) ? set : VK_NULL_HANDLE;
    f->spare[i].layout = layout;
    if (!f->spare[i].n) f->spare[i].n = rec_alloc_n(f, layout, f->spare[i].set, REC_SPARE_SETS);
    return f->spare[i].n ? f->spare[i].set[--f->spare[i].n] : VK_NULL_HANDLE;
}
static void frame_reset_run(const void* p) {
    RecPools* f = &g_rpools[*(const u32*)p];
    desc_batch_flush(); /* writes that name a set in the pools about to be reset go first */
    vk_desc_pools_keep(f->pool, &f->npool, &f->peak, &f->uses, f->took);
    for (int i = 0; i < f->npool; i++) vkResetDescriptorPool(g_vk.dev, f->pool[i], 0);
    f->ipool = 0;
    f->took = 0;
    memset(f->spare, 0, sizeof f->spare);
}
void vk_rec_frame_reset(u32 slot) {
    g_frame_sets[slot] = 0;
    if (g_rec <= 0) return;
    *(u32*)rec_push(frame_reset_run, sizeof slot) = slot;
    rec_commit();
}
u32 vk_rec_frame_sets(u32 slot) { return g_frame_sets[slot]; }

static u64 write_info_size(const VkWriteDescriptorSet* w) {
    switch (w->descriptorType) {
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC: return w->descriptorCount * sizeof(VkDescriptorBufferInfo);
        case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
        case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER: return w->descriptorCount * sizeof(VkBufferView);
        default: return w->descriptorCount * sizeof(VkDescriptorImageInfo);
    }
}
static const void* write_info(const VkWriteDescriptorSet* w) {
    switch (w->descriptorType) {
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC: return w->pBufferInfo;
        case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
        case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER: return w->pTexelBufferView;
        default: return w->pImageInfo;
    }
}
/* The recording thread writes a set with one vkUpdateDescriptorSets for many sets, not one a set (one game makes
   ~1100 a frame). A made set's writes wait here until the recording thread has to apply them: before it binds a set,
   before it resets a frame's pools (the sets the writes name have to still be whole), and before it ends a command
   buffer. The writes and their buffer/image/view descriptors are copied out of the ring packet, whose memory the
   recording thread passes and the producer then reuses. Only the recording thread touches this, so it needs no lock */
#define DESC_BATCH_WRITES 1024
#define DESC_BATCH_BYTES  (256ull << 10)
static struct {
    VkWriteDescriptorSet w[DESC_BATCH_WRITES];
    u8 info[DESC_BATCH_BYTES];
    u32 n;      /* writes in w */
    u64 ibytes; /* info bytes used */
} g_desc_batch;
_Atomic u64 g_desc_merged; /* writes that shared a vkUpdateDescriptorSets with an earlier one (vk_rec_command_report) */
static void desc_batch_flush(void) {
    if (!g_desc_batch.n) return;
    u32 n = g_desc_batch.n;
    atomic_fetch_add_explicit(&g_desc_merged, n - 1, memory_order_relaxed);
    g_desc_batch.n = 0;
    g_desc_batch.ibytes = 0;
    vkUpdateDescriptorSets(g_vk.dev, n, g_desc_batch.w, 0, NULL);
}
typedef struct {
    u32 n, slot, id;
    VkDescriptorSetLayout layout;
} NewSetArgs;
static void new_set_run(const void* p) {
    const NewSetArgs* x = p;
    /* the packet is the recording thread's now: the copied writes are pointed at their copied descriptors in place */
    VkWriteDescriptorSet* w = (VkWriteDescriptorSet*)((u8*)p + REC_AL(sizeof *x));
    const u8* pinfo = (const u8*)w + REC_AL(x->n * sizeof *w); /* the descriptors vk_rec_new_set copied into the packet */
    VkDescriptorSet set = rec_alloc(&g_rpools[x->slot], x->layout);
    g_rset[x->id] = set;
    if (!set) {
        LOG_LIMIT(1, "vulkan: a frame used all its descriptor pools (draws are drawn without their resources)\n");
        return;
    }
    for (u32 i = 0; i < x->n; i++) {
        u64 isize = write_info_size(&w[i]);
        if (g_desc_batch.n == DESC_BATCH_WRITES || g_desc_batch.ibytes + REC_AL(isize) > DESC_BATCH_BYTES)
            desc_batch_flush();
        VkWriteDescriptorSet* dw = &g_desc_batch.w[g_desc_batch.n++];
        u8* di = g_desc_batch.info + g_desc_batch.ibytes;
        *dw = w[i];
        dw->dstSet = set;
        memcpy(di, pinfo, isize); /* the packet's copy, not the caller's memory, which its next frame reuses */
        g_desc_batch.ibytes += REC_AL(isize);
        dw->pBufferInfo = (const void*)di;
        dw->pImageInfo = (const void*)di;
        dw->pTexelBufferView = (const void*)di;
        pinfo += REC_AL(isize);
    }
}
VkDescriptorSet vk_rec_new_set(VkDescriptorSetLayout layout, u32 n, const VkWriteDescriptorSet* w) {
    u64 size = REC_AL(sizeof(NewSetArgs)) + REC_AL(n * sizeof *w);
    int chained = 0;
    /* a slot's pools are reset when its frame starts, in the first vk_cmd (frame_recycle): before the frame's first
       set is made from them, not after it, which freed the set its draw then bound */
    vk_cmd();
    u32 slot = vk_frame_slot();
    for (u32 i = 0; i < n; i++) {
        size += REC_AL(write_info_size(&w[i]));
        chained |= w[i].pNext != NULL;
    }
    if (g_rec <= 0 || chained) { /* allocated and written here, from the frame's own pools */
        VkDescriptorSet set = vk_desc_alloc(layout);
        VkWriteDescriptorSet c[SNX_VKB_END];
        if (!set || n > SNX_VKB_END) return VK_NULL_HANDLE;
        memcpy(c, w, n * sizeof *w);
        for (u32 i = 0; i < n; i++) c[i].dstSet = set;
        vk_rec_sync();
        if (n) vkUpdateDescriptorSets(g_vk.dev, n, c, 0, NULL);
        return set;
    }
    NewSetArgs* x = rec_push(new_set_run, size);
    VkWriteDescriptorSet* c = (VkWriteDescriptorSet*)((u8*)x + REC_AL(sizeof *x));
    u8* info = (u8*)c + REC_AL(n * sizeof *c);
    u32 id = g_rset_next++ & (REC_SETS - 1);
    *x = (NewSetArgs){n, slot, id, layout};
    memcpy(c, w, n * sizeof *w);
    for (u32 i = 0; i < n; i++) {
        u64 s = write_info_size(&w[i]);
        memcpy(info, write_info(&w[i]), s);
        info += REC_AL(s);
    }
    g_frame_sets[slot]++;
    rec_commit();
    return (VkDescriptorSet)(uintptr_t)(REC_STANDIN | id);
}

/* the command kinds the recording thread drained since the last report, by name, most first: where its time (the ring
   dwell before a submit) went. Logged by gpuprof.c's report, which then resets the counters */
void vk_rec_command_report(double presents, int count) {
    static const struct {
        RecFn fn;
        const char* name;
    } names[] = {
        {draw_run, "draws"},       {draw_indexed_run, "indexed draws"}, {draw_mesh_run, "mesh draws"},
        {draw_indirect_run, "indirect draws"}, {dispatch_run, "dispatches"},
        {dispatch_indirect_run, "indirect dispatches"}, {sets_run, "descriptor set binds"},
        {new_set_run, "descriptor sets made"}, {push_run, "push constants"}, {vb_run, "vertex buffer binds"},
        {vertex_input_run, "vertex input binds"}, {bind_pipeline_run, "pipeline binds"}, {barrier_run, "barriers"},
        {rendering_run, "render passes begun"}, {viewport_run, "viewports"}, {scissor_run, "scissors"},
        {copy_buffer_run, "buffer copies"}, {copy_image_run, "image copies"}, {blit_run, "blits"},
        {resolve_run, "resolves"}, {buffer_to_image_run, "buffer-to-image copies"},
        {image_to_buffer_run, "image-to-buffer copies"}, {clear_color_run, "color clears"},
        {clear_depth_run, "depth clears"}, {fill_run, "fills"}, {clear_att_run, "attachment clears"},
        {cond_run, "conditional renders"}, {copy_indirect_run, "indirect copies"}, {xfb_run, "transform feedback"},
        {heap_bind_run, "heap binds"}, {heap_push_run, "heap pushes"}, {descriptor_buffer_run, "descriptor buffers"},
        {descriptor_offsets_run, "descriptor offsets"}, {blend_run, "blend constants"},
        {clamp_range_run, "clamp ranges"}, {ck_run, "checkpoints"}, {frame_reset_run, "frame resets"},
        {begin_run, "command buffers begun"}, {rec_call_run, "calls"},
    };
    g_rec_kind_count = count;
    if (!g_rec_kind_total) return;
    char line[512];
    int at = 0;
    for (u64 i = 0; i < REC_KINDS; i++)
        for (size_t j = 0; j < sizeof names / sizeof *names; j++)
            if (g_rec_kind[i].fn == names[j].fn)
                at += snprintf(line + at, sizeof line - (size_t)at, "%s %s %.0f",
                               at ? "," : "", names[j].name, (double)g_rec_kind[i].n / (presents ? presents : 1.0));
    LOG("vulkan: recording thread commands a present:%s\n", line);
    LOG("vulkan: recording thread merged %.0f barriers into an earlier one's call a present\n",
        (double)g_barrier_merged / (presents ? presents : 1.0));
    LOG("vulkan: recording thread batched %.0f descriptor writes into an earlier one's call a present\n",
        (double)atomic_exchange_explicit(&g_desc_merged, 0, memory_order_relaxed) / (presents ? presents : 1.0));
    memset(g_rec_kind, 0, sizeof g_rec_kind);
    g_rec_kind_total = 0;
    g_barrier_merged = 0;
}
