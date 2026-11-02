/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from Eden (src/core/hle/service/vi/application_display_service.cpp, system_display_service.cpp,
   vi_types.h, vi_results.h, src/core/hle/service/nvnflinger/parcel.h, buffer_queue_producer.cpp,
   graphic_buffer_producer.h, hos_binder_driver.cpp, window.h, ui/graphic_buffer.h, ui/fence.h):
   SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
   SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project (GPL-2.0-or-later)
   SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
   SPDX-FileCopyrightText: Copyright 2010 The Android Open Source Project
   The NvGraphicBuffer's planes as libnx's nvidia/graphic_buffer.h lays them out (ISC). */
/* vi:u, vi:s and vi:m: the display, and the binder driver the game's nvnflinger talks to. Each layer is an Android
   buffer queue whose buffers the game preallocates: a queued buffer is shown by the renderer once the GPU has
   reached the fences it came with (on the GPU thread), and is free for the game again after that */
#include "nx/hle/hle.h"
#include "nx/hle/services/services_int.h"
#include "nx/hle/services/vi.h"
#include "nx/hle/services/nvdrv.h"

#include "hle/services/vi.swipc.inc"

#define VI_OPERATION_FAILED RC(114, 1)
#define VI_NOT_FOUND        RC(114, 7)

/* android::status_t */
#define BQ_OK            0
#define BQ_NEEDS_REALLOC 1
#define BQ_NO_INIT       (-19)
#define BQ_BAD_VALUE     (-22)

/* IGraphicBufferProducer's transactions */
enum {
    BQ_REQUEST_BUFFER = 1,
    BQ_SET_BUFFER_COUNT = 2,
    BQ_DEQUEUE_BUFFER = 3,
    BQ_DETACH_BUFFER = 4,
    BQ_DETACH_NEXT_BUFFER = 5,
    BQ_ATTACH_BUFFER = 6,
    BQ_QUEUE_BUFFER = 7,
    BQ_CANCEL_BUFFER = 8,
    BQ_QUERY = 9,
    BQ_CONNECT = 10,
    BQ_DISCONNECT = 11,
    BQ_ALLOCATE_BUFFERS = 13,
    BQ_SET_PREALLOCATED_BUFFER = 14,
    BQ_GET_BUFFER_HISTORY = 17
};

#define VI_QBI      84    /* QueueBufferInput */
#define VI_FENCE    36    /* android::Fence: a count and four NvFences */
#define VI_WIDTH    1280
#define VI_HEIGHT   720
#define VI_STRAY_ID 0x10000u /* vi's own layer ids, apart from am's managed ones */
/* the layer the display shows. There's no compositor: only the first layer, the one a game opens for its window (and
   opens again into the same place after closing it), is shown and paced. Another layer's frames come free once their
   fences are reached, unseen, where the console would compose them (edenprobe's binder suite opens one beside its
   window, whose frames were shown as the program's) */
#define VI_SHOWN    0

enum { SLOT_FREE, SLOT_DEQUEUED, SLOT_QUEUED };

/* ============================================================ layers */
/* the layer with this id, made on its first open: the binder id is its index + 1. Under the lock */
static int vi_layer_open(u64 layer_id) {
    int i, free_i = -1;
    for (i = 0; i < VI_LAYERS; i++) {
        if (g_hle.vi.layer[i].layer_id == layer_id) return i + 1;
        if (!g_hle.vi.layer[i].layer_id && free_i < 0) free_i = i;
    }
    if (free_i < 0) return 0;
    if (!g_hle.vi.layer[free_i].event) {
        g_hle.vi.layer[free_i].event = owned_event("vi:buffer queue");
        ev_signal(g_hle.vi.layer[free_i].event);
    }
    g_hle.vi.layer[free_i].layer_id = layer_id;
    g_hle.vi.layer[free_i].width = VI_WIDTH;
    g_hle.vi.layer[free_i].height = VI_HEIGHT;
    g_hle.vi.layer[free_i].format = 1; /* RGBA8888 */
    g_hle.vi.layer[free_i].max_buffer_count = 2; /* and SetBufferCount's answer for a window that never asks */
    return free_i + 1;
}
/* CloseLayer and DestroyStrayLayer: the layer's buffer queue goes, and its place is free for the next layer opened.
   Its frames still waiting for their fences are dropped (vi_present), and a DEQUEUE waiting for a buffer answers
   NO_INIT. Under the lock */
static void vi_layer_close(u64 layer_id) {
    int i;
    for (i = 0; i < VI_LAYERS; i++) {
        ViLayer* l = &g_hle.vi.layer[i];
        if (l->layer_id != layer_id) continue;
        l->layer_id = 0;
        l->connected = 0;
        l->interval = 0;
        l->max_buffer_count = 2;
        l->gen++;
        memset(l->slot, 0, sizeof l->slot);
        ev_signal(l->event); /* as when a buffer comes free: the waiter finds the queue gone */
        pc_broadcast(&g_host.vi.freed);
        return;
    }
    LOG_LIMIT(4, "vi: closing layer %llx, which isn't open\n", (unsigned long long)layer_id);
}
static ViLayer* vi_binder(i32 binder_id) {
    return binder_id >= 1 && binder_id <= VI_LAYERS && g_hle.vi.layer[binder_id - 1].layer_id
               ? &g_hle.vi.layer[binder_id - 1]
               : NULL;
}

/* the native window OpenLayer and CreateStrayLayer give: a parcel holding one flattened binder (vi::NativeWindow:
   magic 2, process 1, the binder id, "dispdrv"), with a zero in its object list */
static int vi_native_window(IpcBuf out, int binder, i64* size) {
    u8 p[0x3c];
    R_UNLESS(binder, VI_OPERATION_FAILED);
    memset(p, 0, sizeof p);
    snx_st_le32(p, 0x28); /* the parcel's header: data size and offset, objects' size and offset */
    snx_st_le32(p + 4, 0x10);
    snx_st_le32(p + 8, 4);
    snx_st_le32(p + 0xc, 0x38);
    snx_st_le32(p + 0x10, 2);
    snx_st_le32(p + 0x14, 1);
    snx_st_le64(p + 0x18, (u64)binder);
    memcpy(p + 0x28, "dispdrv", 8);
    *size = (i64)sizeof p; /* the size it needs, even when the buffer is short of it */
    if (out.size < sizeof p) {
        WARN_LIMIT(4, "vi: the native window's buffer has 0x%llx bytes, 0x%x are needed\n",
                   (unsigned long long)out.size, (unsigned)sizeof p);
        R_SUCCEED();
    }
    memcpy(GP(out.addr), p, sizeof p);
    memset(GP(out.addr + sizeof p), 0, (size_t)((out.size < 0x100 ? out.size : 0x100) - sizeof p));
    R_SUCCEED();
}

/* ============================================================ parcels */
typedef struct {
    const u8* d;
    u32 size, pos;
} ParcelIn;
typedef struct {
    u8 d[0x200];
    u32 n;
} ParcelOut;

/* past the header and the interface token (a policy word, the length, then length + 1 UTF-16 units) */
static void parcel_open(ParcelIn* p, const u8* d, u32 size) {
    u32 off = 0, len = 0;
    p->d = d;
    p->size = size;
    if (size >= 16) off = snx_ld_le32(d + 4);
    p->pos = off < size ? off : size;
    if (p->pos + 8 <= size) {
        len = snx_ld_le32(d + p->pos + 4);
        p->pos += 8;
        if (len < size) p->pos += ((len + 1) * 2 + 3) & ~3u;
        if (p->pos > size) p->pos = size;
    }
}
/* n bytes, then aligned to 4 as Parcel reads; zeros past the end. A parcel's numbers are little-endian, the guest's */
static void parcel_read(ParcelIn* p, void* out, u32 n) {
    if (p->pos + n <= p->size)
        memcpy(out, p->d + p->pos, n);
    else
        memset(out, 0, n);
    p->pos = (p->pos + n + 3) & ~3u;
}
static u32 parcel_u32(ParcelIn* p) {
    u8 v[4];
    parcel_read(p, v, 4);
    return snx_ld_le32(v);
}
static void parcel_write(ParcelOut* p, const void* v, u32 n) {
    if (p->n + ((n + 3) & ~3u) > sizeof p->d) return;
    memcpy(p->d + p->n, v, n);
    memset(p->d + p->n + n, 0, ((n + 3) & ~3u) - n);
    p->n += (n + 3) & ~3u;
}
static void parcel_w32(ParcelOut* p, u32 v) {
    u8 b[4];
    snx_st_le32(b, v);
    parcel_write(p, b, 4);
}
/* a flattened object: present, its size as Flattenable's length and fd count, then its bytes */
static void parcel_flat(ParcelOut* p, const void* v, u32 n) {
    u8 len[8];
    snx_st_le64(len, n);
    parcel_w32(p, 1);
    parcel_write(p, len, 8);
    parcel_write(p, v, n);
}
/* the reply: a parcel header, the data, no objects */
static void parcel_reply(IpcBuf reply, const ParcelOut* p) {
    u8 out[sizeof p->d + 16];
    u32 hdr[4];
    u64 n = 16 + p->n;
    hdr[0] = p->n;
    hdr[1] = 16;
    hdr[2] = 0;
    hdr[3] = 16 + p->n;
    snx_le32_array(hdr, 4);
    memcpy(out, hdr, 16);
    memcpy(out + 16, p->d, p->n);
    if (n > reply.size) {
        WARN_LIMIT(4, "vi: a binder reply of 0x%llx bytes doesn't fit the game's 0x%llx\n", (unsigned long long)n,
                   (unsigned long long)reply.size);
        return;
    }
    memcpy(GP(reply.addr), out, (size_t)n);
}

/* ============================================================ presenting */
/* the NvGraphicBuffer is kept as the game flattened it, its words little-endian */
static u32 gb32(const ViSlot* s, u32 off) { return snx_ld_le32(s->gbuf + off); }
/* on the GPU thread once the frame's fences are reached: shown if its layer is the display's, then free for the game.
   key is the layer's generation, its index and the slot (vi_key): a frame of a layer closed meanwhile is dropped, as
   its queue is gone */
static uintptr_t vi_key(int li, int slot) {
    return (uintptr_t)(g_hle.vi.layer[li].gen & 0xFFFF) << 16 | (uintptr_t)li << 8 | (uintptr_t)slot;
}
static void vi_present(void* arg) {
    uintptr_t key = (uintptr_t)arg;
    int li = (int)(key >> 8 & 0xff);
    ViLayer* l = &g_hle.vi.layer[li];
    ViSlot* s = &l->slot[key & 0xff];
    u32 nvmap, offset, w, h, pitch, layout, bhl, fmt, transform;
    i32 crop[4];
    pm_lock(&g_host.vi.lock);
    if (vi_key(li, (int)(key & 0xff)) != key || s->state != SLOT_QUEUED) {
        pm_unlock(&g_host.vi.lock);
        return;
    }
    /* the NvGraphicBuffer: its nvmap id, format and size, and the first plane's layout (libnx's NvSurface) */
    nvmap = gb32(s, 0x2c);
    fmt = gb32(s, 0x10);
    w = gb32(s, 0x04);
    h = gb32(s, 0x08);
    pitch = gb32(s, 0x70);
    offset = gb32(s, 0x78);
    layout = gb32(s, 0x6c);
    bhl = gb32(s, 0x80);
    memcpy(crop, s->crop, sizeof crop);
    transform = s->transform;
    if (!pitch) pitch = gb32(s, 0x0c) * ((fmt >= 4 && fmt <= 7 && fmt != 5) ? 2 : 4);
    pm_unlock(&g_host.vi.lock);
    if (li == VI_SHOWN)
        vi_show_buffer(nvmap, offset, w, h, pitch, layout, bhl, fmt, crop, transform & 3, /* the flips; no rotation */
                       (int)(key & 0xff));
    pm_lock(&g_host.vi.lock);
    if (vi_key(li, (int)(key & 0xff)) == key && s->state == SLOT_QUEUED) {
        s->state = SLOT_FREE;
        s->freed = ++g_hle.vi.frees;
    }
    pm_unlock(&g_host.vi.lock);
    ev_signal(l->event);
    pc_broadcast(&g_host.vi.freed);
}
/* QueueBuffer's input: the timestamp, crop, scaling, transform, swap interval and the fence, laid out packed. A
   parcel too short for it (in NULL) queues the whole buffer at a swap interval of 1 */
static void vi_queue(ViLayer* l, int slot, const u8* in, ParcelOut* out) {
    ViSlot* s = &l->slot[slot];
    u32 i, pending = 0, interval;
    i32 swap = 1, crop[4] = {0, 0, 0, 0};
    u32 transform = 0;
    if (in) {
        memcpy(crop, in + 12, 16);
        snx_le32_array(crop, 4);
        transform = snx_ld_le32(in + 32);
        swap = (i32)snx_ld_le32(in + 44);
    }
    /* the SDK's intervals are 0 to 4; a negative one is the default */
    interval = swap < 0 ? 1 : swap > 4 ? 4 : (u32)swap;
    if (interval != l->interval) LOG_LIMIT(16, "vi: swap interval %u\n", interval);
    if (memcmp(crop, s->crop, sizeof crop) || transform != s->transform)
        LOG_LIMIT(16, "vi: crop %d,%d to %d,%d, transform %u\n", crop[0], crop[1], crop[2], crop[3], transform);
    memcpy(s->crop, crop, sizeof crop);
    s->transform = transform;
    s->state = SLOT_QUEUED;
    l->interval = interval;
    for (i = 0; i < VI_SLOTS; i++) pending += l->slot[i].state == SLOT_QUEUED;
    parcel_w32(out, l->width);
    parcel_w32(out, l->height);
    parcel_w32(out, 0); /* transform hint */
    parcel_w32(out, pending);
}

/* the oldest free slot, those with a buffer first; -1 for none. Under the lock */
static int vi_free_slot(const ViLayer* l) {
    int i, best = -1, any = -1;
    for (i = 0; i < VI_SLOTS; i++) {
        if (l->slot[i].state != SLOT_FREE) continue;
        if (l->slot[i].has_buffer) {
            if (best < 0 || l->slot[i].freed < l->slot[best].freed) best = i;
        } else if (any < 0)
            any = i;
    }
    if (best >= 0) return best;
    for (i = 0; i < VI_SLOTS; i++)
        if (l->slot[i].has_buffer) return -1; /* the game's buffers are all in use: wait for one */
    return any;
}

/* one transaction on a layer's buffer queue */
static void vi_transact(ViLayer* l, u32 code, IpcBuf parcel, IpcBuf reply) {
    ParcelIn in;
    ParcelOut out;
    i32 status = BQ_OK, slot;
    u32 v, interval = 0, gen, fence_none[9];
    int qslot = -1, li = (int)(l - g_hle.vi.layer), dequeued = 0, qbi_ok = 0, waits = 0;
    uintptr_t qkey = 0;
    u8 qbi[VI_QBI];
    parcel_open(&in, GP(parcel.addr), (u32)(parcel.size < 0x10000 ? parcel.size : 0x10000));
    out.n = 0;
    memset(fence_none, 0xff, sizeof fence_none);
    fence_none[0] = 0;
    pm_lock(&g_host.vi.lock);
    /* 0: the layer closed after vi_binder_call found it, and its queue is gone */
    switch (l->layer_id ? code : 0) {
        case 0: status = BQ_NO_INIT; break;
        case BQ_REQUEST_BUFFER:
            slot = (i32)parcel_u32(&in);
            if (slot < 0 || slot >= VI_SLOTS)
                status = BQ_BAD_VALUE;
            else if (!l->slot[slot].has_buffer)
                parcel_w32(&out, 0);
            else
                parcel_flat(&out, l->slot[slot].gbuf, VI_GBUF);
            break;
        case BQ_DEQUEUE_BUFFER:
            /* async, width, height, format, usage: the buffers are the game's own, whatever it asks */
            gen = l->gen;
            /* every buffer the game gave is in use: wait for one, never taking one back from the display */
            while ((slot = vi_free_slot(l)) < 0 && l->gen == gen)
                if (kthread_wait(&g_host.vi.freed, &g_host.vi.lock, 1000000000ll))
                    WARN_LIMIT(8, "vi: no buffer came free for %d s, still waiting\n", ++waits);
            if (l->gen != gen) { /* the layer closed while it waited */
                status = BQ_NO_INIT;
                break;
            }
            l->slot[slot].state = SLOT_DEQUEUED;
            if (!l->slot[slot].has_buffer) status = BQ_NEEDS_REALLOC;
            parcel_w32(&out, (u32)slot);
            parcel_flat(&out, fence_none, VI_FENCE); /* the buffer was shown and freed: no fence to wait for */
            interval = l->interval;
            dequeued = 1;
            break;
        case BQ_QUEUE_BUFFER:
            slot = (i32)parcel_u32(&in);
            parcel_u32(&in); /* the flattened size, */
            parcel_u32(&in); /* and its fd count */
            qbi_ok = in.pos + VI_QBI <= in.size;
            parcel_read(&in, qbi, VI_QBI);
            if (slot < 0 || slot >= VI_SLOTS || l->slot[slot].state != SLOT_DEQUEUED || !l->slot[slot].has_buffer)
                status = BQ_BAD_VALUE;
            else {
                vi_queue(l, slot, qbi_ok ? qbi : NULL, &out);
                qslot = slot;
                qkey = vi_key(li, slot);
            }
            break;
        case BQ_CANCEL_BUFFER:
            slot = (i32)parcel_u32(&in);
            if (slot >= 0 && slot < VI_SLOTS && l->slot[slot].state == SLOT_DEQUEUED) {
                l->slot[slot].state = SLOT_FREE;
                l->slot[slot].freed = ++g_hle.vi.frees;
                ev_signal(l->event);
                pc_broadcast(&g_host.vi.freed);
            }
            break;
        case BQ_QUERY:
            v = parcel_u32(&in);
            switch (v) {
                case 0:                      /* Width */
                case 6: v = l->width; break; /* DefaultWidth */
                case 1:
                case 7: v = l->height; break;
                case 2: v = l->format; break;
                case 3: v = 0; break;  /* MinUndequeuedBuffers: HOS never uses the async buffer path */
                case 9:                /* ConsumerRunningBehind */
                case 10:               /* ConsumerUsageBits */
                case 11: v = 0; break; /* StickyTransform */
                case 12: v = l->max_buffer_count; break;
                default:
                    v = 0;
                    status = BQ_BAD_VALUE;
                    break;
            }
            parcel_w32(&out, v);
            break;
        case BQ_CONNECT:
            /* enable listener, api, producer controlled by app; the output's queue size is all that changes */
            l->connected = 1;
            parcel_w32(&out, l->width);
            parcel_w32(&out, l->height);
            parcel_w32(&out, 0);
            parcel_w32(&out, 0);
            break;
        case BQ_DISCONNECT:
            if (!l->connected) status = BQ_NO_INIT;
            l->connected = 0;
            break;
        case BQ_SET_PREALLOCATED_BUFFER:
            slot = (i32)parcel_u32(&in);
            if (slot < 0 || slot >= VI_SLOTS) {
                status = BQ_BAD_VALUE;
                break;
            }
            memset(&l->slot[slot], 0, sizeof l->slot[slot]);
            if (parcel_u32(&in)) { /* the buffer is there: its flattened size and fd count, then the NvGraphicBuffer */
                v = parcel_u32(&in);
                parcel_u32(&in);
                if (v < 0x84 || in.pos + 0x84 > in.size) { /* short of the first plane's fields */
                    WARN_LIMIT(4, "vi: slot %d's graphic buffer has 0x%x bytes, too few: the slot is cleared\n",
                               (int)slot, v);
                    ev_signal(l->event);
                    pc_broadcast(&g_host.vi.freed);
                    break;
                }
                parcel_read(&in, l->slot[slot].gbuf, v < VI_GBUF ? v : VI_GBUF);
                l->slot[slot].has_buffer = 1;
                l->width = gb32(&l->slot[slot], 0x04);
                l->height = gb32(&l->slot[slot], 0x08);
                l->format = gb32(&l->slot[slot], 0x10);
            }
            ev_signal(l->event);
            pc_broadcast(&g_host.vi.freed);
            break;
        case BQ_GET_BUFFER_HISTORY: /* no history is kept */
        case BQ_DETACH_BUFFER:
        case BQ_DETACH_NEXT_BUFFER:
        case BQ_ATTACH_BUFFER:
        case BQ_ALLOCATE_BUFFERS: break;
        case BQ_SET_BUFFER_COUNT: /* the game's window asks for 2; answering the 64 slots made NVN abort */
            v = parcel_u32(&in);
            if (v > VI_SLOTS) status = BQ_BAD_VALUE;
            else l->max_buffer_count = v ? v : 2; /* 0 is the default, as Eden's override */
            break;
        default: WARN_LIMIT(4, "vi: binder transaction %u isn't known, it answers success\n", code); break;
    }
    pm_unlock(&g_host.vi.lock);
    parcel_w32(&out, (u32)status);
    parcel_reply(reply, &out);
    if (qslot >= 0) {
        /* shown once the GPU reaches the fences the frame was queued with: nvdrv waits for those it can't leave to
           the GPU thread. The slot is queued and the reply written: a game pause lets this finish rather than
           making the request again on a save state's load */
        kthread_commit();
        u32 fences, i, ids[4], vals[4], n = 0;
        fences = snx_ld_le32(qbi + 48);
        for (i = 0; i < 4 && i < fences; i++) {
            ids[n] = snx_ld_le32(qbi + 52 + 8 * i);
            vals[n] = snx_ld_le32(qbi + 56 + 8 * i);
            if (ids[n] != ~0u) n++;
        }
        nv_gpu_call_after(n, ids, vals, vi_present, (void*)qkey);
    }
    if (dequeued && li == VI_SHOWN) present_pace(interval, slot); /* only the display's frames wait for it */
}

/* ============================================================ IHOSBinderDriver */
static int vi_binder_call(i32 binder_id, u32 code, IpcBuf parcel, IpcBuf reply) {
    ViLayer* l;
    R_UNLESS(parcel.size >= 16, VI_OPERATION_FAILED); /* not even a parcel's header */
    pm_lock(&g_host.vi.lock);
    l = vi_binder(binder_id);
    pm_unlock(&g_host.vi.lock);
    R_UNLESS(l, VI_NOT_FOUND);
    vi_transact(l, code, parcel, reply);
    R_SUCCEED();
}
static int nns_hosbinder_IHOSBinderDriver_TransactParcel(SvcObj* self, i32 binder_id, u32 code, IpcBuf parcel,
                                                         IpcBuf reply, u32 flags) {
    (void)self, (void)flags;
    return vi_binder_call(binder_id, code, parcel, reply);
}
static int nns_hosbinder_IHOSBinderDriver_TransactParcelAuto(SvcObj* self, i32 binder_id, u32 code, IpcBuf parcel,
                                                             IpcBuf reply, u32 flags) {
    (void)self, (void)flags;
    return vi_binder_call(binder_id, code, parcel, reply);
}
/* the buffer queue's event, signaled when a buffer comes free */
static int nns_hosbinder_IHOSBinderDriver_GetNativeHandle(SvcObj* self, i32 binder_id, u32 type_id, u32* event) {
    ViLayer* l;
    (void)self, (void)type_id;
    pm_lock(&g_host.vi.lock);
    l = vi_binder(binder_id);
    pm_unlock(&g_host.vi.lock);
    R_UNLESS(l, VI_NOT_FOUND);
    *event = ev_handle(l->event);
    R_SUCCEED();
}

/* ============================================================ root services */
static SvcObj* vi_display_service(void) { return nn_visrv_sf_IApplicationDisplayService_new(); }
static int nn_visrv_sf_IApplicationRootService_GetDisplayServiceWithWorkaround(SvcObj* self, u32 policy,
                                                                               SvcObj** service) {
    (void)self, (void)policy;
    *service = vi_display_service();
    R_SUCCEED();
}
static int nn_visrv_sf_IApplicationRootService_GetDisplayService(SvcObj* self, u32 policy, SvcObj** service) {
    (void)self, (void)policy;
    *service = vi_display_service();
    R_SUCCEED();
}
static int nn_visrv_sf_ISystemRootService_GetDisplayService(SvcObj* self, u32 policy, SvcObj** service) {
    (void)self, (void)policy;
    *service = vi_display_service();
    R_SUCCEED();
}
static int nn_visrv_sf_ISystemRootService_GetDisplayServiceWithProxyNameExchange(SvcObj* self,
                                                                                 const nn_vi_ProxyName* proxy_name,
                                                                                 u32 policy, SvcObj** service) {
    (void)self, (void)proxy_name, (void)policy;
    *service = vi_display_service();
    R_SUCCEED();
}
static int nn_visrv_sf_IManagerRootService_GetDisplayService(SvcObj* self, u32 policy, SvcObj** service) {
    (void)self, (void)policy;
    *service = vi_display_service();
    R_SUCCEED();
}
static int nn_visrv_sf_IManagerRootService_GetDisplayServiceWithProxyNameExchange(SvcObj* self,
                                                                                  const nn_vi_ProxyName* proxy_name,
                                                                                  u32 policy, SvcObj** service) {
    (void)self, (void)proxy_name, (void)policy;
    *service = vi_display_service();
    R_SUCCEED();
}

/* ============================================================ IApplicationDisplayService */
static int nn_visrv_sf_IApplicationDisplayService_GetRelayService(SvcObj* self, SvcObj** service) {
    (void)self;
    *service = nns_hosbinder_IHOSBinderDriver_new();
    R_SUCCEED();
}
static int nn_visrv_sf_IApplicationDisplayService_GetSystemDisplayService(SvcObj* self, SvcObj** service) {
    (void)self;
    *service = nn_visrv_sf_ISystemDisplayService_new();
    R_SUCCEED();
}
static int nn_visrv_sf_IApplicationDisplayService_GetManagerDisplayService(SvcObj* self, SvcObj** service) {
    (void)self;
    *service = nn_visrv_sf_IManagerDisplayService_new();
    R_SUCCEED();
}
static int nn_visrv_sf_IApplicationDisplayService_GetIndirectDisplayTransactionService(SvcObj* self, SvcObj** service) {
    (void)self;
    *service = nns_hosbinder_IHOSBinderDriver_new();
    R_SUCCEED();
}
/* the one display: "Default", one layer, 1920x1080 at most */
static int nn_visrv_sf_IApplicationDisplayService_ListDisplays(SvcObj* self, IpcBuf displays, i64* count) {
    u8 info[0x60];
    (void)self;
    if (displays.size < sizeof info) R_SUCCEED();
    memset(info, 0, sizeof info);
    memcpy(info, "Default", 8);
    info[0x40] = 1; /* has_limited_layers */
    snx_st_le64(info + 0x48, 1);
    snx_st_le64(info + 0x50, 1920);
    snx_st_le64(info + 0x58, 1080);
    memcpy(GP(displays.addr), info, sizeof info);
    *count = 1;
    R_SUCCEED();
}
/* the names Switchbrew lists, each its own display id */
static int vi_display_id(const nn_vi_DisplayName* name, u64* id) {
    static const char* const names[] = {"Default", "External", "Edid", "Internal", "Null"};
    char n[0x40];
    u64 i;
    memcpy(n, name->v, sizeof n);
    n[sizeof n - 1] = 0;
    for (i = 0; i < sizeof names / sizeof names[0]; i++)
        if (!strcmp(n, names[i])) {
            *id = i;
            R_SUCCEED();
        }
    R_THROW(VI_OPERATION_FAILED);
}
static int nn_visrv_sf_IApplicationDisplayService_OpenDisplay(SvcObj* self, const nn_vi_DisplayName* name,
                                                              u64* display_id) {
    (void)self;
    return vi_display_id(name, display_id);
}
static int nn_visrv_sf_IApplicationDisplayService_OpenDefaultDisplay(SvcObj* self, u64* display_id) {
    (void)self;
    *display_id = 0;
    R_SUCCEED();
}
/* fixed, docked or not, as on the console */
static int nn_visrv_sf_IApplicationDisplayService_GetDisplayResolution(SvcObj* self, u64 display_id, i64* width,
                                                                       i64* height) {
    (void)self, (void)display_id;
    *width = VI_WIDTH;
    *height = VI_HEIGHT;
    R_SUCCEED();
}
static int nn_visrv_sf_IApplicationDisplayService_OpenLayer(SvcObj* self, const nn_vi_DisplayName* display_name,
                                                            u64 layer_id, u64 aruid, IpcBuf native_window,
                                                            i64* native_window_size) {
    u64 display;
    int binder;
    (void)self, (void)aruid;
    R_TRY(vi_display_id(display_name, &display));
    pm_lock(&g_host.vi.lock);
    binder = vi_layer_open(layer_id);
    pm_unlock(&g_host.vi.lock);
    return vi_native_window(native_window, binder, native_window_size);
}
static int nn_visrv_sf_IApplicationDisplayService_CloseLayer(SvcObj* self, u64 layer_id) {
    (void)self;
    pm_lock(&g_host.vi.lock);
    vi_layer_close(layer_id);
    pm_unlock(&g_host.vi.lock);
    R_SUCCEED();
}
static int nn_visrv_sf_IApplicationDisplayService_DestroyStrayLayer(SvcObj* self, u64 layer_id) {
    (void)self;
    pm_lock(&g_host.vi.lock);
    vi_layer_close(layer_id);
    pm_unlock(&g_host.vi.lock);
    R_SUCCEED();
}
static int nn_visrv_sf_IApplicationDisplayService_CreateStrayLayer(SvcObj* self, u32 layer_flags, u64 display_id,
                                                                   IpcBuf native_window, u64* layer_id,
                                                                   i64* native_window_size) {
    int binder;
    (void)self, (void)layer_flags, (void)display_id;
    pm_lock(&g_host.vi.lock);
    *layer_id = VI_STRAY_ID + g_hle.vi.next_stray++;
    binder = vi_layer_open(*layer_id);
    pm_unlock(&g_host.vi.lock);
    return vi_native_window(native_window, binder, native_window_size);
}
/* nn::vi::ScalingMode to the Android one */
static int nn_visrv_sf_IApplicationDisplayService_ConvertScalingMode(SvcObj* self, u32 mode, u64* converted) {
    static const u64 android[] = {3, 0, 1, 2, 4};
    (void)self;
    R_UNLESS(mode < sizeof android / sizeof android[0], VI_OPERATION_FAILED);
    *converted = android[mode];
    R_SUCCEED();
}
static int nn_visrv_sf_IApplicationDisplayService_GetIndirectLayerImageRequiredMemoryInfo(SvcObj* self, i64 width,
                                                                                          i64 height, i64* size,
                                                                                          i64* alignment) {
    const i64 block = 0x20000;
    (void)self;
    *size = (width * height * 4 + block - 1) / block * block;
    *alignment = 0x1000;
    R_SUCCEED();
}
static int nn_visrv_sf_IApplicationDisplayService_GetDisplayVsyncEvent(SvcObj* self, u64 display_id, u32* event) {
    (void)self, (void)display_id;
    *event = ev_handle(g_hle.vi.vsync);
    R_SUCCEED();
}
static int nn_visrv_sf_IApplicationDisplayService_GetDisplayVsyncEventForDebug(SvcObj* self, u64 display_id,
                                                                               u32* event) {
    (void)self, (void)display_id;
    *event = ev_handle(g_hle.vi.vsync);
    R_SUCCEED();
}

/* ============================================================ ISystemDisplayService, IManagerDisplayService */
static int nn_visrv_sf_ISystemDisplayService_GetZOrderCountMin(SvcObj* self, u64 display_id, i64* count) {
    (void)self, (void)display_id;
    *count = 1;
    R_SUCCEED();
}
static int nn_visrv_sf_ISystemDisplayService_GetZOrderCountMax(SvcObj* self, u64 display_id, i64* count) {
    (void)self, (void)display_id;
    *count = 255;
    R_SUCCEED();
}
static int nn_visrv_sf_ISystemDisplayService_GetDisplayLogicalResolution(SvcObj* self, u64 display_id, i32* width,
                                                                         i32* height) {
    (void)self, (void)display_id;
    *width = 1920;
    *height = 1080;
    R_SUCCEED();
}
static int nn_visrv_sf_ISystemDisplayService_GetDisplayMode(SvcObj* self, u64 display_id, nn_vi_DisplayModeInfo* mode) {
    (void)self, (void)display_id;
    mode->width = g_settings.tv_mode ? 1920 : VI_WIDTH;
    mode->height = g_settings.tv_mode ? 1080 : VI_HEIGHT;
    mode->refresh_rate = (float)VI_REFRESH_HZ;
    mode->unknown = 0;
    R_SUCCEED();
}
static int nn_visrv_sf_IManagerDisplayService_CreateManagedLayer(SvcObj* self, u32 layer_flags, u64 display_id,
                                                                 nn_applet_AppletResourceUserId aruid, u64* layer_id) {
    (void)self, (void)layer_flags, (void)display_id, (void)aruid;
    pm_lock(&g_host.vi.lock);
    *layer_id = VI_STRAY_ID + g_hle.vi.next_stray++;
    pm_unlock(&g_host.vi.lock);
    R_SUCCEED();
}
static int nn_visrv_sf_IManagerDisplayService_AcquireLayerTexturePresentingEvent(SvcObj* self, u64 layer_id,
                                                                                 u32* event) {
    (void)self, (void)layer_id;
    return new_event(event, "vi:layer texture presenting", 0);
}
static int nn_visrv_sf_IManagerDisplayService_GetDisplayHotplugEvent(SvcObj* self, u64 display_id, u32* event) {
    (void)self, (void)display_id;
    return new_event(event, "vi:display hotplug", 0);
}
static int nn_visrv_sf_IManagerDisplayService_GetDisplayModeChangedEvent(SvcObj* self, u64 display_id, u32* event) {
    (void)self, (void)display_id;
    return new_event(event, "vi:display mode changed", 0);
}

/* ============================================================ the display */
/* the main loop's 60 Hz: the display's vsync, also for the Switch 2 display sessions' vsync monitors */
void vi_vsync_tick(void) {
    if (g_hle.vi.vsync) ev_signal(g_hle.vi.vsync);
    u32 events[VI2_SESSIONS * VI2_GROUPS], n = 0;
    pm_lock(&g_host.vi.lock);
    for (u32 live = g_hle.vi.vi2_live; live; live &= live - 1) {
        const Vi2Session* s = &g_hle.vi.vi2[snx_ctz32(live)];
        for (u32 k = 0; k < VI2_GROUPS; ++k)
            if (s->monitor_display[k] && !s->monitor_kind[k] && s->group_events[k]) {
                events[n++] = s->group_events[k];
                kobj_ref(kobj_at(s->group_events[k])); /* vi2_destroy may let go of it once the lock is free */
            }
    }
    pm_unlock(&g_host.vi.lock);
    for (u32 i = 0; i < n; ++i) {
        kevent_signal(kev(events[i]));
        kobj_unref(kobj_at(events[i]));
    }
}

/* ============================================================ the Switch 2 display session */
/* nn::vi2's user library (the same in the three SDKs at hand) asks for the last notification index (1), the last
   session group (3) and the last request group (5), and caps them at 0x3FF, 7 and 7. It sizes its notification bits
   from 1 (VI2_NOTIFICATIONS), hands that index back to 11 and 13, and hands 5 back to 51. It keeps min(3, 7) + 1, but
   no code of its display library reads that count: its request groups come from a bitmap of its own, of 8 groups.
   The answers are eden-ns2's (0, 0, 7); Eglantine's 1 and 1 give the guest the same notification bits. Neither is
   measured on a console. */
static int nn_gsv_IVi2Session_GetNotificationLimit(SvcObj* self, u32* last) {
    (void)self;
    *last = VI2_LAST_NOTIFICATION;
    return 0;
}
static int nn_gsv_IVi2Session_GetSessionLimit(SvcObj* self, u32* last) {
    (void)self, (void)last;
    return 0;
}
static int nn_gsv_IVi2Session_GetRequestGroupLimit(SvcObj* self, u32* last) {
    (void)self;
    *last = 7;
    return 0;
}
/* One event for the session, signaled while a notification is pending. The SDK attaches it with manual clear. */
static int nn_gsv_IVi2Session_GetNotificationEvent(SvcObj* self, u32 last, u32* event) {
    Vi2Session* s = self->data;
    R_UNLESS(last == VI2_LAST_NOTIFICATION, RC(1, 116));
    pm_lock(&g_host.vi.lock);
    if (!s->event) {
        KEvent* e = kevent_new("vi2 notification");
        kobj_ref(&e->o);
        s->event = kobj_id(&e->o);
    }
    u32 id = s->event, pending = 0;
    for (u32 i = 0; i < VI2_NOTIFICATION_WORDS; ++i) pending |= s->notifications[i];
    pm_unlock(&g_host.vi.lock);
    if (pending) kevent_signal(kev(id));
    *event = h_new(kobj_at(id));
    return *event ? 0 : RC(1, 105);
}
static int nn_gsv_IVi2Session_ReleaseNotificationEvent(SvcObj* self, u32 last) {
    (void)self;
    R_UNLESS(last == VI2_LAST_NOTIFICATION, RC(1, 116));
    return 0;
}
/* The pending bits, as many words as the buffer holds. The words given are no longer pending. */
static int nn_gsv_IVi2Session_GetNotifications(SvcObj* self, IpcBuf out) {
    Vi2Session* s = self->data;
    u32 words =
        out.size / sizeof(u32) < VI2_NOTIFICATION_WORDS ? (u32)(out.size / sizeof(u32)) : VI2_NOTIFICATION_WORDS;
    u32 taken[VI2_NOTIFICATION_WORDS];
    pm_lock(&g_host.vi.lock);
    for (u32 i = 0; i < words; ++i) {
        taken[i] = s->notifications[i];
        s->notifications[i] = 0;
    }
    pm_unlock(&g_host.vi.lock);
    memcpy(GP(out.addr), taken, words * sizeof(u32)); /* outside the lock: the write can trap */
    return 0;
}
/* the display's name as 8 bytes, "default" */
#define VI2_DEFAULT_DISPLAY 0x00746C7561666564ull
/* Stream-play initializes its output resources before a guest connects. These are distinct display identities,
   not aliases of the main display. Only the default display has a local presentation endpoint. */
static int vi2_display_index(u64 name) {
    static const u64 names[VI2_DISPLAYS] = {VI2_DEFAULT_DISPLAY, 0x0031726579616c70ull,
                                           0x0032726579616c70ull, 0x0033726579616c70ull};
    for (u32 i = 0; i < VI2_DISPLAYS; ++i)
        if (name == names[i]) return (int)i;
    return -1;
}
_Static_assert(VI2_NOTIFICATIONS <= 32, "Vi2Session.client_slots holds a bit for each notification");
/* A notification for the guest: its bit, then the session's event. Called under HostVi.lock, which the caller
   releases before it signals the returned event (0 for none). */
static u32 vi2_notify_nl(Vi2Session* s, u32 bit) {
    s->notifications[bit / 32] |= 1u << (bit % 32);
    return s->event;
}
/* The notification bit tells the guest when that display's last reference has closed. */
static int nn_gsv_IVi2Session_OpenDisplay(SvcObj* self, u64 name, u32 notification) {
    Vi2Session* s = self->data;
    int display = vi2_display_index(name);
    R_UNLESS(display >= 0, RC(1, 121));
    R_UNLESS(notification < VI2_NOTIFICATIONS, RC(1, 116));
    pm_lock(&g_host.vi.lock);
    int rc = s->display_refs[display] < 1024 ? 0 : RC(1, 103);
    if (!rc) {
        s->display_notification[display] = notification;
        ++s->display_refs[display];
    }
    pm_unlock(&g_host.vi.lock);
    return rc;
}
static int nn_gsv_IVi2Session_CloseDisplay(SvcObj* self, u64 name) {
    Vi2Session* s = self->data;
    int display = vi2_display_index(name);
    R_UNLESS(display >= 0, RC(1, 121));
    u32 event = 0;
    pm_lock(&g_host.vi.lock);
    int rc = s->display_refs[display] ? 0 : RC(1, 121);
    if (!rc && !--s->display_refs[display]) event = vi2_notify_nl(s, s->display_notification[display]);
    pm_unlock(&g_host.vi.lock);
    if (event) kevent_signal(kev(event));
    return rc;
}
/* the client with the token, NULL for none; under HostVi.lock */
static Vi2Client* vi2_client_nl(Vi2Session* s, u64 token) {
    for (u32 i = 0; token && i < VI2_CLIENTS; ++i)
        if (s->clients[i].token == token) return &s->clients[i];
    return NULL;
}
/* A client names two distinct notification bits, for its retirement and its availability. No two clients share a
   bit or a token. Its buffer slots start free. */
static int nn_gsv_IVi2Session_RegisterClientState(SvcObj* self, u64 token, u32 retire, u32 available) {
    Vi2Session* s = self->data;
    R_UNLESS(token && retire < VI2_NOTIFICATIONS && available < VI2_NOTIFICATIONS && retire != available, RC(1, 116));
    u32 bits = 1u << retire | 1u << available;
    pm_lock(&g_host.vi.lock);
    Vi2Client* free_client = NULL;
    for (u32 i = VI2_CLIENTS; i-- > 0;)
        if (!s->clients[i].token) free_client = &s->clients[i];
    int rc = s->client_slots & bits || vi2_client_nl(s, token) ? RC(1, 125) : free_client ? 0 : RC(1, 103);
    if (!rc) {
        *free_client = (Vi2Client){.token = token,
                                   .retire_bit = retire,
                                   .available_bit = available,
                                   .free_slots = (1u << VI2_BUFFER_SLOTS) - 1};
        s->client_slots |= bits;
    }
    pm_unlock(&g_host.vi.lock);
    return rc;
}
/* The client's retirement bit tells the guest that it is gone. */
static int nn_gsv_IVi2Session_UnregisterClientState(SvcObj* self, u64 token) {
    Vi2Session* s = self->data;
    R_UNLESS(token, RC(1, 116));
    u32 event = 0;
    pm_lock(&g_host.vi.lock);
    Vi2Client* client = vi2_client_nl(s, token);
    if (client) {
        event = vi2_notify_nl(s, client->retire_bit);
        s->client_slots &= ~(1u << client->retire_bit | 1u << client->available_bit);
        if (s->endpoint == token) s->endpoint = 0;
        *client = (Vi2Client){0};
    }
    pm_unlock(&g_host.vi.lock);
    R_UNLESS(client, RC(1, 121));
    if (event) kevent_signal(kev(event));
    return 0;
}
/* the buffer slots the client may fill */
static int nn_gsv_IVi2Session_GetClientAvailability(SvcObj* self, u64 token, u64* free_slots) {
    Vi2Session* s = self->data;
    pm_lock(&g_host.vi.lock);
    Vi2Client* client = vi2_client_nl(s, token);
    if (client) *free_slots = client->free_slots;
    pm_unlock(&g_host.vi.lock);
    return client ? 0 : RC(1, 121);
}
/* Only the local display has a presentation endpoint. Stream-play output resources can be initialized, but guest
   streaming and presentation are not supported. */
static int nn_gsv_IVi2Session_BindClientDisplay(SvcObj* self, u64 token, u64 name) {
    Vi2Session* s = self->data;
    int display = vi2_display_index(name);
    pm_lock(&g_host.vi.lock);
    Vi2Client* client = vi2_client_nl(s, token);
    int rc = !client || display < 0 || !s->display_refs[display] ? RC(1, 121) : client->bound ? RC(1, 125) : 0;
    if (!rc) client->bound = (u8)(display + 1);
    pm_unlock(&g_host.vi.lock);
    return rc;
}
static int nn_gsv_IVi2Session_UnbindClientDisplay(SvcObj* self, u64 token) {
    Vi2Session* s = self->data;
    pm_lock(&g_host.vi.lock);
    Vi2Client* client = vi2_client_nl(s, token);
    int rc = client && client->bound ? 0 : RC(1, 121);
    if (!rc) client->bound = 0;
    pm_unlock(&g_host.vi.lock);
    return rc;
}
/* The default display has one endpoint, which a client bound to it opens */
static int nn_gsv_IVi2Session_OpenEndpoint(SvcObj* self, u64 name, u64 display, u64 token) {
    Vi2Session* s = self->data;
    R_UNLESS(name == VI2_DEFAULT_DISPLAY && display == name, RC(1, 121));
    pm_lock(&g_host.vi.lock);
    Vi2Client* client = vi2_client_nl(s, token);
    int rc = !client || client->bound != 1 ? RC(1, 121) : s->endpoint ? RC(1, 125) : 0;
    if (!rc) s->endpoint = token;
    pm_unlock(&g_host.vi.lock);
    return rc;
}
static int nn_gsv_IVi2Session_CloseEndpoint(SvcObj* self, u64 name, u64 display) {
    Vi2Session* s = self->data;
    R_UNLESS(name == VI2_DEFAULT_DISPLAY && display == name, RC(1, 121));
    pm_lock(&g_host.vi.lock);
    int rc = s->endpoint ? 0 : RC(1, 121);
    s->endpoint = 0;
    pm_unlock(&g_host.vi.lock);
    return rc;
}
/* ---- the display's state. The values are eden-ns2's, which reads them from the display service while no driver record
   of the display exists: the runtime's display is SDR at a fixed 60 Hz, so the game is offered no HDR and no
   variable refresh rate */
#define VI2_INVALID_NAME RC(114, 4) /* a name of all zero bytes */
static int vi2_display_name(u64 name) { return !name ? VI2_INVALID_NAME : vi2_display_index(name) >= 0 ? 0 : VI_NOT_FOUND; }
/* the output format (1, SDR), while the session holds the display open */
static int nn_gsv_IVi2Session_QueryHdrStatus(SvcObj* self, u64 name, u64* format, u16* value) {
    Vi2Session* s = self->data;
    int display = vi2_display_index(name);
    pm_lock(&g_host.vi.lock);
    int open = display >= 0 && s->display_refs[display] != 0;
    pm_unlock(&g_host.vi.lock);
    R_UNLESS(open, VI_NOT_FOUND);
    *format = 1;
    *value = 0;
    R_SUCCEED();
}
static int nn_gsv_IVi2Session_QueryHdrPaperWhite(SvcObj* self, u64 name, u16* paper_white) {
    (void)self;
    R_TRY(vi2_display_name(name));
    *paper_white = 0;
    R_SUCCEED();
}
/* one refresh rate, one output format and no rate the display can vary to */
static int nn_gsv_IVi2Session_QueryModeCapability(SvcObj* self, u64 name, u64* refresh_rates, u64* hdr_formats,
                                                   u64* variable_refresh_rates) {
    (void)self;
    R_TRY(vi2_display_name(name));
    *refresh_rates = 1;
    *hdr_formats = 1;
    *variable_refresh_rates = 0;
    R_SUCCEED();
}
/* ---- presents */
/* The output encoding the SDK's native window sets before its first present. After the display reported SDR, an 8-bit
   RGBA window of one game sends 0x14 (what eden-ns2 sees by default) and another's 0x5, and the display shows either as
   it is. What the values name is not known: another one, as an HDR window's could be, needs a conversion the
   presenter does not have, and is refused */
static int vi2_encoding_shown(u64 encoding) { return encoding == 0x5 || encoding == 0x14; }
static int nn_gsv_IVi2Session_SetClientOutputEncoding(SvcObj* self, u64 token, u64 encoding) {
    Vi2Session* s = self->data;
    pm_lock(&g_host.vi.lock);
    Vi2Client* client = vi2_client_nl(s, token);
    int rc = !client ? RC(1, 121) : !vi2_encoding_shown(encoding) ? RC(1, 33) : 0;
    if (!rc) client->encoded = 1;
    pm_unlock(&g_host.vi.lock);
    if (client && rc) WARN_LIMIT(4, "vi2: output encoding %#llx is not supported\n", (unsigned long long)encoding);
    return rc;
}
/* the GPU thread's callback for a present, after its fence: session, client and slot in the low bits, the present's
   number above them */
_Static_assert(sizeof(uintptr_t) >= 8, "a present's callback key holds its number and its slot");
_Static_assert(VI2_SESSIONS <= 16 && VI2_CLIENTS <= 16 && VI2_BUFFER_SLOTS <= 16, "four bits each in the key");
static uintptr_t vi2_key(u32 session, u32 client, u32 slot, u32 present) {
    return (uintptr_t)present << 32 | (uintptr_t)session << 8 | (uintptr_t)client << 4 | slot;
}
/* The image is drawn: shown when the client is on the display, and its slot stays the display's until the next
   frame replaces it. The slot it replaces comes free, and the client's availability bit tells the guest */
static void vi2_present(void* arg) {
    uintptr_t key = (uintptr_t)arg;
    u32 si = (u32)(key >> 8 & 0xf), ci = (u32)(key >> 4 & 0xf), slot = (u32)(key & 0xf), present = (u32)(key >> 32);
    /* a save state's key is checked as the handler made it */
    if (si >= VI2_SESSIONS || ci >= VI2_CLIENTS || slot >= VI2_BUFFER_SLOTS || !present) return;
    Vi2Session* s = &g_hle.vi.vi2[si];
    Vi2Client* c = &s->clients[ci];
    pm_lock(&g_host.vi.lock);
    /* a session no longer live is cleared by IPC outside this lock: nothing of it is read */
    if (!(g_hle.vi.vi2_live >> si & 1) || c->queued[slot].present != present) {
        pm_unlock(&g_host.vi.lock);
        return;
    }
    Vi2Queued q = c->queued[slot];
    int shown = c->bound == 1 && s->endpoint == c->token;
    pm_unlock(&g_host.vi.lock);
    u32 memory;
    NvGsvImageShape shape;
    if (shown && !nv_native_window_image(q.image, &memory, &shape)) {
        u32 w = (shape.extent & 0xffff) + 1, h = (shape.extent >> 16) + 1;
        const i32 crop[4] = {0, 0, (i32)(q.extent & 0xffff) + 1, (i32)(q.extent >> 16) + 1};
        /* RGBA8, block linear with the image's block height in GOBs */
        vi_show_buffer(memory, 0, w, h, shape.pitch_gobs * 64, 3, shape.block_height_log2 - 3, 1, crop, 0, (int)slot);
    }
    u32 event = 0;
    pm_lock(&g_host.vi.lock);
    if ((g_hle.vi.vi2_live >> si & 1) && c->queued[slot].present == present) {
        c->queued[slot].present = 0;
        /* the frame on the display before this one comes free; with no display, this one and that one */
        u64 freed = c->shown ? 1ull << (c->shown - 1) : 0;
        if (shown) c->shown = (u8)(slot + 1);
        else {
            freed |= 1ull << slot;
            c->shown = 0;
        }
        if (freed) {
            c->free_slots |= freed;
            event = vi2_notify_nl(s, c->available_bit);
            if (event) kobj_ref(kobj_at(event)); /* vi2_destroy may let go of it once the lock is free */
        }
    }
    pm_unlock(&g_host.vi.lock);
    if (event) {
        kevent_signal(kev(event));
        kobj_unref(kobj_at(event));
    }
}
/* The image goes to the display once the GPU reaches its fence. Only what the presenter shows is taken: an RGBA8
   window image of 16-GOB blocks, block linear, from its start, at most its size, in the SDR encoding. Like a Switch 1
   frame, the present then holds the game to the display's refresh */
static int nn_gsv_IVi2Session_PresentClientBuffer(SvcObj* self, u64 token, u8 slot, u32 image,
                                                  const nn_gsv_Vi2BufferInfo* info,
                                                  const nn_gsv_NativeSyncHandle* fence, u64* free_slots,
                                                  nn_gsv_NativeSyncHandle* release) {
    (void)release; /* the display gives no fence back: a slot comes free only after the display let go of it */
    Vi2Session* s = self->data;
    u32 id = ~fence->inverted_id;
    R_UNLESS(slot < VI2_BUFFER_SLOTS && !fence->reserved && fence->value <= UINT32_MAX, RC(1, 116));
    R_UNLESS(id == ~0u || id < SYNCPT_IDS, RC(1, 116));
    u32 memory;
    NvGsvImageShape shape;
    R_TRY(nv_native_window_image(image, &memory, &shape));
    u32 ew = info->extent & 0xffff, eh = info->extent >> 16;
    if (info->offset || info->layout != 3 || ew > (shape.extent & 0xffff) || eh > shape.extent >> 16 ||
        shape.pixel_format != 1 || shape.block_height_log2 != 7) {
        WARN_LIMIT(4, "vi2: a present of offset %u, layout %u, format %u and blocks of 2^%u rows is not supported\n",
                   info->offset, info->layout, shape.pixel_format, shape.block_height_log2);
        return RC(1, 33);
    }
    pm_lock(&g_host.vi.lock);
    Vi2Client* client = vi2_client_nl(s, token);
    int rc = !client                                ? RC(1, 121)
             : !client->encoded                     ? RC(1, 33)
             : !(client->free_slots >> slot & 1)    ? RC(1, 125)
                                                    : 0;
    uintptr_t key = 0;
    if (!rc) {
        if (!++g_hle.vi.vi2_presents) ++g_hle.vi.vi2_presents;
        client->queued[slot] = (Vi2Queued){g_hle.vi.vi2_presents, image, info->extent};
        client->free_slots &= ~(1ull << slot);
        *free_slots = client->free_slots;
        key = vi2_key((u32)(s - g_hle.vi.vi2), (u32)(client - s->clients), slot, g_hle.vi.vi2_presents);
    }
    pm_unlock(&g_host.vi.lock);
    R_TRY(rc);
    /* the slot is queued and the reply known: a game pause lets the present finish */
    kthread_commit();
    u32 value = (u32)fence->value;
    nv_gpu_call_after(id == ~0u ? 0 : 1, &id, &value, vi2_present, (void*)key);
    present_pace(1, slot);
    R_SUCCEED();
}
/* The client gives a slot back without an image: it is free again, and a present queued in it is not shown */
static int nn_gsv_IVi2Session_CancelClientBuffer(SvcObj* self, u64 token, u8 slot, const nn_gsv_Vi2BufferInfo* info,
                                                 u64* free_slots, nn_gsv_NativeSyncHandle* release) {
    (void)info, (void)release;
    Vi2Session* s = self->data;
    R_UNLESS(slot < VI2_BUFFER_SLOTS, RC(1, 116));
    pm_lock(&g_host.vi.lock);
    Vi2Client* client = vi2_client_nl(s, token);
    if (client) {
        client->queued[slot].present = 0;
        if (client->shown == slot + 1) client->shown = 0;
        client->free_slots |= 1ull << slot;
        *free_slots = client->free_slots;
    }
    pm_unlock(&g_host.vi.lock);
    return client ? 0 : RC(1, 121);
}
/* Request groups, as eden-ns2 answers them: vi's result 4 for a group out of range, 5 for one already made or configured,
   7 for a missing event */
#define VI2_GROUP_RANGE  RC(114, 4)
#define VI2_GROUP_EXISTS RC(114, 5)
/* The group keeps its event only once the caller has a handle to it: a caller without one could neither make the
   group again nor duplicate its event */
static int nn_gsv_IVi2Session_CreateRequestGroupEvent(SvcObj* self, u32 group, u32* event) {
    Vi2Session* s = self->data;
    R_UNLESS(group < VI2_GROUPS, VI2_GROUP_RANGE);
    pm_lock(&g_host.vi.lock);
    int busy = s->group_events[group] != 0;
    pm_unlock(&g_host.vi.lock);
    R_UNLESS(!busy, VI2_GROUP_EXISTS);
    KEvent* e = kevent_new("vi2 request group");
    kobj_ref(&e->o); /* the session's reference, once the group holds the event */
    u32 handle = h_new(&e->o);
    pm_lock(&g_host.vi.lock);
    int rc = !handle ? RC(1, 105) : s->group_events[group] ? VI2_GROUP_EXISTS : 0;
    if (!rc) s->group_events[group] = kobj_id(&e->o);
    pm_unlock(&g_host.vi.lock);
    if (rc) { /* no handle, or another thread made the group meanwhile */
        if (handle) h_close(handle);
        kobj_unref(&e->o);
        return rc;
    }
    *event = handle;
    return 0;
}
/* Another handle to a group's event, for a caller that shows it has one */
static int nn_gsv_IVi2Session_DuplicateRequestGroupEvent(SvcObj* self, u32 group, u32 handle, u32* event) {
    Vi2Session* s = self->data;
    R_UNLESS(group < VI2_GROUPS, VI2_GROUP_RANGE);
    pm_lock(&g_host.vi.lock);
    u32 id = s->group_events[group];
    pm_unlock(&g_host.vi.lock);
    R_UNLESS(id, VI_NOT_FOUND);
    KObj* shown = h_get_ref(handle, KO_EVENT);
    R_UNLESS(shown, RC(1, 114));
    int same = kobj_id(shown) == id;
    kobj_unref(shown);
    R_UNLESS(same, RC(1, 114));
    *event = h_new(kobj_at(id));
    return *event ? 0 : RC(1, 105);
}
static int nn_gsv_IVi2Session_ConfigureRequestGroups(SvcObj* self, u32 limit) {
    Vi2Session* s = self->data;
    R_UNLESS(limit < VI2_GROUPS, VI2_GROUP_RANGE);
    pm_lock(&g_host.vi.lock);
    int rc = s->groups_configured ? VI2_GROUP_EXISTS : 0;
    if (!rc) {
        s->group_limit = limit;
        s->groups_configured = 1;
    }
    pm_unlock(&g_host.vi.lock);
    return rc;
}
/* 261 to 272 register (odd) and unregister (even) a group's event for one of six kinds of display change on a
   display, kind (command - 261) / 2. Kind 0 is the vsync: the display's refresh signals the event. For the other
   kinds the event is signaled at once, so that the guest reads the state it watches, which never changes here. */
static int vi2_monitor(SvcObj* self, u64 name, u32 group, u32 kind) {
    Vi2Session* s = self->data;
    R_UNLESS(group < VI2_GROUPS && name, VI2_GROUP_RANGE);
    pm_lock(&g_host.vi.lock);
    u32 event = s->group_events[group];
    int rc = vi2_display_index(name) < 0 || !event ? VI_NOT_FOUND : s->monitor_display[group] ? VI2_GROUP_EXISTS : 0;
    if (!rc) {
        s->monitor_display[group] = name;
        s->monitor_kind[group] = (u8)kind;
    }
    pm_unlock(&g_host.vi.lock);
    if (!rc && kind) kevent_signal(kev(event));
    return rc;
}
/* Unregistering never fails, as in eden-ns2: only a registration of that display, group and kind goes */
static int vi2_unmonitor(SvcObj* self, u64 name, u32 group, u32 kind) {
    Vi2Session* s = self->data;
    u32 event = 0;
    pm_lock(&g_host.vi.lock);
    if (group < VI2_GROUPS && name && s->monitor_display[group] == name && s->monitor_kind[group] == kind) {
        s->monitor_display[group] = 0;
        event = s->group_events[group];
    }
    pm_unlock(&g_host.vi.lock);
    if (event) kevent_signal(kev(event));
    return 0;
}
static int nn_gsv_IVi2Session_RegisterVsyncMonitor(SvcObj* self, u64 name, u32 group) {
    return vi2_monitor(self, name, group, 0);
}
static int nn_gsv_IVi2Session_UnregisterVsyncMonitor(SvcObj* self, u64 name, u32 group) {
    return vi2_unmonitor(self, name, group, 0);
}
static int nn_gsv_IVi2Session_RegisterRefreshRateMonitor(SvcObj* self, u64 name, u32 group) {
    return vi2_monitor(self, name, group, 1);
}
static int nn_gsv_IVi2Session_UnregisterRefreshRateMonitor(SvcObj* self, u64 name, u32 group) {
    return vi2_unmonitor(self, name, group, 1);
}
static int nn_gsv_IVi2Session_RegisterHdrStatusMonitor(SvcObj* self, u64 name, u32 group) {
    return vi2_monitor(self, name, group, 2);
}
static int nn_gsv_IVi2Session_UnregisterHdrStatusMonitor(SvcObj* self, u64 name, u32 group) {
    return vi2_unmonitor(self, name, group, 2);
}
static int nn_gsv_IVi2Session_RegisterHdrPaperWhiteMonitor(SvcObj* self, u64 name, u32 group) {
    return vi2_monitor(self, name, group, 3);
}
static int nn_gsv_IVi2Session_UnregisterHdrPaperWhiteMonitor(SvcObj* self, u64 name, u32 group) {
    return vi2_unmonitor(self, name, group, 3);
}
static int nn_gsv_IVi2Session_RegisterBufferMaxResolutionMonitor(SvcObj* self, u64 name, u32 group) {
    return vi2_monitor(self, name, group, 4);
}
static int nn_gsv_IVi2Session_UnregisterBufferMaxResolutionMonitor(SvcObj* self, u64 name, u32 group) {
    return vi2_unmonitor(self, name, group, 4);
}
static int nn_gsv_IVi2Session_RegisterModeCapabilityMonitor(SvcObj* self, u64 name, u32 group) {
    return vi2_monitor(self, name, group, 5);
}
static int nn_gsv_IVi2Session_UnregisterModeCapabilityMonitor(SvcObj* self, u64 name, u32 group) {
    return vi2_unmonitor(self, name, group, 5);
}
static int vi2_dispatch(SvcObj* self, IpcCtx* c) {
    R_UNLESS(c->tipc, RC(1, 33));
    /* the SDK reads the event's slot before the result */
    c->tipc_error_moves = c->cmd == 11 || c->cmd == 31 || c->cmd == 32 ? 1 : 0;
    return nn_gsv_IVi2Session_dispatch(self, c);
}
static void vi2_destroy(SvcObj* self) {
    Vi2Session* s = self->data;
    /* Stop vsync delivery before IPC releases the pool slot, which it zeroes outside this lock */
    pm_lock(&g_host.vi.lock);
    g_hle.vi.vi2_live &= ~(1u << (u32)(s - g_hle.vi.vi2));
    pm_unlock(&g_host.vi.lock);
    kobj_unref(kobj_at(s->event));
    for (u32 i = 0; i < VI2_GROUPS; ++i) kobj_unref(kobj_at(s->group_events[i]));
}
int vi2_state_valid(const HleVi* v) {
    if (v->vi2_live >> VI2_SESSIONS) return 0;
    for (u32 i = 0; i < VI2_SESSIONS; ++i) {
        const Vi2Session* s = &v->vi2[i];
        if (((v->vi2_live >> i) & 1) != (s->obj != 0)) return 0;
        if (s->group_limit >= VI2_GROUPS || s->groups_configured > 1) return 0;
        for (u32 display = 0; display < VI2_DISPLAYS; ++display)
            if (s->display_notification[display] >= VI2_NOTIFICATIONS || s->display_refs[display] > 1024) return 0;
        for (u32 k = 0; k < VI2_GROUPS; ++k)
            if (s->monitor_kind[k] >= VI2_MONITOR_KINDS ||
                (s->monitor_display[k] && (!s->group_events[k] || vi2_display_index(s->monitor_display[k]) < 0))) return 0;
        u32 bits = 0;
        int endpoint_found = !s->endpoint;
        for (u32 k = 0; k < VI2_CLIENTS; ++k) {
            const Vi2Client* c = &s->clients[k];
            if (!c->token) {
                if (c->retire_bit || c->available_bit || c->free_slots || c->bound || c->encoded || c->shown)
                    return 0;
                for (u32 q = 0; q < VI2_BUFFER_SLOTS; ++q)
                    if (c->queued[q].present || c->queued[q].image || c->queued[q].extent) return 0;
                continue;
            }
            if (c->retire_bit >= VI2_NOTIFICATIONS || c->available_bit >= VI2_NOTIFICATIONS ||
                c->retire_bit == c->available_bit || c->free_slots >> VI2_BUFFER_SLOTS || c->bound > VI2_DISPLAYS ||
                c->encoded > 1 || c->shown > VI2_BUFFER_SLOTS || (c->shown && c->free_slots >> (c->shown - 1) & 1))
                return 0;
            /* a slot queued for the display is the display's, not the client's */
            for (u32 q = 0; q < VI2_BUFFER_SLOTS; ++q)
                if (c->queued[q].present && (c->free_slots >> q & 1)) return 0;
            u32 own = 1u << c->retire_bit | 1u << c->available_bit;
            if (bits & own) return 0;
            bits |= own;
            for (u32 j = 0; j < k; ++j)
                if (s->clients[j].token == c->token) return 0;
            if (c->token == s->endpoint && c->bound == 1) endpoint_found = 1;
        }
        if (bits != s->client_slots || !endpoint_found) return 0;
    }
    return 1;
}
SvcObj* vi2_session_new(void) {
    SvcObj* o = SVCOBJ_NEW_DATA("nn::gsv::IVi2Session", vi2_dispatch, g_hle.vi.vi2, g_hle.vi.vi2_next,
                                "Switch 2 display sessions");
    o->destroy = vi2_destroy;
    pm_lock(&g_host.vi.lock);
    g_hle.vi.vi2_live |= 1u << (u32)((Vi2Session*)o->data - g_hle.vi.vi2);
    pm_unlock(&g_host.vi.lock);
    return o;
}

static SvcObj* new_vi_u(void) { return nn_visrv_sf_IApplicationRootService_new(); }
static SvcObj* new_vi_s(void) { return nn_visrv_sf_ISystemRootService_new(); }
static SvcObj* new_vi_m(void) { return nn_visrv_sf_IManagerRootService_new(); }
void svc_vi_init(void) {
    pm_init(&g_host.vi.lock);
    pc_init(&g_host.vi.freed);
    g_hle.vi.vsync = owned_event("vi:vsync");
    nn_visrv_sf_IApplicationRootService_register(new_vi_u);
    nn_visrv_sf_ISystemRootService_register(new_vi_s);
    nn_visrv_sf_IManagerRootService_register(new_vi_m);
}
/* the functions a save state may hold of this file (state_fns.h) */
STATE_FN_TABLE(g_vi_state_fns,
               STATE_FN("vi", vi_present),
               STATE_FN("vi", vi2_present),
               STATE_FN("vi", vi2_dispatch),
               STATE_FN("vi", vi2_destroy));
