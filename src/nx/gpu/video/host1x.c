/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from Eden (src/core/hle/service/nvdrv/devices/nvhost_nvdec_common.cpp, nvhost_nvdec_common.h,
   nvhost_nvdec.cpp, nvhost_vic.cpp, src/video_core/cdma_pusher.cpp, cdma_pusher.h, src/video_core/host1x/control.cpp,
   control.h):
   SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
   SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
   SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
   SPDX-FileCopyrightText: 2021 yuzu Emulator Project
   SPDX-FileCopyrightText: 2021 Skyline Team and Contributors
   SPDX-FileCopyrightText: Ryujinx Team and Contributors (cdma_pusher.cpp, MIT) */
/* host1x: the channels of /dev/nvhost-nvdec and /dev/nvhost-vic. nvdrv.c opens a channel per fd and passes it the
   fd's ioctls (Eden's nvhost_nvdec_common); this file keeps the IOVA space the engines address guest memory through,
   and a worker per channel that runs the submitted command streams in order (Eden's CDmaPusher), handing each
   Execute to nvdec.c or vic.c.

   A SUBMIT returns at once with the fences its syncpoint increments will reach, as on the console, and the job runs
   later on the channel's worker: decoding inside the ioctl held the game's submitting thread, and whatever it had
   locked, for a whole picture. Every syncpoint comes from nvdrv's syncpt_alloc, which keeps the ids below the 128
   NVIDIA's stream library sizes its tables at.

   Locks, taken in this order: a channel's lk or the engines' lock, then the IOVA table's, then nvdrv's. A job's
   command buffers run under the engines' lock (the decoders and VIC are not reentrant); a host class wait lets go
   of it, since the job waited for may be another channel's */
#include "nx/gpu/video/host1x_int.h"
#include "nx/gpu/video/host1x_regs.h"
#include "nx/gpu/video/nvdec_regs.h"
#include "nx/gpu/video/vic_regs.h"
#include "nx/runtime/platform/tracy.h"
#include "nx/runtime/state_io.h"

/* NvResult, the codes these ioctls answer with */
enum { NV_OK = 0, NV_BAD_PARAMETER = 4, NV_INSUFFICIENT_MEMORY = 6 };

/* The ioctls (Eden's nvhost_nvdec::Ioctl1 and nvhost_vic::Ioctl1): the group in bits 8-15, the number in 0-7 */
static u32 ioctl_group(u32 cmd) { return (cmd >> 8) & 0xFFu; }
static u32 ioctl_number(u32 cmd) { return cmd & 0xFFu; }
enum { IOCTL_GROUP_HOST1X = 0x00, IOCTL_GROUP_NVMAP = 0x48 /* 'H' */ };
enum {
    IOCTL_SUBMIT = 0x01,
    IOCTL_GET_SYNCPOINT = 0x02,
    IOCTL_GET_WAITBASE = 0x03,
    IOCTL_SET_SUBMIT_TIMEOUT = 0x07,
    IOCTL_MAP_BUFFER = 0x09,
    IOCTL_UNMAP_BUFFER = 0x0A,
    IOCTL_GET_CLK_RATE = 0x23
};
enum { IOCTL_SET_NVMAP_FD = 0x01 }; /* in IOCTL_GROUP_NVMAP */
#define ENGINE_CLOCK_RATE 614400000u /* GetClkRate's answer, Eden's: the engine clock in Hz */

/* IoctlSubmit: four counts, then cmd_buffer_count CommandBuffers, relocation_count Relocs and as many reloc shifts,
   syncpoint_count SyncptIncrs and fence_count u32 fences written back, all by byte offset */
enum {
    SUBMIT_CMD_BUFFER_COUNT = 0x0,
    SUBMIT_RELOCATION_COUNT = 0x4,
    SUBMIT_SYNCPOINT_COUNT = 0x8,
    SUBMIT_FENCE_COUNT = 0xC,
    SUBMIT_HEADER_SIZE = 0x10
};
enum { CMD_BUFFER_MEMORY_ID = 0x0, CMD_BUFFER_OFFSET = 0x4, CMD_BUFFER_WORD_COUNT = 0x8, CMD_BUFFER_SIZE = 0xC };
#define RELOC_SIZE       0x10u
#define RELOC_SHIFT_SIZE 0x4u
enum { SYNCPT_INCR_ID = 0x0, SYNCPT_INCR_INCREMENTS = 0x4, SYNCPT_INCR_SIZE = 0x14 /* and three unknown words */ };
#define FENCE_SIZE 4u
/* IoctlGetSyncpoint {param; out value}, IoctlGetWaitbase {unknown; out value}, IoctlGetClkRate {out clk_rate,
   module_id}: two u32 each */
enum { GET_ID_PARAM = 0x0, GET_ID_VALUE = 0x4, GET_ID_SIZE = 0x8 };
enum { GET_CLK_RATE_RATE = 0x0, GET_CLK_RATE_MODULE_ID = 0x4, GET_CLK_RATE_SIZE = 0x8 };
/* IoctlMapBuffer {num_entries, data_address, attach_host_ch_das}, then MapBufferEntries {map_handle; out
   map_address} */
enum { MAP_BUFFER_NUM_ENTRIES = 0x0, MAP_BUFFER_HEADER_SIZE = 0xC };
enum { MAP_ENTRY_HANDLE = 0x0, MAP_ENTRY_ADDRESS = 0x4, MAP_ENTRY_SIZE = 0x8 };

#define H1_REGS      0x400u    /* method registers: 0x1000 bytes */
#define H1_SYNCPTS   16u       /* NVHOST_MODULE_MAX_SYNCPTS: the syncpoint indexes a channel hands out */
#define H1_QUEUE     64u       /* jobs a channel holds before a SUBMIT waits for room */
#define H1_JOB_WORDS (1u << 24) /* the most words a job may copy */
#define H1_CB_WORDS  0x100000u  /* the most words one command buffer may hold */
#define H1_STACK     (1u << 20) /* a worker's stack */
#define HOST_WAIT_NS 2000000000 /* a host class wait gives up after 2 s, as the GPU's stream waits do */

/* The IOVA space. The engines address memory through 32-bit IOVAs (MAP_BUFFER returns a u32; Eden notes NVDEC's
   address space is 32-bit), here [IOVA_BASE, IOVA_END): from 256 MiB (0 and the low IOVAs are never handed out) to
   4 GiB. It is handed out in pages of IOVA_PAGE, this allocator's granularity: a mapping spans its size in pages plus
   a free page, so an engine running past a buffer reaches nothing mapped. A table of mappings sorted by IOVA, first
   fit, at most IOVA_SLOTS of them */
#define IOVA_BASE  0x10000000ull
#define IOVA_END   0x100000000ull
#define IOVA_PAGE  0x10000ull
#define IOVA_SLOTS 1024u

typedef struct {
    u64 iova, span, addr; /* span: the IOVA range taken, guard page included */
    u32 size, handle;
} IovaMap;

/* Statically initialized: nvmap's FREE can call host1x_forget_handle on any thread before a channel opens */
static struct {
    PMutex iova_lk;           /* the table */
    IovaMap iova[IOVA_SLOTS]; /* sorted by iova */
    u32 niova;
    PMutex engine_lk; /* the engines: one job's command buffers at a time */
    u32 jobs;         /* queued or running on any channel, with __atomic operations: what host1x_drain waits for */
} g_host1x = {PMUTEX_INIT, {{0}}, 0, PMUTEX_INIT, 0};

/* A job is one block of words: the command buffer count, the increment count, each command buffer as its length and
   words (length 0 for one that was refused), then each increment as (syncpoint id, count) */
struct H1Ch {
    PMutex lk; /* the queue, the flags below it and syncpt[] */
    PCond cv;  /* the queue changed, or the worker ended */
    u32* queue[H1_QUEUE];
    u32 head, count;
    int started, closing, exited;
    u32 syncpt[H1_SYNCPTS]; /* 0: not handed out yet (syncpt_alloc never returns 0) */
    /* the worker's alone, then the close's */
    int is_vic;
    u32 cls, method, payload; /* the class the stream addresses, THI's method 0, LoadSyncptPayload32's value */
    NvdecCtx* dec;
    u32 regs[H1_REGS];
};

static void wr32(u8* p, u32 off, u32 v) { snx_st_le32(p + off, v); }

/* ---- the IOVA table */
/* the guest memory behind [iova, iova + len), when a single mapping holds all of it */
u8* iova_ptr(u64 iova, u64 len) {
    u8* p = NULL;
    u32 lo = 0, hi;
    if (!len) return NULL;
    pm_lock(&g_host1x.iova_lk);
    hi = g_host1x.niova;
    while (lo < hi) { /* the last mapping starting at or below iova */
        u32 mid = lo + (hi - lo) / 2;
        if (g_host1x.iova[mid].iova <= iova)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo) {
        const IovaMap* m = &g_host1x.iova[lo - 1];
        u64 off = iova - m->iova;
        if (off < m->size && len <= m->size - off) p = GP(m->addr + off);
    }
    pm_unlock(&g_host1x.iova_lk);
    return p;
}
static void iova_remove(u32 i) {
    memmove(&g_host1x.iova[i], &g_host1x.iova[i + 1], (g_host1x.niova - i - 1) * sizeof g_host1x.iova[0]);
    g_host1x.niova--;
}
/* nvmap handle h at an IOVA: the one it has while its buffer stays where it is, or the first free range. The handle's
   liveness is read under the table's lock, so a FREE racing this either is seen or forgets the new mapping */
static u32 iova_map(u32 h, u32* out) {
    u64 addr, span, at = IOVA_BASE;
    u32 size, i, r = NV_OK;
    pm_lock(&g_host1x.iova_lk);
    if (!nvmap_get(h, &addr, &size) || !addr || !size) {
        r = NV_BAD_PARAMETER;
        goto done;
    }
    for (i = 0; i < g_host1x.niova; i++)
        if (g_host1x.iova[i].handle == h) {
            if (g_host1x.iova[i].addr == addr && g_host1x.iova[i].size == size) {
                *out = (u32)g_host1x.iova[i].iova;
                goto done;
            }
            iova_remove(i); /* the handle names another buffer now */
            break;
        }
    if (g_host1x.niova == IOVA_SLOTS) {
        r = NV_INSUFFICIENT_MEMORY;
        goto done;
    }
    span = ((u64)size + IOVA_PAGE - 1) / IOVA_PAGE * IOVA_PAGE + IOVA_PAGE; /* whole pages and the guard page */
    for (i = 0; i < g_host1x.niova; i++) {
        if (g_host1x.iova[i].iova - at >= span) break;
        at = g_host1x.iova[i].iova + g_host1x.iova[i].span;
    }
    if (at > IOVA_END || IOVA_END - at < span) {
        r = NV_INSUFFICIENT_MEMORY;
        goto done;
    }
    memmove(&g_host1x.iova[i + 1], &g_host1x.iova[i], (g_host1x.niova - i) * sizeof g_host1x.iova[0]);
    g_host1x.iova[i].iova = at;
    g_host1x.iova[i].span = span;
    g_host1x.iova[i].addr = addr;
    g_host1x.iova[i].size = size;
    g_host1x.iova[i].handle = h;
    g_host1x.niova++;
    *out = (u32)at;
done:
    pm_unlock(&g_host1x.iova_lk);
    return r;
}
void host1x_forget_handle(u32 h) {
    u32 i = 0;
    pm_lock(&g_host1x.iova_lk);
    while (i < g_host1x.niova)
        if (g_host1x.iova[i].handle == h)
            iova_remove(i);
        else
            i++;
    pm_unlock(&g_host1x.iova_lk);
}

/* ---- running a job on the worker */
/* a host class wait: syncpoint id reaching thresh. The engines' lock is let go meanwhile */
static void host_wait(u32 id, u32 thresh) {
    pm_unlock(&g_host1x.engine_lk);
    if (!syncpt_wait(id, thresh, HOST_WAIT_NS))
        WARN_LIMIT(8, "host1x: a job waited 2 s for syncpoint %u to reach %u (it is at %u), and goes on\n", id, thresh,
                   syncpt_read(id));
    pm_lock(&g_host1x.engine_lk);
}
/* WaitSyncpt's 24-bit threshold, as the full value nearest the syncpoint's: its high bits, a wrap up when that lands
   more than half the 24-bit range behind */
static u32 wait_threshold(u32 cur, u32 v) {
    const u32 range = 1u << CONTROL_WAIT_SYNCPT_THRESHOLD_BITS;
    u32 t = (cur & ~(range - 1)) | control_wait_syncpt_threshold(v);
    if ((i32)(t - cur) < -(i32)(range / 2)) t += range;
    return t;
}
/* the host class's methods (Eden's Control::ProcessMethod) */
static void control_method(H1Ch* ch, u32 method, u32 v) {
    switch (method) {
        case CONTROL_WAIT_SYNCPT: {
            u32 id = control_wait_syncpt_id(v);
            host_wait(id, wait_threshold(syncpt_read(id), v));
            break;
        }
        case CONTROL_LOAD_SYNCPT_PAYLOAD_32: ch->payload = v; break;
        case CONTROL_WAIT_SYNCPT_32: host_wait(v, ch->payload); break;
        default: break; /* increments come from the submit's list */
    }
}
/* a method of the class the stream addresses (Eden's CDmaPusher::ExecuteCommand): the host class's own, or an
   engine's through THI's SetMethod0 and SetMethod1. A write of Execute runs the engine on the channel's registers */
static void execute_command(H1Ch* ch, u32 method, u32 v) {
    if (ch->cls == CH_CLASS_CONTROL) {
        control_method(ch, method, v);
        return;
    }
    if (method == THI_SET_METHOD0)
        ch->method = v;
    else if (method == THI_SET_METHOD1 && ch->method < H1_REGS) {
        ch->regs[ch->method] = v;
        if (ch->is_vic && ch->method == VIC_EXECUTE)
            vic_execute(ch->regs);
        else if (!ch->is_vic && ch->method == NVDEC_EXECUTE)
            nvdec_execute(ch->dec, ch->regs);
    }
}
/* the methods a mask names, from method: a word of w for each bit set. 0 when the buffer ends first */
static int run_mask(H1Ch* ch, u32 method, u32 mask, u32 bits, const u32* w, u32 n, u32* i) {
    u32 k;
    for (k = 0; k < bits; k++)
        if (mask & (1u << k)) {
            if (*i >= n) return 0;
            execute_command(ch, method + k, w[(*i)++]);
        }
    return 1;
}
/* one command buffer's words (Eden's CDmaPusher thread loop): SetClass, Incrementing, NonIncrementing, Mask and
   Immediate. Any other submission mode, or a buffer ending inside a command, stops it */
static void run_words(H1Ch* ch, const u32* w, u32 n) {
    u32 i = 0;
    while (i < n) {
        u32 c = w[i++], mode = ch_header_submission_mode(c), method = ch_header_method_offset(c), k, count;
        switch (mode) {
            case CH_SUBMISSION_SET_CLASS:
                ch->cls = ch_set_class_id(c);
                if (!run_mask(ch, method, ch_set_class_mask(c), CH_SET_CLASS_MASK_BITS, w, n, &i)) return;
                break;
            case CH_SUBMISSION_INCREMENTING:
            case CH_SUBMISSION_NON_INCREMENTING:
                count = ch_header_value(c);
                if (count > n - i) return;
                for (k = 0; k < count; k++)
                    execute_command(ch, mode == CH_SUBMISSION_INCREMENTING ? method + k : method, w[i++]);
                break;
            case CH_SUBMISSION_MASK:
                if (!run_mask(ch, method, ch_header_value(c), CH_MASK_BITS, w, n, &i)) return;
                break;
            case CH_SUBMISSION_IMMEDIATE: execute_command(ch, method, ch_immediate_data(c)); break;
            default:
                WARN_LIMIT(8, "host1x: command %08x (submission mode %u) ends its buffer\n", c, mode);
                return;
        }
    }
}
/* a queued job: its command buffers under the engines' lock, then its syncpoint increments, and the value its
   first reaches, which the guest waits on when it waits for the job */
static u32 run_job(H1Ch* ch, const u32* job) {
    u32 ncb = job[0], ninc = job[1], k = 2, c, value = 0;
    pm_lock(&g_host1x.engine_lk);
    for (c = 0; c < ncb; c++) {
        u32 len = job[k++];
        run_words(ch, job + k, len);
        k += len;
    }
    pm_unlock(&g_host1x.engine_lk);
    for (c = 0; c < ninc; c++, k += 2) {
        syncpt_add(job[k], job[k + 1]);
        if (!c) value = syncpt_read(job[k]);
    }
    return value;
}
static void worker(void* arg) {
    H1Ch* ch = arg;
    SNX_THREAD_NAME("host1x");
    pm_lock(&ch->lk);
    for (;;) {
        u32* job;
        while (!ch->count && !ch->closing) pc_wait(&ch->cv, &ch->lk, -1);
        if (!ch->count) break; /* closing, and every queued job ran */
        job = ch->queue[ch->head];
        pm_unlock(&ch->lk);
        /* two channels differ only in their engine and class, and a job of either is waited for at its syncpoint */
        SNX_ZONE_T(z_job, "host1x job", "%s class %04x syncpt %u", ch->is_vic ? "vic" : "nvdec", ch->cls,
                   ch->syncpt[0]);
        /* the job's first syncpoint's value: only the zone reads it */
        [[maybe_unused]] u32 spval = run_job(ch, job);
        SNX_ZONE_VALUE(z_job, spval);
        SNX_ZONE_END(z_job);
        free(job);
        __atomic_sub_fetch(&g_host1x.jobs, 1, __ATOMIC_RELEASE);
        pm_lock(&ch->lk);
        ch->head = (ch->head + 1) % H1_QUEUE;
        ch->count--;
        pc_broadcast(&ch->cv); /* room for a waiting SUBMIT */
    }
    ch->exited = 1;
    pc_broadcast(&ch->cv);
    pm_unlock(&ch->lk);
}

/* ---- the ioctls */
static int owns_syncpt(const H1Ch* ch, u32 id) {
    u32 i;
    for (i = 0; i < H1_SYNCPTS; i++)
        if (ch->syncpt[i] && ch->syncpt[i] == id) return 1;
    return 0;
}
/* Submit (IoctlSubmit): the command buffers' words copied into a job, its fences promised, the job queued for the
   channel's worker */
static u32 submit(H1Ch* ch, u8* d, u32 n) {
    u32 ncb, nrel, ninc, nfe, i, k, *job;
    u64 o_cb = SUBMIT_HEADER_SIZE, o_inc, o_fe, total, *addr;
    int foreign = 0;
    if (n < SUBMIT_HEADER_SIZE) return NV_BAD_PARAMETER;
    ncb = rd32(d, SUBMIT_CMD_BUFFER_COUNT);
    nrel = rd32(d, SUBMIT_RELOCATION_COUNT);
    ninc = rd32(d, SUBMIT_SYNCPOINT_COUNT);
    nfe = rd32(d, SUBMIT_FENCE_COUNT);
    /* in 64 bits: the counts can wrap a u32 sum */
    o_inc = o_cb + (u64)CMD_BUFFER_SIZE * ncb + (u64)(RELOC_SIZE + RELOC_SHIFT_SIZE) * nrel;
    o_fe = o_inc + (u64)SYNCPT_INCR_SIZE * ninc;
    if (o_fe > n) return NV_BAD_PARAMETER;
    if (nrel) WARN_LIMIT(1, "host1x: a submit's %u relocations are not applied\n", nrel);
    /* an increment of a syncpoint the channel was not given refuses the whole job, before anything runs */
    pm_lock(&ch->lk);
    for (i = 0; i < ninc && !foreign; i++)
        foreign = !owns_syncpt(ch, rd32(d, (u32)(o_inc + (u64)SYNCPT_INCR_SIZE * i + SYNCPT_INCR_ID)));
    pm_unlock(&ch->lk);
    if (foreign) {
        WARN_LIMIT(8, "host1x: a submit increments syncpoint %u, which isn't the channel's\n",
                   rd32(d, (u32)(o_inc + (u64)SYNCPT_INCR_SIZE * (i - 1) + SYNCPT_INCR_ID)));
        return NV_BAD_PARAMETER;
    }
    /* each command buffer's handle looked up once: a refused one runs as empty, its job's increments still happen
       so the game doesn't wait for good */
    addr = malloc(ncb ? ncb * sizeof *addr : 1);
    if (!addr) return NV_INSUFFICIENT_MEMORY;
    total = 2 + ncb + 2ull * ninc; /* the job's words: see H1Ch */
    for (i = 0; i < ncb; i++) {
        u32 at = (u32)(o_cb + (u64)CMD_BUFFER_SIZE * i), memid = rd32(d, at + CMD_BUFFER_MEMORY_ID);
        u32 off = rd32(d, at + CMD_BUFFER_OFFSET), words = rd32(d, at + CMD_BUFFER_WORD_COUNT), size;
        u64 a;
        addr[i] = 0;
        if (!nvmap_get(memid, &a, &size) || !a || !words || words > H1_CB_WORDS || (off & 3) ||
            (u64)off + 4ull * words > size) {
            WARN_LIMIT(8, "host1x: command buffer %u of a submit skipped (handle %u, offset %u, %u words)\n", i, memid,
                       off, words);
            continue;
        }
        addr[i] = a + off;
        total += words;
    }
    if (total - 2 > H1_JOB_WORDS) {
        free(addr);
        return NV_BAD_PARAMETER;
    }
    job = malloc((size_t)total * 4);
    if (!job) {
        free(addr);
        return NV_INSUFFICIENT_MEMORY;
    }
    job[0] = ncb;
    job[1] = ninc;
    k = 2;
    for (i = 0; i < ncb; i++) {
        u32 words = addr[i] ? rd32(d, (u32)(o_cb + (u64)CMD_BUFFER_SIZE * i + CMD_BUFFER_WORD_COUNT)) : 0;
        job[k++] = words;
        if (words) memcpy(job + k, GP(addr[i]), (size_t)words * 4);
        snx_le32_array(job + k, words); /* the stream's words, read as numbers on the worker */
        k += words;
    }
    free(addr);
    for (i = 0; i < ninc; i++, k += 2) {
        job[k] = rd32(d, (u32)(o_inc + (u64)SYNCPT_INCR_SIZE * i + SYNCPT_INCR_ID));
        job[k + 1] = rd32(d, (u32)(o_inc + (u64)SYNCPT_INCR_SIZE * i + SYNCPT_INCR_INCREMENTS));
    }
    /* the fences are promised in queue order, under the lock the job is queued in */
    pm_lock(&ch->lk);
    while (ch->count == H1_QUEUE) pc_wait(&ch->cv, &ch->lk, -1);
    for (i = 0; i < ninc; i++) {
        u32 v = syncpt_promise(job[k - 2 * ninc + 2 * i], job[k - 2 * ninc + 2 * i + 1]);
        if (i < nfe && o_fe + (u64)FENCE_SIZE * (i + 1) <= n) wr32(d, (u32)(o_fe + (u64)FENCE_SIZE * i), v);
    }
    ch->queue[(ch->head + ch->count) % H1_QUEUE] = job;
    ch->count++;
    __atomic_add_fetch(&g_host1x.jobs, 1, __ATOMIC_RELAXED);
    pc_broadcast(&ch->cv);
    if (!ch->started) {
        ch->started = 1;
        if (plat_thread(worker, ch, H1_STACK)) fatal("host1x: cannot start a channel's worker thread\n");
    }
    pm_unlock(&ch->lk);
    return NV_OK;
}
/* a game pause, with no guest thread left to submit: polled, as pauses are rare and the jobs take milliseconds */
int host1x_drain(int timeout_ms) {
    u64 t0 = plat_ns();
    while (__atomic_load_n(&g_host1x.jobs, __ATOMIC_ACQUIRE)) {
        if (plat_ns() - t0 >= (u64)timeout_ms * 1000000u) {
            WARN("pause: host1x still has %u jobs after %d ms\n", __atomic_load_n(&g_host1x.jobs, __ATOMIC_RELAXED),
                 timeout_ms);
            return -1;
        }
        plat_sleep_ns(1000000);
    }
    return 0;
}
/* GetSyncpoint (IoctlGetSyncpoint): the channel's syncpoint at index param, allocated when first asked for */
static u32 get_syncpt(H1Ch* ch, u8* d, u32 n) {
    u32 idx, id;
    if (n < GET_ID_SIZE) return NV_BAD_PARAMETER;
    idx = rd32(d, GET_ID_PARAM);
    if (idx >= H1_SYNCPTS) return NV_BAD_PARAMETER;
    pm_lock(&ch->lk);
    if (!ch->syncpt[idx]) ch->syncpt[idx] = syncpt_alloc();
    id = ch->syncpt[idx];
    pm_unlock(&ch->lk);
    wr32(d, GET_ID_VALUE, id);
    return NV_OK;
}
/* MapBuffer (IoctlMapBuffer): every entry that fits the buffer is mapped, and the first failure is the result (the
   SDK reads only that) */
static u32 map_buffer(u8* d, u32 n) {
    u32 count, i, r = NV_OK;
    if (n < MAP_BUFFER_HEADER_SIZE) return NV_BAD_PARAMETER;
    count = rd32(d, MAP_BUFFER_NUM_ENTRIES);
    if (count > (n - MAP_BUFFER_HEADER_SIZE) / MAP_ENTRY_SIZE) count = (n - MAP_BUFFER_HEADER_SIZE) / MAP_ENTRY_SIZE;
    for (i = 0; i < count; i++) {
        u32 at = MAP_BUFFER_HEADER_SIZE + MAP_ENTRY_SIZE * i, iova = 0;
        u32 e = iova_map(rd32(d, at + MAP_ENTRY_HANDLE), &iova);
        wr32(d, at + MAP_ENTRY_ADDRESS, iova);
        if (e && r == NV_OK) r = e;
    }
    return r;
}

H1Ch* host1x_open(int is_vic) {
    H1Ch* ch = calloc(1, sizeof *ch);
    if (!ch) return NULL;
    if (!is_vic && !(ch->dec = nvdec_new())) {
        free(ch);
        return NULL;
    }
    ch->is_vic = is_vic;
    ch->cls = is_vic ? CH_CLASS_GRAPHICS_VIC : CH_CLASS_NVDEC;
    pm_init(&ch->lk);
    pc_init(&ch->cv);
    return ch;
}
void host1x_close(H1Ch* ch) {
    u32 i;
    if (!ch) return;
    pm_lock(&ch->lk); /* the game may still wait for what it queued: the worker runs it first */
    ch->closing = 1;
    pc_broadcast(&ch->cv);
    while (ch->started && !ch->exited) pc_wait(&ch->cv, &ch->lk, -1);
    pm_unlock(&ch->lk);
    pm_lock(&g_host1x.engine_lk);
    nvdec_free(ch->dec);
    pm_unlock(&g_host1x.engine_lk);
    for (i = 0; i < H1_SYNCPTS; i++)
        if (ch->syncpt[i]) syncpt_free(ch->syncpt[i]);
    pm_destroy(&ch->lk);
    free(ch);
}
u32 host1x_ioctl(H1Ch* ch, u32 cmd, u8* d, u32 n) {
    u32 group = ioctl_group(cmd), nr = ioctl_number(cmd);
    if (!ch) return NV_BAD_PARAMETER;
    if (group == IOCTL_GROUP_HOST1X) switch (nr) {
            case IOCTL_SUBMIT: return submit(ch, d, n);
            case IOCTL_GET_SYNCPOINT: return get_syncpt(ch, d, n);
            case IOCTL_GET_WAITBASE: /* every wait base is 0 */
                if (n >= GET_ID_SIZE) wr32(d, GET_ID_VALUE, 0);
                return NV_OK;
            case IOCTL_SET_SUBMIT_TIMEOUT: return NV_OK;
            case IOCTL_MAP_BUFFER: return map_buffer(d, n);
            case IOCTL_UNMAP_BUFFER: return NV_OK; /* NVDEC and VIC map the same handles around every job, so the
                                                      mapping lives until nvmap frees the handle */
            case IOCTL_GET_CLK_RATE:
                if (n >= GET_CLK_RATE_SIZE) {
                    wr32(d, GET_CLK_RATE_RATE, ENGINE_CLOCK_RATE);
                    wr32(d, GET_CLK_RATE_MODULE_ID, 0);
                }
                return NV_OK;
            default: break;
        }
    else if (group == IOCTL_GROUP_NVMAP && nr == IOCTL_SET_NVMAP_FD)
        return NV_OK;
    WARN_LIMIT(16, "host1x: ioctl %08x not handled\n", cmd);
    return NV_OK;
}

/* ---- save states */
int host1x_iova_save(StateIo* io) {
    u32 i;
    int r;
    pm_lock(&g_host1x.iova_lk);
    r = io_put32(io, g_host1x.niova);
    for (i = 0; i < g_host1x.niova && !r; i++)
        r = io_put64(io, g_host1x.iova[i].iova) | io_put64(io, g_host1x.iova[i].span) |
            io_put64(io, g_host1x.iova[i].addr) | io_put32(io, g_host1x.iova[i].size) |
            io_put32(io, g_host1x.iova[i].handle);
    pm_unlock(&g_host1x.iova_lk);
    return r ? -1 : 0;
}
struct H1Iova {
    IovaMap map[IOVA_SLOTS];
    u32 n;
};
H1Iova* host1x_iova_parse(StateIo* io) {
    H1Iova* t = (H1Iova*)calloc(1, sizeof *t);
    u64 end = IOVA_BASE;
    u32 i;
    if (!t || io_get32(io, &t->n) || t->n > IOVA_SLOTS) goto bad;
    for (i = 0; i < t->n; i++) {
        IovaMap* m = &t->map[i];
        if (io_get64(io, &m->iova) || io_get64(io, &m->span) || io_get64(io, &m->addr) || io_get32(io, &m->size) ||
            io_get32(io, &m->handle))
            goto bad;
        /* sorted, apart, inside the IOVA space and the guest's, each one inside its span */
        if (m->iova < end || m->span > IOVA_END - m->iova || m->size > m->span || m->addr > AS_TOP ||
            m->size > AS_TOP - m->addr)
            goto bad;
        end = m->iova + m->span;
    }
    return t;
bad:
    free(t);
    return NULL;
}
void host1x_iova_install(H1Iova* t) {
    pm_lock(&g_host1x.iova_lk);
    memcpy(g_host1x.iova, t->map, sizeof *g_host1x.iova * t->n);
    g_host1x.niova = t->n;
    pm_unlock(&g_host1x.iova_lk);
    free(t);
}
void host1x_iova_free(H1Iova* t) { free(t); }
int host1x_channel_save(const H1Ch* ch, StateIo* io) {
    return io_put32(io, (u32)ch->is_vic) | io_put(io, ch->syncpt, sizeof ch->syncpt) | io_put32(io, ch->cls) |
                   io_put32(io, ch->method) | io_put32(io, ch->payload) | io_put(io, ch->regs, sizeof ch->regs)
               ? -1
               : 0;
}
H1Ch* host1x_channel_load(StateIo* io) {
    u32 is_vic;
    H1Ch* ch;
    if (io_get32(io, &is_vic) || !(ch = host1x_open(is_vic != 0))) return NULL;
    if (io_get(io, ch->syncpt, sizeof ch->syncpt) || io_get32(io, &ch->cls) || io_get32(io, &ch->method) ||
        io_get32(io, &ch->payload) || io_get(io, ch->regs, sizeof ch->regs)) {
        host1x_channel_discard(ch);
        return NULL;
    }
    return ch;
}
void host1x_channel_discard(H1Ch* ch) {
    if (!ch) return;
    memset(ch->syncpt, 0, sizeof ch->syncpt); /* the state's: not this channel's to free */
    host1x_close(ch);
}
