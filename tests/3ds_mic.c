/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* mic:u (services/mic.c): the replies of its commands, the settings a game stores and the buffer full event, as
   Azahar's mic_u.cpp answers, reached through srv: as a game does. Then the samples a fake recording device
   (support/mic_fake.c) gives, in the block a game lent: each encoding, 15 at a time on the rate's clock, the part
   that loops or fills, its offset in the block's last word, the event at each fill, silence with no device, the
   millisecond the host wakes for them, a late clock, a new rate and a state's reopening */
#include "3ds/ipc/ctr_ipc.h"
#include "3ds/runtime/internal.h"
#include "3ds/services/mic.h"
#include "snx_settings.h"
#include "support/3ds_guest.h"
#include "support/mic_fake.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define BLOCK 0x1000u /* the block a game lends, at CTR_HEAP */
#define PART  0x10u   /* where in it StartSampling's part starts */

/* the reply's header to a request of these words */
static uint32_t call(SnxCtrProcess* p, uint32_t session, const uint32_t* words, unsigned count) {
    ctr_call(p, session, words, count, SNX_CTR_SVC_RETURN, 0);
    assert(!ctr_command_word(p, 1));
    return ctr_command_word(p, 0);
}
/* one heap page committed and lent as a block, readable and writable by both sides, as libctru's micInit makes it */
static uint32_t make_block(SnxCtrProcess* p) {
    uint32_t* r = p->cpu.state.cpu.r;
    r[0] = 3, r[1] = CTR_HEAP, r[2] = 0, r[3] = BLOCK, r[4] = 3; /* ControlMemory: commit */
    assert(snx_ctr_svc(p, 1) == SNX_CTR_SVC_RETURN && !r[0]);
    r[0] = 3, r[1] = CTR_HEAP, r[2] = BLOCK, r[3] = 3; /* CreateMemoryBlock */
    assert(snx_ctr_svc(p, 0x1e) == SNX_CTR_SVC_RETURN && !r[0]);
    return r[1];
}
static void start(SnxCtrProcess* p, uint32_t mic, uint32_t encoding, uint32_t rate, uint32_t size, uint32_t loop) {
    const uint32_t words[] = {0x00030140, encoding, rate, PART, size, loop};
    assert(call(p, mic, words, 6) == 0x00030040);
}
static uint32_t block_word(SnxCtrProcess* p, uint32_t at) {
    uint32_t v = 0;
    assert(snx_ctr_read_ram(p->space, CTR_HEAP + at, &v, 4) == SNX_CTR_MEM_OK);
    return v;
}
static void block_bytes(SnxCtrProcess* p, uint32_t at, void* out, uint32_t size) {
    assert(snx_ctr_read_ram(p->space, CTR_HEAP + at, out, size) == SNX_CTR_MEM_OK);
}
static void block_clear(SnxCtrProcess* p) {
    static const unsigned char zero[BLOCK];
    assert(snx_ctr_write_ram(p->space, CTR_HEAP, zero, BLOCK) == SNX_CTR_MEM_OK);
}
/* the clock's next n units of 15 samples due, which the update writes */
static void units(SnxCtrProcess* p, uint64_t n) {
    static const uint64_t hz[4] = {32728, 16364, 10909, 8182};
    snx_ctr_mic_update(p, p->mic.next_ns + (n - 1) * (15 * 1000000000ull / hz[p->mic.sample_rate]));
}
static int16_t g_values[] = {0, -1, 0x1234, -0x8000, 0x7fff};
static int16_t values(uint64_t n) { return g_values[n % 5]; }

/* the samples of the fake device, which records a ramp: 405 at first, 5 blocks of 10 ms at 8182 Hz, so the recording
   holds its 40 ms (327) and hands them over from the first */
static void samples(SnxCtrProcess* p, uint32_t mic, uint32_t event) {
    unsigned char bytes[64];
    uint16_t words[32];
    block_clear(p);
    start(p, mic, 3, 3, 64, 1); /* PCM16 signed at 8182 Hz, a part of 32 samples that loops */
    assert(g_mic_fake.dev && g_mic_fake.dev->rate == 8182 && !strcmp(g_mic_fake.device, ""));
    g_mic_fake.dev->recorded = 405;
    units(p, 1);
    block_bytes(p, PART, words, 30);
    for (unsigned i = 0; i < 15; ++i) assert(words[i] == i);
    assert(block_word(p, BLOCK - 4) == 30 && !p->objects[event].data.event.signaled);
    units(p, 2); /* 30 more: 17 fill the part, which starts again, and 13 follow */
    block_bytes(p, PART, words, 64);
    for (unsigned i = 0; i < 32; ++i) assert(words[i] == (i < 13 ? 32 + i : i));
    assert(block_word(p, BLOCK - 4) == 26 && p->objects[event].data.event.signaled);
    p->objects[event].data.event.signaled = 0;
    /* one that does not loop stays full, signalled once, the offset at its end */
    block_clear(p);
    start(p, mic, 3, 3, 64, 0);
    g_mic_fake.dev->recorded = 405;
    units(p, 3);
    assert(block_word(p, BLOCK - 4) == 64 && p->objects[event].data.event.signaled);
    p->objects[event].data.event.signaled = 0;
    units(p, 1);
    block_bytes(p, PART, words, 64);
    for (unsigned i = 0; i < 32; ++i) assert(words[i] == i);
    assert(block_word(p, BLOCK - 4) == 64 && !p->objects[event].data.event.signaled);
    /* each encoding, of 0, -1, 0x1234, -0x8000 and 0x7fff: unsigned is offset by half the range, 8 bits are the high
       byte */
    static const unsigned char expected[4][10] = {{0x80, 0x7f, 0x92, 0x00, 0xff},
                                                  {0x00, 0x80, 0xff, 0x7f, 0x34, 0x92, 0x00, 0x00, 0xff, 0xff},
                                                  {0x00, 0xff, 0x12, 0x80, 0x7f},
                                                  {0x00, 0x00, 0xff, 0xff, 0x34, 0x12, 0x00, 0x80, 0xff, 0x7f}};
    g_mic_fake.signal = values;
    for (uint32_t encoding = 0; encoding < 4; ++encoding) {
        block_clear(p);
        start(p, mic, encoding, 3, 64, 1);
        g_mic_fake.dev->recorded = 405;
        units(p, 1);
        block_bytes(p, PART, bytes, 10);
        assert(!memcmp(bytes, expected[encoding], encoding & 1 ? 10 : 5));
        assert(block_word(p, BLOCK - 4) == (encoding & 1 ? 30u : 15u));
    }
    g_mic_fake.signal = NULL;
    p->objects[event].data.event.signaled = 0;
    /* no device: silence, which unsigned PCM8 writes as 0x80 */
    assert(ctr_send_header(p, mic, 0x00050000) == 0x00050040 && !g_mic_fake.dev);
    g_mic_fake.none = true;
    block_clear(p);
    start(p, mic, 0, 3, 64, 1);
    units(p, 1);
    block_bytes(p, PART, bytes, 16);
    for (unsigned i = 0; i < 15; ++i) assert(bytes[i] == 0x80);
    assert(!bytes[15] && block_word(p, BLOCK - 4) == 15);
    g_mic_fake.none = false;
    /* at 32728 Hz a unit is 458 us: the host wakes each third, and writes the three. At 8182 Hz, each one */
    start(p, mic, 3, 0, 0x800, 1);
    uint64_t unit = 15 * 1000000000ull / 32728;
    assert(snx_ctr_mic_deadline(p) == p->mic.next_ns + 2 * unit);
    g_mic_fake.dev->recorded = 2000;
    snx_ctr_mic_update(p, snx_ctr_mic_deadline(p));
    assert(block_word(p, BLOCK - 4) == 90);
    /* a second late writes the last 100 ms: 218 units at 32728 Hz */
    g_mic_fake.dev->recorded = 20000;
    snx_ctr_mic_update(p, p->mic.next_ns + 1000000000ull);
    assert(block_word(p, BLOCK - 4) == (90 + 218 * 30) % 0x800);
    /* a new rate records anew at it */
    unsigned opens = g_mic_fake.opens, closes = g_mic_fake.closes;
    const uint32_t adjust[] = {0x00040040, 3};
    assert(call(p, mic, adjust, 2) == 0x00040040);
    assert(g_mic_fake.opens == opens + 1 && g_mic_fake.closes == closes + 1 && g_mic_fake.dev->rate == 8182);
    assert(snx_ctr_mic_deadline(p) == p->mic.next_ns);
    /* a state loaded while it samples opens its own; one that does not sample has none */
    snx_ctr_mic_reopen(p);
    assert(g_mic_fake.opens == opens + 2 && g_mic_fake.closes == closes + 2 && g_mic_fake.dev);
    assert(ctr_send_header(p, mic, 0x00050000) == 0x00050040 && !g_mic_fake.dev && !p->mic.next_ns);
    assert(snx_ctr_mic_deadline(p) == UINT64_MAX);
    snx_ctr_mic_reopen(p);
    assert(g_mic_fake.opens == opens + 2 && !g_mic_fake.dev);
    /* unmapped, nothing more reaches the block */
    start(p, mic, 3, 3, 64, 1);
    assert(ctr_send_header(p, mic, 0x00020000) == 0x00020040 && !p->mic.shared);
    block_clear(p);
    g_mic_fake.dev->recorded = 405;
    units(p, 1);
    assert(!block_word(p, PART) && !block_word(p, BLOCK - 4));
    assert(ctr_send_header(p, mic, 0x00050000) == 0x00050040);
}
int main(void) {
    g_settings.microphone[0] = 0;              /* the system's default device, the fake one; "off" by default */
    g_settings.microphone_noise_reduction = 0; /* the samples as recorded */
    unsigned char code[0x3000] = {0};
    SnxCtrImage image = ctr_image(code, sizeof code, 0x1000);
    SnxCtrProcess* p = snx_ctr_process_new(&image, SNX_CTR_OLD);
    assert(p);
    uint32_t srv = ctr_srv(p);
    const uint32_t registration[] = {0x00010002, 0x20, 0};
    ctr_call(p, srv, registration, 3, SNX_CTR_SVC_RETURN, 0);
    uint32_t mic = ctr_service_handle(p, srv, "mic:u");
    /* nothing samples, and the settings start at zero */
    assert(ctr_send_header(p, mic, 0x00060000) == 0x00060080 && !ctr_command_word(p, 2));
    assert(ctr_send_header(p, mic, 0x00090000) == 0x00090080 && !ctr_command_word(p, 2));
    assert(ctr_send_header(p, mic, 0x000b0000) == 0x000b0080 && !ctr_command_word(p, 2));
    assert(ctr_send_header(p, mic, 0x000e0000) == 0x000e0080 && !ctr_command_word(p, 2));
    /* MapSharedMem takes the size and a handle, UnmapSharedMem nothing: both answer a result alone */
    const uint32_t map[] = {0x00010042, 0x1000, 0, CTR_PROCESS_HANDLE};
    assert(call(p, mic, map, 4) == 0x00010040);
    assert(ctr_send_header(p, mic, 0x00020000) == 0x00020040);
    /* StartSampling (encoding, rate, offset, size, loop) starts it, AdjustSampling changes the rate, StopSampling ends
       it */
    const uint32_t start[] = {0x00030140, 1, 2, 0, 0x800, 1}, adjust[] = {0x00040040, 3};
    assert(call(p, mic, start, 6) == 0x00030040);
    assert(ctr_send_header(p, mic, 0x00060000) == 0x00060080 && ctr_command_word(p, 2) == 1);
    assert(p->mic.encoding == 1 && p->mic.sample_rate == 2);
    assert(call(p, mic, adjust, 2) == 0x00040040 && p->mic.sample_rate == 3);
    assert(ctr_send_header(p, mic, 0x00050000) == 0x00050040);
    assert(ctr_send_header(p, mic, 0x00060000) == 0x00060080 && !ctr_command_word(p, 2));
    /* the settings are kept and read back: a gain is a byte, a bool the word's low byte (0x100 is false) */
    const uint32_t gain[] = {0x00080040, 0x1a5}, power[] = {0x000a0040, 2}, clamp[] = {0x000d0040, 0x100};
    const uint32_t shell[] = {0x000f0040, 1}, version[] = {0x00100040, 0x11};
    assert(call(p, mic, gain, 2) == 0x00080040);
    assert(ctr_send_header(p, mic, 0x00090000) == 0x00090080 && ctr_command_word(p, 2) == 0xa5);
    assert(call(p, mic, power, 2) == 0x000a0040);
    assert(ctr_send_header(p, mic, 0x000b0000) == 0x000b0080 && ctr_command_word(p, 2) == 1);
    assert(call(p, mic, clamp, 2) == 0x000d0040);
    assert(ctr_send_header(p, mic, 0x000e0000) == 0x000e0080 && !ctr_command_word(p, 2));
    assert(call(p, mic, shell, 2) == 0x000f0040 && p->mic.allow_shell_closed == 1);
    assert(call(p, mic, version, 2) == 0x00100040);
    /* SetIirFilterMic gives its mapped buffer back */
    const uint32_t filter[] = {0x000c0042, 0x20, 0x20a, 0x300000};
    assert(call(p, mic, filter, 4) == 0x000c0042 && ctr_command_word(p, 2) == 0x20a &&
           ctr_command_word(p, 3) == 0x300000);
    /* the buffer full event is a real event, the same one each time */
    assert(ctr_send_header(p, mic, 0x00070000) == 0x00070042 && !ctr_command_word(p, 2));
    uint32_t id = snx_ctr_handle_object(p, ctr_command_word(p, 3));
    assert(id && id == p->mic.event && p->objects[id].type == CTR_OBJ_EVENT);
    assert(ctr_send_header(p, mic, 0x00070000) == 0x00070042 && snx_ctr_handle_object(p, ctr_command_word(p, 3)) == id);
    /* a block lent: the service keeps it until it is unmapped */
    uint32_t block = make_block(p), object = snx_ctr_handle_object(p, block);
    const uint32_t lend[] = {0x00010042, BLOCK, 0, block};
    assert(call(p, mic, lend, 4) == 0x00010040 && p->mic.shared == object && p->mic.shared_size == BLOCK);
    samples(p, mic, id);
    assert(call(p, mic, lend, 4) == 0x00010040 && p->mic.shared == object);
    /* a size past the block's: what was sent is said, and the run stops on it */
    const uint32_t past[] = {0x00010042, BLOCK + 0x1000, 0, block};
    ctr_request(p, past, 4);
    ctr_send(p, mic, SNX_CTR_SVC_UNSUPPORTED, 0);
    assert(p->mic.shared == object && p->mic.shared_size == BLOCK);
    /* no such encoding or rate: what was sent is said, and the run stops on it */
    const uint32_t encoding[] = {0x00030140, 4, 0, PART, 64, 1}, rate[] = {0x00030140, 0, 4, PART, 64, 1};
    ctr_request(p, encoding, 6);
    ctr_send(p, mic, SNX_CTR_SVC_UNSUPPORTED, 0);
    ctr_request(p, rate, 6);
    ctr_send(p, mic, SNX_CTR_SVC_UNSUPPORTED, 0);
    assert(!p->mic.sampling);
    snx_ctr_process_free(p);
    puts("3ds mic:u: its commands, settings, buffer full event and samples passed");
    return 0;
}
