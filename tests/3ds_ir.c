/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* ir:USER through the production SVC and IPC paths, as Azahar answers with no IR accessory in range: the block's
   startup bytes, written only when the block grants the other process write access, the three events, and the
   connection commands that never connect. */
#include "3ds/kernel/ctr_kernel.h"
#include "3ds/runtime/internal.h"
#include "3ds/ipc/ctr_ipc.h"
#include "snx_settings.h"
#include "support/3ds_guest.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define BLOCK_SIZE       0x1000u
#define META_HEADER      0x10u /* SharedMemoryHeader */
#define META_SIZE        0x20u /* the header and the empty receive BufferInfo that follows it */
#define DONT_CARE        0x10000000u
#define PERM_READ        1u
#define PERM_RW          3u
#define WRONG_PERMISSION 0xd900182eu
#define INVALID_STATE    0xe0a01bf5u

enum {
    RESULT_PASS,
    RESULT_USAGE,
    RESULT_NO_SERVICE,
    RESULT_PERMISSION,
    RESULT_DEVICE,
    RESULT_OWNERSHIP,
    RESULT_METADATA
};

static void fill(SnxCtrProcess* p, uint32_t va, unsigned char value, uint32_t size) {
    unsigned char bytes[16];
    memset(bytes, value, sizeof bytes);
    for (uint32_t done = 0; done < size; done += (uint32_t)sizeof bytes) {
        uint32_t n = size - done < sizeof bytes ? size - done : (uint32_t)sizeof bytes;
        assert(snx_ctr_write_ram(p->space, va + done, bytes, n) == SNX_CTR_MEM_OK);
    }
}
static SnxCtrSvcResult try_send(SnxCtrProcess* p, uint32_t handle) {
    p->cpu.state.cpu.r[0] = handle;
    return snx_ctr_svc(p, SVC_SEND_SYNC_REQUEST);
}
static uint32_t control(SnxCtrProcess* p, uint32_t op, uint32_t va, uint32_t source, uint32_t size,
                        uint32_t permissions) {
    uint32_t* r = p->cpu.state.cpu.r;
    r[0] = op;
    r[1] = va;
    r[2] = source;
    r[3] = size;
    r[4] = permissions;
    assert(snx_ctr_svc(p, 1) == SNX_CTR_SVC_RETURN);
    return r[0];
}
/* A block over one committed heap page, as libctru makes the IR one: other permissions from the service's view, mine
   for the owner, which CreateMemoryBlock applies to the range it locks. */
static uint32_t make_block(SnxCtrProcess* p, uint32_t other, uint32_t mine, uint32_t address) {
    uint32_t* r = p->cpu.state.cpu.r;
    assert(!control(p, 3, address, 0, BLOCK_SIZE, PERM_RW));
    r[0] = other;
    r[1] = address;
    r[2] = BLOCK_SIZE;
    r[3] = mine;
    assert(snx_ctr_svc(p, 0x1e) == SNX_CTR_SVC_RETURN && !r[0]);
    return r[1];
}
static uint32_t close_handle(SnxCtrProcess* p, uint32_t handle) {
    p->cpu.state.cpu.r[0] = handle;
    assert(snx_ctr_svc(p, 0x23) == SNX_CTR_SVC_RETURN);
    return p->cpu.state.cpu.r[0];
}
static uint32_t release(SnxCtrProcess* p, uint32_t address) { return control(p, 1, address, 0, BLOCK_SIZE, 0); }
static unsigned protection(SnxCtrProcess* p, uint32_t address) {
    unsigned permissions = 0;
    assert(snx_ctr_query(p->space, address, NULL, &permissions) == SNX_CTR_MEM_OK);
    return permissions & 3;
}
static void register_client(SnxCtrProcess* p, uint32_t srv) {
    uint32_t words[] = {0x00010002, 0x20, 0xdeadbeef};
    ctr_request(p, words, 3);
    ctr_send(p, srv, SNX_CTR_SVC_RETURN, 0);
    assert(ctr_command_word(p, 0) == 0x00010040 && !ctr_command_word(p, 1) && !ctr_command_word(p, 2));
}
/* InitializeIrNopShared: six normal words (the first is the declared shared size) and the shared block, copied or
   moved (Azahar ir_user.cpp, command 0x0018) */
static SnxCtrSvcResult try_initialize(SnxCtrProcess* p, uint32_t ir, uint32_t memory, bool moved, uint32_t declared) {
    uint32_t words[9] = {0x00180182, declared, 0x3f0, 2, 0x3f0, 2, 115200, moved ? 0x10u : 0u, memory};
    ctr_request(p, words, 9);
    return try_send(p, ir);
}
static void initialize_declared(SnxCtrProcess* p, uint32_t ir, uint32_t memory, uint32_t declared) {
    assert(try_initialize(p, ir, memory, false, declared) == SNX_CTR_SVC_RETURN);
    assert(ctr_command_word(p, 0) == 0x00180040 && !ctr_command_word(p, 1));
}
static void initialize(SnxCtrProcess* p, uint32_t ir, uint32_t memory, bool moved) {
    assert(try_initialize(p, ir, memory, moved, BLOCK_SIZE) == SNX_CTR_SVC_RETURN);
    assert(ctr_command_word(p, 0) == 0x00180040 && !ctr_command_word(p, 1));
}
static void finalize(SnxCtrProcess* p, uint32_t ir) {
    uint32_t words[] = {0x00020000};
    ctr_request(p, words, 1);
    ctr_send(p, ir, SNX_CTR_SVC_RETURN, 0);
    assert(ctr_command_word(p, 0) == 0x00020040 && !ctr_command_word(p, 1));
}
static uint32_t event(SnxCtrProcess* p, uint32_t ir, uint32_t command) {
    uint32_t words[] = {command << 16};
    ctr_request(p, words, 1);
    ctr_send(p, ir, SNX_CTR_SVC_RETURN, 0);
    assert(ctr_command_word(p, 0) == (command << 16 | 0x42) && !ctr_command_word(p, 1) && !ctr_command_word(p, 2));
    return ctr_command_word(p, 3);
}
/* A zeroed header with initialized set (SharedMemoryHeader, 16 bytes): what InitializeIrNopShared writes */
static void header_initialized(SnxCtrProcess* p, uint32_t address) {
    unsigned char header[16];
    assert(snx_ctr_read_ram(p->space, address, header, sizeof header) == SNX_CTR_MEM_OK);
    for (unsigned i = 0; i < 14; ++i) assert(!header[i]);
    assert(header[14] == 1 && !header[15]);
}
static void header_untouched(SnxCtrProcess* p, uint32_t address, unsigned char value) {
    unsigned char header[16];
    assert(snx_ctr_read_ram(p->space, address, header, sizeof header) == SNX_CTR_MEM_OK);
    for (unsigned i = 0; i < sizeof header; ++i) assert(header[i] == value);
}
static bool header_is(SnxCtrProcess* p, uint32_t address, const unsigned char* expected) {
    unsigned char header[16];
    assert(snx_ctr_read_ram(p->space, address, header, sizeof header) == SNX_CTR_MEM_OK);
    return !memcmp(header, expected, sizeof header);
}
/* a stand-in answers success without writing the header, so the header is checked, not only the reply */
static int probe(SnxCtrProcess* p, uint32_t ir) {
    uint32_t block = make_block(p, PERM_RW, PERM_RW, CTR_HEAP);
    ctr_put(p, CTR_HEAP, 0xa5a5a5a5);
    ctr_put(p, CTR_HEAP + 12, 0xa5a5a5a5);
    initialize(p, ir, block, false);
    unsigned char header[16];
    assert(snx_ctr_read_ram(p->space, CTR_HEAP, header, sizeof header) == SNX_CTR_MEM_OK);
    int result = header[14] == 1 && !header[15] && !header[3] && !header[7] ? RESULT_PASS : RESULT_NO_SERVICE;
    finalize(p, ir);
    assert(!close_handle(p, block));
    assert(!release(p, CTR_HEAP));
    return result;
}
static int startup_case(SnxCtrProcess* p, uint32_t ir) {
    /* libctru's IR setup: the owner may read the block, while the service may write it */
    uint32_t block = make_block(p, PERM_RW, PERM_READ, CTR_HEAP);
    assert(protection(p, CTR_HEAP) == PERM_READ);
    initialize(p, ir, block, false);
    header_initialized(p, CTR_HEAP);
    const uint32_t commands[] = {0x000a, 0x000b, 0x000c};
    uint32_t handles[3], objects[3];
    for (unsigned i = 0; i < 3; ++i) {
        handles[i] = event(p, ir, commands[i]);
        objects[i] = snx_ctr_handle_object(p, handles[i]);
        assert(handles[i] && objects[i] && p->objects[objects[i]].type == CTR_OBJ_EVENT);
        assert(!p->objects[objects[i]].data.event.signaled);
        assert(p->objects[objects[i]].data.event.reset_type == CTR_RESET_ONESHOT);
        for (unsigned j = 0; j < i; ++j) assert(objects[j] != objects[i]);
    }
    for (unsigned i = 0; i < 3; ++i) assert(!close_handle(p, handles[i]));
    for (unsigned i = 0; i < 3; ++i) assert(p->objects[objects[i]].type == CTR_OBJ_EVENT);
    finalize(p, ir);
    for (unsigned i = 0; i < 3; ++i) assert(p->objects[objects[i]].type == CTR_OBJ_EVENT);
    assert(!close_handle(p, block));
    assert(!release(p, CTR_HEAP));
    return RESULT_PASS;
}
static int permissions_case(SnxCtrProcess* p, uint32_t ir) {
    const struct {
        uint32_t other, mine;
    } refused[] = {{PERM_READ, PERM_RW}, {0, PERM_RW}, {DONT_CARE, PERM_RW}, {PERM_READ, DONT_CARE}};
    /* owner read-only, service writable: the valid libctru combination */
    uint32_t block = make_block(p, PERM_RW, PERM_READ, CTR_HEAP);
    assert(protection(p, CTR_HEAP) == PERM_READ);
    initialize(p, ir, block, false);
    header_initialized(p, CTR_HEAP);
    assert(release(p, CTR_HEAP) == INVALID_STATE); /* the service adopted the block */
    finalize(p, ir);
    assert(!close_handle(p, block));
    assert(!release(p, CTR_HEAP));
    /* owner dont care: the owner keeps read-write, the service may write */
    block = make_block(p, PERM_RW, DONT_CARE, CTR_HEAP);
    assert(protection(p, CTR_HEAP) == PERM_RW);
    ctr_put(p, CTR_HEAP + 0x20, 0x11223344);
    initialize(p, ir, block, false);
    header_initialized(p, CTR_HEAP);
    assert(ctr_word(p, CTR_HEAP + 0x20) == 0x11223344);
    finalize(p, ir);
    assert(!close_handle(p, block));
    assert(!release(p, CTR_HEAP));
    /* a block without write for the other process, dont care included, is refused before any header write */
    for (unsigned i = 0; i < sizeof refused / sizeof *refused; ++i) {
        block = make_block(p, refused[i].other, refused[i].mine, CTR_HEAP);
        assert(protection(p, CTR_HEAP) == PERM_RW);
        fill(p, CTR_HEAP, 0xa5, META_SIZE);
        uint32_t words[9] = {0x00180182, 0x1000, 0x3f0, 2, 0x3f0, 2, 115200, 0, block};
        ctr_request(p, words, 9);
        ctr_send(p, ir, SNX_CTR_SVC_RETURN, 0);
        if (ctr_command_word(p, 0) != 0x00180040 || ctr_command_word(p, 1) != WRONG_PERMISSION)
            return RESULT_PERMISSION;
        header_untouched(p, CTR_HEAP, 0xa5);
        header_untouched(p, CTR_HEAP + META_HEADER, 0xa5);
        finalize(p, ir);
        assert(!close_handle(p, block));
        assert(!release(p, CTR_HEAP));
    }
    return RESULT_PASS;
}
/* No accessory is in range, so a connection is only tried: the status event never fires, and the commands that need a
   device answer as Azahar does without one. */
static int accessory_case(SnxCtrProcess* p, uint32_t ir) {
    uint32_t block = make_block(p, PERM_RW, PERM_READ, CTR_HEAP);
    initialize(p, ir, block, false);
    header_initialized(p, CTR_HEAP);
    uint32_t require[] = {0x00060040, 1};
    ctr_request(p, require, 2);
    if (try_send(p, ir) != SNX_CTR_SVC_RETURN || ctr_command_word(p, 0) != 0x00060040 || ctr_command_word(p, 1))
        return RESULT_DEVICE;
    const unsigned char requested[16] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 0, 0, 0, 0, 1, 0};
    if (!header_is(p, CTR_HEAP, requested)) return RESULT_DEVICE;
    uint32_t status = event(p, ir, 0x000c), status_object = snx_ctr_handle_object(p, status);
    if (!status_object || p->objects[status_object].type != CTR_OBJ_EVENT ||
        p->objects[status_object].data.event.signaled)
        return RESULT_DEVICE;
    assert(!close_handle(p, status));
    uint32_t disconnect[] = {0x00090000};
    ctr_request(p, disconnect, 1);
    if (try_send(p, ir) != SNX_CTR_SVC_RETURN || ctr_command_word(p, 0) != 0x00090040 || ctr_command_word(p, 1))
        return RESULT_DEVICE;
    const unsigned char disconnected[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0, 0, 1, 0};
    if (!header_is(p, CTR_HEAP, disconnected)) return RESULT_DEVICE;
    uint32_t send_nop[] = {0x000d0042, 4, (4 << 14) | 2, CTR_HEAP + 0x100};
    ctr_request(p, send_nop, 4);
    if (try_send(p, ir) != SNX_CTR_SVC_RETURN || ctr_command_word(p, 0) != 0x000d0040 ||
        ctr_command_word(p, 1) != 0xc8a10c0du)
        return RESULT_DEVICE;
    uint32_t release_none[] = {0x00190040, 0};
    ctr_request(p, release_none, 2);
    if (try_send(p, ir) != SNX_CTR_SVC_RETURN || ctr_command_word(p, 0) != 0x00190040 || ctr_command_word(p, 1))
        return RESULT_DEVICE;
    uint32_t release_one[] = {0x00190040, 1};
    ctr_request(p, release_one, 2);
    if (try_send(p, ir) != SNX_CTR_SVC_RETURN || ctr_command_word(p, 0) != 0x00190040 ||
        ctr_command_word(p, 1) != 0xc8810fefu)
        return RESULT_DEVICE;
    uint32_t unmodeled[] = {0x00130000};
    ctr_request(p, unmodeled, 1);
    if (try_send(p, ir) != SNX_CTR_SVC_RETURN || ctr_command_word(p, 0) != 0x00130040 || ctr_command_word(p, 1))
        return RESULT_DEVICE;
    uint32_t past_last[] = {0x001b0000};
    ctr_request(p, past_last, 1);
    if (try_send(p, ir) != SNX_CTR_SVC_UNSUPPORTED) return RESULT_DEVICE;
    finalize(p, ir);
    assert(!close_handle(p, block));
    assert(!release(p, CTR_HEAP));
    return RESULT_PASS;
}
static int lifecycle_case(SnxCtrProcess* p, uint32_t ir) {
    /* a moved block handle: the caller's entry leaves while the service keeps the block */
    uint32_t moved = make_block(p, PERM_RW, PERM_RW, CTR_HEAP);
    uint32_t moved_object = snx_ctr_handle_object(p, moved);
    assert(moved_object && p->objects[moved_object].type == CTR_OBJ_SHARED);
    uint32_t init[] = {0x00180182, 0x1000, 0x3f0, 2, 0x3f0, 2, 115200, 0x10, moved};
    ctr_request(p, init, 9);
    ctr_send(p, ir, SNX_CTR_SVC_RETURN, 0);
    assert(ctr_command_word(p, 0) == 0x00180040 && !ctr_command_word(p, 1));
    header_initialized(p, CTR_HEAP);
    assert(!snx_ctr_handle_object(p, moved));
    ctr_put(p, CTR_HEAP + 0x10, 0x5a5a5a5a);
    /* a refused reinitialization keeps the first block adopted and the header as it was */
    uint32_t second = make_block(p, PERM_READ, PERM_RW, CTR_HEAP + 0x1000);
    uint32_t words[9] = {0x00180182, 0x1000, 0x3f0, 2, 0x3f0, 2, 115200, 0, second};
    ctr_request(p, words, 9);
    if (try_send(p, ir) != SNX_CTR_SVC_RETURN) return RESULT_OWNERSHIP;
    if (ctr_command_word(p, 0) != 0x00180040 || ctr_command_word(p, 1) != WRONG_PERMISSION) return RESULT_PERMISSION;
    if (ctr_word(p, CTR_HEAP + 0x10) != 0x5a5a5a5a) return RESULT_OWNERSHIP;
    header_initialized(p, CTR_HEAP);
    if (release(p, CTR_HEAP) != INVALID_STATE) return RESULT_OWNERSHIP;
    /* two receive event handles name one object, which the service keeps after both close */
    uint32_t handles[2], objects[2];
    for (unsigned i = 0; i < 2; ++i) {
        handles[i] = event(p, ir, 0x000a);
        objects[i] = snx_ctr_handle_object(p, handles[i]);
        if (!objects[i] || p->objects[objects[i]].type != CTR_OBJ_EVENT) return RESULT_OWNERSHIP;
    }
    if (objects[0] != objects[1]) return RESULT_OWNERSHIP;
    assert(!close_handle(p, handles[0]));
    assert(!close_handle(p, handles[1]));
    if (p->objects[objects[0]].type != CTR_OBJ_EVENT) return RESULT_OWNERSHIP;
    /* a status event taken before finalization stays after it */
    uint32_t status_handle = event(p, ir, 0x000c);
    uint32_t status = snx_ctr_handle_object(p, status_handle);
    if (!status || p->objects[status].type != CTR_OBJ_EVENT) return RESULT_OWNERSHIP;
    assert(!close_handle(p, status_handle));
    finalize(p, ir);
    if (p->objects[status].type != CTR_OBJ_EVENT) return RESULT_OWNERSHIP;
    if (p->objects[moved_object].type != CTR_OBJ_FREE) return RESULT_OWNERSHIP; /* the service released the block */
    if (release(p, CTR_HEAP)) return RESULT_OWNERSHIP;
    assert(!close_handle(p, second));
    if (release(p, CTR_HEAP + 0x1000)) return RESULT_OWNERSHIP;
    return RESULT_PASS;
}
/* The modeled layout is 16 bytes of header followed by four empty BufferInfo words. The declared size may be
   exactly 32 even though a real CreateMemoryBlock is page-sized; a failed reinitialization must keep both blocks
   and the service's original reference unchanged. */
static int metadata_case(SnxCtrProcess* p, uint32_t ir) {
    uint32_t block = make_block(p, PERM_RW, PERM_RW, CTR_HEAP);
    uint32_t object = snx_ctr_handle_object(p, block);
    assert(object && p->objects[object].refs == 1);
    fill(p, CTR_HEAP, 0xa5, META_SIZE + 4);
    initialize_declared(p, ir, block, META_SIZE);
    header_initialized(p, CTR_HEAP);
    for (unsigned i = 0; i < 4; ++i)
        if (ctr_word(p, CTR_HEAP + META_HEADER + i * 4)) return RESULT_METADATA;
    assert(ctr_word(p, CTR_HEAP + META_SIZE) == 0xa5a5a5a5u && p->objects[object].refs == 2);
    fill(p, CTR_HEAP, 0x3c, META_SIZE);
    assert(try_initialize(p, ir, block, false, META_SIZE - 1) == SNX_CTR_SVC_UNSUPPORTED);
    assert(try_initialize(p, ir, block, false, BLOCK_SIZE + 1) == SNX_CTR_SVC_UNSUPPORTED);
    header_untouched(p, CTR_HEAP, 0x3c);
    header_untouched(p, CTR_HEAP + META_HEADER, 0x3c);
    assert(ctr_word(p, CTR_HEAP + META_SIZE) == 0xa5a5a5a5u && p->objects[object].refs == 2);

    uint32_t second = make_block(p, PERM_READ, PERM_RW, CTR_HEAP + BLOCK_SIZE);
    uint32_t other = snx_ctr_handle_object(p, second);
    assert(other && p->objects[other].refs == 1);
    fill(p, CTR_HEAP + BLOCK_SIZE, 0x5a, META_SIZE + 4);
    assert(try_initialize(p, ir, second, false, META_SIZE) == SNX_CTR_SVC_RETURN);
    assert(ctr_command_word(p, 0) == 0x00180040 && ctr_command_word(p, 1) == WRONG_PERMISSION);
    header_untouched(p, CTR_HEAP + BLOCK_SIZE, 0x5a);
    header_untouched(p, CTR_HEAP + BLOCK_SIZE + META_HEADER, 0x5a);
    assert(ctr_word(p, CTR_HEAP + BLOCK_SIZE + META_SIZE) == 0x5a5a5a5au);
    assert(try_initialize(p, ir, ir, false, META_SIZE) == SNX_CTR_SVC_UNSUPPORTED);
    assert(try_initialize(p, ir, 0, false, META_SIZE) == SNX_CTR_SVC_UNSUPPORTED);
    header_untouched(p, CTR_HEAP, 0x3c);
    header_untouched(p, CTR_HEAP + META_HEADER, 0x3c);
    header_untouched(p, CTR_HEAP + BLOCK_SIZE, 0x5a);
    header_untouched(p, CTR_HEAP + BLOCK_SIZE + META_HEADER, 0x5a);
    assert(ctr_word(p, CTR_HEAP + META_SIZE) == 0xa5a5a5a5u);
    assert(p->objects[object].refs == 2 && p->objects[other].refs == 1);
    finalize(p, ir);
    assert(p->objects[object].refs == 1 && p->objects[other].refs == 1);
    assert(!close_handle(p, block) && !release(p, CTR_HEAP));
    assert(!close_handle(p, second) && !release(p, CTR_HEAP + BLOCK_SIZE));
    return RESULT_PASS;
}
int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s startup|permissions|accessory|lifecycle|metadata\n", argv[0]);
        return RESULT_USAGE;
    }
    /* the console's answers: an unknown command stops the run; these are ir:USER commands */
    g_settings.ctr_answer_unknown_commands = 0;
    unsigned char code[0x3000] = {0};
    SnxCtrImage image = ctr_image(code, sizeof code, 0x1000);
    SnxCtrProcess* p = snx_ctr_process_new(&image, SNX_CTR_OLD);
    assert(p);
    uint32_t srv = ctr_srv(p);
    assert(srv);
    register_client(p, srv);
    uint32_t ir = ctr_service_handle(p, srv, "ir:USER");
    int result = probe(p, ir);
    if (!result) {
        if (!strcmp(argv[1], "startup"))
            result = startup_case(p, ir);
        else if (!strcmp(argv[1], "permissions"))
            result = permissions_case(p, ir);
        else if (!strcmp(argv[1], "accessory"))
            result = accessory_case(p, ir);
        else if (!strcmp(argv[1], "lifecycle"))
            result = lifecycle_case(p, ir);
        else if (!strcmp(argv[1], "metadata"))
            result = metadata_case(p, ir);
        else
            result = RESULT_USAGE;
    }
    snx_ctr_process_free(p);
    if (result) fprintf(stderr, "3ds ir test: %s failed with %d\n", argv[1], result);
    return result;
}
