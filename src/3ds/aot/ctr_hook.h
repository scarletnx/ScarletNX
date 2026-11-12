/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Functions of the game's own code the runtime runs in C instead (aot/hook.c), as the Switch's NVN functions are
   (nx/gpu/nvn/nvn_loader.c): the dispatcher looks at the hook table before it runs the block at a function's entry,
   and a hooked function is the hook's from there to its return. A 3DS game is stripped and links its SDK in, so
   nothing names a function: a hook finds its functions by their code. The SDK's IPC stubs (each stores its command's
   header in the TLS command buffer and makes svcSendSyncRequest) are found and run without the TLS buffer and the SVC:
   the request goes to its service from words the hook holds (snx_ctr_ipc_exchange). */
#ifndef SNX_CTR_HOOK_H
#define SNX_CTR_HOOK_H
#include "3ds/runtime/ctr_process.h"
#include "common/cpu/arm/arm.h"
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    SNX_CTR_HOOK_DONE,    /* the function ran: the CPU is at its return, its results in place */
    SNX_CTR_HOOK_DECLINE, /* the hook changed nothing and leaves this call to the game's code */
    SNX_CTR_HOOK_STOP     /* the run stops: cpu->exit says why (SNX_ARM_BACKEND_UNSUPPORTED, a data abort) */
} SnxCtrHookResult;
typedef struct SnxCtrHooks SnxCtrHooks;
/* the hook at the ARM function entry pc, or NULL: one bit test for an address that has none */
const struct SnxCtrHook* snx_ctr_hook_at(const SnxCtrHooks* hooks, uint32_t pc);
SnxCtrHookResult snx_ctr_hook_run(SnxCtrProcess* process, SnxArmCPU* cpu, const struct SnxCtrHook* hook);
/* Finds the functions the hooks take in the process's static text, before the game runs: NULL when none is found or
   memory runs out. summary (when given) says what was found, in a line without its newline */
SnxCtrHooks* snx_ctr_hooks_find(SnxCtrProcess* process, char* summary, size_t summary_size);
/* what the hooks ran, for the run's report: one line without its newline */
void snx_ctr_hooks_report(const SnxCtrHooks* hooks, char* line, size_t size);
void snx_ctr_hooks_free(SnxCtrHooks* hooks);
#endif
