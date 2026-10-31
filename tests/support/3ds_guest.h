/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_TEST_GUEST_H
#define SNX_TEST_GUEST_H
#include "3ds/runtime/internal.h"
#include <stddef.h>
#include <stdint.h>

/* the command buffer of the main thread, in its TLS */
#define CTR_COMMAND           (SNX_CTR_INITIAL_TLS + 0x80)
#define SVC_SEND_SYNC_REQUEST 0x32u

/* a little-endian word of guest memory */
uint32_t ctr_word(SnxCtrProcess* p, uint32_t va);
void ctr_put(SnxCtrProcess* p, uint32_t va, uint32_t value);
uint32_t ctr_command_word(SnxCtrProcess* p, unsigned index);
/* the words of a request in the command buffer, with the rest of its 64 words zero */
void ctr_request(SnxCtrProcess* p, const uint32_t* words, unsigned count);
/* SendSyncRequest on the handle, which ends in state. r0 is then result when the request returned, and the handle when
   it did not. */
void ctr_send(SnxCtrProcess* p, uint32_t handle, SnxCtrSvcResult state, uint32_t result);
void ctr_call(SnxCtrProcess* p, uint32_t handle, const uint32_t* words, unsigned count, SnxCtrSvcResult state,
              uint32_t result);
/* A command of only its header, which succeeds on the session. It returns the header of the reply. */
uint32_t ctr_send_header(SnxCtrProcess* p, uint32_t session, uint32_t header);
/* a field of 2 or 4 bytes of hid:USER's shared memory, little endian, as the game reads it */
uint32_t ctr_hid_shared(SnxCtrProcess* p, uint32_t offset, unsigned size);
unsigned ctr_object_count(SnxCtrProcess* p);
/* a process image of one code page at 0x100000, one read-only page at 0x200000 and one data page at 0x300000 */
SnxCtrImage ctr_image(unsigned char* code, size_t code_size, uint32_t bss_size);
#endif
