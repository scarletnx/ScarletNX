/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_VITA_JIT_H
#define SNX_VITA_JIT_H
#include "vita/runtime/process.h"

typedef struct VitaJit VitaJit;
/* NULL with the reason in error when this host's native back end makes no AArch32 blocks */
VitaJit* vita_jit_new(VitaProcess* process, char* error, size_t error_size);
/* the block at pc, compiled from the guest's memory now unless it was before; NULL when it can't be */
SnxArmBlock vita_jit_block(VitaJit* jit, uint32_t pc, bool thumb);
/* appends the image's blocks compiled here to folder's hints: how many */
size_t vita_jit_save_hints(VitaJit* jit, const char* folder);
void vita_jit_free(VitaJit* jit);
#endif
