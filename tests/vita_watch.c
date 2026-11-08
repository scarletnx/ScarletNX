/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "vita/runtime/process.h"
#include "vita/runtime/watch.h"
#include "common/utilities/common.h"
#include "common/platform/vm.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#define WATCH_ADDRESS 0x20000000u
#define WATCH_SIZE    0x10000u

static void guest_write(VitaProcess* p, uint32_t address, unsigned char value) {
#ifdef _WIN32
    ((volatile unsigned char*)p->mem)[address] = value;
#else
    assert(vita_watch_fault((uintptr_t)(p->mem + address)));
    ((volatile unsigned char*)p->mem)[address] = value;
#endif
}

int main(void) {
    VitaProcess* process = calloc(1, sizeof *process);
    assert(process);
    process->mem = plat_mem_reserve(VITA_SPACE + 0x10000, 0);
    assert(process->mem);
    assert(!plat_mem_protect(process->mem + WATCH_ADDRESS, WATCH_SIZE, PLAT_MEM_READ | PLAT_MEM_WRITE));
    assert(vita_watch_init(process));

    uint64_t since = vita_watch_protect(WATCH_ADDRESS, WATCH_SIZE);
    assert(!vita_watch_changed(WATCH_ADDRESS, WATCH_SIZE, since));
    guest_write(process, WATCH_ADDRESS, 1);
    assert(vita_watch_changed(WATCH_ADDRESS, WATCH_SIZE, since));

    since = vita_watch_protect(WATCH_ADDRESS, WATCH_SIZE);
    assert(!vita_watch_changed(WATCH_ADDRESS, WATCH_SIZE, since));
    guest_write(process, WATCH_ADDRESS, 2);
    assert(vita_watch_changed(WATCH_ADDRESS, WATCH_SIZE, since));

    vita_watch_release(WATCH_ADDRESS, WATCH_SIZE);
    assert(vita_watch_changed(WATCH_ADDRESS, WATCH_SIZE, since));
    ((volatile unsigned char*)process->mem)[WATCH_ADDRESS] = 3;
    assert(process->mem[WATCH_ADDRESS] == 3);
    vita_watch_free();
    plat_mem_release(process->mem, VITA_SPACE + 0x10000);
    free(process);
    puts("vita_watch: page writes invalidate cached GPU data");
    return 0;
}
