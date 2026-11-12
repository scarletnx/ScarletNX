/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Exercise the real runtime loader with synthetic files and a bounded guest mapping. */
#include "nx/runtime/loader.h"
#include "nx/runtime/main.h"
#include "nx/hle/filesystem/fs.h"
#include "nx/memory/mem.h"
#include "nx/runtime/platform/log.h"
#include "common/utilities/patches.h"
#include <stdlib.h>
#include <string.h>

static u8 file[0x4000], guest[0x5000];
static size_t file_size;
static int root_file, nso, maps, unmaps, permissions;
static char* patch_paths[1]; /* the patches fs_patches gives, patch_count of them */
static size_t patch_count;
static unsigned checks;
static SnxModInfo info;
static SnxModule module;
const SnxModule* g_mod = &module;
u8* g_mem = guest;
u64 g_code_memory;
char g_datadir[1024];

#define CHECK(v)                                                                                                       \
    do {                                                                                                               \
        ++checks;                                                                                                      \
        if (!(v)) {                                                                                                    \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #v);                                                    \
            exit(1);                                                                                                   \
        }                                                                                                              \
    } while (0)

void logf_(int level, const char* fmt, ...) {
    (void)level;
    (void)fmt;
}
size_t fs_patches(char* const** paths) {
    *paths = patch_paths;
    return patch_count;
}
int fs_map_file(const char* path, PFileMap* map) {
    if (nso) {
        if (strcmp(path, "exefs/main")) return -1;
    } else {
        /* A stale NSO named main must not shadow main.nro. */
        CHECK(strcmp(path, "exefs/main") && strcmp(path, "main"));
        if (strcmp(path, root_file ? "main.nro" : "exefs/main.nro")) return -1;
    }
    u8* bytes = malloc(file_size ? file_size : 1);
    CHECK(bytes != NULL);
    memcpy(bytes, file, file_size);
    map->data = bytes;
    map->size = file_size;
    ++maps;
    return 0;
}
void plat_file_unmap(PFileMap* map) {
    free((void*)map->data);
    ++unmaps;
}
int mem_map(u64 addr, u64 size, u32 state, u32 perm) {
    CHECK(addr == info.base && size == info.size && addr + size <= sizeof guest);
    CHECK(state == MS_CODE && perm == PERM_RW);
    memset(guest + addr, 0xcc, (size_t)size);
    return 0;
}
int mem_set_state(u64 addr, u64 size, u32 state, u32 perm) {
    static const u32 perms[] = {PERM_RX, PERM_R, PERM_RW};
    CHECK(permissions < 3 && perm == perms[permissions]);
    CHECK(addr == info.base + (u64)permissions * 0x1000);
    CHECK(size == (permissions == 2 ? 0x2000 : 0x1000));
    CHECK(state == (permissions == 2 ? MS_CODEDATA : MS_CODE));
    ++permissions;
    return 0;
}
static void put32(size_t off, u32 value) {
    for (unsigned i = 0; i < 4; ++i) file[off + i] = (u8)(value >> (8 * i));
}
static void fixture(void) {
    memset(file, 0, sizeof file);
    memset(&info, 0, sizeof info);
    memcpy(file + 0x10, "NRO0", 4);
    put32(0x18, 0x3000);
    put32(0x24, 0x1000);
    put32(0x28, 0x1000);
    put32(0x2c, 0x1000);
    put32(0x30, 0x2000);
    put32(0x34, 0x1000);
    put32(0x38, 0x1000);
    memset(file + 0x40, 0x42, 32);
    file[0x100] = 0x71;
    file[0x1000] = 0x72;
    file[0x2000] = 0x73;
    memset(file + 0x3000, 0xff, 0x40);
    memcpy(file + 0x3000, "ASET", 4);
    file_size = sizeof file;
    info.name = "main";
    info.base = 0x1000;
    info.size = 0x4000;
    memset(info.build_id, 0x42, 32);
    info.text_size = info.ro_size = info.data_size = info.bss_size = 0x1000;
    info.ro_off = 0x1000;
    info.data_off = 0x2000;
    info.is_nro = 1;
    module.nmods = 1;
    module.mods = &info;
    maps = unmaps = permissions = 0;
    g_code_memory = 0;
}
/* An NSO laid out as the NRO above: its text LZ4-compressed (one block of literals), rodata and data stored */
static void nso_fixture(void) {
    size_t at = 0x100, i, rest;
    fixture();
    nso = 1;
    memset(file, 0, sizeof file);
    memcpy(file, "NSO0", 4);
    put32(0x0c, 1);
    memset(file + 0x40, 0x42, 32);
    put32(0x3c, 0x1000);
    put32(0x10, (u32)at);
    put32(0x18, 0x1000);
    file[at++] = 0xf0;
    for (rest = 0x1000 - 15; rest >= 255; rest -= 255) file[at++] = 255;
    file[at++] = (u8)rest;
    for (i = 0; i < 0x1000; ++i) file[at + i] = (u8)(i * 7);
    put32(0x60, (u32)(at + 0x1000 - 0x100));
    at += 0x1000;
    for (i = 1; i < 3; ++i) {
        put32(0x10 + i * 0x10, (u32)at);
        put32(0x14 + i * 0x10, (u32)i * 0x1000);
        put32(0x18 + i * 0x10, 0x1000);
        memset(file + at, 0x70 + (int)i, 0x1000);
        at += 0x1000;
    }
    file_size = at;
    info.is_nro = 0;
}
/* the module refused: as, -1 for a file that is not a valid one, -2 for another build of the one the descriptor
   names (its build ID, segments or BSS another's), which the driver recompiles */
static void rejected(int as) {
    CHECK(loader_load() == as);
    CHECK(maps == 1 && unmaps == 1 && !permissions && !g_code_memory);
    for (size_t i = info.base; i < info.base + info.size; ++i) CHECK(guest[i] == 0xcc);
}
int main(void) {
    for (root_file = 0; root_file < 2; ++root_file) {
        fixture();
        if (root_file) {
            file_size = 0x3000; /* optional assets */
            put32(0x38, 0x40);
            info.bss_size = 0x40; /* the rest of the last page is mapping padding */
        }
        CHECK(!loader_load());
        CHECK(maps == 1 && unmaps == 1 && permissions == 3 && g_code_memory == info.size);
        CHECK(!memcmp(guest + info.base, file, 0x3000));
        for (size_t i = 0x3000; i < info.size; ++i) CHECK(guest[info.base + i] == 0);
    }
    root_file = 0;
    for (size_t i = 0; i < 0x80; ++i) {
        fixture();
        file_size = i;
        rejected(-1);
    }
    static const u32 offsets[] = {0x10, 0x18, 0x18, 0x18, 0x1c, 0x20, 0x24, 0x28, 0x30, 0x34, 0x38, 0x40};
    static const u32 values[] = {0, 0x7f, 0x4000, 0xffffffff, 2, 4, 0x800, 0x1004, 0x2004, 0xffffffff, 0xffffffff, 0};
    for (size_t i = 0; i < sizeof offsets / sizeof *offsets; ++i) {
        fixture();
        put32(offsets[i], values[i]);
        rejected(offsets[i] == 0x40 ? -2 : -1);
    }
    fixture();
    file_size = 0x2fff;
    rejected(-1);
    fixture();
    info.size = 0x3000; /* no room for BSS */
    rejected(-1);
    fixture();
    info.ro_size -= 4; /* the recompiled descriptor must match the file */
    rejected(-2);
    fixture();
    info.bss_size = 0;
    rejected(-2);
    nso_fixture();
    CHECK(!loader_load());
    CHECK(maps == 1 && unmaps == 1 && permissions == 3 && g_code_memory == info.size);
    for (size_t i = 0; i < 0x1000; ++i) CHECK(guest[info.base + i] == (u8)(i * 7));
    CHECK(guest[info.base + 0x1000] == 0x71 && guest[info.base + 0x2fff] == 0x72);
    for (size_t i = 0x3000; i < info.size; ++i) CHECK(guest[info.base + i] == 0);
    nso_fixture();
    file[0x40] ^= 1; /* another build */
    rejected(-2);
    nso_fixture();
    put32(0x38, 0x4000); /* data past the mapping */
    rejected(-1);
    nso_fixture();
    file_size -= 1; /* data past the file */
    rejected(-1);
    nso_fixture();
    put32(0x60, 0x100); /* a block cut short */
    CHECK(loader_load() == -1 && unmaps == 1 && !permissions && !g_code_memory);
    { /* the mods' patches the program was recompiled with, over the segments as copied: one for this build, a second
         named after another build left out; one that writes past the segments stops the load */
        static const u8 fits[] = {'P',  'A',  'T',  'C',  'H',  0x00, 0x01, 0x05, 0x00, 0x02, 0xab,
                                  0xcd, 0x00, 0x11, 0x00, 0x00, 0x01, 0xee, 'E',  'O',  'F'};
        static const u8 past[] = {'P', 'A', 'T', 'C', 'H', 0x00, 0x31, 0x00, 0x00, 0x02, 0xab, 0xcd, 'E', 'O', 'F'};
        char *dir = snx_temp_dir(NULL), name[80];
        CHECK(dir != NULL);
        for (int i = 0; i < 32; ++i) memcpy(name + 2 * i, "42", 2);
        memcpy(name + 64, ".ips", 5);
        patch_paths[0] = snx_path_join(dir, name);
        patch_count = 1;
        CHECK(!snx_write_file(patch_paths[0], fits, sizeof fits));
        nso_fixture();
        CHECK(!loader_load() && permissions == 3);
        CHECK(guest[info.base + 5] == 0xab && guest[info.base + 6] == 0xcd && guest[info.base + 7] == (u8)(7 * 7));
        CHECK(guest[info.base + 0x1000] == 0xee && guest[info.base + 0x1001] == 0x71);
        nso_fixture();
        file[0x40] = 0x43; /* another build, which the recompiled descriptor names too */
        info.build_id[0] = 0x43;
        CHECK(!loader_load() && guest[info.base + 5] == (u8)(5 * 7) && guest[info.base + 0x1000] == 0x71);
        CHECK(!snx_write_file(patch_paths[0], past, sizeof past));
        nso_fixture();
        CHECK(loader_load() == -1 && !permissions && !g_code_memory);
        patch_count = 0;
        snx_remove_tree(dir);
        free(patch_paths[0]);
        free(dir);
    }
    printf("Runtime NRO and NSO: %u checks passed\n", checks);
    return 0;
}
