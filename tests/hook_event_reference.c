/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The kernel event a hooked SDK object attaches to the game's SystemEvent (hook_attach_event,
   src/nx/nn/guest_call.c), with the kernel's objects stubbed to their reference counts: the object that asked for
   it holds a reference of its own beside the handle's, so dropping it when the object goes leaves the event alive for
   as long as the game's SystemEvent keeps the handle, and the event is destroyed once, when the last of the two goes */
#include "nx/nn/guest_call.c"
#include <stdio.h>
#include <stdlib.h>

static unsigned checks;
#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        ++checks;                                                                                                      \
        if (!(x)) {                                                                                                    \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);                                                    \
            exit(1);                                                                                                   \
        }                                                                                                              \
    } while (0)

u8* g_mem;
const SnxArmModule* g_arm_mod; /* a 64-bit game: hook_guest_call runs the function itself */
void hook_guest_call_a32(CPU* c, u32 fn, u32 a0, u32 a1, u32 a2, u32 a3) {
    (void)c, (void)fn, (void)a0, (void)a1, (void)a2, (void)a3;
}
static KEvent event;
static unsigned made, destroyed, handles;
static int table_full, no_attach;
KEvent* kevent_new(const char* name) {
    (void)name;
    made++;
    event = (KEvent){0};
    return &event;
}
void kobj_ref(KObj* o) { o->refs++; }
void kobj_unref(KObj* o) {
    if (!o) return;
    CHECK(o->refs > 0); /* a reference nobody held */
    if (!--o->refs) destroyed++;
}
u32 h_new(KObj* o) { /* a handle holds a reference, as the kernel's does */
    if (table_full) return 0;
    o->refs++;
    handles++;
    return 0x55;
}
u64 kernel_find_symbol_exact(const char* name) { return (void)name, no_attach ? 0 : 0x4000; }

int main(void) {
    static CPU cpu;
    CPU* c = &cpu;
    /* the event itself: the caller's reference and the handle's */
    KEvent* e = hook_attach_event(c, 0, "test");
    CHECK(e == &event && made == 1 && handles == 1 && event.o.refs == 2 && !destroyed);
    kobj_unref(&e->o); /* the object that asked goes: the game's handle still holds the event */
    CHECK(event.o.refs == 1 && !destroyed);
    kobj_unref(&e->o); /* the SystemEvent's destruction closes the handle */
    CHECK(destroyed == 1);

    /* a full handle table: the event is destroyed there, once, and the caller gets none */
    table_full = 1;
    CHECK(hook_attach_event(c, 0, "test") == NULL && made == 2 && destroyed == 2 && event.o.refs == 0);
    table_full = 0;
    /* an SDK with no AttachReadableHandleToSystemEvent: no event is made */
    no_attach = 1;
    CHECK(hook_attach_event(c, 0, "test") == NULL && made == 2 && destroyed == 2);
    printf("hook_event_reference: %u checks passed\n", checks);
    return 0;
}
