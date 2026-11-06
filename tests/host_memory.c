/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The host's side of a flat guest space (common/memory/host.c), on every host the CI builds: one object mapped at two
   places of a reservation is the same memory, a commit is zeros of its own, a page unmapped goes back to the
   reservation, to be mapped again elsewhere in the object, and another object filled from it holds its bytes */
#include "common/memory/host.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    const unsigned rw = SNX_HOST_READ | SNX_HOST_WRITE;
    size_t page = snx_host_page();
    SnxHostMemory memory;
    unsigned char* base;
    /* the views read and written through volatile: the compiler sees two addresses where the host maps one page, and
       may read one before a store to the other */
    volatile unsigned char *v, *object;
    if (!page) {
        fputs("host_memory: this host makes no views (snx_host_page is 0)\n", stderr);
        return 1;
    }
    assert(snx_host_memory_new(&memory, 2 * page));
    base = snx_host_reserve(8 * page);
    assert(base);
    v = base;
    object = memory.view;
    /* the object at 0, its first page again at 4 */
    assert(snx_host_map(&memory, base, 0, 0, 2 * page, rw));
    assert(snx_host_map(&memory, base, 4 * page, 0, page, rw));
    v[5] = 42;
    assert(v[4 * page + 5] == 42 && object[5] == 42);
    v[5 * page - 1] = 7;
    assert(v[page - 1] == 7);
    object[page] = 9;
    assert(v[page] == 9);
    /* pages 2 and 3 zeros that are no view of it */
    assert(snx_host_commit(base, 2 * page, 2 * page));
    assert(!v[2 * page] && !v[4 * page - 1]);
    v[2 * page] = 5;
    assert(v[2 * page] == 5 && object[0] != 5 && object[page] == 9);
    assert(snx_host_protect(base, 0, page, SNX_HOST_READ) && v[5] == 42);
    /* page 4 back to the reservation, then the object's second page there */
    assert(snx_host_unmap(base, 4 * page, page));
    assert(snx_host_map(&memory, base, 4 * page, page, page, rw));
    assert(v[4 * page] == 9);
    /* another object filled with this one's bytes, through a mapping of its own */
    SnxHostMemory copy;
    assert(snx_host_memory_new(&copy, 2 * page));
    assert(snx_host_memory_fill(&copy, memory.view));
    assert(copy.view[5] == 42 && copy.view[page - 1] == 7 && copy.view[page] == 9);
    snx_host_memory_free(&copy);
    snx_host_release(base, 8 * page);
    snx_host_memory_free(&memory);
    printf("host_memory: ok, %zu byte pages\n", page);
    return 0;
}
