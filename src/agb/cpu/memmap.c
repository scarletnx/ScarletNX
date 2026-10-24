/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "agb/cpu/memmap.h"
#include "agb/board/board.h"
#include "common/utilities/common.h"
#include <stdio.h>
#include <string.h>

#ifndef AGB_MAP_SIZE
#define AGB_MAP_SIZE 0x10000000u
#endif

/* The console mirrors every region across its window: the unit is the region's own size. The palette and
   OAM are smaller than a host page, so their mirrors repeat every page. VRAM is 96 KiB in a 128 KiB
   window whose last 32 KiB repeat the 32 KiB above the bitmap region. */
typedef struct {
    uint32_t address, window, unit;
    int backing;
} AgbMirror;

/* A host memory object of size bytes for one region: its index in the map's backings, -1 when the host makes none */
static int map_backing(AgbMap* map, size_t size) {
    if (map->backing_count == (int)(sizeof(map->backings) / sizeof(map->backings[0]))) return -1;
    if (!snx_host_memory_new(&map->backings[map->backing_count], size)) return -1;
    return map->backing_count++;
}

/* The mirrors of one region, one backing mapped again and again. */
static int mirror_region(AgbMap* map, uint32_t address, uint32_t window, uint32_t unit, int backing) {
    for (uint32_t offset = 0; offset < window; offset += unit) {
        uint64_t within = offset % map->backings[backing].size;
        if (!snx_host_map(&map->backings[backing], map->base, address + offset, within, unit,
                          SNX_HOST_READ | SNX_HOST_WRITE))
            return -1;
    }
    return 0;
}

static int mirror_vram(AgbMap* map, uint32_t base, int backing) {
    for (uint32_t offset = 0; offset < 0x1000000; offset += 0x20000) {
        for (uint32_t page = 0; page < 0x20000; page += (uint32_t)map->page) {
            /* the window's last 32 KiB repeat the 32 KiB above the bitmap region */
            uint32_t within = page >= 0x18000 ? page - 0x8000 : page;
            if (!snx_host_map(&map->backings[backing], map->base, base + 0x06000000 + offset + page, within, map->page,
                              SNX_HOST_READ | SNX_HOST_WRITE))
                return -1;
        }
    }
    return 0;
}

/* RAM of zeros at [at, at + size) of the window, else -1 with error saying where and why */
static int commit(AgbMap* map, uint64_t at, uint64_t size, char* error, size_t error_size) {
    char why[128];
    if (snx_host_commit(map->base, at, size)) return 0;
    snx_os_error(why, sizeof why);
    snprintf(error, error_size, "cannot commit the guest memory at 0x%08llX, %llu bytes: %s", (unsigned long long)at,
             (unsigned long long)size, why);
    return -1;
}

/* Each region a host memory object, mapped at every mirror in both halves; the window's rest is RAM of zeros. */
static int mirror(AgbMap* map, AgbBoard* board, size_t rom, size_t rom_backing_size, char* error, size_t error_size) {
    enum { BIOS, EWRAM, IWRAM, IO, PALETTE, VRAM, OAM, SRAM };
    /* a region smaller than a page takes a page, its mirrors inside one the same bytes no more */
    size_t small = map->page > 0x1000 ? map->page : 0x1000, bios_size = map->page > 0x4000 ? map->page : 0x4000;
    size_t sizes[] = {bios_size, AGB_EWRAM_SIZE, AGB_IWRAM_SIZE, 0x10000, small, 0x20000, small, AGB_SRAM_SIZE};
    char why[128];
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i)
        if (map_backing(map, sizes[i]) < 0) {
            snx_os_error(why, sizeof why);
            snprintf(error, error_size, "cannot make the guest's memory: host memory object %zu of 8, %zu bytes: %s",
                     i + 1, sizes[i], why);
            return -1;
        }
    {
        AgbMirror mirrors[] = {
            {0x00000000, 0x2000000, (uint32_t)bios_size, BIOS}, {0x02000000, 0x1000000, AGB_EWRAM_SIZE, EWRAM},
            {0x03000000, 0x1000000, AGB_IWRAM_SIZE, IWRAM},     {0x04000000, 0x1000000, 0x10000, IO},
            {0x05000000, 0x1000000, (uint32_t)small, PALETTE},  {0x07000000, 0x1000000, (uint32_t)small, OAM},
            {0x0E000000, 0x2000000, AGB_SRAM_SIZE, SRAM},
        };
        for (uint32_t base = 0;; base = 0x80000000u) {
            for (size_t i = 0; i < sizeof(mirrors) / sizeof(mirrors[0]); ++i) {
                if (mirror_region(map, base + mirrors[i].address, mirrors[i].window, mirrors[i].unit,
                                  mirrors[i].backing)) {
                    snx_os_error(why, sizeof why);
                    snprintf(error, error_size, "cannot mirror the guest memory region at 0x%08X: %s",
                             base + mirrors[i].address, why);
                    return -1;
                }
            }
            if (mirror_vram(map, base, VRAM)) {
                snx_os_error(why, sizeof why);
                snprintf(error, error_size, "cannot mirror the guest video RAM at 0x%08X: %s", base + 0x06000000, why);
                return -1;
            }
            if (base) break;
        }
    }
    /* past the 28 bits the console decodes, and in both halves, the window is plain memory */
    if (commit(map, 0x10000000u, 0x70000000u, error, error_size) ||
        commit(map, 0x90000000u, AGB_MAP_SIZE + AGB_MAP_GUARD - 0x90000000u, error, error_size))
        return -1;
    if (!board->has_cartridge) /* the cartridge windows too, with no cartridge in them */
        return commit(map, 0x08000000u, 0x06000000u, error, error_size) ||
                       commit(map, 0x88000000u, 0x06000000u, error, error_size)
                   ? -1
                   : 0;
    {
        /* Each of the three wait state windows holds the image once. Past it the cartridge drives no data
           and the bus keeps the halfword address it latched, GBATEK's "Address/2 AND FFFFh". */
        int backing = map_backing(map, rom_backing_size),
            bus = rom_backing_size < AGB_ROM_MAX ? map_backing(map, AGB_ROM_BUS_PERIOD) : -2;
        if (backing < 0 || bus == -1) {
            snx_os_error(why, sizeof why);
            snprintf(error, error_size, "cannot make the guest's cartridge memory, %zu bytes: %s",
                     backing < 0 ? rom_backing_size : (size_t)AGB_ROM_BUS_PERIOD, why);
            return -1;
        }
        {
            uint8_t* image = map->backings[backing].view;
            memcpy(image, board->cartridge.data, rom);
            for (size_t i = rom; i < rom_backing_size; ++i) image[i] = agb_rom_open_bus8((uint32_t)i);
            if (bus >= 0)
                for (uint32_t i = 0; i < AGB_ROM_BUS_PERIOD; ++i) map->backings[bus].view[i] = agb_rom_open_bus8(i);
        }
        for (uint32_t window = 0x08000000; window < 0x0E000000; window += AGB_ROM_MAX) {
            for (uint32_t base = 0;; base = 0x80000000u) {
                if (mirror_region(map, base + window, (uint32_t)rom_backing_size, (uint32_t)rom_backing_size,
                                  backing) ||
                    (bus >= 0 && mirror_region(map, base + window + (uint32_t)rom_backing_size,
                                               AGB_ROM_MAX - (uint32_t)rom_backing_size, AGB_ROM_BUS_PERIOD, bus))) {
                    snx_os_error(why, sizeof why);
                    snprintf(error, error_size, "cannot mirror the guest's cartridge window at 0x%08X: %s",
                             base + window, why);
                    return -1;
                }
                if (base) break;
            }
        }
    }
    return 0;
}

int agb_map_init(AgbMap* map, AgbBoard* board, char* error, size_t error_size) {
    size_t rom = board->has_cartridge ? board->cartridge.size : AGB_ROM_MIN;
    /* the image in whole units of the open bus pattern that follows it, which repeats every 128 KiB */
    size_t rom_backing_size = (rom + AGB_ROM_BUS_PERIOD - 1) & ~(size_t)(AGB_ROM_BUS_PERIOD - 1);
    memset(map, 0, sizeof(*map));
    map->page = snx_host_page();
    map->mirrored = map->page && map->page <= AGB_MAP_VIEW_MAX;
    map->base = snx_host_reserve(AGB_MAP_SIZE + AGB_MAP_GUARD);
    if (!map->base) {
        char why[128];
        snx_os_error(why, sizeof why);
        snprintf(error, error_size, "cannot reserve the guest memory window, %llu bytes of address space: %s",
                 (unsigned long long)(AGB_MAP_SIZE + AGB_MAP_GUARD), why);
        return -1;
    }
    if (map->mirrored) {
        if (mirror(map, board, rom, rom_backing_size, error, error_size)) {
            agb_map_destroy(map, board);
            return -1;
        }
    } else {
        /* no view as small as a mirror: the window is RAM, and each mirror a copy of its own that diverges */
        if (commit(map, 0, AGB_MAP_SIZE + AGB_MAP_GUARD, error, error_size)) {
            agb_map_destroy(map, board);
            return -1;
        }
        if (board->has_cartridge)
            for (uint32_t window = 0x08000000; window < 0x0E000000; window += AGB_ROM_MAX) {
                memcpy(map->base + window, board->cartridge.data, rom);
                for (uint32_t i = (uint32_t)rom; i < AGB_ROM_MAX; ++i) map->base[window + i] = agb_rom_open_bus8(i);
            }
    }
    /* the board's devices read and write the regions through these pointers */
    agb_memory_free(&board->memory);
    memset(map->base + 0x0E000000, 0xFF, AGB_SRAM_SIZE); /* save memory reads 0xFF until written */
    board->memory.ewram = map->base + 0x02000000;
    board->memory.iwram = map->base + 0x03000000;
    board->memory.palette = map->base + 0x05000000;
    board->memory.vram = map->base + 0x06000000;
    board->memory.oam = map->base + 0x07000000;
    board->memory.sram = map->base + 0x0E000000;
    board->memory.owned = 0;
    map->adopted = 1;
    return 0;
}

void agb_map_destroy(AgbMap* map, AgbBoard* board) {
    if (map->base) {
        snx_host_release(map->base, AGB_MAP_SIZE + AGB_MAP_GUARD);
        if (map->adopted) agb_memory_init(&board->memory); /* the devices fall back to plain buffers */
    }
    for (int i = 0; i < map->backing_count; ++i) snx_host_memory_free(&map->backings[i]);
    memset(map, 0, sizeof(*map));
}
