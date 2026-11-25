/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* SceFiber: user-mode coroutines, each a set of registers and a stack that a thread runs in place of its own code
   until the fiber returns to it. The stack is either a context buffer the game gives at init, or, if it gives none,
   whichever thread's stack the fiber is started or switched to from. sceFiberRun keeps the thread's registers and
   loads the fiber's (at its entry the first time, after its last sceFiberReturnToThread since); sceFiberReturnToThread
   does the reverse. The game's SceFiber holds the slot's index, the registers stay here. As the vitasdk headers
   (MIT) declare, and Vita3K's own SceFiber.cpp (GPL-2.0-or-later) confirms where they're silent */
#include "vita/hle/hle.h"
#include "vita/hle/hle.hook.h"
#include <string.h>

#define SCE_FIBER_ERROR_NULL       0x80590001u
#define SCE_FIBER_ERROR_ALIGNMENT  0x80590002u
#define SCE_FIBER_ERROR_RANGE      0x80590003u
#define SCE_FIBER_ERROR_INVALID    0x80590004u
#define SCE_FIBER_ERROR_PERMISSION 0x80590005u
#define SCE_FIBER_ERROR_STATE      0x80590006u
#define SCE_FIBER_ERROR_BUSY       0x80590007u
#define MAGIC 0x52424946u /* "FIBR", then the slot's index */

static VitaFiber* fiber_of(VitaProcess* p, uint32_t address) {
    if (!address || !vita_mapped(p, address, 8) || vita_read32(p, address) != MAGIC) return NULL;
    uint32_t index = vita_read32(p, address + 4);
    return index < VITA_FIBERS && p->fibers[index].address == address ? &p->fibers[index] : NULL;
}
/* _sceFiberInitializeImpl(fiber, name, entry, argOnInitialize, addrContext, sizeContext, params): addrContext and
   sizeContext may both be 0, meaning the fiber runs on whichever thread's stack it's started from (sceFiberRun's,
   or the last switch's) instead of a context of its own */
uint32_t vita_fiber_initialize(VitaProcess* p, VitaThread* t, uint32_t address, uint32_t name, uint32_t entry,
                               uint32_t arg_on_initialize, uint32_t context, uint32_t size, uint32_t params) {
    (void)t, (void)params;
    if (!address || !entry || !name) return SCE_FIBER_ERROR_NULL;
    if (size && size < 512) return SCE_FIBER_ERROR_RANGE;
    if ((address & 7) || (context & 7) || (size & 7) || (!context) != (!size)) return SCE_FIBER_ERROR_INVALID;
    if (context && !vita_mapped(p, context, size)) return SCE_FIBER_ERROR_RANGE;
    unsigned slot = VITA_FIBERS;
    for (unsigned i = 0; i < VITA_FIBERS && slot == VITA_FIBERS; ++i)
        if (!p->fibers[i].address) slot = i;
    if (slot == VITA_FIBERS) return SCE_FIBER_ERROR_INVALID;
    p->fibers[slot] = (VitaFiber){.address = address, .entry = entry, .argument = arg_on_initialize, .context = context,
                                  .context_size = size};
    memset(vita_ptr(p, address), 0, 128);
    vita_write32(p, address, MAGIC), vita_write32(p, address + 4, slot);
    return 0;
}
uint32_t vita_fiber_finalize(VitaProcess* p, VitaThread* t, uint32_t fiber) {
    (void)t;
    VitaFiber* f = fiber_of(p, fiber);
    if (!f) return fiber ? SCE_FIBER_ERROR_INVALID : SCE_FIBER_ERROR_NULL;
    if (f->running) return SCE_FIBER_ERROR_STATE;
    memset(vita_ptr(p, f->address), 0, 8);
    memset(f, 0, sizeof *f);
    return 0;
}
/* sceFiberRun(fiber, argOnRunTo, argOnRun*): the thread's registers kept, the fiber's loaded, the value the fiber
   sees returned in its own r0 (the fiber's sceFiberReturnToThread returns 0, its entry gets them as arguments) */
uint32_t vita_fiber_run(VitaProcess* p, VitaThread* t, uint32_t fiber, uint32_t to, uint32_t arg_on_run) {
    VitaFiber* f = fiber_of(p, fiber);
    if (!f) return fiber ? SCE_FIBER_ERROR_INVALID : SCE_FIBER_ERROR_NULL;
    if (t->fiber) return SCE_FIBER_ERROR_PERMISSION;
    if (f->running) return SCE_FIBER_ERROR_BUSY;
    uint32_t caller_sp = t->cpu.r[13]; /* before t->cpu is overwritten below, for a context-less fiber's stack */
    t->own = t->cpu;
    t->own.r[0] = 0;
    t->fiber_out = arg_on_run;
    t->fiber = (uint32_t)(f - p->fibers) + 1;
    f->running = t->uid;
    if (!f->started) {
        SnxArmCPU* c = &t->cpu;
        memset(c->r, 0, sizeof c->r);
        c->r[0] = f->argument, c->r[1] = to;
        c->r[13] = (f->context ? (f->context + f->context_size) : caller_sp) & ~7u;
        c->r[14] = VITA_FIBER_RETURN;
        c->cpsr = (c->cpsr & ~SNX_ARM_T) | (f->entry & 1 ? SNX_ARM_T : 0);
        c->r[15] = f->entry & ~1u;
        f->started = 1;
        return c->r[0];
    }
    t->cpu = f->cpu;
    if (f->in_out) vita_write32(p, f->in_out, to);
    return 0;
}
/* the thread's own registers back, what the fiber returns written where sceFiberRun asked */
static void back(VitaProcess* p, VitaThread* t, uint32_t value) {
    VitaFiber* f = &p->fibers[t->fiber - 1];
    f->running = 0;
    if (t->fiber_out) vita_write32(p, t->fiber_out, value);
    t->fiber = 0;
    t->cpu = t->own;
}
/* sceFiberReturnToThread(argOnReturn, argOnRun*): the fiber's registers kept, at its return, for its next run */
uint32_t vita_fiber_return_to_thread(VitaProcess* p, VitaThread* t, uint32_t arg_on_return, uint32_t arg_on_run) {
    if (!t->fiber) return SCE_FIBER_ERROR_PERMISSION;
    VitaFiber* f = &p->fibers[t->fiber - 1];
    f->cpu = t->cpu;
    f->cpu.r[0] = 0;
    f->in_out = arg_on_run;
    back(p, t, arg_on_return);
    return 0; /* sceFiberRun's */
}
void vita_fiber_ended(VitaProcess* p, VitaThread* t) {
    if (!t->fiber) {
        vita_thread_end(p, t, 0);
        return;
    }
    p->fibers[t->fiber - 1].started = 0; /* run again from its entry */
    back(p, t, 0);
}
uint32_t vita_fiber_get_self(VitaProcess* p, VitaThread* t, uint32_t out) {
    if (!out) return SCE_FIBER_ERROR_NULL;
    if (!t->fiber) return SCE_FIBER_ERROR_PERMISSION;
    vita_write32(p, out, p->fibers[t->fiber - 1].address);
    return 0;
}
/* _sceFiberInitializeWithInternalOptionImpl(fiber, name, entry, argOnInitialize, addrContext, sizeContext): the
   same as _sceFiberInitializeImpl, minus its params argument, which initialize() never reads anyway */
uint32_t vita_fiber_initialize_with_internal_option(VitaProcess* p, VitaThread* t, uint32_t fiber, uint32_t name,
                                                    uint32_t entry, uint32_t arg_on_initialize, uint32_t addr_context,
                                                    uint32_t size_context, uint32_t params) {
    return vita_fiber_initialize(p, t, fiber, name, entry, arg_on_initialize, addr_context, size_context, params);
}
/* sceFiberOptParamInitialize(optParam): zero it, same as the real one does */
uint32_t vita_fiber_opt_param_initialize(VitaProcess* p, VitaThread* t, uint32_t out) {
    (void)t;
    if (!out) return SCE_FIBER_ERROR_NULL;
    if (!vita_mapped(p, out, 128)) return SCE_FIBER_ERROR_RANGE;
    memset(vita_ptr(p, out), 0, 128);
    return 0;
}
/* sceFiberGetInfo(fiber, out SceFiberInfo): entry, argOnInitialize, addrContext, sizeContext, name[32], padding[80] */
uint32_t vita_fiber_get_info(VitaProcess* p, VitaThread* t, uint32_t fiber, uint32_t out) {
    (void)t;
    VitaFiber* f = fiber_of(p, fiber);
    if (!f) return fiber ? SCE_FIBER_ERROR_INVALID : SCE_FIBER_ERROR_NULL;
    if (!out) return SCE_FIBER_ERROR_NULL;
    if (!vita_mapped(p, out, 0x170)) return SCE_FIBER_ERROR_RANGE;
    memset(vita_ptr(p, out), 0, 0x170);
    vita_write32(p, out, f->entry);
    vita_write32(p, out + 4, f->argument);
    vita_write32(p, out + 8, f->context);
    vita_write32(p, out + 12, f->context_size);
    return 0;
}
/* sceFiberSwitch(fiber, argOnRunTo, argOnRun*): what sceFiberRun then sceFiberReturnToThread would, from inside
   another fiber (switching straight to a sibling, instead of going back to the thread first) */
uint32_t vita_fiber_do_switch(VitaProcess* p, VitaThread* t, uint32_t fiber, uint32_t to, uint32_t out) {
    VitaFiber* f = fiber_of(p, fiber);
    if (!f) return fiber ? SCE_FIBER_ERROR_INVALID : SCE_FIBER_ERROR_NULL;
    if (!t->fiber) return SCE_FIBER_ERROR_PERMISSION; /* no fiber running on this thread to switch away from */
    if (f->running) return SCE_FIBER_ERROR_BUSY;
    VitaFiber* from = &p->fibers[t->fiber - 1];
    from->cpu = t->cpu;
    from->cpu.r[0] = 0;
    from->in_out = out;
    from->running = 0;
    /* fiber_out stays whatever the thread's own sceFiberRun set: a later sceFiberReturnToThread, however many
       switches happen first, still writes back where the thread expects it. own.r[13]: the thread's own sp, for a
       context-less fiber's stack, same as a context-less fiber's run() uses */
    t->fiber = (uint32_t)(f - p->fibers) + 1;
    f->running = t->uid;
    if (!f->started) {
        SnxArmCPU* c = &t->cpu;
        memset(c->r, 0, sizeof c->r);
        c->r[0] = f->argument, c->r[1] = to;
        c->r[13] = (f->context ? (f->context + f->context_size) : t->own.r[13]) & ~7u;
        c->r[14] = VITA_FIBER_RETURN;
        c->cpsr = (c->cpsr & ~SNX_ARM_T) | (f->entry & 1 ? SNX_ARM_T : 0);
        c->r[15] = f->entry & ~1u;
        f->started = 1;
        return c->r[0];
    }
    t->cpu = f->cpu;
    if (f->in_out) vita_write32(p, f->in_out, to);
    return 0;
}
