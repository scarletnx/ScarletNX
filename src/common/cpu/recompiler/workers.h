/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What the recompiles that compile their units on several threads share (library.c, nx/loader/module_llvm.c and
   module_arm.c): the lock of their queue and the threads that run its worker (worker_threads.h), and a unit's IR kept
   as a file */
#ifndef SNX_RECOMPILER_WORKERS_H
#define SNX_RECOMPILER_WORKERS_H
#include "common/cpu/recompiler/llvm/ir.h"
#include "common/cpu/recompiler/worker_threads.h"
#include "common/utilities/common.h"
#include <llvm-c/Core.h>
#include <stdlib.h>
#include <string.h>

/* the unit's IR as <dir>/<name>.ll: 0, or nonzero when it cannot be written */
static inline int snx_save_ir(SnxIr* ir, const char* dir, const char* name) {
    char *text = snx_ir_text(ir), *file = snx_format("%s.ll", name), *path = snx_path_join(dir, file);
    int rc = snx_write_file(path, text, strlen(text));
    LLVMDisposeMessage(text);
    free(file);
    free(path);
    return rc;
}

#endif
