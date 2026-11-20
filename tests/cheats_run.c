/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A game's cheats as each frame runs them (src/nx/runtime/cheats.c), with the runtime stubbed: the file is the
   test's, the game's memory an array. A cheat that writes a value of 8 bytes takes both of the value's words, and one
   cut after the first writes nothing */
#include "nx/hle/filesystem/fs.h"
#include "nx/loader/module_abi.h"
#include "nx/memory/mem.h"
#include "nx/runtime/cheats.h"
#include "nx/runtime/loader.h"
#include "nx/runtime/platform/log.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAIN 0x10000u /* the main module's address */

static u8 memory[1 << 20];
u8* g_mem = memory;
u64 g_as_size = sizeof memory;
static const SnxModInfo g_mods[1] = {{.name = "main", .base = MAIN, .build_id = {0xab, 0xcd}}};
static const SnxModule g_module = {.nmods = 1, .mods = g_mods};
const SnxModule* g_mod = &g_module;

void logf_(int lvl, const char* fmt, ...) { (void)lvl, (void)fmt; }
/* all of the game's memory is one readable and writable block */
void mem_query(u64 addr, MemBlock* out) {
    (void)addr;
    *out = (MemBlock){.addr = 0, .size = sizeof memory, .perm = PERM_R | PERM_W};
}
/* the data folder's one file: the cheats of the main module's build ID */
u8* fs_read_file(const char* path, u64* size) {
    static const char text[] = "[whole]\n"
                               "08000000 00000100 11111111 22222222\n"
                               "[cut after the value's first word]\n"
                               "08000000 00000200 33333333\n"
                               "[four bytes]\n"
                               "04000000 00000300 44444444\n";
    if (strcmp(path, "cheats/ABCD000000000000.txt")) return NULL;
    u8* copy = malloc(sizeof text);
    assert(copy);
    memcpy(copy, text, sizeof text);
    *size = sizeof text - 1;
    return copy;
}

int main(void) {
    cheats_frame();
    assert(gr64(MAIN + 0x100) == 0x1111111122222222ull);
    assert(gr64(MAIN + 0x200) == 0); /* half a value is none: nothing written */
    assert(gr64(MAIN + 0x300) == 0x44444444ull);
    puts("cheats_run: ok");
    return 0;
}
