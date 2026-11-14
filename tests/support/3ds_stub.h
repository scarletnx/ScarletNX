/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_TEST_3DS_STUB_H
#define SNX_TEST_3DS_STUB_H
#include <stdint.h>

/* A stub of the SDK's shape (aot/hook.c) at a byte offset in text: its header from header_mov, the handle from the
   global at handle_global, the SVC, then the reply's word 2 out and its result back */
void ctr_stub(unsigned char* text, uint32_t offset, uint32_t header_mov, uint32_t handle_global);
#endif
