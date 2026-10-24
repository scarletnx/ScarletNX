/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from Eden (src/core/hle/service/nvdrv/devices/nvhost_nvdec_common.cpp, nvhost_nvdec.cpp, nvhost_vic.cpp,
   src/video_core/cdma_pusher.cpp, src/video_core/host1x/control.cpp):
   SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
   SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
   SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
   SPDX-FileCopyrightText: 2021 yuzu Emulator Project
   SPDX-FileCopyrightText: Ryujinx Team and Contributors (cdma_pusher.cpp, MIT) */
/* host1x: the channels of /dev/nvhost-nvdec and /dev/nvhost-vic. Their ioctls, the IOVA space the engines address
   guest memory through, and a worker per channel that runs the submitted command streams in order.

   A SUBMIT returns at once with the fences its syncpoint increments will reach, as on the console, and the job runs
   later on the channel's worker: decoding inside the ioctl held the game's submitting thread, and whatever it had
   locked, for a whole picture. Every syncpoint comes from nvdrv's syncpt_alloc, which keeps the ids below the 128
   NVIDIA's stream library sizes its tables at.

   Locks, taken in this order: a channel's lk or the engines' lock, then the IOVA table's, then nvdrv's. A job's
   command buffers run under the engines' lock (the decoders and VIC are not reentrant); a host class wait lets go
   of it, since the job waited for may be another channel's */
#include "nx/gpu/video/host1x_int.h"
#include "nx/runtime/platform/tracy.h"
#include "nx/runtime/state_io.h"

enum { NV_OK = 0, NV_BAD_PARAMETER = 4, NV_INSUFFICIENT_MEMORY = 6 };

#define H1_REGS       0x400u /* method registers: 0x1000 bytes */
#define H1_SYNCPTS    16u    /* NVHOST_MODULE_MAX_SYNCPTS */
#define H1_QUEUE      64u    /* jobs a channel holds before a SUBMIT waits for room */
#define H1_JOB_WORDS  (1u << 24)
#define H1_CB_WORDS   0x100000u
#define H1_STACK      (1u << 20)
#define CLASS_HOST    0x01u
#define CLASS_VIC     0x5Du
#define CLASS_NVDEC   0xF0u
#define METHOD_EXEC   (0x300u / 4)
#define HOST_WAIT_NS  2000000000

/* The IOVAs handed out: [IOVA_BASE, IOVA_END), so each fits the u32 MAP_BUFFER returns. A mapping spans its size in
   64 KiB pages plus a free 64 KiB page, so an engine running past a buffer reaches nothing mapped */
#define IOVA_BASE  0x10000000ull
#define IOVA_END   0x100000000ull
#define IOVA_PAGE  0x10000ull
#define IOVA_SLOTS 1024u

typedef struct {
    u64 iova, span, addr; /* span: the IOVA range taken, guard page included */
    u32 size, handle;
} IovaMap;

/* Statically initialized: nvmap's FREE can call host1x_forget_handle on any thread before a channel opens */
static PMutex g_iova_lk = PMUTEX_INIT;
static IovaMap g_iova[IOVA_SLOTS]; /* sorted by iova */
static u32 g_niova;
static PMutex g_engine_lk = PMUTEX_INIT;
static u32 g_jobs; /* queued or running on any channel, with __atomic operations: what host1x_drain waits for */

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
    u32 cls, method, payload; /* the class the stream addresses, the engine method selected, LOAD_SYNCPT_PAYLOAD_32 */
    NvdecCtx* dec;
    u32 regs[H1_REGS];
};

static void wr32(u8* p, u32 off, u32 v) { snx_st_le32(p + off, v); }

/* ---- the IOVA table */
u8* iova_ptr(u64 iova, u64 len) {
    u8* p = NULL;
    u32 lo = 0, hi;
    if (!len) return NULL;
    pm_lock(&g_iova_lk);
    hi = g_niova;
    while (lo < hi) { /* the last mapping starting at or below iova */
        u32 mid = lo + (hi - lo) / 2;
        if (g_iova[mid].iova <= iova)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo) {
        const IovaMap* m = &g_iova[lo - 1];
        u64 off = iova - m->iova;
        if (off < m->size && len <= m->size - off) p = GP(m->addr + off);
    }
    pm_unlock(&g_iova_lk);
    return p;
}
static void iova_remove(u32 i) {
    memmove(&g_iova[i], &g_iova[i + 1], (g_niova - i - 1) * sizeof g_iova[0]);
    g_niova--;
}
/* nvmap handle h at an IOVA: the one it has while its buffer stays where it is, or the first free range. The handle's
   liveness is read under the table's lock, so a FREE racing this either is seen or forgets the new mapping */
static u32 iova_map(u32 h, u32* out) {
    u64 addr, span, at = IOVA_BASE;
    u32 size, i, r = NV_OK;
    pm_lock(&g_iova_lk);
    if (!nvmap_get(h, &addr, &size) || !addr || !size) {
        r = NV_BAD_PARAMETER;
        goto done;
    }
    for (i = 0; i < g_niova; i++)
        if (g_iova[i].handle == h) {
            if (g_iova[i].addr == addr && g_iova[i].size == size) {
                *out = (u32)g_iova[i].iova;
                goto done;
            }
            iova_remove(i); /* the handle names another buffer now */
            break;
        }
    if (g_niova == IOVA_SLOTS) {
        r = NV_INSUFFICIENT_MEMORY;
        goto done;
    }
    span = ((u64)size + IOVA_PAGE - 1) / IOVA_PAGE * IOVA_PAGE + IOVA_PAGE;
    for (i = 0; i < g_niova; i++) {
        if (g_iova[i].iova - at >= span) break;
        at = g_iova[i].iova + g_iova[i].span;
    }
    if (at > IOVA_END || IOVA_END - at < span) {
        r = NV_INSUFFICIENT_MEMORY;
        goto done;
    }
    memmove(&g_iova[i + 1], &g_iova[i], (g_niova - i) * sizeof g_iova[0]);
    g_iova[i].iova = at;
    g_iova[i].span = span;
    g_iova[i].addr = addr;
    g_iova[i].size = size;
    g_iova[i].handle = h;
    g_niova++;
    *out = (u32)at;
done:
    pm_unlock(&g_iova_lk);
    return r;
}
void host1x_forget_handle(u32 h) {
    u32 i = 0;
    pm_lock(&g_iova_lk);
    while (i < g_niova)
        if (g_iova[i].handle == h)
            iova_remove(i);
        else
            i++;
    pm_unlock(&g_iova_lk);
}

/* ---- running a job on the worker */
/* a host class wait: syncpoint id reaching thresh. The engines' lock is let go meanwhile */
static void host_wait(u32 id, u32 thresh) {
    pm_unlock(&g_engine_lk);
    if (!syncpt_wait(id, thresh, HOST_WAIT_NS))
        WARN_LIMIT(8, "host1x: a job waited 2 s for syncpoint %u to reach %u (it is at %u), and goes on\n", id, thresh,
                   syncpt_read(id));
    pm_lock(&g_engine_lk);
}
static void host_method(H1Ch* ch, u32 off, u32 v) {
    switch (off) {
        case 0x08: { /* WAIT_SYNCPT: index in bits 24-31, a 24 bit threshold, taken next to the syncpoint's value */
            u32 id = v >> 24, cur = syncpt_read(id), t = (cur & ~0xFFFFFFu) | (v & 0xFFFFFFu);
            if ((i32)(t - cur) < -(i32)0x800000) t += 0x1000000u;
            host_wait(id, t);
            break;
        }
        case 0x4E: ch->payload = v; break; /* LOAD_SYNCPT_PAYLOAD_32 */
        case 0x50: host_wait(v, ch->payload); break; /* WAIT_SYNCPT_32 */
        default: break; /* increments come from the submit's list */
    }
}
static void engine_method(H1Ch* ch, u32 off, u32 v) {
    if (ch->cls == CLASS_HOST) {
        host_method(ch, off, v);
        return;
    }
    if (off == 0x10) /* the method a write at 0x11 goes to, as a word index */
        ch->method = v;
    else if (off == 0x11 && ch->method < H1_REGS) {
        ch->regs[ch->method] = v;
        if (ch->method == METHOD_EXEC) {
            if (ch->is_vic)
                vic_execute(ch->regs);
            else
                nvdec_execute(ch->dec, ch->regs);
        }
    }
}
/* one command buffer's words: SETCLASS, INCR, NONINCR, MASK and IMM. Anything else, or a buffer ending inside a
   command, stops it */
static void run_words(H1Ch* ch, const u32* w, u32 n) {
    u32 i = 0;
    while (i < n) {
        u32 c = w[i++], op = c >> 28, off = (c >> 16) & 0xFFF, k, cnt;
        switch (op) {
            case 0: /* SETCLASS: class in bits 6-15, then a word for each of 6 mask bits */
                ch->cls = (c >> 6) & 0x3FF;
                for (k = 0; k < 6; k++)
                    if (c & (1u << k)) {
                        if (i >= n) return;
                        engine_method(ch, off + k, w[i++]);
                    }
                break;
            case 1: /* INCR */
            case 2: /* NONINCR */
                cnt = c & 0xFFFF;
                if (cnt > n - i) return;
                for (k = 0; k < cnt; k++) engine_method(ch, op == 1 ? off + k : off, w[i++]);
                break;
            case 3: /* MASK: a word for each of 16 mask bits */
                for (k = 0; k < 16; k++)
                    if (c & (1u << k)) {
                        if (i >= n) return;
                        engine_method(ch, off + k, w[i++]);
                    }
                break;
            case 4: engine_method(ch, off, c & 0xFFFF); break; /* IMM */
            default: WARN_LIMIT(8, "host1x: command %08x (opcode %u) ends its buffer\n", c, op); return;
        }
    }
}
static void run_job(H1Ch* ch, const u32* job) {
    u32 ncb = job[0], ninc = job[1], k = 2, c;
    pm_lock(&g_engine_lk);
    for (c = 0; c < ncb; c++) {
        u32 len = job[k++];
        run_words(ch, job + k, len);
        k += len;
    }
    pm_unlock(&g_engine_lk);
    for (c = 0; c < ninc; c++, k += 2) syncpt_add(job[k], job[k + 1]);
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
        SNX_ZONE(z_job, "host1x job");
        run_job(ch, job);
        SNX_ZONE_END(z_job);
        free(job);
        __atomic_sub_fetch(&g_jobs, 1, __ATOMIC_RELEASE);
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
/* SUBMIT {u32 cmdbufs, relocs, increments, fences}, then the cmdbufs {memid, offset, words}, the relocs (16 bytes) and
   their shifts (4), the increments {id, count, 3 unused} and the fences, u32s written back */
static u32 submit(H1Ch* ch, u8* d, u32 n) {
    u32 ncb, nrel, ninc, nfe, i, k, *job;
    u64 o_cb = 16, o_inc, o_fe, total, *addr;
    int foreign = 0;
    if (n < 16) return NV_BAD_PARAMETER;
    ncb = rd32(d, 0);
    nrel = rd32(d, 4);
    ninc = rd32(d, 8);
    nfe = rd32(d, 12);
    o_inc = o_cb + 12ull * ncb + 20ull * nrel; /* the relocs and their shifts between */
    o_fe = o_inc + 20ull * ninc;
    if (o_fe > n) return NV_BAD_PARAMETER;
    if (nrel) WARN_LIMIT(1, "host1x: a submit's %u relocations are not applied\n", nrel);
    /* an increment of a syncpoint the channel was not given refuses the whole job, before anything runs */
    pm_lock(&ch->lk);
    for (i = 0; i < ninc && !foreign; i++) foreign = !owns_syncpt(ch, rd32(d, (u32)(o_inc + 20ull * i)));
    pm_unlock(&ch->lk);
    if (foreign) {
        WARN_LIMIT(8, "host1x: a submit increments syncpoint %u, which isn't the channel's\n",
                   rd32(d, (u32)(o_inc + 20ull * (i - 1))));
        return NV_BAD_PARAMETER;
    }
    /* each command buffer's handle looked up once: a refused one runs as empty, its job's increments still happen
       so the game doesn't wait for good */
    addr = malloc(ncb ? ncb * sizeof *addr : 1);
    if (!addr) return NV_INSUFFICIENT_MEMORY;
    total = 2 + ncb + 2ull * ninc;
    for (i = 0; i < ncb; i++) {
        u32 at = (u32)(o_cb + 12ull * i), memid = rd32(d, at), off = rd32(d, at + 4), words = rd32(d, at + 8), size;
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
        u32 words = addr[i] ? rd32(d, (u32)(o_cb + 12ull * i + 8)) : 0;
        job[k++] = words;
        if (words) memcpy(job + k, GP(addr[i]), (size_t)words * 4);
        snx_le32_array(job + k, words); /* the stream's words, read as numbers on the worker */
        k += words;
    }
    free(addr);
    for (i = 0; i < ninc; i++, k += 2) {
        job[k] = rd32(d, (u32)(o_inc + 20ull * i));
        job[k + 1] = rd32(d, (u32)(o_inc + 20ull * i + 4));
    }
    /* the fences are promised in queue order, under the lock the job is queued in */
    pm_lock(&ch->lk);
    while (ch->count == H1_QUEUE) pc_wait(&ch->cv, &ch->lk, -1);
    for (i = 0; i < ninc; i++) {
        u32 v = syncpt_promise(job[k - 2 * ninc + 2 * i], job[k - 2 * ninc + 2 * i + 1]);
        if (i < nfe && o_fe + 4ull * i + 4 <= n) wr32(d, (u32)(o_fe + 4ull * i), v);
    }
    ch->queue[(ch->head + ch->count) % H1_QUEUE] = job;
    ch->count++;
    __atomic_add_fetch(&g_jobs, 1, __ATOMIC_RELAXED);
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
    while (__atomic_load_n(&g_jobs, __ATOMIC_ACQUIRE)) {
        if (plat_ns() - t0 >= (u64)timeout_ms * 1000000u) {
            WARN("pause: host1x still has %u jobs after %d ms\n", __atomic_load_n(&g_jobs, __ATOMIC_RELAXED),
                 timeout_ms);
            return -1;
        }
        plat_sleep_ns(1000000);
    }
    return 0;
}
/* GET_SYNCPOINT {u32 index; out u32 id}: the channel's syncpoint at that index, allocated when first asked for */
static u32 get_syncpt(H1Ch* ch, u8* d, u32 n) {
    u32 idx, id;
    if (n < 8) return NV_BAD_PARAMETER;
    idx = rd32(d, 0);
    if (idx >= H1_SYNCPTS) return NV_BAD_PARAMETER;
    pm_lock(&ch->lk);
    if (!ch->syncpt[idx]) ch->syncpt[idx] = syncpt_alloc();
    id = ch->syncpt[idx];
    pm_unlock(&ch->lk);
    wr32(d, 4, id);
    return NV_OK;
}
/* MAP_BUFFER {u32 count, 2 unused; then {u32 handle; out u32 iova} each}: every entry that fits the buffer is mapped,
   and the first failure is the result (the SDK reads only that) */
static u32 map_buffer(u8* d, u32 n) {
    u32 count, i, r = NV_OK;
    if (n < 12) return NV_BAD_PARAMETER;
    count = rd32(d, 0);
    if (count > (n - 12) / 8) count = (n - 12) / 8;
    for (i = 0; i < count; i++) {
        u32 iova = 0, e = iova_map(rd32(d, 12 + 8 * i), &iova);
        wr32(d, 16 + 8 * i, iova);
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
    ch->cls = is_vic ? CLASS_VIC : CLASS_NVDEC;
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
    pm_lock(&g_engine_lk);
    nvdec_free(ch->dec);
    pm_unlock(&g_engine_lk);
    for (i = 0; i < H1_SYNCPTS; i++)
        if (ch->syncpt[i]) syncpt_free(ch->syncpt[i]);
    pm_destroy(&ch->lk);
    free(ch);
}
u32 host1x_ioctl(H1Ch* ch, u32 cmd, u8* d, u32 n) {
    u32 group = (cmd >> 8) & 0xFF, nr = cmd & 0xFF;
    if (!ch) return NV_BAD_PARAMETER;
    if (group == 0x00) switch (nr) {
            case 0x01: return submit(ch, d, n);
            case 0x02: return get_syncpt(ch, d, n);
            case 0x03: /* GET_WAITBASE {u32; out u32 base} */
                if (n >= 8) wr32(d, 4, 0);
                return NV_OK;
            case 0x07: return NV_OK; /* SET_SUBMIT_TIMEOUT */
            case 0x09: return map_buffer(d, n);
            case 0x0A: return NV_OK; /* UNMAP_BUFFER: NVDEC and VIC map the same handles around every job, so the
                                        mapping lives until nvmap frees the handle */
            case 0x23: /* GET_CLK_RATE {out u32 rate, u32 module}: the engine clock Eden reports */
                if (n >= 8) {
                    wr32(d, 0, 614400000);
                    wr32(d, 4, 0);
                }
                return NV_OK;
            default: break;
        }
    else if (group == 0x48 && nr == 0x01) /* SET_NVMAP_FD */
        return NV_OK;
    WARN_LIMIT(16, "host1x: ioctl %08x not handled\n", cmd);
    return NV_OK;
}

/* ---- save states */
int host1x_iova_save(StateIo* io) {
    u32 i;
    int r;
    pm_lock(&g_iova_lk);
    r = io_put32(io, g_niova);
    for (i = 0; i < g_niova && !r; i++)
        r = io_put64(io, g_iova[i].iova) | io_put64(io, g_iova[i].span) | io_put64(io, g_iova[i].addr) |
            io_put32(io, g_iova[i].size) | io_put32(io, g_iova[i].handle);
    pm_unlock(&g_iova_lk);
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
        if (m->iova < end || m->span > IOVA_END - m->iova || m->size > m->span || m->addr > AS_MAX ||
            m->size > AS_MAX - m->addr)
            goto bad;
        end = m->iova + m->span;
    }
    return t;
bad:
    free(t);
    return NULL;
}
void host1x_iova_install(H1Iova* t) {
    pm_lock(&g_iova_lk);
    memcpy(g_iova, t->map, sizeof *g_iova * t->n);
    g_niova = t->n;
    pm_unlock(&g_iova_lk);
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
