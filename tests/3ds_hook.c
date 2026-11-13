/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The hooks on the SDK's IPC stubs (aot/hook.c): a stub found in the text runs to its return without the SVC and the
   TLS command buffer, its reply the one svcSendSyncRequest gets; a transport error takes the stub's own error path; a
   request to srv: is declined with nothing changed, the static buffer descriptors the stub set put back; and a stub
   whose TLS pointer escapes, or that calls out, is left to the SVC, as is a function that makes the SVC on a request
   whose header it didn't store (libctru's svcSendSyncRequest, the only SVC 0x32 of a libctru homebrew). */
#include "3ds/aot/ctr_hook.h"
#include "3ds/ipc/ctr_ipc.h"
#include "3ds/runtime/internal.h"
#include "common/utilities/endian.h"
#include "support/3ds_guest.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define CODE          0x100000u
#define DATA          0x300000u /* the data page: the handles' globals and the out parameters */
#define RETURN        0x100f00u /* where the stubs return to */
#define HANDLE_GLOBAL (DATA + 0x00)
#define OUT_PARAM     (DATA + 0x10)
#define DPS           0x41660000u /* hid:USER's GetGyroscopeLowRawToDpsCoefficient answer (services/hid.c) */

static unsigned char code[0x3000];
static void put(uint32_t address, const uint32_t* words, unsigned count) {
    for (unsigned i = 0; i < count; ++i) snx_st_le32(code + (address - CODE) + 4 * i, words[i]);
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
        HANDLE_GLOBAL,      /* the handle's global */
    };
    put(at, words, sizeof words / sizeof *words);
}
/* the SDK's GX command enqueue (aot/hook.c's), its calls to getter and trigger */
static const uint32_t enqueue[] = {
    0xe92d47f0u, 0xe5902000u, 0xe3520000u, 0x059f00dcu, 0x0a000035u, 0xe5d23001u, 0xe353000fu, 0x259f00d0u,
    0x2a000031u, 0xe1923f9fu, 0xe1a02803u, 0xe59fe0c4u, 0xe203c0ffu, 0xe1a02c22u, 0xe08cc002u, 0xe3a08020u,
    0xe08e4c9eu, 0xe5919018u, 0xe1a0e1aeu, 0xe04ee20eu, 0xe08cc00eu, 0xe590e000u, 0xe088c28cu, 0xe08cc00eu,
    0xe89141f0u, 0xe591101cu, 0xe58c101cu, 0xe58c9018u, 0xe88c41f0u, 0xe3a01000u, 0xee071f9au, 0xe2821001u,
    0xe3c32cffu, 0xe1a01401u, 0xe2011cffu, 0xe1811002u, 0xe5903000u, 0xe1832f91u, 0xe3520000u, 0x0a000009u,
    0xe5901000u, 0xe1911f9fu, 0xe1a03801u, 0xe3c12cffu, 0xe1a01c23u, 0xe2811001u, 0xe1a01401u, 0xe2011cffu,
    0xe1811002u, 0xeafffff1u, 0xe1a00801u, 0xe1a00c20u, 0xe3500001u, 0x1a000003u, 0xeb000000u, 0xe320f000u,
    0xe320f000u, 0xeb000000u, 0xe3a00000u, 0xe8bd87f0u, 0xd8a02bf8u, 0xd8a02a02u, 0x88888889u};
static void put_enqueue(uint32_t at, uint32_t getter, uint32_t trigger) {
    put(at, enqueue, sizeof enqueue / sizeof *enqueue);
    uint32_t calls[2] = {0xeb000000u | (((getter - (at + 54 * 4 + 8)) >> 2) & 0xffffffu),
                         0xeb000000u | (((trigger - (at + 57 * 4 + 8)) >> 2) & 0xffffffu)};
    put(at + 54 * 4, &calls[0], 1);
    put(at + 57 * 4, &calls[1], 1);
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
    stub(CODE + 0x000, 0xe3a00815u);  /* mov r0, #0x150000 */
    stub(CODE + 0x100, 0xe3a00815u);
    const uint32_t bad[] = {0xe59f0028u}; /* ldr r0, [pc, #0x28]: the header from a literal after the handle's */
    stub(CODE + 0x200, 0);
    put(CODE + 0x200 + 3 * 4, bad, 1);
    put(CODE + 0x200 + 15 * 4, (const uint32_t[]){0x00150040u}, 1);
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
        HANDLE_GLOBAL,
    };
    put(CODE + 0x300, statics, sizeof statics / sizeof *statics);
    const uint32_t escapes[] = {
        0xe92d4010u, /* push {r4, lr} */
        0xee1d4f70u, /* mrc p15, 0, r4, c13, c0, 3 */
        0xe5840080u, /* str r0, [r4, #0x80] */
        0xe5840100u, /* str r0, [r4, #0x100]... */
        0xe5814000u, /* str r4, [r1]: the TLS pointer escapes */
        0xef000032u, /* svc 0x32 */
        0xe8bd8010u, /* pop {r4, pc} */
    };
    put(CODE + 0x400, escapes, sizeof escapes / sizeof *escapes);
    const uint32_t calls[] = {
        0xe92d4010u, /* push {r4, lr} */
        0xee1d4f70u, /* mrc p15, 0, r4, c13, c0, 3 */
        0xe5840080u, /* str r0, [r4, #0x80] */
        0xef000032u, /* svc 0x32 */
        0xebfffffeu, /* bl: a call out */
        0xe8bd8010u, /* pop {r4, pc} */
    };
    put(CODE + 0x500, calls, sizeof calls / sizeof *calls);
    /* the GSP handle's getter, a TriggerCmdReqQueue stub, the enqueue calling them, and an enqueue whose trigger is
       another command's stub, which is not the SDK's */
    put(CODE + 0x600, (const uint32_t[]){0xe59f0000u, 0xe12fff1eu, HANDLE_GLOBAL}, 3);
    stub(CODE + 0x700, 0xe3a00703u); /* mov r0, #0xc0000 */
    put_enqueue(CODE + 0x800, CODE + 0x600, CODE + 0x700);
    put_enqueue(CODE + 0x900, CODE + 0x600, CODE);
    /* libctru's svcSendSyncRequest after the BX LR of the function before it: its caller filled the TLS buffer */
    put(CODE + 0xa00, (const uint32_t[]){0xe12fff1eu, 0xef000032u, 0xe12fff1eu}, 3);
    /* a stub that stores its header on one path only */
    const uint32_t one_path[] = {
        0xe92d4010u, /* push {r4, lr} */
        0xee1d4f70u, /* mrc p15, 0, r4, c13, c0, 3 */
        0xe3510000u, /* cmp r1, #0 */
        0x0a000000u, /* beq past the store */
        0xe5840080u, /* str r0, [r4, #0x80] */
        0xe59f0008u, /* ldr r0, [pc, #8] */
        0xe5900000u, /* ldr r0, [r0] */
        0xef000032u, /* svc 0x32 */
        0xe8bd8010u, /* pop {r4, pc} */
        HANDLE_GLOBAL,
    };
    put(CODE + 0xb00, one_path, sizeof one_path / sizeof *one_path);
    /* one that stores its header and its argument with one STRD: its own request */
    const uint32_t pair[] = {
        0xe92d4010u, /* push {r4, lr} */
        0xee1d4f70u, /* mrc p15, 0, r4, c13, c0, 3 */
        0xe3a02815u, /* mov r2, #0x150000 */
        0xe3a03001u, /* mov r3, #1 */
        0xe1c428f0u, /* strd r2, r3, [r4, #0x80] */
        0xe59f0008u, /* ldr r0, [pc, #8] */
        0xe5900000u, /* ldr r0, [r0] */
        0xef000032u, /* svc 0x32 */
        0xe8bd8010u, /* pop {r4, pc} */
        HANDLE_GLOBAL,
    };
    put(CODE + 0xc00, pair, sizeof pair / sizeof *pair);
    /* one whose error path, which stores nothing, meets the request's path past the SVC: its own request still */
    const uint32_t early[] = {
        0xe92d4010u, /* push {r4, lr} */
        0xee1d4f70u, /* mrc p15, 0, r4, c13, c0, 3 */
        0xe3510000u, /* cmp r1, #0 */
        0x03e00000u, /* mvneq r0, #0 */
        0x0a000004u, /* beq to the pop */
        0xe3a00815u, /* mov r0, #0x150000 */
        0xe5840080u, /* str r0, [r4, #0x80] */
        0xe59f0008u, /* ldr r0, [pc, #8] */
        0xe5900000u, /* ldr r0, [r0] */
        0xef000032u, /* svc 0x32 */
        0xe8bd8010u, /* pop {r4, pc} */
        HANDLE_GLOBAL,
    };
    put(CODE + 0xd00, early, sizeof early / sizeof *early);
    SnxCtrImage image = ctr_image(code, sizeof code, 0x1000);
    image.segment[0].bytes = 0x1000;
    SnxCtrProcess* p = snx_ctr_process_new(&image, SNX_CTR_OLD);
    assert(p);
    char summary[160];
    SnxCtrHooks* hooks = snx_ctr_hooks_find(p, summary, sizeof summary);
    assert(hooks);
    printf("%s\n", summary);
    assert(!strcmp(summary, "7 of the 11 IPC stubs in the code run without the SVC, and the GX command queue's "
                            "enqueue runs each command at once"));
    assert(snx_ctr_hook_at(hooks, CODE + 0x800) && !snx_ctr_hook_at(hooks, CODE + 0x900));
    assert(snx_ctr_hook_at(hooks, CODE) && snx_ctr_hook_at(hooks, CODE + 0x100) &&
           snx_ctr_hook_at(hooks, CODE + 0x200) && snx_ctr_hook_at(hooks, CODE + 0x300) &&
           snx_ctr_hook_at(hooks, CODE + 0xc00) && snx_ctr_hook_at(hooks, CODE + 0xd00));
    assert(!snx_ctr_hook_at(hooks, CODE + 0x400) && !snx_ctr_hook_at(hooks, CODE + 0x500));
    assert(!snx_ctr_hook_at(hooks, CODE + 0xa04) && !snx_ctr_hook_at(hooks, CODE + 0xb00));
    assert(!snx_ctr_hook_at(hooks, CODE + 4) && !snx_ctr_hook_at(hooks, CODE + 0x2000));

    /* the reply the SVC gets, for the hook's to match */
    uint32_t hid = 0;
    assert(!snx_ctr_session_create(p, SNX_CTR_SERVICE_HID, &hid));
    ctr_call(p, hid, (const uint32_t[]){0x00150000}, 1, SNX_CTR_SVC_RETURN, 0);
    assert(ctr_command_word(p, 0) == 0x00150080 && ctr_command_word(p, 1) == 0 && ctr_command_word(p, 2) == DPS);
    ctr_request(p, NULL, 0); /* the TLS buffer cleared: the hooks never touch it */
    assert(tls_buffer_clear(p));

    /* the hooked stub: its result in r0, the reply's word out, r4 and r5 and the stack as they were, the return taken */
    ctr_put(p, HANDLE_GLOBAL, hid);
    SnxArmCPU* cpu = call(p, CODE, OUT_PARAM, RETURN);
    uint32_t sp = cpu->r[13], r4 = cpu->r[4], r5 = cpu->r[5];
    assert(snx_ctr_hook_run(p, cpu, snx_ctr_hook_at(hooks, CODE)) == SNX_CTR_HOOK_DONE);
    assert(cpu->r[15] == RETURN && !(cpu->cpsr & SNX_ARM_T) && cpu->r[0] == 0 && cpu->r[1] == 0);
    assert(cpu->r[13] == sp && cpu->r[4] == r4 && cpu->r[5] == r5);
    assert(ctr_word(p, OUT_PARAM) == DPS && tls_buffer_clear(p));
    /* a return to Thumb code */
    ctr_put(p, OUT_PARAM, 0);
    cpu = call(p, CODE + 0x100, OUT_PARAM, RETURN | 1);
    assert(snx_ctr_hook_run(p, cpu, snx_ctr_hook_at(hooks, CODE + 0x100)) == SNX_CTR_HOOK_DONE);
    assert(cpu->r[15] == RETURN && (cpu->cpsr & SNX_ARM_T) && ctr_word(p, OUT_PARAM) == DPS);

    /* the kernel's refusal: the transport's result, the stub's error path (nothing out, its flags from the ANDS) */
    ctr_put(p, OUT_PARAM, 0x5a5a5a5a);
    cpu = call(p, CODE + 0x200, OUT_PARAM, RETURN);
    assert(snx_ctr_hook_run(p, cpu, snx_ctr_hook_at(hooks, CODE + 0x200)) == SNX_CTR_HOOK_DONE);
    assert(cpu->r[15] == RETURN && cpu->r[0] == SNX_CTR_IPC_BAD_DESCRIPTOR && cpu->r[1] == 0x80000000u);
    assert((cpu->cpsr >> 31) && ctr_word(p, OUT_PARAM) == 0x5a5a5a5a && tls_buffer_clear(p));

    /* srv: is the game's code's: the call declines with its registers, flags and the TLS as they were */
    uint32_t srv = ctr_srv(p);
    assert(srv);
    ctr_request(p, NULL, 0);
    ctr_put(p, HANDLE_GLOBAL, srv);
    ctr_put(p, SNX_CTR_INITIAL_TLS + 0x180, 0x11111111);
    ctr_put(p, SNX_CTR_INITIAL_TLS + 0x184, 0x22222222);
    cpu = call(p, CODE + 0x300, 0x00345678, RETURN);
    cpu->cpsr = 0x60000010;
    SnxArmCPU before = *cpu;
    assert(snx_ctr_hook_run(p, cpu, snx_ctr_hook_at(hooks, CODE + 0x300)) == SNX_CTR_HOOK_DECLINE);
    assert(!memcmp(cpu->r, before.r, sizeof cpu->r) && cpu->cpsr == before.cpsr);
    assert(ctr_word(p, SNX_CTR_INITIAL_TLS + 0x180) == 0x11111111 && ctr_word(p, SNX_CTR_INITIAL_TLS + 0x184) == 0x22222222);
    assert(tls_buffer_clear(p));
    /* the same stub on hid:USER sends its request, the descriptors it set staying in the TLS as the game's code
       leaves them */
    ctr_put(p, HANDLE_GLOBAL, hid);
    cpu = call(p, CODE + 0x300, 0x00345678, RETURN);
    assert(snx_ctr_hook_run(p, cpu, snx_ctr_hook_at(hooks, CODE + 0x300)) == SNX_CTR_HOOK_DONE);
    assert(cpu->r[15] == RETURN && cpu->r[0] == 0);
    assert(ctr_word(p, SNX_CTR_INITIAL_TLS + 0x180) == 0x00345678 && ctr_word(p, SNX_CTR_INITIAL_TLS + 0x184) == 2);

    /* a handle to nothing: declined, for the SVC to answer */
    ctr_put(p, HANDLE_GLOBAL, 0x12345);
    cpu = call(p, CODE, OUT_PARAM, RETURN);
    assert(snx_ctr_hook_run(p, cpu, snx_ctr_hook_at(hooks, CODE)) == SNX_CTR_HOOK_DECLINE && cpu->r[15] == CODE);

    /* the enqueue: no queue (its object's first word 0), and a queue with no GPU right held: the game's code */
    ctr_put(p, DATA + 0x20, 0);
    cpu = call(p, CODE + 0x800, DATA + 0x20, RETURN);
    cpu->r[1] = DATA + 0x40;
    assert(snx_ctr_hook_run(p, cpu, snx_ctr_hook_at(hooks, CODE + 0x800)) == SNX_CTR_HOOK_DECLINE);
    assert(cpu->r[15] == CODE + 0x800 && cpu->r[0] == DATA + 0x20);
    ctr_put(p, DATA + 0x20, DATA + 0x100);
    assert(snx_ctr_hook_run(p, cpu, snx_ctr_hook_at(hooks, CODE + 0x800)) == SNX_CTR_HOOK_DECLINE);
    assert(cpu->r[15] == CODE + 0x800 && cpu->r[0] == DATA + 0x20);

    char report[200];
    snx_ctr_hooks_report(hooks, report, sizeof report);
    printf("%s\n", report);
    assert(!strcmp(report, "hooks: 4 requests sent by 4 IPC stubs without the SVC, 2 left to it; 0 GX commands run "
                           "at their enqueue, 2 queued"));
    snx_ctr_hooks_free(hooks);
    snx_ctr_process_free(p);
    puts("3ds hook: ok");
    return 0;
}
