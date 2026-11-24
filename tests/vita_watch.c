/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "vita/runtime/process.h"
#include "vita/runtime/watch.h"
#include "common/utilities/common.h"
#include "common/platform/vm.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <setjmp.h>
#include <signal.h>
#include <unistd.h>
#endif

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

#ifndef _WIN32
static struct {
    sigjmp_buf jump;              /* where a fault the watch does not know goes */
    volatile sig_atomic_t faults; /* the pages' first writes */
} g_test;
static void on_fault(int signal, siginfo_t* info, void* context) {
    (void)signal, (void)context;
    if (vita_watch_fault((uintptr_t)info->si_addr))
        ++g_test.faults;
    else
        siglongjmp(g_test.jump, 1);
}
#endif
/* a store the runtime makes of what the GPU wrote (vita_watch_store): rows apart across two pages, the bytes between
   them kept, no generation counted and the pages watched as before; and into memory nothing maps, which is a fault the
   handler is told, not a wait for a lock the store holds */
static void check_store(VitaProcess* p) {
    unsigned char rows[3 * 16];
    uint32_t at = WATCH_ADDRESS + (uint32_t)plat_mem_page() - 24;
    memset(rows, 0xaa, sizeof rows);
    vita_watch_release(WATCH_ADDRESS, WATCH_SIZE);
    memset(p->mem + WATCH_ADDRESS, 0x11, WATCH_SIZE);
    uint64_t since = vita_watch_protect(WATCH_ADDRESS, WATCH_SIZE);
    assert(vita_watch_store(at, rows, 8, 16, 3));
    for (uint32_t i = 0; i < 64; ++i) {
        unsigned char want = i >= 16 && i < 56 && (i - 16) % 16 < 8 ? 0xaa : 0x11;
        assert(p->mem[at - 16 + i] == want);
    }
    assert(!vita_watch_changed(WATCH_ADDRESS, WATCH_SIZE, since));
#ifndef _WIN32
    struct sigaction action = {.sa_sigaction = on_fault, .sa_flags = SA_SIGINFO | SA_NODEFER};
    sigaction(SIGSEGV, &action, NULL);
    sigaction(SIGBUS, &action, NULL); /* macOS's, for a page whose protection refuses the write */
    ((volatile unsigned char*)p->mem)[at] = 1;
    ((volatile unsigned char*)p->mem)[at + 24] = 1;
    assert(g_test.faults == 2 && vita_watch_changed(at, 1, since) && vita_watch_changed(at + 24, 1, since));
    alarm(60);
    if (!sigsetjmp(g_test.jump, 1)) {
        vita_watch_store(WATCH_ADDRESS + 0x100000, rows, 16, 16, 1);
        assert(!"a store into memory nothing maps comes back");
    }
    /* the lock is free: a page's first write goes through */
    vita_watch_protect(WATCH_ADDRESS, WATCH_SIZE);
    ((volatile unsigned char*)p->mem)[at] = 2;
    assert(g_test.faults == 3);
    alarm(0);
#endif
}
/* what each generation wrote, as pages, for the last 1024 of them */
static void check_wrote(void) {
    uint64_t page = plat_mem_page(), begin, end;
    vita_watch_written(WATCH_ADDRESS + 0x100, 0x2000);
    uint64_t now = vita_watch_epoch();
    assert(vita_watch_wrote(now, &begin, &end));
    assert(begin == WATCH_ADDRESS && end == WATCH_ADDRESS + (0x2100 + page - 1) / page * page);
    assert(!vita_watch_wrote(now + 1, &begin, &end));
    for (unsigned i = 0; i < 1024; ++i) vita_watch_written(WATCH_ADDRESS + 0x8000, 1);
    assert(!vita_watch_wrote(now, &begin, &end));
    for (unsigned i = 1; i <= 1024; ++i)
        assert(vita_watch_wrote(now + i, &begin, &end) && begin == WATCH_ADDRESS + 0x8000 && end == begin + page);
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
    check_store(process);
    check_wrote();
    vita_watch_free();
    plat_mem_release(process->mem, VITA_SPACE + 0x10000);
    free(process);
    puts("vita_watch: page writes invalidate cached GPU data");
    return 0;
}
