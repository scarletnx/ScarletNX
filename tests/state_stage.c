/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The save state's staging of the HLE (src/nx/runtime/state.c, included whole for its statics): a state file is the
   player's, or one someone gave them, and what it says of the HLE is checked before a load changes anything. A copy of
   g_hle that holds nothing is whole; a service object whose data is larger than the HLE itself is refused. Nothing
   else of the loader runs here: what it calls of the runtime is left unresolved at link time, but for what the
   staging's own way out calls */
#include "nx/runtime/state.c"
#include <stdio.h>

#define CHECK(expr)                                                                                                    \
    do {                                                                                                               \
        if (!(expr)) {                                                                                                 \
            fprintf(stderr, "state_stage.c:%d: check failed: %s\n", __LINE__, #expr);                                  \
            return 1;                                                                                                  \
        }                                                                                                              \
    } while (0)

Hle g_hle;
void snx_socket_unref(SnxSocket* s) { (void)s; } /* staged_free's, of sockets a refused copy never had */

static struct {
    Hle copy; /* the state's copy of g_hle, as a file gives it */
    u8 bytes[4];
} g_t;

int main(void) {
    Names names = {0};
    Buf paths = {0}, heap = {g_t.bytes, 4, 0, sizeof g_t.bytes}; /* four zero bytes: HEAP_END */
    Hle* ld = &g_t.copy;
    CHECK(!stage_hle(ld, &heap, &names, &paths));

    /* a service object's data is a slot of its service's pool in g_hle, named by the slot's first word: an accessor
       of am's here, object 6. With the size of a slot it is whole */
    SvcObj* o = &ld->ipc.svcobjs[5];
    atomic_store_explicit(&o->refs, 1, memory_order_relaxed);
    o->dispatch = (void*)(uintptr_t)1;
    o->data = &g_hle.am.accessors[0];
    o->datasize = sizeof g_hle.am.accessors[0];
    ld->am.accessors[0].obj = 6;
    CHECK(!hle_check(ld));
    /* and larger than the whole HLE it is in no pool of it */
    o->datasize = sizeof(Hle) + 0x100;
    CHECK(hle_check(ld) == -1);
    memset(o, 0, sizeof *o);
    ld->am.accessors[0].obj = 0;

    puts("state_stage: an empty copy and a service object's size passed");
    return 0;
}
