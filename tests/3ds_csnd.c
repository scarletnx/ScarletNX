/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* csnd:SND: Initialize's shared memory and mutex, the masks, the command lists ExecuteCommands walks and the states
   UpdateState writes back, as Azahar's csnd_snd.cpp has them */
#include "3ds/runtime/internal.h"
#include "3ds/ipc/ctr_ipc.h"
#include "common/utilities/endian.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define COMMAND (SNX_CTR_INITIAL_TLS + 0x80)
#define STATE_ERROR 0xc8a0b7ffu
static uint32_t word(SnxCtrProcess* p, unsigned index) {
    unsigned char b[4];
    assert(snx_ctr_read_ram(p->space, COMMAND + index * 4, b, 4) == SNX_CTR_MEM_OK);
    return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}
static uint32_t send(SnxCtrProcess* p, uint32_t session, uint32_t header, const uint32_t args[5]) {
    uint32_t in[6] = {header};
    for (unsigned i = 0; args && i < 5; ++i) in[i + 1] = args[i];
    unsigned char bytes[24];
    for (unsigned i = 0; i < 6; ++i)
        for (unsigned j = 0; j < 4; ++j) bytes[i * 4 + j] = (unsigned char)(in[i] >> (j * 8));
    assert(snx_ctr_write_ram(p->space, COMMAND, bytes, sizeof bytes) == SNX_CTR_MEM_OK);
    p->cpu.state.cpu.r[0] = session;
    assert(snx_ctr_svc(p, 0x32) == SNX_CTR_SVC_RETURN && !p->cpu.state.cpu.r[0]);
    return word(p, 0);
}
static uint32_t shared_read(SnxCtrProcess* p, uint32_t offset, unsigned size) {
    unsigned char b[4] = {0};
    assert(snx_ctr_phys_read(p->memory, p->objects[p->csnd.shared].data.shared.physical + offset, b, size) ==
           SNX_CTR_MEM_OK);
    return size == 1 ? b[0] : size == 2 ? snx_ld_le16(b) : snx_ld_le32(b);
}
static void shared_write(SnxCtrProcess* p, uint32_t offset, const void* bytes, unsigned size) {
    assert(snx_ctr_phys_write(p->memory, p->objects[p->csnd.shared].data.shared.physical + offset, bytes, size) ==
           SNX_CTR_MEM_OK);
}
/* a command block of Azahar's layout: next offset, command, finished byte, then the parameters */
static void block(SnxCtrProcess* p, uint32_t offset, uint16_t next, uint16_t id, uint32_t p0, uint32_t p1, uint32_t p2) {
    unsigned char b[0x20] = {0};
    snx_st_le16(b, next);
    snx_st_le16(b + 2, id);
    snx_st_le32(b + 8, p0);
    snx_st_le32(b + 12, p1);
    snx_st_le32(b + 16, p2);
    shared_write(p, offset, b, sizeof b);
}
static void execute(SnxCtrProcess* p, uint32_t s, uint32_t first) {
    const uint32_t args[5] = {first};
    assert(send(p, s, 0x00030040, args) == 0x00030040 && !word(p, 1));
}
int main(void) {
    unsigned char code[0x3000] = {0};
    SnxCtrImage image = {.segment = {{0x100000, 1, 16}, {0x200000, 1, 4}, {0x300000, 1, 4}},
                         .stack_size = 0x4000,
                         .main_priority = 0x30,
                         .bss_size = 0x1000,
                         .code = code,
                         .code_size = sizeof code};
    SnxCtrProcess* p = snx_ctr_process_new(&image, SNX_CTR_OLD);
    assert(p);
    uint32_t s = 0;
    assert(!snx_ctr_session_create(p, SNX_CTR_SERVICE_CSND, &s));
    /* a command before Initialize has no memory to read */
    const uint32_t first[5] = {0x400};
    send(p, s, 0x00030040, first);
    assert(word(p, 1) == STATE_ERROR);
    /* Initialize(size, master, channels, captures, direct): a mutex and the shared memory, two copied handles */
    const uint32_t layout[5] = {0x800, 0, 0x10, 0x100, 0x200};
    assert(send(p, s, 0x00010140, layout) == 0x00010043 && !word(p, 1) && word(p, 2) == 0x04000000u);
    uint32_t mutex = snx_ctr_handle_object(p, word(p, 3)), memory = snx_ctr_handle_object(p, word(p, 4));
    assert(mutex && p->objects[mutex].type == CTR_OBJ_MUTEX && memory && p->objects[memory].type == CTR_OBJ_SHARED);
    assert(p->objects[memory].data.shared.size == 0x1000); /* a page at least */
    send(p, s, 0x00010140, layout);
    assert(word(p, 1) == 0xe0a0183bu);
    /* the masks: channels 8 to 31, and two capture units */
    assert(send(p, s, 0x00050000, NULL) == 0x00050080 && !word(p, 1) && word(p, 2) == 0xffffff00u);
    assert(send(p, s, 0x00070000, NULL) == 0x00070080 && !word(p, 1) && word(p, 2) == 0);
    assert(send(p, s, 0x00070000, NULL) == 0x00070080 && !word(p, 1) && word(p, 2) == 1);
    send(p, s, 0x00070000, NULL);
    assert(word(p, 1) == STATE_ERROR && word(p, 2) == UINT32_MAX);
    /* a list: ConfigureChannel (channel 8, IMA-ADPCM, playing), SetAdpcmState, ApplyMasks, UpdateState; a channel not
       acquired (2) is ignored */
    block(p, 0x400, 0x420, 0x00e, 8 | 2u << 12 | 1u << 14, 0, 0);
    block(p, 0x420, 0x440, 0x00b, 8, 0x1234, 0x15);
    block(p, 0x440, 0x460, 0x00e, 2 | 1u << 14, 0, 0);
    block(p, 0x460, 0x480, 0x200, 0, 0, 0);
    block(p, 0x480, 0xffff, 0x300, 0, 0, 0);
    execute(p, s, 0x400);
    assert(shared_read(p, 0x404, 1) == 1); /* the first block is finished */
    assert(shared_read(p, 0, 4) == 0xffffff00u && shared_read(p, 4, 4) == 3);
    assert(shared_read(p, 0x10, 1) == 1 && shared_read(p, 0x14, 2) == 0x1234 && shared_read(p, 0x16, 1) == 0x15);
    assert(shared_read(p, 0x1c, 1) == 0 && shared_read(p, 0x20, 2) == 0); /* channel 9 plays nothing */
    /* a stop, and a configuration that is not ADPCM clears its state and plays */
    block(p, 0x400, 0x420, 0x000, 8, 0, 0);
    block(p, 0x420, 0x440, 0x300, 0, 0, 0);
    block(p, 0x440, 0x460, 0x00e, 8 | 1u << 12 | 1u << 14, 0, 0);
    block(p, 0x460, 0xffff, 0x300, 0, 0, 0);
    execute(p, s, 0x400);
    assert(shared_read(p, 0x10, 1) == 1 && shared_read(p, 0x14, 2) == 0);
    /* a list that never ends is cut where the memory ends, not read forever */
    block(p, 0x400, 0x400, 0x300, 0, 0, 0);
    execute(p, s, 0x400);
    /* data cache calls take the ranges of RAM and the process handle */
    const uint32_t cache[5] = {0x14000000u, 0x100, 0, 0xffff8001u}, bad_cache[5] = {0x10000000u, 0x100, 0, 0xffff8001u};
    assert(send(p, s, 0x00090082, cache) == 0x00090040 && !word(p, 1));
    send(p, s, 0x000a0082, bad_cache);
    assert(word(p, 1) == 0xe0e3fff6u);
    /* Reset stops every channel, and a released unit, or one not held, answers the invalid state */
    assert(send(p, s, 0x000c0000, NULL) == 0x000c0040 && !word(p, 1));
    block(p, 0x400, 0xffff, 0x300, 0, 0, 0);
    execute(p, s, 0x400);
    assert(shared_read(p, 0x10, 1) == 0);
    const uint32_t unit[5] = {1};
    assert(send(p, s, 0x00080040, unit) == 0x00080040 && !word(p, 1));
    send(p, s, 0x00080040, unit);
    assert(word(p, 1) == STATE_ERROR);
    assert(send(p, s, 0x00060000, NULL) == 0x00060040 && !word(p, 1) && !p->csnd.channel_mask);
    /* Shutdown ends the session's use of the memory */
    assert(send(p, s, 0x00020000, NULL) == 0x00020040 && !word(p, 1) && !p->csnd.initialized);
    send(p, s, 0x00030040, first);
    assert(word(p, 1) == STATE_ERROR);
    snx_ctr_process_free(p);
    puts("CTR csnd:SND initialization, masks, command lists and states passed");
    return 0;
}
