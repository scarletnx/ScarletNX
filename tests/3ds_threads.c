/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "3ds/runtime/internal.h"
#include "3ds/memory/host.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "3ds/aot/ctr_module.h"
#include "common/utilities/common.h"
#include "support/3ds_guest.h"
static uint32_t event(SnxCtrProcess* p, uint32_t reset) {
    uint32_t* r = p->cpu.state.cpu.r;
    r[1] = reset;
    assert(snx_ctr_svc(p, 0x17) == SNX_CTR_SVC_RETURN && !r[0]);
    return r[1];
}
static void spin_ms(unsigned ms) {
    double start = snx_seconds();
    while (snx_seconds() - start < ms / 1000.0) {}
}
static uint32_t svc(SnxCtrProcess* p, uint32_t number, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t r4,
                    SnxCtrSvcResult expected) {
    uint32_t* r = p->cpu.state.cpu.r;
    r[0] = r0;
    r[1] = r1;
    r[2] = r2;
    r[3] = r3;
    r[4] = r4;
    assert(snx_ctr_svc(p, number) == expected);
    return r[1];
}
static uint32_t thread(SnxCtrProcess* p, uint32_t priority, uint32_t entry) {
    uint32_t handle = svc(p, 8, priority, entry, 0, SNX_CTR_STACK_TOP - 0x1000, 0xfffffffe, SNX_CTR_SVC_RETURN);
    assert(!p->cpu.state.cpu.r[0]);
    return handle;
}
/* Blocks for the dispatcher: one that loops on itself making no SVC, one that ends the process */
static unsigned finished;
static void spin_block(SnxArmCPU* cpu) { cpu->exit = SNX_ARM_OK; }
static void exit_block(SnxArmCPU* cpu) {
    ++finished;
    cpu->exit = SNX_ARM_SVC;
    cpu->syndrome = 0x03;
}
/* Timers, pulse events, SleepThread, ExitThread (Azahar kernel/{timer,thread,svc}.cpp) */
static void timing(SnxCtrProcess* p) {
    uint32_t* r = p->cpu.state.cpu.r;
    /* a one-shot timer, 1 ms: the only thread waits on it, the scheduler sleeps the host until it fires */
    uint32_t timer = svc(p, 0x1a, 0, CTR_RESET_ONESHOT, 0, 0, 0, SNX_CTR_SVC_RETURN),
             id = snx_ctr_handle_object(p, timer);
    assert(!r[0] && p->objects[id].type == CTR_OBJ_TIMER);
    double start = snx_seconds(); /* before the timer starts, so a host stall cannot shorten what is measured */
    svc(p, 0x1b, timer, 0, 1000000, 0, 0, SNX_CTR_SVC_RETURN); /* initial r2:r3, interval r1:r4 */
    assert(!r[0] && p->objects[id].data.timer.deadline && !p->objects[id].data.timer.signaled);
    svc(p, 0x24, timer, 0, UINT32_MAX, UINT32_MAX, 0, SNX_CTR_SVC_BLOCKED);
    assert(snx_ctr_schedule(p) && p->current_thread == 2 && !r[0] && snx_seconds() - start >= 0.0009);
    assert(!p->objects[id].data.timer.signaled && !p->objects[id].data.timer.deadline); /* acquired, not periodic */
    /* negative delays are refused before the handle is looked up */
    svc(p, 0x1b, 0, 0, 0, 0x80000000u, 0, SNX_CTR_SVC_RETURN);
    assert(r[0] == 0xd8e007fdu);
    /* a sticky periodic timer with no initial delay is signaled at once and stays so for every waiter */
    uint32_t sticky = svc(p, 0x1a, 0, CTR_RESET_STICKY, 0, 0, 0, SNX_CTR_SVC_RETURN),
             sid = snx_ctr_handle_object(p, sticky);
    svc(p, 0x1b, sticky, 1000000, 0, 0, 0, SNX_CTR_SVC_RETURN);
    assert(!r[0] && p->objects[sid].data.timer.signaled && p->objects[sid].data.timer.deadline);
    for (unsigned i = 0; i < 2; ++i) {
        svc(p, 0x24, sticky, 0, UINT32_MAX, UINT32_MAX, 0, SNX_CTR_SVC_RETURN);
        assert(!r[0] && p->objects[sid].data.timer.signaled);
    }
    svc(p, 0x1d, sticky, 0, 0, 0, 0, SNX_CTR_SVC_RETURN); /* ClearTimer */
    assert(!r[0] && !p->objects[sid].data.timer.signaled && p->objects[sid].data.timer.deadline);
    svc(p, 0x1c, sticky, 0, 0, 0, 0, SNX_CTR_SVC_RETURN); /* CancelTimer */
    assert(!r[0] && !p->objects[sid].data.timer.deadline);
    /* a pulse event wakes its waiters and is clear again at once */
    uint32_t pulse = svc(p, 0x17, 0, CTR_RESET_PULSE, 0, 0, 0, SNX_CTR_SVC_RETURN),
             pid = snx_ctr_handle_object(p, pulse);
    svc(p, 0x18, pulse, 0, 0, 0, 0, SNX_CTR_SVC_RETURN);
    assert(!r[0] && !p->objects[pid].data.event.signaled);
    /* SleepThread(0) gives the core to a thread of worse priority, which keeps it only until the next scheduling */
    uint32_t other = thread(p, 49, 0x100004), oid = snx_ctr_handle_object(p, other);
    assert(snx_ctr_schedule(p) && p->current_thread == 2);
    svc(p, 0x0a, 0, 0, 0, 0, 0, SNX_CTR_SVC_RETURN);
    assert(snx_ctr_schedule(p) && p->current_thread == oid);
    assert(snx_ctr_schedule(p) && p->current_thread == 2);
    /* ExitThread: the thread waited on is dead and the mutex it held is free again */
    uint32_t mutex = svc(p, 0x13, 0, 0, 0, 0, 0, SNX_CTR_SVC_RETURN), mid = snx_ctr_handle_object(p, mutex);
    svc(p, 0x24, other, 0, UINT32_MAX, UINT32_MAX, 0, SNX_CTR_SVC_BLOCKED);
    assert(snx_ctr_schedule(p) && p->current_thread == oid);
    svc(p, 0x24, mutex, 0, UINT32_MAX, UINT32_MAX, 0, SNX_CTR_SVC_RETURN);
    assert(!r[0] && p->objects[mid].data.mutex.owner == oid);
    svc(p, 0x09, 0, 0, 0, 0, 0, SNX_CTR_SVC_BLOCKED);
    assert(p->objects[oid].data.thread.dead && !p->objects[mid].data.mutex.depth);
    assert(snx_ctr_schedule(p) && p->current_thread == 2 && !r[0] && !snx_ctr_kernel_blocked(p));
    svc(p, 0x24, mutex, 0, UINT32_MAX, UINT32_MAX, 0, SNX_CTR_SVC_RETURN);
    assert(!r[0] && p->objects[mid].data.mutex.owner == 2);
    assert(!snx_ctr_handle_close(p, other) && !p->objects[oid].type); /* the last reference frees it */
    /* SleepThread(-1) never wakes: with no other thread nothing is left to run */
    svc(p, 0x0a, UINT32_MAX, UINT32_MAX, 0, 0, 0, SNX_CTR_SVC_BLOCKED);
    assert(snx_ctr_thread(p)->wake_ns == UINT64_MAX && !snx_ctr_schedule(p));
    snx_ctr_thread(p)->wake_ns = 0;
    /* the process's last thread exiting ends it */
    svc(p, 0x09, 0, 0, 0, 0, 0, SNX_CTR_SVC_EXIT);
}
/* The dispatcher reaches the scheduler every SNX_CTR_POLL_BLOCKS blocks: a better-priority thread whose sleep ends
   while the main thread loops without an SVC takes the core, and ends the process */
static void preemption(SnxCtrProcess* p) {
    static SnxArmBlock arm[1024], thumb[2048];
    arm[0] = spin_block; /* 0x100000 */
    arm[2] = exit_block; /* 0x100008 */
    SnxCtrModule module = {.text_address = 0x100000, .text_bytes = 4096, .arm_blocks = arm, .thumb_blocks = thumb};
    uint32_t better = thread(p, 0x20, 0x100008), bid = snx_ctr_handle_object(p, better);
    p->objects[bid].data.thread.wake_ns = snx_ctr_now_ns() + 2000000; /* asleep for 2 ms */
    p->cpu.state.cpu.r[15] = 0x100000;
    finished = 0;
    uint64_t executed = 0;
    assert(snx_ctr_dispatch(&module, p, UINT64_C(4000000000), &executed) == SNX_CTR_RUN_EXIT);
    assert(finished == 1 && p->current_thread == bid && executed < UINT64_C(4000000000));
}
/* The cores take turns: a thread of better priority on the system core that loops and never blocks still leaves the
   application core's main thread its turns, and the main thread ends the process */
static void cores(SnxCtrProcess* p) {
    static SnxArmBlock arm[1024], thumb[2048];
    arm[0] = spin_block; /* 0x100000 */
    arm[2] = exit_block; /* 0x100008 */
    SnxCtrModule module = {.text_address = 0x100000, .text_bytes = 4096, .arm_blocks = arm, .thumb_blocks = thumb};
    uint32_t busy = svc(p, 8, 0x20, 0x100000, 0, SNX_CTR_STACK_TOP - 0x1000, 1, SNX_CTR_SVC_RETURN);
    uint32_t bid = snx_ctr_handle_object(p, busy);
    assert(!p->cpu.state.cpu.r[0] && p->objects[bid].data.thread.core == 1);
    p->cpu.state.cpu.r[15] = 0x100008;
    finished = 0;
    uint64_t executed = 0;
    assert(snx_ctr_dispatch(&module, p, UINT64_C(100000000), &executed) == SNX_CTR_RUN_EXIT);
    assert(finished == 1 && p->current_thread == 2);
}
int main(void) {
    unsigned char code[0x3000] = {0};
    SnxCtrImage image = ctr_image(code, sizeof code, 4096);
    for (unsigned model = 0; model < 2; ++model) {
        SnxCtrProcess* p = snx_ctr_process_new(&image, (SnxCtrModel)model);
        assert(p);
        SnxArmCPU* cpu = &p->cpu.state.cpu;
        uint32_t* r = cpu->r;
        r[0] = 49;
        r[1] = 0x100004;
        r[2] = 37;
        r[3] = SNX_CTR_STACK_TOP - 0x1000;
        r[4] = 0xfffffffe;
        assert(snx_ctr_svc(p, 8) == SNX_CTR_SVC_RETURN && !r[0]);
        uint32_t thread = r[1], tid = snx_ctr_handle_object(p, thread);
        assert(tid && tid != 2 && p->objects[tid].refs == 2 && snx_ctr_schedule(p) && p->current_thread == 2);
        uint32_t h = event(p, 0), eid = snx_ctr_handle_object(p, h), duplicate;
        assert(!snx_ctr_handle_add(p, eid, &duplicate));
        cpu->tpidrurw = 0x12345678;
        cpu->d[7] = 0x12345678abcdef01ull;
        cpu->fpscr = 0x80000000;
        r[5] = 0x55555555;
        r[0] = h;
        r[2] = r[3] = UINT32_MAX;
        cpu->exit = SNX_ARM_SVC;
        cpu->syndrome = 0x24;
        assert(snx_ctr_svc(p, 0x24) == SNX_CTR_SVC_BLOCKED && p->objects[eid].refs == 3);
        uint64_t calls = p->svc_count;
        assert(snx_ctr_svc(p, 3) == SNX_CTR_SVC_BLOCKED && p->svc_count == calls);
        assert(snx_ctr_schedule(p) && p->current_thread == tid);
        assert(r[0] == 37 && r[15] == 0x100004 && cpu->tpidruro == SNX_CTR_INITIAL_TLS + 512);
        /* the flat space stays the core's across the switch, where the host has one */
        assert(cpu->mem == snx_ctr_space_base(p->space) && p->cpu.state.context == &p->cpu);
        assert(cpu->mem_size == (cpu->mem ? CTR_HOST_SPACE : 0));
        assert(!snx_ctr_handle_close(p, h) && p->objects[eid].refs == 2); /* wait owns its own reference */
        r[0] = duplicate;
        assert(snx_ctr_svc(p, 0x18) == SNX_CTR_SVC_RETURN && !r[0]);
        assert(!p->objects[eid].data.event.signaled && p->objects[eid].refs == 1);
        uint32_t mutex;
        assert(!snx_ctr_object_create(p, CTR_OBJ_MUTEX, &mutex));
        uint32_t mid = snx_ctr_handle_object(p, mutex);
        for (unsigned i = 0; i < 2; ++i) {
            r[0] = mutex;
            r[2] = r[3] = UINT32_MAX;
            assert(snx_ctr_svc(p, 0x24) == SNX_CTR_SVC_RETURN && !r[0]);
        }
        assert(snx_ctr_schedule(p) && p->current_thread == 2);
        assert(!r[0] && r[5] == 0x55555555 && !cpu->exit && !cpu->syndrome && !cpu->exclusive_valid);
        assert(cpu->tpidruro == SNX_CTR_INITIAL_TLS && cpu->tpidrurw == 0x12345678 && cpu->fpscr == 0x80000000);
        assert(cpu->d[7] == 0x12345678abcdef01ull);
        assert(!snx_ctr_handle_close(p, duplicate) && !p->objects[eid].type);
        r[0] = mutex;
        assert(snx_ctr_svc(p, 0x14) == SNX_CTR_SVC_RETURN && r[0] == 0xd8e0041f &&
               p->objects[mid].data.mutex.depth == 2);
        r[0] = mutex;
        r[2] = r[3] = UINT32_MAX;
        assert(snx_ctr_svc(p, 0x24) == SNX_CTR_SVC_BLOCKED && snx_ctr_schedule(p) && p->current_thread == tid);
        r[0] = mutex;
        assert(snx_ctr_svc(p, 0x14) == SNX_CTR_SVC_RETURN && !r[0]);
        assert(p->objects[2].data.thread.waiting_object == mid && p->objects[mid].data.mutex.depth == 1);
        r[0] = mutex;
        assert(snx_ctr_svc(p, 0x14) == SNX_CTR_SVC_RETURN && !r[0]);
        assert(p->objects[mid].data.mutex.owner == 2 && p->objects[mid].data.mutex.depth == 1);
        assert(snx_ctr_schedule(p) && p->current_thread == 2 && !r[0]);
        r[0] = mutex;
        assert(snx_ctr_svc(p, 0x14) == SNX_CTR_SVC_RETURN && !r[0]);
        assert(!snx_ctr_handle_close(p, mutex));
        assert(snx_ctr_svc(p, 0x21) == SNX_CTR_SVC_RETURN && !r[0]);
        uint32_t arbiter = r[1];
        ctr_put(p, 0x300300, 0);
        r[0] = arbiter;
        r[1] = 0x300300;
        r[2] = r[3] = 1;
        assert(snx_ctr_svc(p, 0x22) == SNX_CTR_SVC_BLOCKED && snx_ctr_schedule(p) && p->current_thread == tid);
        r[0] = arbiter;
        r[1] = 0x300300;
        r[2] = 0;
        r[3] = 1;
        assert(snx_ctr_svc(p, 0x22) == SNX_CTR_SVC_RETURN && !r[0] && snx_ctr_schedule(p) && p->current_thread == 2);
        assert(!r[0] && !snx_ctr_kernel_blocked(p));
        /* a timed wait (WaitIfLessThanWithTimeout, 1 s in r4:r5) signaled before its deadline wakes with success */
        r[0] = arbiter;
        r[1] = 0x300300;
        r[2] = 3;
        r[3] = 1;
        r[4] = 1000000000u;
        r[5] = 0;
        assert(snx_ctr_svc(p, 0x22) == SNX_CTR_SVC_BLOCKED && snx_ctr_schedule(p) && p->current_thread == tid);
        r[0] = arbiter;
        r[1] = 0x300300;
        r[2] = 0;
        r[3] = 1;
        assert(snx_ctr_svc(p, 0x22) == SNX_CTR_SVC_RETURN && !r[0] && snx_ctr_schedule(p) && p->current_thread == 2);
        assert(!r[0] && !snx_ctr_kernel_blocked(p) && !snx_ctr_handle_close(p, arbiter));
        uint32_t first = event(p, 0), second = event(p, 0);
        uint32_t first_id = snx_ctr_handle_object(p, first), second_id = snx_ctr_handle_object(p, second);
        ctr_put(p, 0x300200, first);
        ctr_put(p, 0x300204, second);
        p->objects[first_id].data.event.signaled = 1;
        r[0] = UINT32_MAX;
        r[1] = 0x300200;
        r[2] = 2;
        r[3] = 1;
        r[4] = UINT32_MAX;
        assert(snx_ctr_svc(p, 0x25) == SNX_CTR_SVC_BLOCKED && p->objects[first_id].data.event.signaled);
        assert(snx_ctr_schedule(p) && p->current_thread == tid);
        r[0] = second;
        assert(snx_ctr_svc(p, 0x18) == SNX_CTR_SVC_RETURN && !r[0]);
        assert(!p->objects[first_id].data.event.signaled && !p->objects[second_id].data.event.signaled);
        assert(snx_ctr_schedule(p) && p->current_thread == 2 && !r[0] && r[1] == UINT32_MAX);
        p->objects[first_id].data.event.signaled = 1;
        ctr_put(p, 0x300204, UINT32_MAX);
        r[0] = 0;
        r[1] = 0x300200;
        r[2] = 2;
        r[3] = 0;
        r[4] = 0;
        assert(snx_ctr_svc(p, 0x25) == SNX_CTR_SVC_RETURN && r[0] == 0xd8e007f7);
        assert(p->objects[first_id].data.event.signaled); /* validate all handles before consumption */
        r[0] = second;
        r[1] = UINT32_MAX;
        r[2] = r[3] = 0;
        assert(snx_ctr_svc(p, 0x24) == SNX_CTR_SVC_RETURN && r[0] == 0x09401bfe);
        /* A finite wait blocks until its deadline and then times out: the main thread (0x30) takes the core back
           from tid (49) at the first scheduling past it. 20 ms, so a stalled host does not pass the deadline
           before tid is scheduled */
        r[0] = second;
        r[2] = 20000000;
        r[3] = 0;
        assert(snx_ctr_svc(p, 0x24) == SNX_CTR_SVC_BLOCKED && snx_ctr_kernel_blocked(p));
        assert(snx_ctr_schedule(p) && p->current_thread == tid);
        spin_ms(25);
        assert(snx_ctr_schedule(p) && p->current_thread == 2 && r[0] == 0x09401bfe && !snx_ctr_kernel_blocked(p));
        assert(p->objects[second_id].refs == 1); /* the wait's reference is gone */
        /* the same wait signaled before its deadline (1 s) wakes with success */
        r[0] = second;
        r[2] = 1000000000u;
        r[3] = 0;
        assert(snx_ctr_svc(p, 0x24) == SNX_CTR_SVC_BLOCKED && snx_ctr_schedule(p) && p->current_thread == tid);
        r[0] = second;
        assert(snx_ctr_svc(p, 0x18) == SNX_CTR_SVC_RETURN && !r[0]);
        assert(snx_ctr_schedule(p) && p->current_thread == 2 && !r[0] && !snx_ctr_kernel_blocked(p));
        assert(!p->objects[second_id].data.event.signaled && p->objects[second_id].refs == 1);
        ctr_put(p, 0x300204, second);
        r[0] = 20000000;
        r[1] = 0x300200;
        r[2] = 2;
        r[3] = 1;
        r[4] = 0;
        assert(snx_ctr_svc(p, 0x25) == SNX_CTR_SVC_BLOCKED && p->objects[first_id].data.event.signaled);
        assert(snx_ctr_schedule(p) && p->current_thread == tid);
        spin_ms(25);
        assert(snx_ctr_schedule(p) && p->current_thread == 2 && r[0] == 0x09401bfe && r[1] == UINT32_MAX);
        assert(p->objects[first_id].data.event.signaled); /* a wait-all that timed out consumed nothing */
        assert(!snx_ctr_handle_close(p, thread) && p->objects[tid].refs == 1); /* live thread root */
        r[0] = second;
        r[2] = r[3] = UINT32_MAX;
        assert(snx_ctr_svc(p, 0x24) == SNX_CTR_SVC_BLOCKED && snx_ctr_schedule(p) && p->current_thread == tid);
        r[0] = second;
        r[2] = r[3] = UINT32_MAX;
        assert(snx_ctr_svc(p, 0x24) == SNX_CTR_SVC_BLOCKED && !snx_ctr_schedule(p));
        SnxArmCPU frozen = *cpu;
        const SnxCtrModule empty = {0};
        uint64_t executed;
        assert(snx_ctr_dispatch(&empty, p, 10, &executed) == SNX_CTR_RUN_BLOCKED && !executed);
        assert(!memcmp(&frozen, cpu, sizeof frozen));
        snx_ctr_process_free(p);
        p = snx_ctr_process_new(&image, (SnxCtrModel)model);
        assert(p);
        r = p->cpu.state.cpu.r;
        uint32_t heap;
        assert(!snx_ctr_kernel_memory(p, 3, 0x08000000, 0, 4096, 3, &heap));
        unsigned char recycled[4096];
        memset(recycled, 0xaa, sizeof recycled);
        assert(snx_ctr_write_ram(p->space, heap, recycled, sizeof recycled) == SNX_CTR_MEM_OK);
        assert(!snx_ctr_kernel_memory(p, 1, heap, 0, 4096, 0, &heap));
        for (unsigned i = 1; i < 32; ++i) { /* the APPLICATION limit's 32 threads, the main one included */
            r[0] = 49;
            r[1] = 0x100004;
            r[2] = i;
            r[3] = SNX_CTR_STACK_TOP;
            r[4] = 0xfffffffe;
            assert(snx_ctr_svc(p, 8) == SNX_CTR_SVC_RETURN && !r[0]);
            tid = snx_ctr_handle_object(p, r[1]);
            assert(p->objects[tid].data.thread.context.tpidruro == SNX_CTR_INITIAL_TLS + i * 512);
            assert(!snx_ctr_handle_close(p, r[1]) && p->objects[tid].refs == 1);
            if (i == 8) {
                assert(snx_ctr_read_ram(p->space, SNX_CTR_INITIAL_TLS + 4096, recycled, sizeof recycled) ==
                       SNX_CTR_MEM_OK);
                for (unsigned j = 0; j < sizeof recycled; ++j) assert(!recycled[j]);
            }
        }
        r[0] = 49;
        r[1] = 0x100004;
        r[2] = 0;
        r[3] = SNX_CTR_STACK_TOP;
        r[4] = 0xfffffffe;
        assert(snx_ctr_svc(p, 8) == SNX_CTR_SVC_RETURN && r[0] == 0xc860180c && !r[1]);
        assert(ctr_object_count(p) == 33); /* no leaked partially created thirty-third thread */
        snx_ctr_process_free(p);
        p = snx_ctr_process_new(&image, (SnxCtrModel)model);
        assert(p);
        timing(p);
        snx_ctr_process_free(p);
        p = snx_ctr_process_new(&image, (SnxCtrModel)model);
        assert(p);
        preemption(p);
        snx_ctr_process_free(p);
        p = snx_ctr_process_new(&image, (SnxCtrModel)model);
        assert(p);
        cores(p);
        snx_ctr_process_free(p);
    }
    puts(
        "Serialized native thread contexts/TLS, recursive wait ownership, real event wakeups and atomic wait-all passed");
    return 0;
}
