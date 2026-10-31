/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project
   csnd:SND, the old sound hardware's service. Ported from Azahar's core/hle/service/csnd/csnd_snd.cpp
   (GPL-2.0-or-later): the shared memory and mutex Initialize makes, the command lists ExecuteCommands walks, the
   channel and capture-unit masks, and the states UpdateState writes back are Azahar's. Like Azahar's, it makes no
   sound and emulates no capture. The state a game can read is modelled (whether a channel plays, its ADPCM state,
   the masks); a stop or a pause takes effect at once, without Azahar's fade of some milliseconds. The commands that
   only set what the hardware would play (volumes, addresses, timers, loops) are accepted and ignored */
#include "3ds/services/csnd.h"
#include "3ds/runtime/internal.h"
#include "common/utilities/endian.h"
#include <stdio.h>

#define CSND_ERR_INVALID_STATE 0xc8a0b7ffu
#define CSND_ERR_ALREADY_INIT  0xe0a0183bu
#define CSND_ERR_ADDRESS       0xe0e3fff6u
#define CSND_CLIENT_CHANNELS   0xffffff00u
#define CSND_CAPTURE_UNITS     2u
#define CSND_PAGE              0x1000u
#define CSND_MAX_SIZE          0x100000u
#define CSND_BLOCK             0x20u
#define CSND_LIST_END          0xffffu
#define CSND_STATE_SIZE        0xcu

static void reply(SnxCtrIpc* c, unsigned normal, unsigned translated) {
    c->out[0] = (c->in[0] & 0xffff0000u) | normal << 6 | translated;
    c->out[1] = 0;
}
static uint32_t physical(const SnxCtrProcess* p) { return p->objects[p->csnd.shared].data.shared.physical; }
/* the shared memory is the service's to read and write at any offset the game gave: all accesses are bounds checked */
static bool inside(const SnxCtrCsnd* s, uint32_t offset, uint32_t length) {
    return (uint64_t)offset + length <= s->size;
}
static bool read_bytes(SnxCtrProcess* p, uint32_t offset, void* bytes, uint32_t length) {
    return inside(&p->csnd, offset, length) && snx_ctr_phys_read(p->memory, physical(p) + offset, bytes, length) == SNX_CTR_MEM_OK;
}
static void write_bytes(SnxCtrProcess* p, uint32_t offset, const void* bytes, uint32_t length) {
    if (inside(&p->csnd, offset, length)) (void)snx_ctr_phys_write(p->memory, physical(p) + offset, bytes, length);
}
static void put32(SnxCtrProcess* p, uint32_t offset, uint32_t v) {
    unsigned char b[4];
    snx_st_le32(b, v);
    write_bytes(p, offset, b, 4);
}
static void put16(SnxCtrProcess* p, uint32_t offset, uint16_t v) {
    unsigned char b[2];
    snx_st_le16(b, v);
    write_bytes(p, offset, b, 2);
}
static void put8(SnxCtrProcess* p, uint32_t offset, uint8_t v) { write_bytes(p, offset, &v, 1); }
/* a channel's place in the states: the number of acquired channels before it */
static uint32_t ordinal(const SnxCtrCsnd* s, unsigned channel) {
    uint32_t below = s->channel_mask & ((1u << channel) - 1u), count = 0;
    for (; below; below &= below - 1) ++count;
    return count;
}
static void clear_channel_slot(SnxCtrProcess* p, unsigned channel) {
    SnxCtrCsnd* s = &p->csnd;
    put32(p, s->channel_offset + ordinal(s, channel) * CSND_STATE_SIZE + 8, 0);
}
static void update_state(SnxCtrProcess* p) {
    SnxCtrCsnd* s = &p->csnd;
    put32(p, s->master_offset, s->mask_register[0] & s->channel_mask);
    put32(p, s->master_offset + 4, s->mask_register[1] & s->capture_mask);
    uint32_t index = 0;
    for (unsigned ch = 0; ch < 32; ++ch) {
        if (!(s->channel_mask >> ch & 1)) continue;
        uint32_t base = s->channel_offset + index++ * CSND_STATE_SIZE;
        put8(p, base, s->channel[ch].enable);
        put16(p, base + 4, (uint16_t)s->channel[ch].adpcm);
        put8(p, base + 6, (uint8_t)(s->channel[ch].adpcm >> 16 & 0x7f));
        put32(p, base + 8, 0);
    }
    for (unsigned unit = 0; unit < CSND_CAPTURE_UNITS; ++unit) {
        if (!(s->capture_mask >> unit & 1)) continue;
        put8(p, s->capture_offset + unit * 8, 0);
        put32(p, s->capture_offset + unit * 8 + 4, 0);
    }
}
static void command(SnxCtrProcess* p, uint32_t id, const uint32_t params[6]) {
    SnxCtrCsnd* s = &p->csnd;
    unsigned channel = params[0] & 0x1f;
    if (id == 0x200) { /* ApplyMasks */
        s->mask_register[0] = s->channel_mask;
        s->mask_register[1] = s->capture_mask;
        return;
    }
    if (id == 0x300) {
        update_state(p);
        return;
    }
    if (id >= 0x100) return; /* capture is not emulated (Azahar warns of it) */
    if (!(s->channel_mask >> channel & 1)) return;
    SnxCtrCsndChannel* ch = &s->channel[channel];
    uint32_t value = params[1];
    switch (id) {
        case 0x000: ch->enable = value != 0; break;                    /* SetPlayStateRaw */
        case 0x001: ch->enable = value != 0; break;                    /* SetPlayState */
        case 0x00a: clear_channel_slot(p, channel); break;             /* SetBlock */
        case 0x00b: ch->adpcm = (value & 0xffff) | (params[2] & 0xff) << 16; ch->loop_adpcm = 0; break;
        case 0x00c: ch->loop_adpcm = (value & 0xffff) | (params[2] & 0xff) << 16; break;
        case 0x00e: { /* ConfigureChannel: format [12:13], play [14] of the first word */
            uint32_t format = params[0] >> 12 & 3;
            ch->enable = 0;
            if (format != 2) ch->adpcm = ch->loop_adpcm = 0; /* only IMA-ADPCM keeps a state */
            if (params[0] >> 14 & 1) ch->enable = 1;
            clear_channel_slot(p, channel);
            break;
        }
        default: break; /* what the hardware would play with: nothing here plays */
    }
}
static void execute(SnxCtrIpc* c) {
    if (!snx_ctr_ipc_expect(c, 1, 0)) return;
    SnxCtrProcess* p = c->process;
    SnxCtrCsnd* s = &p->csnd;
    if (!s->initialized) {
        snx_ctr_ipc_error_reply(c, CSND_ERR_INVALID_STATE);
        return;
    }
    uint32_t first = c->in[1], offset = first, remaining = s->size / CSND_BLOCK;
    while (offset != CSND_LIST_END) {
        unsigned char block[CSND_BLOCK];
        if (!read_bytes(p, offset, block, CSND_BLOCK) || !remaining--) {
            fprintf(stderr, "3ds csnd: a command list leaves the shared memory or does not end (offset 0x%x)\n", offset);
            break;
        }
        uint32_t params[6];
        for (unsigned i = 0; i < 6; ++i) params[i] = snx_ld_le32(block + 8 + i * 4);
        command(p, snx_ld_le16(block + 2), params);
        offset = snx_ld_le16(block);
    }
    put8(p, first + 4, 1); /* the first block is finished */
    reply(c, 1, 0);
}
static void initialize(SnxCtrIpc* c) {
    if (!snx_ctr_ipc_expect(c, 5, 0)) return;
    SnxCtrProcess* p = c->process;
    SnxCtrCsnd* s = &p->csnd;
    if (s->initialized) {
        snx_ctr_ipc_error_reply(c, CSND_ERR_ALREADY_INIT);
        return;
    }
    uint32_t size = c->in[1] ? c->in[1] : 1;
    if (size > CSND_MAX_SIZE) {
        snx_ctr_ipc_error_reply(c, 0xd86007f3u); /* out of memory */
        return;
    }
    size = (size + CSND_PAGE - 1) & ~(CSND_PAGE - 1);
    uint32_t mutex = 0, memory = 0, result = snx_ctr_object_create(p, CTR_OBJ_MUTEX, &mutex);
    if (!result) result = snx_ctr_object_create(p, CTR_OBJ_SHARED, &memory);
    uint32_t pages = result ? 0 : snx_ctr_shared_pages(p, size);
    if (!result && !pages) result = 0xd86007f3u;
    if (result) {
        if (mutex) (void)snx_ctr_handle_close(p, mutex);
        if (memory) (void)snx_ctr_handle_close(p, memory);
        snx_ctr_ipc_error_reply(c, result);
        return;
    }
    s->mutex = snx_ctr_handle_object(p, mutex);
    s->shared = snx_ctr_handle_object(p, memory);
    ++p->objects[s->mutex].refs;
    ++p->objects[s->shared].refs; /* the service's own, while the process lives */
    CtrObject* shared = &p->objects[s->shared];
    shared->data.shared.physical = pages;
    shared->data.shared.size = size;
    shared->data.shared.permissions = SNX_CTR_READ | SNX_CTR_WRITE;
    s->size = size;
    s->master_offset = c->in[2];
    s->channel_offset = c->in[3];
    s->capture_offset = c->in[4];
    s->direct_offset = c->in[5];
    s->initialized = 1;
    reply(c, 1, 3);
    c->out[2] = 0x04000000u; /* two copied handles: the mutex, then the shared memory */
    snx_ctr_ipc_output_handle(c, 3, mutex);
    snx_ctr_ipc_output_handle(c, 4, memory);
    if (c->state != SNX_CTR_IPC_RETURN) {
        snx_ctr_ipc_drop_handle(c, mutex);
        snx_ctr_ipc_drop_handle(c, memory);
        snx_ctr_object_release(p, s->mutex);
        snx_ctr_object_release(p, s->shared);
        s->initialized = 0;
        s->mutex = s->shared = 0;
    }
}
static void shutdown_service(SnxCtrProcess* p) {
    SnxCtrCsnd* s = &p->csnd;
    if (!s->initialized) return;
    snx_ctr_object_release(p, s->mutex);
    snx_ctr_object_release(p, s->shared);
    s->mutex = s->shared = 0;
    s->initialized = 0;
    /* the channels keep playing and stay acquired */
}
static bool cache_range(uint32_t address, uint32_t size) {
    uint64_t end = (uint64_t)address + size;
    return (address >= 0x14000000u && end <= 0x1c000000u) || (address >= 0x30000000u && end <= 0x40000000u);
}
void snx_ctr_csnd_dispatch(SnxCtrIpc* c) {
    SnxCtrProcess* p = c->process;
    SnxCtrCsnd* s = &p->csnd;
    switch (c->in[0] >> 16) {
        case 0x01: initialize(c); break;
        case 0x02:
            if (!snx_ctr_ipc_expect(c, 0, 0)) return;
            shutdown_service(p);
            reply(c, 1, 0);
            break;
        case 0x03: execute(c); break;
        case 0x05: /* AcquireSoundChannels: 8 to 31, 0 to 7 are for direct sound */
            if (!snx_ctr_ipc_expect(c, 0, 0)) return;
            s->channel_mask = CSND_CLIENT_CHANNELS;
            reply(c, 2, 0);
            c->out[2] = s->channel_mask;
            break;
        case 0x06: /* ReleaseSoundChannels */
            if (!snx_ctr_ipc_expect(c, 0, 0)) return;
            s->channel_mask = 0;
            reply(c, 1, 0);
            break;
        case 0x07: { /* AcquireCapUnit */
            if (!snx_ctr_ipc_expect(c, 0, 0)) return;
            unsigned unit = 0;
            while (unit < CSND_CAPTURE_UNITS && s->capture_in_use >> unit & 1) ++unit;
            reply(c, 2, 0);
            if (unit == CSND_CAPTURE_UNITS) {
                c->out[1] = CSND_ERR_INVALID_STATE;
                c->out[2] = UINT32_MAX;
                break;
            }
            s->capture_in_use |= 1u << unit;
            s->capture_mask |= 1u << unit;
            if (s->initialized) {
                put8(p, s->capture_offset + unit * 8, 0);
                put32(p, s->capture_offset + unit * 8 + 4, 0);
            }
            c->out[2] = unit;
            break;
        }
        case 0x08: /* ReleaseCapUnit(unit) */
            if (!snx_ctr_ipc_expect(c, 1, 0)) return;
            if (c->in[1] >= CSND_CAPTURE_UNITS || !(s->capture_mask >> c->in[1] & 1)) {
                snx_ctr_ipc_error_reply(c, CSND_ERR_INVALID_STATE);
                break;
            }
            s->capture_mask &= ~(1u << c->in[1]);
            s->capture_in_use &= ~(1u << c->in[1]);
            reply(c, 1, 0);
            break;
        case 0x09: case 0x0a: case 0x0b: /* Flush, Store and InvalidateDataCache(address, size, process): the range only */
            if (!snx_ctr_ipc_expect(c, 2, 2)) return;
            if (!cache_range(c->in[1], c->in[2]))
                snx_ctr_ipc_error_reply(c, CSND_ERR_ADDRESS);
            else
                reply(c, 1, 0);
            break;
        case 0x0c: /* Reset: every channel stops */
            if (!snx_ctr_ipc_expect(c, 0, 0)) return;
            for (unsigned ch = 0; ch < 32; ++ch) s->channel[ch].enable = 0;
            reply(c, 1, 0);
            break;
        default: c->state = SNX_CTR_IPC_UNKNOWN; break; /* PlaySoundDirectly (4) is empty in Azahar too */
    }
}
