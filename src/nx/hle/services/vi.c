/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* vi + the nvnflinger buffer queue. There's no compositor: QueueBuffer has the GPU thread show the frame once the work
   queued before it is done, and DequeueBuffer waits out the last frame's turn on the display (its swap interval) before
   handing the game a buffer for the next, as the console's compositor holds it (present_pace). */
#include "nx/hle/services/vi.h"
#include "nx/runtime/main.h"
#include "nx/runtime/options.h"
#include "nx/gpu/gpu.h"
#include "nx/hle/services/nvdrv.h"
#include "nx/hle/services/services.h"
#include "nx/hle/kernel/ipc.h"
#include "nx/hle/services/services_int.h"
#include "hle/services/vi.swipc.inc"
#include "nx/hle/kernel/kernel.h"
#include "nx/memory/mem.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    int used;
    u32 afmt;
    u32 nvmap_id;
    u32 w, h, stride, fmt, layout, bhl, pitch, offset;
    u64 size;
    u8 raw[0x16C];
    u32 rawsize;
    int queued;    /* held: dequeued by the game, or queued and not shown yet */
    u32 gen;       /* counts dequeues, so an earlier present does not free the slot */
    i32 crop[4];   /* the part of the buffer shown (left, top, right, bottom); all 0 for the whole buffer */
    u32 transform; /* the queued frame's Android buffer transform, retained with its crop */
} Slot;
static struct {
    Slot slots[64]; /* lock */
    KEvent* vsync_ev;
    KEvent* bufavail_ev;
    PMutex lock;
    int width, height;
    PCond free_cv;
    /* the last crop and transform QUEUE_BUFFER logged (lock) */
    u32 crop_transform;
    i32 crop[4];
    u32 interval;                   /* the last queued frame's swap interval: the next DEQUEUE_BUFFER's pace (lock) */
    u64 managed_layer, stray_layer; /* the next CreateManagedLayer and CreateStrayLayer ids */
} vi = {.width = 1280,
        .height = 720,
        .crop_transform = 0xFFFFFFFFu,
        .interval = 1,
        .managed_layer = 100,
        .stray_layer = 200};

/* ============================================================ parcels */
typedef struct {
    u8* d;
    u32 size, pos;
} PR;
/* reads stop at the end of the parcel: a read past it gives 0 and leaves pos at the end */
static u32 pr_u32(PR* p) {
    u32 v = 0;
    if (p->size - p->pos >= 4) {
        memcpy(&v, p->d + p->pos, 4);
        p->pos += 4;
    } else
        p->pos = p->size;
    return v;
}
/* n bytes of the parcel, or NULL when it has fewer left */
static const u8* pr_ptr(PR* p, u32 n) {
    if (p->size - p->pos < n) {
        p->pos = p->size;
        return NULL;
    }
    const u8* r = p->d + p->pos;
    p->pos += n;
    return r;
}
static void pr_skip_token(PR* p) {
    pr_u32(p);
    i32 n = (i32)pr_u32(p);
    if (n <= 0) return;
    u64 skip = (((u64)n + 1) * 2 + 3) & ~3ull; /* the UTF-16 string and its terminator, padded to 4 */
    p->pos = skip > p->size - p->pos ? p->size : p->pos + (u32)skip;
}
typedef struct {
    u8 d[0x800];
    u32 n;
} PW;
static void pw_u32(PW* w, u32 v) {
    memcpy(w->d + w->n, &v, 4);
    w->n += 4;
}
static void pw_bytes(PW* w, const void* p, u32 n) {
    memcpy(w->d + w->n, p, n);
    w->n += (n + 3) & ~3u;
}
static void pw_flat(PW* w, const void* p, u32 n) {
    pw_u32(w, n);
    pw_u32(w, 0);
    pw_bytes(w, p, n);
}
static void pw_out(PW* w, u64 addr, u64 size) {
    u32 hdr[4] = {w->n, 0x10, 0, 0x10 + w->n};
    if (!addr || size < 0x10 + w->n) {
        WARN("binder: reply parcel (%u bytes) does not fit the game's buffer (%llu bytes)\n", 0x10 + w->n,
             (unsigned long long)size);
        return;
    }
    u8* o = GP(addr);
    memcpy(o, hdr, 16);
    memcpy(o + 0x10, w->d, w->n);
}

/* ============================================================ present */
/* a slot is free again: after its frame is shown, or cancelled. gen is the dequeue the frame came from */
static void slot_release(i32 slot, u32 gen) {
    pm_lock(&vi.lock);
    if (slot >= 0 && slot < 64 && vi.slots[slot].gen == gen) vi.slots[slot].queued = 0;
    pc_broadcast(&vi.free_cv);
    pm_unlock(&vi.lock);
    kevent_signal(vi.bufavail_ev);
}
/* a queued frame, shown on the GPU thread after the work before it, with the slot as it was queued */
typedef struct {
    i32 slot;
    Slot s;
    u32 nf, id[4], val[4];
} QueuedFrame;
static void present_queued(void* p) {
    QueuedFrame* q = p;
    /* nv_gpu_call_after left only fences the submissions before this reach, and they have all run: one not reached
       now fell short, and waiting here would only stall the GPU thread */
    for (u32 i = 0; i < q->nf; i++)
        if (q->id[i] != 0xFFFFFFFFu && !syncpt_wait(q->id[i], q->val[i], 0))
            WARN_LIMIT(8, "binder: QUEUE_BUFFER slot %d: fence syncpoint %u is at %u, short of %u, presenting anyway\n",
                       q->slot, q->id[i], syncpt_read(q->id[i]), q->val[i]);
    Slot* s = &q->s;
    if (s->used)
        vi_show_buffer(s->nvmap_id, s->offset, s->w, s->h, s->pitch, s->layout, s->bhl, s->afmt, s->crop, s->transform);
    slot_release(q->slot, s->gen);
    free(q);
}

static int binder_transact(u32 id, u32 code, PR* in, PW* out) {
    (void)id;
    switch (code) {
        case 10: { /* CONNECT */
            u32 qo[4] = {(u32)vi.width, (u32)vi.height, 0, 0};
            pw_bytes(out, qo, 16);
            pw_u32(out, 0);
            return 0;
        }
        case 11: pw_u32(out, 0); return 0;
        case 14: { /* SET_PREALLOCATED_BUFFER */
            i32 slot = (i32)pr_u32(in);
            u32 has = pr_u32(in);
            if (slot >= 0 && slot < 64) {
                Slot ns;
                memset(&ns, 0, sizeof ns);
                Slot* s = &ns;
                u32 len = has ? pr_u32(in) : 0;
                if (has) pr_u32(in);
                /* the fields below reach 0x84 bytes into the GraphicBuffer */
                const u8* g = has ? pr_ptr(in, len) : NULL;
                if (has && (!g || len < 0x84)) {
                    WARN("vi: buffer %d: GraphicBuffer of %u bytes is too short\n", slot, len);
                    g = NULL;
                }
                if (g) {
                    if (g_verbose >= 2) {
                        LOG("graphicbuffer len=%x:", len);
                        for (int k = 0; k < 0x60; k += 4) {
                            u32 q;
                            memcpy(&q, g + k, 4);
                            LOG(" %x", q);
                        }
                        LOG("\n");
                    }
                    if (len > sizeof s->raw) len = sizeof s->raw;
                    memcpy(s->raw, g, len);
                    s->rawsize = len;
                    /* NvGraphicBuffer layout: hdr(12) unk0 nvmap_id unk2 magic pid type usage format ext_format stride total_size num_planes unk12 planes[] */
                    /* GraphicBuffer: 'GBFR' + 10 words, followed by the NvGraphicBuffer minus its NativeHandle */
                    const u8* nb = g + 0x1C;
                    memcpy(&s->afmt, g + 0x10, 4);
                    memcpy(&s->nvmap_id, nb + 0x10, 4);
                    memcpy(&s->stride, nb + 0x30, 4);
                    const u8* pl = nb + 0x40;
                    memcpy(&s->w, pl, 4);
                    memcpy(&s->h, pl + 4, 4);
                    u64 cf;
                    memcpy(&cf, pl + 8, 8);
                    s->fmt = (u32)cf;
                    memcpy(&s->layout, pl + 0x10, 4);
                    memcpy(&s->pitch, pl + 0x14, 4);
                    memcpy(&s->offset, pl + 0x1C, 4);
                    memcpy(&s->bhl, pl + 0x24, 4);
                    s->used = 1;
                    LOG("vi: buffer %d  %ux%u pitch=%u layout=%u bhl=%u fmt=%x nvmap=%u\n", slot, s->w, s->h, s->pitch,
                        s->layout, s->bhl, s->fmt, s->nvmap_id);
                }
                pm_lock(&vi.lock);
                ns.gen = vi.slots[slot].gen; /* a present still queued from the old buffer mustn't free a new dequeue */
                vi.slots[slot] = ns;
                pm_unlock(&vi.lock);
            }
            pw_u32(out, 0);
            return 0;
        }
        case 1: { /* REQUEST_BUFFER */
            i32 slot = (i32)pr_u32(in);
            pm_lock(&vi.lock);
            Slot* s = (slot >= 0 && slot < 64) ? &vi.slots[slot] : NULL;
            if (s && s->used) {
                pw_u32(out, 1);
                pw_flat(out, s->raw, s->rawsize);
            } else
                pw_u32(out, 0);
            pm_unlock(&vi.lock);
            pw_u32(out, 0);
            return 0;
        }
        case 3: { /* DEQUEUE_BUFFER */
            /* a slot is held from DEQUEUE until its frame is shown or cancelled, and with every buffer held this waits
               for one, as the console's buffer queue does. Handing out a held buffer instead lets the game draw into
               a frame the GPU thread has yet to show. The wait ends: the GPU thread reaches every queued present,
               since each of its stream waits gives up after a few seconds (gpu_park). A long one means the GPU
               thread is behind, and the log says so with the slots held. First the last frame's turn on the display
               is waited out, without vi's lock */
            pm_lock(&vi.lock);
            u32 pace = vi.interval;
            pm_unlock(&vi.lock);
            present_pace(pace);
            pm_lock(&vi.lock);
            int k = -1, nused = 0;
            for (int i = 0; i < 64; i++) nused += vi.slots[i].used;
            if (!nused) { /* no buffers yet: nothing would ever free one */
                pm_unlock(&vi.lock);
                WARN("binder: DEQUEUE_BUFFER before any buffer was set\n");
                pw_u32(out, 0);
                pw_u32(out, 0);
                pw_u32(out, (u32)-19); /* NO_INIT */
                return 0;
            }
            for (;;) {
                for (int i = 0; i < 64; i++)
                    if (vi.slots[i].used && !vi.slots[i].queued) {
                        k = i;
                        break;
                    }
                if (k >= 0) break;
                if (pc_wait(&vi.free_cv, &vi.lock, 1000000000)) {
                    u64 held = 0;
                    nused = 0;
                    for (int i = 0; i < 64; i++) {
                        nused += vi.slots[i].used;
                        if (vi.slots[i].used && vi.slots[i].queued) held |= 1ull << i;
                    }
                    if (!nused) { /* the buffers were detached meanwhile (SET_PREALLOCATED_BUFFER): none will come */
                        pm_unlock(&vi.lock);
                        pw_u32(out, 0);
                        pw_u32(out, 0);
                        pw_u32(out, (u32)-19); /* NO_INIT */
                        return 0;
                    }
                    WARN_LIMIT(8,
                               "binder: DEQUEUE_BUFFER found all %d buffers held (slots %llx) for 1 s: still waiting\n",
                               nused, (unsigned long long)held);
                }
            }
            vi.slots[k].queued = 1;
            vi.slots[k].gen++;
            pm_unlock(&vi.lock);
            pw_u32(out, (u32)k);
            pw_u32(out, 1);
            u32 fence[9];
            memset(fence, 0, sizeof fence);
            fence[1] = 0xFFFFFFFFu;
            for (int i = 1; i < 4; i++) fence[1 + i * 2] = 0xFFFFFFFFu;
            pw_flat(out, fence, 0x24);
            pw_u32(out, 0);
            return 0;
        }
        case 7: { /* QUEUE_BUFFER */
            i32 slot = (i32)pr_u32(in);
            u32 len = pr_u32(in);
            pr_u32(in);
            const u8* qi = pr_ptr(in, len);
            if (!qi) len = 0;
            QueuedFrame* q = calloc(1, sizeof *q);
            if (!q) fatal("Out of memory queueing a frame");
            q->slot = slot;
            /* QueueBufferInput's swap interval (after async at 0x28): NVN's present interval, which the compositor
               holds the frame on the display for. A game made for 30 FPS asks for 2. An s32: a negative one is no
               interval the console takes, and one over 4 (15 FPS) a misread parcel that mustn't stall presenting */
            i32 interval = 1;
            if (len >= 0x30) memcpy(&interval, qi + 0x2C, 4);
            if (interval < 0) interval = 1;
            if (interval > 4) interval = 4;
            if (len >= 0x54) { /* the frame's fences */
                memcpy(&q->nf, qi + 0x30, 4);
                if (q->nf > 4) q->nf = 4;
                for (u32 i = 0; i < q->nf; i++) {
                    memcpy(&q->id[i], qi + 0x34 + i * 8, 4);
                    memcpy(&q->val[i], qi + 0x38 + i * 8, 4);
                }
            }
            /* Crop and transform belong to this queued frame, not to the slot's next dequeue. */
            i32 cr[4] = {0, 0, 0, 0};
            u32 tr = 0;
            if (len >= 0x28) {
                memcpy(cr, qi + 0x0C, 16);
                memcpy(&tr, qi + 0x20, 4);
            }
            pm_lock(&vi.lock);
            if (slot >= 0 && slot < 64) {
                q->s = vi.slots[slot];
                memcpy(q->s.crop, cr, 16);
                q->s.transform = tr;
            }
            int changed = len >= 0x28 && (vi.crop_transform != tr || memcmp(vi.crop, cr, 16));
            if (changed) {
                vi.crop_transform = tr;
                memcpy(vi.crop, cr, 16);
            }
            int new_interval = vi.interval != (u32)interval;
            vi.interval = (u32)interval;
            pm_unlock(&vi.lock);
            if (new_interval) LOG_LIMIT(16, "vi: swap interval %d\n", interval); /* a game may switch every frame */
            if (changed) LOG("vi: crop (%d,%d)-(%d,%d), transform %x\n", cr[0], cr[1], cr[2], cr[3], tr);
            /* the compositor shows the frame once the GPU has drawn it (its acquire waits on the frame's fence), and
               QueueBuffer returns at once: shown here, it held the game's queue lock while the GPU thread caught up,
               so its CPU and GPU work never overlapped. A fence no submission promised is waited for here instead */
            nv_gpu_call_after(q->nf, q->id, q->val, present_queued, q);
            u32 qo[4] = {(u32)vi.width, (u32)vi.height, 0, 0};
            pw_bytes(out, qo, 16);
            pw_u32(out, 0);
            return 0;
        }
        case 8: { /* CANCEL_BUFFER: the slot is free again, as after a present */
            i32 slot = (i32)pr_u32(in);
            pm_lock(&vi.lock);
            u32 gen = slot >= 0 && slot < 64 ? vi.slots[slot].gen : 0;
            pm_unlock(&vi.lock);
            slot_release(slot, gen);
            pw_u32(out, 0);
            return 0;
        }
        case 9: { /* QUERY: width, height, format, min undequeued buffers; 6/7 the default width and height */
            u32 what = pr_u32(in);
            u32 v = what == 0 || what == 6   ? (u32)vi.width
                    : what == 1 || what == 7 ? (u32)vi.height
                    : what == 2 || what == 3 ? 1
                                             : 0;
            pw_u32(out, v);
            pw_u32(out, 0);
            return 0;
        }
        case 2:
        case 13:
        case 12: pw_u32(out, 0); return 0;
        /* GET_BUFFER_HISTORY: the status alone, no frames, as Eden answers by default (the console's reply is
           unmeasured). Ender Magnolia asks once a frame, so it is no warning */
        case 17: pw_u32(out, 0); return 0;
    }
    WARN("binder: code %u not implemented\n", code);
    pw_u32(out, 0);
    return 0;
}

/* a transaction: the parcel header {data size, data offset, objects size, objects offset}, and its data has to be in
   the buffer */
static int transact(i32 id, u32 code, IpcBuf parcel, IpcBuf reply) {
    if (!parcel.addr || parcel.size < 16) {
        WARN("binder: transaction %u without a parcel\n", code);
        return RC(114, 1);
    }
    PR in;
    u32 hdr[4];
    u64 is = parcel.size;
    memcpy(hdr, GP(parcel.addr), 16);
    in.d = GP(parcel.addr) + (hdr[1] < is ? hdr[1] : is);
    in.size = hdr[1] < is ? (u32)(is - hdr[1] < hdr[0] ? is - hdr[1] : hdr[0]) : 0;
    in.pos = 0;
    pr_skip_token(&in);
    PW* out = calloc(1, sizeof(PW));
    if (!out) fatal("Out of memory answering a binder transaction");
    binder_transact((u32)id, code, &in, out);
    pw_out(out, reply.addr, reply.size);
    free(out);
    return 0;
}
static int nns_hosbinder_IHOSBinderDriver_TransactParcel(SvcObj* self, i32 binder_id, u32 code, IpcBuf parcel,
                                                         IpcBuf reply, u32 flags) {
    return transact(binder_id, code, parcel, reply);
}
static int nns_hosbinder_IHOSBinderDriver_TransactParcelAuto(SvcObj* self, i32 binder_id, u32 code, IpcBuf parcel,
                                                             IpcBuf reply, u32 flags) {
    return transact(binder_id, code, parcel, reply);
}
static int nns_hosbinder_IHOSBinderDriver_GetNativeHandle(SvcObj* self, i32 binder_id, u32 type_id, u32* event) {
    *event = h_new(&vi.bufavail_ev->o);
    return 0;
}

static int nn_visrv_sf_ISystemDisplayService_GetZOrderCountMin(SvcObj* self, u64 display_id, i64* count) {
    *count = 1;
    return 0;
}
static int nn_visrv_sf_ISystemDisplayService_GetZOrderCountMax(SvcObj* self, u64 display_id, i64* count) {
    *count = 255;
    return 0;
}
static int nn_visrv_sf_ISystemDisplayService_GetDisplayLogicalResolution(SvcObj* self, u64 display_id, i32* width,
                                                                         i32* height) {
    *width = vi.width;
    *height = vi.height;
    return 0;
}
static int nn_visrv_sf_ISystemDisplayService_GetDisplayMode(SvcObj* self, u64 display_id, nn_vi_DisplayModeInfo* mode) {
    mode->width = (u32)vi.width;
    mode->height = (u32)vi.height;
    mode->refresh_rate = (float)VI_REFRESH_HZ;
    return 0;
}
static int nn_visrv_sf_IManagerDisplayService_CreateManagedLayer(SvcObj* self, u32 layer_flags, u64 display_id,
                                                                 nn_applet_AppletResourceUserId aruid, u64* layer_id) {
    *layer_id = __atomic_fetch_add(&vi.managed_layer, 1, __ATOMIC_RELAXED);
    return 0;
}
static int nn_visrv_sf_IManagerDisplayService_AcquireLayerTexturePresentingEvent(SvcObj* self, u64 layer_id,
                                                                                 u32* event) {
    return new_event(event, "vi display", 0);
}
static int nn_visrv_sf_IManagerDisplayService_GetDisplayHotplugEvent(SvcObj* self, u64 display_id, u32* event) {
    return new_event(event, "vi display", 0);
}
static int nn_visrv_sf_IManagerDisplayService_GetDisplayModeChangedEvent(SvcObj* self, u64 display_id, u32* event) {
    return new_event(event, "vi display", 0);
}

/* the native window parcel of a layer, in the game's buffer: HOS Parcel: flat_binder_object {type=2, flags,
   u64 handle, u64 0, char name[8]="dispdrv", ...} + object table {0}. Its size is the reply, written even when the
   buffer has no room for it */
static int native_window(IpcBuf b, u32 binder, i64* size) {
    *size = 0x3C;
    if (!b.addr || b.size < 0x3C) { /* the parcel: its 0x10 header, 0x28 bytes of data, the 4 of its object table */
        WARN("vi: no room for the native window parcel (%llu bytes)\n", (unsigned long long)b.size);
        return 0;
    }
    u32 data[10];
    memset(data, 0, sizeof data);
    data[0] = 2;
    data[1] = 1;
    data[2] = binder;
    data[3] = 0;
    memcpy(&data[6], "dispdrv\0", 8);
    u32 hdr[4] = {0x28, 0x10, 4, 0x38};
    u8* p = GP(b.addr);
    memset(p, 0, b.size < 0x100 ? b.size : 0x100);
    memcpy(p, hdr, 16);
    memcpy(p + 0x10, data, 0x28);
    return 0;
}
static int binder_driver(SvcObj** service) {
    *service = nns_hosbinder_IHOSBinderDriver_new(0);
    return 0;
}
static int nn_visrv_sf_IApplicationDisplayService_GetRelayService(SvcObj* self, SvcObj** service) {
    return binder_driver(service);
}
static int nn_visrv_sf_IApplicationDisplayService_GetIndirectDisplayTransactionService(SvcObj* self, SvcObj** service) {
    return binder_driver(service);
}
static int nn_visrv_sf_IApplicationDisplayService_GetSystemDisplayService(SvcObj* self, SvcObj** service) {
    *service = nn_visrv_sf_ISystemDisplayService_new(0);
    return 0;
}
static int nn_visrv_sf_IApplicationDisplayService_GetManagerDisplayService(SvcObj* self, SvcObj** service) {
    *service = nn_visrv_sf_IManagerDisplayService_new(0);
    return 0;
}
/* the one display, "Default" */
static int nn_visrv_sf_IApplicationDisplayService_ListDisplays(SvcObj* self, IpcBuf displays, i64* count) {
    if (displays.addr) {
        u8 info[0x60];
        memset(info, 0, sizeof info);
        strcpy((char*)info, "Default");
        info[0x40] = 1;
        u64 lc = 1, w = (u64)vi.width, h = (u64)vi.height;
        memcpy(info + 0x48, &lc, 8);
        memcpy(info + 0x50, &w, 8);
        memcpy(info + 0x58, &h, 8);
        memcpy(GP(displays.addr), info, displays.size < 0x60 ? displays.size : 0x60);
    }
    *count = 1;
    return 0;
}
static int nn_visrv_sf_IApplicationDisplayService_OpenDisplay(SvcObj* self, const nn_vi_DisplayName* name,
                                                              u64* display_id) {
    *display_id = 1;
    return 0;
}
static int nn_visrv_sf_IApplicationDisplayService_OpenDefaultDisplay(SvcObj* self, u64* display_id) {
    *display_id = 1;
    return 0;
}
static int nn_visrv_sf_IApplicationDisplayService_GetDisplayResolution(SvcObj* self, u64 display_id, i64* width,
                                                                       i64* height) {
    *width = vi.width;
    *height = vi.height;
    return 0;
}
static int nn_visrv_sf_IApplicationDisplayService_OpenLayer(SvcObj* self, const nn_vi_DisplayName* display_name,
                                                            u64 layer_id, u64 aruid, IpcBuf native_window_buf,
                                                            i64* native_window_size) {
    return native_window(native_window_buf, 1, native_window_size);
}
static int nn_visrv_sf_IApplicationDisplayService_CreateStrayLayer(SvcObj* self, u32 layer_flags, u64 display_id,
                                                                   IpcBuf native_window_buf, u64* layer_id,
                                                                   i64* native_window_size) {
    *layer_id = __atomic_fetch_add(&vi.stray_layer, 1, __ATOMIC_RELAXED);
    return native_window(native_window_buf, 1, native_window_size);
}
/* nn::vi's ScalingMode to the layer's: None, Freeze, ScaleToWindow, ScaleAndCrop, PreserveAspect */
static int nn_visrv_sf_IApplicationDisplayService_ConvertScalingMode(SvcObj* self, u32 mode, u64* converted) {
    static const u8 conv[5] = {3, 0, 1, 2, 4};
    if (mode >= 5) return RC(114, 1);
    *converted = conv[mode];
    return 0;
}
static int nn_visrv_sf_IApplicationDisplayService_GetIndirectLayerImageRequiredMemoryInfo(SvcObj* self, i64 width,
                                                                                          i64 height, i64* size,
                                                                                          i64* alignment) {
    *size = 0x1000;
    *alignment = 0x1000;
    return 0;
}
static int nn_visrv_sf_IApplicationDisplayService_GetDisplayVsyncEvent(SvcObj* self, u64 display_id, u32* event) {
    *event = h_new(&vi.vsync_ev->o);
    return 0;
}
static int nn_visrv_sf_IApplicationDisplayService_GetDisplayVsyncEventForDebug(SvcObj* self, u64 display_id,
                                                                               u32* event) {
    *event = h_new(&vi.vsync_ev->o);
    return 0;
}
static int display_service(SvcObj** service) {
    *service = nn_visrv_sf_IApplicationDisplayService_new(0);
    return 0;
}
static int nn_visrv_sf_IApplicationRootService_GetDisplayServiceWithWorkaround(SvcObj* self, u32 policy,
                                                                               SvcObj** service) {
    return display_service(service);
}
static int nn_visrv_sf_IApplicationRootService_GetDisplayService(SvcObj* self, u32 policy, SvcObj** service) {
    return display_service(service);
}
static int nn_visrv_sf_ISystemRootService_GetDisplayService(SvcObj* self, u32 policy, SvcObj** service) {
    return display_service(service);
}
static int nn_visrv_sf_ISystemRootService_GetDisplayServiceWithProxyNameExchange(SvcObj* self,
                                                                                 const nn_vi_ProxyName* proxy_name,
                                                                                 u32 policy, SvcObj** service) {
    return display_service(service);
}
static int nn_visrv_sf_IManagerRootService_GetDisplayService(SvcObj* self, u32 policy, SvcObj** service) {
    return display_service(service);
}
static int nn_visrv_sf_IManagerRootService_GetDisplayServiceWithProxyNameExchange(SvcObj* self,
                                                                                  const nn_vi_ProxyName* proxy_name,
                                                                                  u32 policy, SvcObj** service) {
    return display_service(service);
}
static SvcObj* new_vi_u(void) { return nn_visrv_sf_IApplicationRootService_new(0); }
static SvcObj* new_vi_s(void) { return nn_visrv_sf_ISystemRootService_new(0); }
static SvcObj* new_vi_m(void) { return nn_visrv_sf_IManagerRootService_new(0); }

void vi_vsync_tick(void) { kevent_signal(vi.vsync_ev); }

void svc_vi_init(void) {
    pm_init(&vi.lock);
    pc_init(&vi.free_cv);
    /* the service keeps these for the whole run: its reference keeps them alive when the game closes its handles */
    vi.vsync_ev = owned_event("vsync");
    vi.bufavail_ev = owned_event("buffer");
    kevent_signal(vi.bufavail_ev);
    if (g_opt_docked) {
        vi.width = 1920;
        vi.height = 1080;
    }
    nn_visrv_sf_IApplicationRootService_register(new_vi_u);
    nn_visrv_sf_ISystemRootService_register(new_vi_s);
    nn_visrv_sf_IManagerRootService_register(new_vi_m);
}
