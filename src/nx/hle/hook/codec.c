/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* nn::codec's hardware Opus decoder of the Ounce's SDK. Its SDK runs it on the audio DSP through nn::codec::hwacod (a
   client session of IHardwareOpusDecoderManager's command 100, a task queue in shared memory), which this runtime
   doesn't have: the functions the game calls are run here instead, on the Opus service's FFmpeg decoder (audio.c).

   A decode request is a packet behind its 8 byte header (a big-endian size and the encoder's final range), as the
   older service's. Each is decoded as it is enqueued and waits; ExecuteHardwareOpusDecoderDecoding completes every
   waiting request of the manager's decoders and signals the manager's system event, whose readable handle the manager's
   initialization attaches to the game's SystemEventType as the SDK's does; Dequeue hands the oldest completed one's
   samples over. A packet the decoder refuses, or a decoder that can't be opened, gives the frame's length of silence,
   so the game's audio goes on. The state is the runtime's, not the guest's: a save state doesn't hold it */
#include "nx/hle/hook/hook.h"
#include "hle/hook/hooks.hook.h"
#include "nx/hle/kernel/kernel.h"
#include "nx/hle/services/services_int.h"
#include "nx/runtime/platform/log.h"
#include <stdlib.h>
#include <string.h>

#define CODEC_MANAGERS     8
#define CODEC_DECODERS     512
#define CODEC_REQUESTS     64                   /* a decoder's waiting and completed requests */
#define CODEC_MAX_SAMPLES  (48000 * 120 / 1000) /* 120 ms at 48 kHz, an Opus packet's longest */
#define CODEC_FREE_BUFFERS 256                  /* what GetHardwareOpusDecoderManagerFreeIoBufferCount answers */
#define CODEC_FAILED       0x1ee6f              /* a refused call: codec's module (111), an error of its own */

typedef struct {
    i16* pcm;
    u32 samples;
} CodecRequest;
typedef struct {
    u64 guest, manager;
    HwOpus* h;
    u32 rate, channels, frame; /* frame: samples a request of silence holds */
    int reset;                 /* the next packet starts the stream again */
    int tried;                 /* h was asked for: a decoder is opened at its first packet, as many are never used */
    CodecRequest req[CODEC_REQUESTS];
    u32 head, completed, waiting;
} CodecDecoder;
typedef struct {
    u64 guest;
    KEvent* event;
} CodecManager;

static PMutex g_lk;
static int g_lk_ready;
static CodecManager g_managers[CODEC_MANAGERS];
static CodecDecoder* g_decoders[CODEC_DECODERS];

static void lock(void) {
    int ready = __atomic_load_n(&g_lk_ready, __ATOMIC_ACQUIRE);
    if (ready != 2) {
        int expected = 0;
        if (__atomic_compare_exchange_n(&g_lk_ready, &expected, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            pm_init(&g_lk);
            __atomic_store_n(&g_lk_ready, 2, __ATOMIC_RELEASE);
        } else
            while (__atomic_load_n(&g_lk_ready, __ATOMIC_ACQUIRE) != 2) {}
    }
    pm_lock(&g_lk);
}
static void unlock(void) { pm_unlock(&g_lk); }

void hook_guest_call(CPU* c, u64 fn, u64 a0, u64 a1, u64 a2, u64 a3) {
    u64 lr = c->x[30], pc = fn;
    c->x[0] = a0;
    c->x[1] = a1;
    c->x[2] = a2;
    c->x[3] = a3;
    do snx_lookup(c, pc)(c, pc);
    while ((pc = c->pc) != lr);
    c->x[30] = lr;
}
KEvent* hook_attach_event(CPU* c, u64 event, const char* name) {
    static const char attach[] =
        "_ZN2nn2os33AttachReadableHandleToSystemEventEPNS0_15SystemEventTypeEjbNS0_14EventClearModeE";
    u64 fn = kernel_find_symbol_exact(attach);
    KEvent* e;
    u32 h;
    if (!fn) return NULL;
    e = kevent_new(name);
    /* a reference held while the handle is made, so that a full handle table destroys the event here */
    kobj_ref(&e->o);
    h = h_new(&e->o); /* the handle's reference, which the SystemEvent's destruction closes; the caller keeps its own */
    kobj_unref(&e->o);
    if (!h) return NULL;
    if (event) hook_guest_call(c, fn, event, h, 1, 1);
    return e;
}

static CodecManager* manager_of(u64 guest) {
    for (u32 i = 0; i < CODEC_MANAGERS; i++)
        if (g_managers[i].guest == guest) return &g_managers[i];
    return NULL;
}
static CodecDecoder** decoder_slot(u64 guest) {
    for (u32 i = 0; i < CODEC_DECODERS; i++)
        if (g_decoders[i] && g_decoders[i]->guest == guest) return &g_decoders[i];
    return NULL;
}
static void drop_requests(CodecDecoder* d) {
    for (u32 i = 0; i < CODEC_REQUESTS; i++) {
        free(d->req[i].pcm);
        d->req[i] = (CodecRequest){0};
    }
    d->head = d->completed = d->waiting = 0;
}

/* the work buffer the game gives the manager: none of it is used here */
u64 codec_manager_work_size(CPU* c, i32 rate, i32 channels, i64 frame, i32 decoders, i32 buffers) {
    (void)c, (void)rate, (void)channels, (void)frame;
    return 0x10000 + (u64)(decoders > 0 ? decoders : 1) * 0x1000 + (u64)(buffers > 0 ? buffers : 1) * 0x100;
}

/* the manager's system event: a kernel event of the runtime's, its readable handle attached to the game's
   SystemEventType by the SDK's own nn::os::AttachReadableHandleToSystemEvent (auto close, auto clear), as the SDK's
   manager does with the DSP session's */
u32 codec_manager_init(CPU* c, u64 manager, u64 event, i32 rate, i32 channels, i64 frame, i32 decoders, i32 buffers,
                       u64 work) {
    CodecManager* m;
    KEvent* e;
    (void)work;
    lock();
    m = manager_of(manager);
    if (!m) m = manager_of(0);
    if (!m) {
        unlock();
        WARN("codec: no room for another hardware Opus decoder manager: the game's audio stops\n");
        return CODEC_FAILED;
    }
    m->guest = manager;
    unlock();
    e = hook_attach_event(c, event, "hwopus manager");
    if (!e) {
        WARN("codec: the SDK exports no AttachReadableHandleToSystemEvent for the hardware Opus manager's event\n");
        lock();
        *m = (CodecManager){0};
        unlock();
        return CODEC_FAILED;
    }
    lock();
    m->event = e;
    unlock();
    LOG("codec: hardware Opus decoder manager %llx: %d Hz, %d channels, frames of %lld us, %d decoders, %d buffers; "
        "decoded here with FFmpeg\n",
        (unsigned long long)manager, rate, channels, (long long)(frame / 1000), decoders, buffers);
    return 0;
}
void codec_manager_finalize(CPU* c, u64 manager) {
    CodecManager* m;
    (void)c;
    lock();
    m = manager_of(manager);
    if (m) {
        if (m->event) kobj_unref(&m->event->o);
        *m = (CodecManager){0};
    }
    unlock();
}
/* every waiting request of the manager's decoders completed (they were decoded when enqueued), and its event
   signaled, as the DSP signals a pass's end */
u32 codec_manager_execute(CPU* c, u64 manager) {
    CodecManager* m;
    (void)c;
    lock();
    for (u32 i = 0; i < CODEC_DECODERS; i++) {
        CodecDecoder* d = g_decoders[i];
        if (d && d->manager == manager) {
            d->completed += d->waiting;
            d->waiting = 0;
        }
    }
    m = manager_of(manager);
    if (m && m->event) kevent_signal(m->event);
    unlock();
    return 0;
}
i32 codec_manager_free_buffers(CPU* c, u64 manager) {
    (void)c, (void)manager;
    return CODEC_FREE_BUFFERS;
}

u32 codec_decoder_init(CPU* c, u64 decoder, i32 rate, i32 channels, i64 frame, u64 manager) {
    CodecDecoder **slot, *d;
    (void)c;
    if (rate <= 0 || channels < 1 || channels > 2) return CODEC_FAILED;
    d = calloc(1, sizeof *d);
    if (!d) return CODEC_FAILED;
    d->guest = decoder;
    d->manager = manager;
    d->rate = (u32)rate;
    d->channels = (u32)channels;
    d->frame = frame > 0 ? (u32)((u64)rate * (u64)frame / 1000000000ull) : (u32)rate / 50;
    if (!d->frame || d->frame > CODEC_MAX_SAMPLES) d->frame = (u32)rate / 50;
    lock();
    slot = decoder_slot(decoder);
    if (slot) { /* initialized again without a finalize: the old one goes */
        hwopus_delete((*slot)->h);
        drop_requests(*slot);
        free(*slot);
    }
    if (!slot)
        for (u32 i = 0; i < CODEC_DECODERS && !slot; i++)
            if (!g_decoders[i]) slot = &g_decoders[i];
    if (!slot) {
        unlock();
        hwopus_delete(d->h);
        free(d);
        WARN_LIMIT(4, "codec: more than %d hardware Opus decoders\n", CODEC_DECODERS);
        return CODEC_FAILED;
    }
    *slot = d;
    unlock();
    LOG_LIMIT(16, "codec: hardware Opus decoder %llx: %d Hz, %d channels, frames of %u samples\n",
              (unsigned long long)decoder, rate, channels, d->frame);
    return 0;
}
void codec_decoder_finalize(CPU* c, u64 decoder) {
    CodecDecoder** slot;
    (void)c;
    lock();
    slot = decoder_slot(decoder);
    if (slot) {
        hwopus_delete((*slot)->h);
        drop_requests(*slot);
        free(*slot);
        *slot = NULL;
    }
    unlock();
}
/* its requests dropped, and the next packet starts the stream again */
void codec_decoder_reset(CPU* c, u64 decoder) {
    CodecDecoder** slot;
    (void)c;
    lock();
    slot = decoder_slot(decoder);
    if (slot) {
        drop_requests(*slot);
        (*slot)->reset = 1;
    }
    unlock();
}

/* a packet decoded now into a waiting request: all of the data is consumed */
u32 codec_decoder_enqueue(CPU* c, u64 decoder, u64 consumed, u64 data, u64 size) {
    static i16 pcm[CODEC_MAX_SAMPLES * 2];
    CodecDecoder** slot;
    CodecDecoder* d;
    CodecRequest* r;
    u32 used = 0, samples = 0;
    (void)c;
    lock();
    slot = decoder_slot(decoder);
    d = slot ? *slot : NULL;
    if (!d || d->completed + d->waiting >= CODEC_REQUESTS) {
        unlock();
        return CODEC_FAILED;
    }
    if (!d->h && !d->tried) {
        d->tried = 1;
        d->h = hwopus_new(d->rate, d->channels);
        if (!d->h)
            WARN_LIMIT(4, "codec: no FFmpeg Opus decoder for %u channels at %u Hz: its requests give silence\n",
                       d->channels, d->rate);
    }
    if (!d->h || R_FAILED(hwopus_decode_packet(d->h, GP(data), size, pcm, sizeof pcm, d->reset, &used, &samples)) ||
        !samples) {
        samples = d->frame;
        memset(pcm, 0, (size_t)samples * d->channels * sizeof *pcm);
        LOG_LIMIT(8, "codec: a packet of %llu bytes the decoder refused: %u samples of silence\n",
                  (unsigned long long)size, samples);
    }
    d->reset = 0;
    r = &d->req[(d->head + d->completed + d->waiting) % CODEC_REQUESTS];
    r->pcm = malloc((size_t)samples * d->channels * sizeof *pcm);
    if (!r->pcm) {
        unlock();
        return CODEC_FAILED;
    }
    memcpy(r->pcm, pcm, (size_t)samples * d->channels * sizeof *pcm);
    r->samples = samples;
    d->waiting++;
    unlock();
    if (consumed) gw64(consumed, size);
    return 0;
}
/* the oldest completed request's samples, interleaved, as many as out holds: none (0 samples) when nothing is
   completed */
u32 codec_decoder_dequeue(CPU* c, u64 decoder, u64 samples, u64 out, u64 size) {
    CodecDecoder** slot;
    CodecDecoder* d;
    CodecRequest* r;
    u32 n = 0;
    (void)c;
    lock();
    slot = decoder_slot(decoder);
    d = slot ? *slot : NULL;
    if (d && d->completed) {
        u64 bytes;
        r = &d->req[d->head];
        n = r->samples;
        bytes = (u64)n * d->channels * sizeof(i16);
        if (bytes > size) {
            n = (u32)(size / (d->channels * sizeof(i16)));
            bytes = (u64)n * d->channels * sizeof(i16);
        }
        memcpy(GP(out), r->pcm, bytes);
        free(r->pcm);
        *r = (CodecRequest){0};
        d->head = (d->head + 1) % CODEC_REQUESTS;
        d->completed--;
    }
    unlock();
    if (samples) gw32(samples, n);
    return d ? 0 : CODEC_FAILED;
}
static i32 count(u64 decoder, int which) {
    CodecDecoder** slot;
    i32 n = 0;
    lock();
    slot = decoder_slot(decoder);
    if (slot) n = (i32)(which == 0 ? (*slot)->waiting : which == 2 ? (*slot)->completed : 0);
    unlock();
    return n;
}
i32 codec_decoder_waiting(CPU* c, u64 decoder) {
    (void)c;
    return count(decoder, 0);
}
/* none is ever executing: Execute completes what waits at once */
i32 codec_decoder_executing(CPU* c, u64 decoder) {
    (void)c;
    return count(decoder, 1);
}
i32 codec_decoder_completed(CPU* c, u64 decoder) {
    (void)c;
    return count(decoder, 2);
}
