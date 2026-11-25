/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A handler's calls into the game's own code (hook.h): a guest function run to its return, and a kernel event or a
   handle attached to the game's SystemEventType through its SDK's nn::os::AttachReadableHandleToSystemEvent, as an
   SDK object attaches its service's */
#include "nx/nn/hook.h"
#include "nx/hle/kernel/kernel.h"
#include "nx/runtime/arm.h"

void hook_guest_call(CPU* c, u64 fn, u64 a0, u64 a1, u64 a2, u64 a3) {
    u64 lr = c->x[30], pc = fn;
    if (g_arm_mod) {
        hook_guest_call_a32(c, (u32)fn, (u32)a0, (u32)a1, (u32)a2, (u32)a3);
        return;
    }
    c->x[0] = a0;
    c->x[1] = a1;
    c->x[2] = a2;
    c->x[3] = a3;
    do snx_lookup(c, pc)(c, pc);
    while ((pc = c->pc) != lr);
    c->x[30] = lr;
}
KEvent* hook_attach_event(CPU* c, u64 event, const char* name) {
    static const char attach[] =
        "_ZN2nn2os33AttachReadableHandleToSystemEventEPNS0_15SystemEventTypeEjbNS0_14EventClearModeE";
    u64 fn = kernel_find_symbol_exact(attach);
    KEvent* e;
    u32 h;
    if (!fn) return NULL;
    e = kevent_new(name);
    kobj_ref(&e->o);  /* the caller's reference, which it drops when its object goes */
    h = h_new(&e->o); /* the handle's, which the SystemEvent's destruction closes */
    if (!h) {         /* a full handle table: the event is destroyed here */
        kobj_unref(&e->o);
        return NULL;
    }
    if (event) hook_guest_call(c, fn, event, h, 1, 1);
    return e;
}
int hook_attach_handle(CPU* c, u64 event, u32 handle) {
    static const char attach[] =
        "_ZN2nn2os33AttachReadableHandleToSystemEventEPNS0_15SystemEventTypeEjbNS0_14EventClearModeE";
    u64 fn = kernel_find_symbol_exact(attach);
    if (!fn || !handle) return 0;
    hook_guest_call(c, fn, event, handle, 1, 1);
    return 1;
}
