/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* nvhost-nvdec / nvhost-vic (host1x channels).
   We parse the host1x stream, decode NVDEC H.264 and VP9 jobs with FFmpeg (the codec's headers get rebuilt from the
   picture setup the game fills in) into the surface each job names, and VIC jobs convert each enabled input slot's
   surface, unscaled and cropped, into the output surface's format, a later slot over an earlier one.
   Struct layouts / method offsets come from NVIDIA's open-gpu-doc (nvdec_drv.h, clc5b0.h, VIC). */
#include "nx/gpu/video/host1x_int.h"

/* ------------------------------------------------------------ IOVA (what the video engines see through the SMMU) */
/* A mapping lives until nvmap frees its handle (host1x_forget_handle), not until UNMAP_BUFFER: NVDEC and VIC map the
   same handles around every job, and players allocate fresh handles for each movie, so a table that only grew filled
   up after a few movies and every later job saw unmapped memory. g_iom is kept sorted by iova so that freed ranges
   are found again first-fit. */
typedef struct {
    u32 iova, size;
    u64 addr;
    u32 h;
} IoMap;
#define IOVA_BASE  0x10000000ull
#define IOVA_END   0x100000000ull /* MAP_BUFFER hands the iova back as a u32 */
#define IOVA_GUARD 0x10000ull
static IoMap g_iom[1024];
static int g_niom;
/* PMUTEX_INIT, not pm_init: nvmap's FREE can get here before any channel opens, from any thread. g_iomlk keeps the
   table, held only to look it up or change it, so a FREE doesn't wait for a decode; g_hlk keeps the engines, held
   around a job's commands on its channel's worker, but not across a syncpoint wait in them (a VIC job waits there
   for the NVDEC job before it). The order: a channel's lock or g_hlk, then g_iomlk, then nvdrv's locks */
static PMutex g_iomlk = PMUTEX_INIT;
static PMutex g_hlk = PMUTEX_INIT;
static u64 iova_span(u32 size) { return (((u64)size + 0xFFFFu) & ~0xFFFFull) + IOVA_GUARD; }
/* the iova handle h is mapped at, mapped now if it isn't. 0 with *err the NvError when it can't be: a handle that
   isn't alive or has no size (BadParameter), or no room (InsufficientMemory). nvmap's ids are its handle numbers, so
   h names the handle either way */
static u32 iova_map(u32 h, u32* err) {
    u64 a;
    u32 sz;
    /* the handle is read with the table held: nvmap's FREE drops a mapping (host1x_forget_handle) after it marks the
       handle dead, so it either finds this mapping in the table or this read finds the handle dead */
    pm_lock(&g_iomlk);
    if (!nvmap_get(h, &a, &sz) || !a || !sz) { /* a made-up size would let iova_ptr hand out memory past the buffer */
        pm_unlock(&g_iomlk);
        WARN_LIMIT(4, "video: nvmap handle %u has no memory or no size, not mapped for NVDEC/VIC\n", h);
        *err = 4;
        return 0;
    }
    for (int i = 0; i < g_niom; i++)
        if (g_iom[i].h == h && g_iom[i].addr == a) {
            u32 io = g_iom[i].iova;
            pm_unlock(&g_iomlk);
            return io;
        }
    u64 need = iova_span(sz), base = IOVA_BASE;
    int at = 0;
    for (; at < g_niom; at++) {
        if (base + need <= g_iom[at].iova) break;
        u64 e = g_iom[at].iova + iova_span(g_iom[at].size);
        if (e > base) base = e;
    }
    if (g_niom >= 1024 || base + need > IOVA_END) {
        int nm = g_niom;
        pm_unlock(&g_iomlk);
        WARN_LIMIT(8,
                   "video: no room to map nvmap handle %u (%u bytes, %d mappings): NVDEC and VIC see unmapped memory\n",
                   h, sz, nm);
        *err = 6;
        return 0;
    }
    memmove(&g_iom[at + 1], &g_iom[at], (size_t)(g_niom - at) * sizeof g_iom[0]);
    g_iom[at].iova = (u32)base;
    g_iom[at].size = sz;
    g_iom[at].addr = a;
    g_iom[at].h = h;
    g_niom++;
    pm_unlock(&g_iomlk);
    return (u32)base;
}
void host1x_forget_handle(u32 h) {
    pm_lock(&g_iomlk);
    int k = 0;
    for (int i = 0; i < g_niom; i++)
        if (g_iom[i].h != h) g_iom[k++] = g_iom[i];
    g_niom = k;
    pm_unlock(&g_iomlk);
}
/* 40-bit iova (engines get it >>8) -> host pointer, for len bytes there inside one mapping (none for len 0: a size
   that wrapped to 0 must not pass). The memory is guest memory, so the pointer stays usable if nvmap frees the handle
   meanwhile */
u8* iova_ptr(u64 iova, u64 len) {
    u8* p = NULL;
    pm_lock(&g_iomlk);
    for (int i = 0; i < g_niom && !p; i++) {
        IoMap* m = &g_iom[i];
        if (len && len <= m->size && iova >= m->iova && iova - m->iova <= m->size - len)
            p = GP(m->addr + (iova - m->iova));
    }
    pm_unlock(&g_iomlk);
    return p;
}

/* ------------------------------------------------------------ host1x command stream */
/* A channel is one nvdrv fd, as on the console: the game opens two NVDEC and two VIC fds per movie and sends a
   stream's jobs through one of them, so the registers and the decoder (its reference pictures and headers) stay with
   their stream. The engines only meet in guest memory: NVDEC writes each picture into the surface its job names, and
   VIC reads its input slot from there.
   As on the console, SUBMIT returns before the engine runs the job: the fences it hands back are the values its
   syncpoint increments are promised to reach, and the channel's worker thread runs its jobs in order and makes the
   increments after each. A VIC job's stream waits for the NVDEC picture it composes with the host class's syncpoint
   wait. Run inside the ioctl, a job held the game's submitting thread for the whole decode or composition, and with
   it whatever lock the game held around the submit */
#define H1_NSYNCPT 16 /* the syncpoint indexes GET_SYNCPOINT takes, nvhost's NVHOST_MODULE_MAX_SYNCPTS */
#define H1_QUEUE   64 /* jobs submitted and not run yet, at most: a submit past them waits for room */
/* a submitted job, one allocation: w holds ncb command buffer lengths, nsp {syncpoint id, increments} pairs, then the
   command buffers' words, copied at the submit */
typedef struct {
    u32 ncb, nsp;
    u32 w[];
} H1Job;
struct H1Ch {
    int is_vic;
    u32 syncpt[H1_NSYNCPT]; /* 0 until GET_SYNCPOINT asks for it */
    NvdecCtx* dec;          /* NVDEC channels only */
    /* lk: the ioctls' state above and the queue. The worker alone touches the engine state below it */
    PMutex lk;
    PCond cv; /* a job queued, a job done, the worker gone */
    H1Job* q[H1_QUEUE];
    u32 head, tail; /* jobs queued, jobs done */
    int worker;     /* the worker thread runs */
    int closing;    /* the fd closed: the worker ends once the queue is empty */
    u32 regs[0x1000 / 4];
    u32 method;
    u32 payload; /* the host class's LOAD_SYNCPT_PAYLOAD_32, the threshold of its WAIT_SYNCPT_32 */
};
static void dev_method(H1Ch* ch, u32 m, u32 v) {
    if (m >= 0x1000 / 4) return; /* past the register file: m << 2 would wrap onto a real register, even 0x300 */
    u32 off = m << 2;
    ch->regs[off / 4] = v;
    if (off == 0x300) {
        if (ch->is_vic)
            vic_execute(ch->regs);
        else
            nvdec_execute(ch->dec, ch->regs);
    }
}
/* the host class's syncpoint wait, without the engines' lock: the job it waits for may be another channel's. The
   console waits as long as it takes. Here a wait gives up after two seconds, as the GPU's stream waits do (gpu_park),
   so a fence nothing reaches can't stop the channel for good */
static void wait_syncpt(H1Ch* ch, u32 id, u32 thresh) {
    pm_unlock(&g_hlk);
    if (!syncpt_wait(id, thresh, 2000000000))
        WARN_LIMIT(8, "video: %s job waited 2 s for syncpoint %u to reach %u (it is at %u), and goes on\n",
                   ch->is_vic ? "vic" : "nvdec", id, thresh, syncpt_read(id));
    pm_lock(&g_hlk);
}
static void reg_write(H1Ch* ch, u32 cls, u32 off, u32 v) {
    /* the host class: its waits are honored (host1x's WAIT_SYNCPT, LOAD_SYNCPT_PAYLOAD_32 and WAIT_SYNCPT_32), and
       its increments come from the submit's list once the job has run */
    if (cls == 0x1) {
        if (off == 0x08)
            wait_syncpt(ch, v >> 24, v & 0xFFFFFF);
        else if (off == 0x4E)
            ch->payload = v;
        else if (off == 0x50)
            wait_syncpt(ch, v, ch->payload);
        return;
    }
    if (off == 0x10)
        ch->method = v;
    else if (off == 0x11)
        dev_method(ch, ch->method, v);
}
static void run_words(H1Ch* ch, const u32* w, u32 n) {
    u32 cls = ch->is_vic ? 0x5D : 0xF0;
    for (u32 i = 0; i < n;) {
        u32 x = w[i++], op = x >> 28;
        switch (op) {
            case 0: {
                cls = (x >> 6) & 0x3FF;
                u32 off = (x >> 16) & 0xFFF, mask = x & 0x3F;
                for (int b = 0; b < 6; b++)
                    if (mask & (1u << b)) {
                        if (i >= n) return;
                        reg_write(ch, cls, off + b, w[i++]);
                    }
                break;
            }
            case 1: {
                u32 off = (x >> 16) & 0xFFF, c = x & 0xFFFF;
                for (u32 k = 0; k < c && i < n; k++) reg_write(ch, cls, off + k, w[i++]);
                break;
            }
            case 2: {
                u32 off = (x >> 16) & 0xFFF, c = x & 0xFFFF;
                for (u32 k = 0; k < c && i < n; k++) reg_write(ch, cls, off, w[i++]);
                break;
            }
            case 3: {
                u32 off = (x >> 16) & 0xFFF, mask = x & 0xFFFF;
                for (int b = 0; b < 16; b++)
                    if (mask & (1u << b)) {
                        if (i >= n) return;
                        reg_write(ch, cls, off + b, w[i++]);
                    }
                break;
            }
            case 4: reg_write(ch, cls, (x >> 16) & 0xFFF, x & 0xFFFF); break;
            default: {
                LOG_LIMIT(6, "video: host1x opcode %u not handled (word %08x)\n", op, x);
                return;
            }
        }
    }
}

/* ------------------------------------------------------------ channel ioctls */
H1Ch* host1x_open(int is_vic) {
    H1Ch* ch = calloc(1, sizeof *ch);
    if (!ch) return NULL;
    ch->is_vic = is_vic;
    if (!is_vic && !(ch->dec = nvdec_new())) {
        free(ch);
        return NULL;
    }
    pm_init(&ch->lk);
    pc_init(&ch->cv);
    return ch;
}
/* a job's command buffers, then the syncpoint increments that say it is done */
static void job_run(H1Ch* ch, const H1Job* j) {
    const u32* inc = j->w + j->ncb;
    const u32* words = inc + (u64)j->nsp * 2;
    pm_lock(&g_hlk);
    for (u32 i = 0; i < j->ncb; i++) {
        run_words(ch, words, j->w[i]);
        words += j->w[i];
    }
    pm_unlock(&g_hlk);
    for (u32 i = 0; i < j->nsp; i++) syncpt_add(inc[i * 2], inc[i * 2 + 1]);
}
/* the channel's worker: its jobs in order, until the fd closes and the queue is empty */
static void ch_worker(void* p) {
    H1Ch* ch = p;
    pm_lock(&ch->lk);
    for (;;) {
        while (ch->head == ch->tail && !ch->closing) pc_wait(&ch->cv, &ch->lk, -1);
        if (ch->head == ch->tail) break;
        H1Job* j = ch->q[ch->tail % H1_QUEUE];
        pm_unlock(&ch->lk);
        job_run(ch, j);
        free(j);
        pm_lock(&ch->lk);
        ch->tail++;
        pc_broadcast(&ch->cv); /* room for a submit, or the close waiting for the queue */
    }
    ch->worker = 0;
    pc_broadcast(&ch->cv);
    pm_unlock(&ch->lk);
}
void host1x_close(H1Ch* ch) {
    if (!ch) return;
    /* the jobs queued run first: the game may still wait for their increments, and the syncpoints go back below */
    pm_lock(&ch->lk);
    ch->closing = 1;
    pc_broadcast(&ch->cv);
    while (ch->worker) pc_wait(&ch->cv, &ch->lk, -1);
    pm_unlock(&ch->lk);
    pm_lock(&g_hlk);
    for (int i = 0; i < H1_NSYNCPT; i++)
        if (ch->syncpt[i]) syncpt_free(ch->syncpt[i]);
    nvdec_free(ch->dec);
    pm_unlock(&g_hlk);
    free(ch);
}
/* syncpoints come from nvdrv's allocator: NvRmStream indexes a 128-entry table by syncpoint id, and an id past it
   (upstream used 200) writes beyond the table into the game's heap. A channel's go back when its fd closes: four
   channels per movie would otherwise use up the ids after a few dozen movies */
static u32 ch_syncpt(H1Ch* ch, u32 i) {
    if (!ch->syncpt[i]) ch->syncpt[i] = syncpt_alloc();
    return ch->syncpt[i];
}
/* whether the channel owns syncpoint id (GET_SYNCPOINT handed it out): the only ones its submits may increment */
static int ch_owns(const H1Ch* ch, u32 id) {
    for (int i = 0; id && i < H1_NSYNCPT; i++)
        if (ch->syncpt[i] == id) return 1;
    return 0;
}
/* SUBMIT: counts, cmdbufs {memid, offset, words}, relocs, reloc shifts, syncpt incrs {id, n, ?x3}, fence thresholds
   (u32 each, written back). The job is copied and queued for the channel's worker, with ch->lk held */
static u32 ch_submit(H1Ch* ch, u8* d, u32 n) {
    const char* eng = ch->is_vic ? "vic" : "nvdec";
    u32 ncb = rd32(d, 0), nrel = rd32(d, 4), nsp = rd32(d, 8), nfe = rd32(d, 12);
    /* in 64 bits: counts from the game can wrap a u32 sum back under n */
    u64 cbo = 16, spo = cbo + (u64)ncb * 12 + (u64)nrel * 16 + (u64)nrel * 4, feo = spo + (u64)nsp * 20;
    if (feo > n) return 4; /* BadParameter */
    /* an increment of a syncpoint the channel doesn't own refuses the whole submit, before it runs */
    u32 k = 0;
    while (k < nsp && ch_owns(ch, rd32(d, (u32)(spo + (u64)k * 20)))) k++;
    if (k < nsp) {
        WARN_LIMIT(4, "video: %s submit increments syncpoint %u, not one of its channel's, refused\n", eng,
                   rd32(d, (u32)(spo + (u64)k * 20)));
        return 4;
    }
    if (nrel) LOG_LIMIT(1, "video: %s submit has %u relocations, which aren't applied\n", eng, nrel);
    /* the command buffers' words: a buffer that can't be read is left out (length 0), and the job's increments still
       happen so the game doesn't wait forever on them. src keeps where each lies in guest memory (0 left out): a second
       nvmap_get could find the handle freed or changed by another thread meanwhile. The memory stays guest memory */
    u64* src = malloc((ncb ? ncb : 1) * sizeof *src);
    if (!src) return 6; /* InsufficientMemory */
    u64 words = 0;
    for (u32 i = 0; i < ncb; i++) {
        u32 cb = (u32)(cbo + (u64)i * 12);
        u32 mem = rd32(d, cb), off = rd32(d, cb + 4), wc = rd32(d, cb + 8);
        u64 a;
        u32 msz;
        nvmap_get(mem, &a, &msz);
        /* host1x gathers words: an offset that isn't one would read u32s at a misaligned address */
        if (!a || !wc || wc > 0x100000 || (off & 3) || (u64)off + (u64)wc * 4 > msz) {
            LOG_LIMIT(4, "video: %s command buffer skipped (nvmap %u, offset %x, %u words, buffer %u bytes)\n", eng,
                      mem, off, wc, msz);
            src[i] = 0;
            continue;
        }
        src[i] = a + off;
        words += wc;
    }
    u64 nw = (u64)ncb + (u64)nsp * 2 + words;
    H1Job* j = nw > (1u << 24) ? NULL : malloc(sizeof *j + nw * sizeof j->w[0]); /* 64 MB of commands is no job */
    if (!j) {
        WARN_LIMIT(4, "video: %s submit of %llu words refused\n", eng, (unsigned long long)nw);
        free(src);
        return nw > (1u << 24) ? 4 : 6;
    }
    j->ncb = ncb;
    j->nsp = nsp;
    u32* inc = j->w + ncb;
    u32* w = inc + (u64)nsp * 2;
    for (u32 i = 0; i < ncb; i++) {
        u32 wc = src[i] ? rd32(d, (u32)(cbo + (u64)i * 12) + 8) : 0;
        if (wc) memcpy(w, GP(src[i]), (size_t)wc * 4);
        j->w[i] = wc;
        w += wc;
    }
    free(src);
    while (ch->head - ch->tail == H1_QUEUE) pc_wait(&ch->cv, &ch->lk, -1);
    /* each fence is the value its syncpoint reaches with this job's increments, promised in the order the jobs run */
    for (u32 i = 0; i < nsp; i++) {
        u32 sp = (u32)(spo + (u64)i * 20);
        u32 id = rd32(d, sp), v = syncpt_promise(id, rd32(d, sp + 4));
        inc[i * 2] = id;
        inc[i * 2 + 1] = rd32(d, sp + 4);
        if (i < nfe && feo + (u64)i * 4 + 4 <= n) memcpy(d + feo + (u64)i * 4, &v, 4);
    }
    ch->q[ch->head % H1_QUEUE] = j;
    ch->head++;
    if (!ch->worker) {
        ch->worker = 1;
        if (plat_thread(ch_worker, ch, (size_t)1 << 20)) fatal("could not start a video engine's thread");
    }
    pc_broadcast(&ch->cv);
    LOG_LIMIT(3, "video: %s submit: %u command buffers, %u relocs, %u syncpoint increments, %u fences (%u bytes)\n",
              eng, ncb, nrel, nsp, nfe, n);
    return 0;
}
u32 host1x_ioctl(H1Ch* ch, u32 cmd, u8* d, u32 n) {
    int is_vic = ch->is_vic;
    pm_lock(&ch->lk);
    u32 r = 0, grp = (cmd >> 8) & 0xFF, nr = cmd & 0xFF;
    if (grp == 0x48) {
        if (nr != 0x01) /* SET_NVMAP_FD: nvmap is one device here */
            LOG_LIMIT(8, "video: %s ioctl %08x not handled\n", is_vic ? "vic" : "nvdec", cmd);
    } else if (grp == 0)
        switch (nr) {
            case 0x01: r = ch_submit(ch, d, n); break;
            case 0x02: { /* GET_SYNCPOINT: {u32 index, u32 id} */
                u32 i = rd32(d, 0);
                if (i >= H1_NSYNCPT) {
                    r = 4; /* BadParameter */
                    break;
                }
                u32 v = ch_syncpt(ch, i);
                memcpy(d + 4, &v, 4);
                break;
            }
            case 0x03: {
                u32 v = 0;
                memcpy(d + 4, &v, 4);
                break;
            } /* GET_WAITBASE */
            case 0x07: break; /* SET_SUBMIT_TIMEOUT */
            case 0x09: {      /* MAP_BUFFER: the SDK reads only the result, which a handle that can't be mapped fails */
                u32 ne = rd32(d, 0);
                for (u32 i = 0; i < ne && 12 + (u64)i * 8 + 8 <= n; i++) {
                    u32 h = rd32(d, 12 + i * 8), err = 0;
                    u32 io = iova_map(h, &err);
                    memcpy(d + 12 + i * 8 + 4, &io, 4);
                    if (err && !r) r = err;
                }
                break;
            }
            /* UNMAP_BUFFER: the mapping stays until nvmap frees the handle (host1x_forget_handle). NVDEC and VIC map
               the same handles and unmap them after every job, so dropping it here would take it from the other */
            case 0x0A: break;
            default: {
                LOG_LIMIT(8, "video: %s ioctl %08x not handled\n", is_vic ? "vic" : "nvdec", cmd);
                break;
            }
        }
    else { LOG_LIMIT(8, "video: %s ioctl %08x not handled\n", is_vic ? "vic" : "nvdec", cmd); }
    pm_unlock(&ch->lk);
    return r;
}
