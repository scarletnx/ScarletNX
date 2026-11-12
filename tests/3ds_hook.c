/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The hooks on the SDK's IPC stubs (aot/hook.c): a stub found in the text runs to its return without the SVC and the
   TLS command buffer, its reply the one svcSendSyncRequest gets; a transport error takes the stub's own error path; a
   request to srv: is declined with nothing changed, the static buffer descriptors the stub set put back; and a stub
   whose TLS pointer escapes, or that calls out, is left to the SVC. */
#include "3ds/aot/ctr_hook.h"
#include "3ds/ipc/ctr_ipc.h"
#include "3ds/runtime/internal.h"
#include "common/utilities/endian.h"
#include "support/3ds_guest.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define TEXT    0x100000u
#define DATA    0x300000u /* the data page: the handles' globals and the out parameters */
#define RETURN  0x100f00u /* where the stubs return to */
#define HANDLE  (DATA + 0x00)
#define OUT     (DATA + 0x10)
#define DPS     0x41660000u /* hid:USER's GetGyroscopeLowRawToDpsCoefficient answer (services/hid.c) */

static unsigned char code[0x3000];
static void put(uint32_t address, const uint32_t* words, unsigned count) {
    for (unsigned i = 0; i < count; ++i) snx_st_le32(code + (address - TEXT) + 4 * i, words[i]);
}
/* A stub of the SDK's shape for a command of only its header with one word out (header in r0's MOV, as imm8 rotated):
   push {r4, r5, lr}; r5 = the out parameter; r4 = TLS; the header at TLS + 0x80, r4 moved there; the handle from the
   global the literal names; the SVC; a transport error returns it, else the reply's word 2 goes out and its result
   back */
static void stub(uint32_t at, uint32_t mov_header) {
    const uint32_t words[] = {
        0xe92d4030u, /* push {r4, r5, lr} */
        0xe1a05000u, /* mov r5, r0 */
        0xee1d4f70u, /* mrc p15, 0, r4, c13, c0, 3 */
        mov_header,  /* mov r0, #header */
        0xe5a40080u, /* str r0, [r4, #0x80]! */
        0xe59f001cu, /* ldr r0, [pc, #0x1c]: the literal below */
        0xe5900000u, /* ldr r0, [r0] */
        0xef000032u, /* svc 0x32 */
        0xe2101102u, /* ands r1, r0, #0x80000000 */
        0x4a000002u, /* bmi to the pop */
        0xe5940008u, /* ldr r0, [r4, #8] */
        0xe5850000u, /* str r0, [r5] */
        0xe5940004u, /* ldr r0, [r4, #4] */
        0xe8bd8030u, /* pop {r4, r5, pc} */
        HANDLE,      /* the handle's global */
    };
    put(at, words, sizeof words / sizeof *words);
}
static SnxArmCPU* call(SnxCtrProcess* p, uint32_t entry, uint32_t r0, uint32_t lr) {
    SnxArmCPU* cpu = snx_ctr_process_cpu(p);
    for (unsigned i = 0; i < 13; ++i) cpu->r[i] = 0x1110 * i;
    cpu->r[0] = r0;
    cpu->r[13] = SNX_CTR_STACK_TOP - 0x100;
    cpu->r[14] = lr;
    cpu->r[15] = entry;
    cpu->cpsr = 0x10;
    return cpu;
}
static bool tls_buffer_clear(SnxCtrProcess* p) {
    for (unsigned i = 0; i < 64; ++i)
        if (ctr_word(p, CTR_COMMAND + 4 * i)) return false;
    return true;
}
int main(void) {
    /* the stubs: GetGyroscopeLowRawToDpsCoefficient (0x00150000), the same command with a normal word it doesn't
       take (0x00150040: the kernel's bad descriptor), a stub that sets the static buffer descriptors at TLS + 0x180
       before its SVC, one that passes its TLS pointer to another function, and one that calls out */
    stub(TEXT + 0x000, 0xe3a00815u);  /* mov r0, #0x150000 */
    stub(TEXT + 0x100, 0xe3a00815u);
    const uint32_t bad[] = {0xe59f0028u}; /* ldr r0, [pc, #0x28]: the header from a literal after the handle's */
    stub(TEXT + 0x200, 0);
    put(TEXT + 0x200 + 3 * 4, bad, 1);
    put(TEXT + 0x200 + 15 * 4, (const uint32_t[]){0x00150040u}, 1);
    const uint32_t statics[] = {
        0xe92d4030u, /* push {r4, r5, lr} */
        0xee1d4f70u, /* mrc p15, 0, r4, c13, c0, 3 */
        0xe2845d06u, /* add r5, r4, #0x180 */
        0xe3a01002u, /* mov r1, #2: a static buffer descriptor */
        0xe8850003u, /* stm r5, {r0, r1} */
        0xe3a00815u, /* mov r0, #0x150000 */
        0xe5a40080u, /* str r0, [r4, #0x80]! */
        0xe59f0008u, /* ldr r0, [pc, #8] */
        0xe5900000u, /* ldr r0, [r0] */
        0xef000032u, /* svc 0x32 */
        0xe8bd8030u, /* pop {r4, r5, pc} */
        HANDLE,
    };
    put(TEXT + 0x300, statics, sizeof statics / sizeof *statics);
    const uint32_t escapes[] = {
        0xe92d4010u, /* push {r4, lr} */
        0xee1d4f70u, /* mrc p15, 0, r4, c13, c0, 3 */
        0xe5840080u, /* str r0, [r4, #0x80] */
        0xe5840100u, /* str r0, [r4, #0x100]... */
        0xe5814000u, /* str r4, [r1]: the TLS pointer escapes */
        0xef000032u, /* svc 0x32 */
        0xe8bd8010u, /* pop {r4, pc} */
    };
    put(TEXT + 0x400, escapes, sizeof escapes / sizeof *escapes);
    const uint32_t calls[] = {
        0xe92d4010u, /* push {r4, lr} */
        0xee1d4f70u, /* mrc p15, 0, r4, c13, c0, 3 */
        0xe5840080u, /* str r0, [r4, #0x80] */
        0xef000032u, /* svc 0x32 */
        0xebfffffeu, /* bl: a call out */
        0xe8bd8010u, /* pop {r4, pc} */
    };
    put(TEXT + 0x500, calls, sizeof calls / sizeof *calls);
    SnxCtrImage image = ctr_image(code, sizeof code, 0x1000);
    image.segment[0].bytes = 0x1000;
    SnxCtrProcess* p = snx_ctr_process_new(&image, SNX_CTR_OLD);
    assert(p);
    char summary[160];
    SnxCtrHooks* hooks = snx_ctr_hooks_find(p, summary, sizeof summary);
    assert(hooks);
    printf("%s\n", summary);
    assert(!strcmp(summary, "4 of the 6 IPC stubs in the code run without the SVC"));
    assert(snx_ctr_hook_at(hooks, TEXT) && snx_ctr_hook_at(hooks, TEXT + 0x100) && snx_ctr_hook_at(hooks, TEXT + 0x200) &&
           snx_ctr_hook_at(hooks, TEXT + 0x300));
    assert(!snx_ctr_hook_at(hooks, TEXT + 0x400) && !snx_ctr_hook_at(hooks, TEXT + 0x500));
    assert(!snx_ctr_hook_at(hooks, TEXT + 4) && !snx_ctr_hook_at(hooks, TEXT + 0x2000));

    /* the reply the SVC gets, for the hook's to match */
    uint32_t hid = 0;
    assert(!snx_ctr_session_create(p, SNX_CTR_SERVICE_HID, &hid));
    ctr_call(p, hid, (const uint32_t[]){0x00150000}, 1, SNX_CTR_SVC_RETURN, 0);
    assert(ctr_command_word(p, 0) == 0x00150080 && ctr_command_word(p, 1) == 0 && ctr_command_word(p, 2) == DPS);
    ctr_request(p, NULL, 0); /* the TLS buffer cleared: the hooks never touch it */
    assert(tls_buffer_clear(p));

    /* the hooked stub: its result in r0, the reply's word out, r4 and r5 and the stack as they were, the return taken */
    ctr_put(p, HANDLE, hid);
    SnxArmCPU* cpu = call(p, TEXT, OUT, RETURN);
    uint32_t sp = cpu->r[13], r4 = cpu->r[4], r5 = cpu->r[5];
    assert(snx_ctr_hook_run(p, cpu, snx_ctr_hook_at(hooks, TEXT)) == SNX_CTR_HOOK_DONE);
    assert(cpu->r[15] == RETURN && !(cpu->cpsr & SNX_ARM_T) && cpu->r[0] == 0 && cpu->r[1] == 0);
    assert(cpu->r[13] == sp && cpu->r[4] == r4 && cpu->r[5] == r5);
    assert(ctr_word(p, OUT) == DPS && tls_buffer_clear(p));
    /* a return to Thumb code */
    ctr_put(p, OUT, 0);
    cpu = call(p, TEXT + 0x100, OUT, RETURN | 1);
    assert(snx_ctr_hook_run(p, cpu, snx_ctr_hook_at(hooks, TEXT + 0x100)) == SNX_CTR_HOOK_DONE);
    assert(cpu->r[15] == RETURN && (cpu->cpsr & SNX_ARM_T) && ctr_word(p, OUT) == DPS);

    /* the kernel's refusal: the transport's result, the stub's error path (nothing out, its flags from the ANDS) */
    ctr_put(p, OUT, 0x5a5a5a5a);
    cpu = call(p, TEXT + 0x200, OUT, RETURN);
    assert(snx_ctr_hook_run(p, cpu, snx_ctr_hook_at(hooks, TEXT + 0x200)) == SNX_CTR_HOOK_DONE);
    assert(cpu->r[15] == RETURN && cpu->r[0] == SNX_CTR_IPC_BAD_DESCRIPTOR && cpu->r[1] == 0x80000000u);
    assert((cpu->cpsr >> 31) && ctr_word(p, OUT) == 0x5a5a5a5a && tls_buffer_clear(p));

    /* srv: is the game's code's: the call declines with its registers, flags and the TLS as they were */
    uint32_t srv = ctr_srv(p);
    assert(srv);
    ctr_request(p, NULL, 0);
    ctr_put(p, HANDLE, srv);
    ctr_put(p, SNX_CTR_INITIAL_TLS + 0x180, 0x11111111);
    ctr_put(p, SNX_CTR_INITIAL_TLS + 0x184, 0x22222222);
    cpu = call(p, TEXT + 0x300, 0x00345678, RETURN);
    cpu->cpsr = 0x60000010;
    SnxArmCPU before = *cpu;
    assert(snx_ctr_hook_run(p, cpu, snx_ctr_hook_at(hooks, TEXT + 0x300)) == SNX_CTR_HOOK_DECLINE);
    assert(!memcmp(cpu->r, before.r, sizeof cpu->r) && cpu->cpsr == before.cpsr);
    assert(ctr_word(p, SNX_CTR_INITIAL_TLS + 0x180) == 0x11111111 && ctr_word(p, SNX_CTR_INITIAL_TLS + 0x184) == 0x22222222);
    assert(tls_buffer_clear(p));
    /* the same stub on hid:USER sends its request, the descriptors it set staying in the TLS as the game's code
       leaves them */
    ctr_put(p, HANDLE, hid);
    cpu = call(p, TEXT + 0x300, 0x00345678, RETURN);
    assert(snx_ctr_hook_run(p, cpu, snx_ctr_hook_at(hooks, TEXT + 0x300)) == SNX_CTR_HOOK_DONE);
    assert(cpu->r[15] == RETURN && cpu->r[0] == 0);
    assert(ctr_word(p, SNX_CTR_INITIAL_TLS + 0x180) == 0x00345678 && ctr_word(p, SNX_CTR_INITIAL_TLS + 0x184) == 2);

    /* a handle to nothing: declined, for the SVC to answer */
    ctr_put(p, HANDLE, 0x12345);
    cpu = call(p, TEXT, OUT, RETURN);
    assert(snx_ctr_hook_run(p, cpu, snx_ctr_hook_at(hooks, TEXT)) == SNX_CTR_HOOK_DECLINE && cpu->r[15] == TEXT);

    char report[160];
    snx_ctr_hooks_report(hooks, report, sizeof report);
    printf("%s\n", report);
    assert(!strcmp(report, "hooks: 4 requests sent by 4 IPC stubs without the SVC, 2 left to it"));
    snx_ctr_hooks_free(hooks);
    snx_ctr_process_free(p);
    puts("3ds hook: ok");
    return 0;
}
