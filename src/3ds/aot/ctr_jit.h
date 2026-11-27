/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_CTR_JIT_H
#define SNX_CTR_JIT_H
#include "3ds/runtime/ctr_process.h"
#include <stddef.h>
#include <stdint.h>
/* The process's blocks the static module lacks, compiled when the dispatch first reaches them (process->compile) until
   snx_ctr_jit_free. NULL with the reason in error when this host's native back end makes no AArch32 blocks */
typedef struct SnxCtrJit SnxCtrJit;
SnxCtrJit* snx_ctr_jit_new(SnxCtrProcess* process, char* error, size_t error_size);
/* The blocks it compiled added to the title's hints (directory/SNX_CTR_HINTS), the ones the file has left out: how
   many it added */
size_t snx_ctr_jit_save_hints(SnxCtrJit* jit, const char* directory);
/* how many blocks it compiled */
uint64_t snx_ctr_jit_compiled(const SnxCtrJit* jit);
void snx_ctr_jit_free(SnxCtrJit* jit);
#endif
