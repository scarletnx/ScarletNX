/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The save state's staging of the HLE (src/nx/runtime/state.c, included whole for its statics): a state file is the
   player's, or one someone gave them, and what it says of the HLE is checked before a load changes anything. A copy of
   g_hle that holds nothing is whole; a service object whose data is larger than the HLE itself, and an am storage
   whose bytes are not as many as the storage says it holds, are refused. A save refused before it starts (the game
   draws with the host's Vulkan, or doesn't stop) says so in the log, which is all a save that settings ask for has.
   Nothing else of the loader runs here: what it calls of the runtime is left unresolved at link time, but for what the
   staging's own way out and those saves call */
#include "nx/runtime/state.c"
#include <stdarg.h>
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
/* the save's */
static char g_said[512];
static int g_gvk, g_stops;
u64 plat_ns(void) { return 0; }
int gvk_active(void) { return g_gvk; }
int nvn_loader_active(void) { return 0; }
int game_pause(void) { return !g_stops; }
void logf_(int lvl, const char* fmt, ...) {
    va_list args;
    (void)lvl;
    va_start(args, fmt);
    vsnprintf(g_said, sizeof g_said, fmt, args);
    va_end(args);
}

static struct {
    Hle copy; /* the state's copy of g_hle, as a file gives it */
    u8 bytes[0x400];
} g_t;

/* the HEAP section of a state whose one record is an am storage's bytes: n of them for slot 0 */
static Buf heap_of_storage(u64 n) {
    Buf b = {g_t.bytes, 0, 0, sizeof g_t.bytes};
    StateIo io = buf_io(&b);
    int r = io_put32(&io, HEAP_STORAGE) | io_put32(&io, 0) | io_put64(&io, n);
    memset(g_t.bytes + b.n, 0x5a, (size_t)n);
    b.n += n;
    r |= io_put32(&io, HEAP_END);
    if (r) b.n = 0;
    b.at = 0;
    return b;
}

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

    /* an am storage of 0x100 bytes takes a record of 0x100, and no shorter one: its bytes are read to its size */
    ld->am.storages[0].size = 0x100;
    heap = heap_of_storage(0x100);
    CHECK(heap.n && !stage_hle(ld, &heap, &names, &paths) && ld->am.storages[0].d &&
          ld->am.storages[0].d[0xff] == 0x5a);
    free(ld->am.storages[0].d);
    ld->am.storages[0].d = NULL;
    heap = heap_of_storage(4);
    CHECK(heap.n && stage_hle(ld, &heap, &names, &paths) == -1);

    char why[256];
    g_gvk = 1;
    CHECK(state_save("states/x.1", why, sizeof why) == -1 && strstr(why, "guest_vulkan"));
    CHECK(strstr(g_said, "state: states/x.1 wasn't saved: the game draws with the host's Vulkan (guest_vulkan)"));
    g_gvk = 0;
    CHECK(state_save("states/x.1", why, sizeof why) == -1 && strstr(why, "didn't stop"));
    CHECK(strstr(g_said, "state: states/x.1 wasn't saved: the game didn't stop to be saved"));
    puts("state_stage: an empty copy, a service object's size, a storage's bytes and a refused save's line passed");
    return 0;
}
