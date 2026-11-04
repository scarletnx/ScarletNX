/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "3ds_guest.h"
#include "3ds/ipc/ctr_ipc.h"
#include "common/utilities/endian.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

uint32_t ctr_word(SnxCtrProcess* p, uint32_t va) {
    unsigned char bytes[4];
    assert(snx_ctr_read_ram(p->space, va, bytes, sizeof bytes) == SNX_CTR_MEM_OK);
    return snx_ld_le32(bytes);
}
void ctr_put(SnxCtrProcess* p, uint32_t va, uint32_t value) {
    unsigned char bytes[4];
    snx_st_le32(bytes, value);
    assert(snx_ctr_write_ram(p->space, va, bytes, sizeof bytes) == SNX_CTR_MEM_OK);
}
uint32_t ctr_command_word(SnxCtrProcess* p, unsigned index) { return ctr_word(p, CTR_COMMAND + 4 * index); }
void ctr_request(SnxCtrProcess* p, const uint32_t* words, unsigned count) {
    unsigned char zero[SNX_CTR_IPC_WORDS * 4] = {0};
    assert(count <= SNX_CTR_IPC_WORDS);
    assert(snx_ctr_write_ram(p->space, CTR_COMMAND, zero, sizeof zero) == SNX_CTR_MEM_OK);
    for (unsigned i = 0; i < count; ++i) ctr_put(p, CTR_COMMAND + 4 * i, words[i]);
}
void ctr_send(SnxCtrProcess* p, uint32_t handle, SnxCtrSvcResult state, uint32_t result) {
    p->cpu.state.cpu.r[0] = handle;
    SnxCtrSvcResult got = snx_ctr_svc(p, SVC_SEND_SYNC_REQUEST);
    if (got != state) fprintf(stderr, "request on 0x%08x: state %d, expected %d\n", handle, (int)got, (int)state);
    assert(got == state);
    assert(p->cpu.state.cpu.r[0] == (state == SNX_CTR_SVC_RETURN ? result : handle));
}
void ctr_call(SnxCtrProcess* p, uint32_t handle, const uint32_t* words, unsigned count, SnxCtrSvcResult state,
              uint32_t result) {
    ctr_request(p, words, count);
    ctr_send(p, handle, state, result);
}
uint32_t ctr_send_header(SnxCtrProcess* p, uint32_t session, uint32_t header) {
    ctr_put(p, CTR_COMMAND, header);
    ctr_send(p, session, SNX_CTR_SVC_RETURN, 0);
    assert(!ctr_command_word(p, 1));
    return ctr_command_word(p, 0);
}
uint32_t ctr_srv(SnxCtrProcess* p) {
    assert(snx_ctr_write_ram(p->space, 0x300100, "srv:", 5) == SNX_CTR_MEM_OK);
    p->cpu.state.cpu.r[1] = 0x300100;
    assert(snx_ctr_svc(p, 0x2d /* ConnectToPort */) == SNX_CTR_SVC_RETURN && !p->cpu.state.cpu.r[0]);
    return p->cpu.state.cpu.r[1];
}
uint32_t ctr_service_handle(SnxCtrProcess* p, uint32_t srv, const char* name) {
    uint32_t length = (uint32_t)strlen(name), words[5] = {0x00050100, 0, 0, length, 1};
    assert(length <= 8);
    for (uint32_t i = 0; i < length; ++i) words[1 + i / 4] |= (uint32_t)(unsigned char)name[i] << (i % 4 * 8);
    ctr_request(p, words, 5);
    ctr_send(p, srv, SNX_CTR_SVC_RETURN, 0);
    assert(ctr_command_word(p, 0) == 0x00050042 && !ctr_command_word(p, 1) && ctr_command_word(p, 2) == 0x10);
    return ctr_command_word(p, 3);
}
uint32_t ctr_hid_shared(SnxCtrProcess* p, uint32_t offset, unsigned size) {
    unsigned char bytes[4] = {0};
    assert(size <= sizeof bytes);
    uint32_t pa = p->objects[p->hid_objects[0]].data.shared.physical;
    assert(snx_ctr_phys_read(p->memory, pa + offset, bytes, size) == SNX_CTR_MEM_OK);
    return snx_ld_le32(bytes);
}
unsigned ctr_object_count(SnxCtrProcess* p) {
    unsigned count = 0;
    for (unsigned i = 1; i < CTR_OBJECTS; ++i) count += !!p->objects[i].type;
    return count;
}
SnxCtrImage ctr_image(unsigned char* code, size_t code_size, uint32_t bss_size) {
    return (SnxCtrImage){.segment = {{0x100000, 1, 16}, {0x200000, 1, 4}, {0x300000, 1, 4}},
                         .bss_size = bss_size,
                         .stack_size = 0x4000,
                         .main_priority = 0x30,
                         .code = code,
                         .code_size = code_size};
}
