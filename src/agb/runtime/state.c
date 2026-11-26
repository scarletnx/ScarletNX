/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A GBA game's save state. Three sections, each with the schema hash of what it holds:

     BOARD   the AgbBoard as it is, but for what the running process keeps: the cartridge (its image, the state names
             it by its hash), the memory's pointers, the gba_color setting, and the save memory's kind and whether the
             runner has kept it on disk
     CPU     the AgbCpu as it is, but for its pointers (the board, the flat window, the bus hook)
     MEMORY  EWRAM, IWRAM, the palette, VRAM and OAM, in that order

   The structs go as their bytes, as the Switch's g_hle does: a state whose types had other fields has another schema,
   and is refused. The recompiled blocks are not state: one built from RAM that a load changed is built again at its
   next dispatch, which compares its code (recomp.c) */
#include "agb/runtime/state.h"
#include "common/platform/time.h"
#include "common/utilities/build_info.h"
#include "common/utilities/common.h"
#include "common/utilities/state_header.h"
#include "common/utilities/state_file.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CLOCK_HZ 16777216u /* the board's cycles in a second */

/* the regions of the MEMORY section, in their order: their sizes, and where the board has them */
static const uint32_t region_size[] = {AGB_EWRAM_SIZE, AGB_IWRAM_SIZE, AGB_PALETTE_SIZE, AGB_VRAM_SIZE, AGB_OAM_SIZE};
#define REGIONS      (sizeof region_size / sizeof *region_size)
#define MEMORY_BYTES (AGB_EWRAM_SIZE + AGB_IWRAM_SIZE + AGB_PALETTE_SIZE + AGB_VRAM_SIZE + AGB_OAM_SIZE)
static void regions(const AgbMemory* memory, uint8_t* at[REGIONS]) {
    at[0] = memory->ewram;
    at[1] = memory->iwram;
    at[2] = memory->palette;
    at[3] = memory->vram;
    at[4] = memory->oam;
}

/* the schema hashes: the walk of each struct's fields and their types, and its size. The walk names an array's
   type and length but not its elements' fields: the structs the board holds arrays of are walked too */
static uint64_t board_schema(const AgbBoard* board) {
    uint64_t h = SNX_FNV_BASIS ^ sizeof *board;
    __builtin_dump_struct(board, snx_state_schema_out, &h);
    __builtin_dump_struct(&board->dma[0], snx_state_schema_out, &h);
    __builtin_dump_struct(&board->timer[0], snx_state_schema_out, &h);
    return h;
}
static uint64_t cpu_schema(const AgbCpu* cpu) {
    uint64_t h = SNX_FNV_BASIS ^ sizeof *cpu;
    __builtin_dump_struct(cpu, snx_state_schema_out, &h);
    return h;
}
static uint64_t memory_schema(void) { return snx_fnv1a(SNX_FNV_BASIS, region_size, sizeof region_size); }

/* the board as a state holds it: what the running process keeps zeroed, so it is no part of the file */
static void board_strip(AgbBoard* board) {
    memset(&board->cartridge, 0, sizeof board->cartridge);
    memset(&board->memory, 0, sizeof board->memory);
    board->ppu.lcd = 0;
    board->save.type = board->save.dirty = 0;
}
static void cpu_strip(AgbCpu* cpu) {
    cpu->board = NULL;
    cpu->arm.mem = NULL;
    cpu->arm.io = NULL;
    cpu->arm.rt = NULL;
}

/* the play time the state holds, from the board's cycles, without the product's overflow past 18 minutes */
static uint64_t guest_ns(uint64_t cycles) {
    return cycles / CLOCK_HZ * 1000000000ull + cycles % CLOCK_HZ * 1000000000ull / CLOCK_HZ;
}

static int put_section(StateWriter* w, const char* tag, uint64_t schema, const void* p, uint64_t n) {
    return sw_begin(w, tag, schema) || sw_put(w, p, n) || sw_end(w) ? -1 : 0;
}

int snx_agb_state_save(AgbBoard* board, AgbCpu* cpu, const char* path, const uint8_t* thumb, char* why,
                       size_t whysize) {
    uint64_t t0 = plat_ns();
    SnxStateHeader h = {0};
    h.format = SNX_STATE_FORMAT;
    h.abi = SNX_AGB_STATE_VERSION;
    h.title_id = agb_board_hash(board);
    h.created = (uint64_t)time(NULL);
    h.guest_ns = guest_ns(board->now);
    h.thumb_w = thumb ? SNX_STATE_THUMB_W : 0;
    h.thumb_h = thumb ? SNX_STATE_THUMB_H : 0;
    snprintf(h.runtime, sizeof h.runtime, "%s", snx_build_info());
    snprintf(h.game, sizeof h.game, "%s", board->cartridge.title);
    char* folder = snx_dirname(path);
    if (folder && folder[0] && snx_mkdirs(folder)) {
        char os[160];
        snprintf(why, whysize, "its folder %s can't be made: %s", folder, snx_os_error(os, sizeof os));
        free(folder);
        return -1;
    }
    free(folder);
    AgbBoard* b = (AgbBoard*)malloc(sizeof *b);
    if (!b) {
        snprintf(why, whysize, "out of memory for a copy of the board (%zu bytes)", sizeof *b);
        return -1;
    }
    memcpy(b, board, sizeof *b); /* its padding too, which goes in the file */
    board_strip(b);
    AgbCpu c;
    memcpy(&c, cpu, sizeof c);
    cpu_strip(&c);
    StateWriter* w = sw_open(path, &h, thumb, why, whysize);
    int r = -1;
    if (w) {
        int failed = put_section(w, "BOARD", board_schema(board), b, sizeof *b) ||
                     put_section(w, "CPU", cpu_schema(cpu), &c, sizeof c) || sw_begin(w, "MEMORY", memory_schema());
        uint8_t* at[REGIONS];
        regions(&board->memory, at);
        for (size_t i = 0; !failed && i < REGIONS; ++i) failed = sw_put(w, at[i], region_size[i]);
        if (failed || sw_end(w)) {
            sw_abort(w);
            snprintf(why, whysize, "the file couldn't be written");
        } else
            r = sw_finish(w, why, whysize);
    }
    free(b);
    if (!r) printf("Saved the state %s in %.1f ms\n", path, (double)(plat_ns() - t0) / 1e6);
    return r;
}

/* the section tagged tag read whole into p, n bytes, with the schema this runtime writes: 0, or -1 with why */
static int get_section(StateReader* r, const char* tag, uint64_t schema, void* p, uint64_t n, char* why,
                       size_t whysize) {
    uint64_t had = 0;
    if (sr_begin(r, tag, &had)) {
        snprintf(why, whysize, "it has no %s section", tag);
        return -1;
    }
    if (had != schema) {
        snprintf(why, whysize, "it holds the GBA's %s in another layout (schema %016llX, this ScarletNX's %016llX)",
                 tag, (unsigned long long)had, (unsigned long long)schema);
        return -1;
    }
    if (sr_left(r) != n) {
        snprintf(why, whysize, "its %s section holds %llu bytes, not the %llu this ScarletNX's holds", tag,
                 (unsigned long long)sr_left(r), (unsigned long long)n);
        return -1;
    }
    if (sr_get(r, p, n)) {
        snprintf(why, whysize, "its %s section is damaged: its data or its checksum doesn't read back", tag);
        return -1;
    }
    return 0;
}

int snx_agb_state_load(AgbBoard* board, AgbCpu* cpu, const char* path, char* why, size_t whysize) {
    uint64_t t0 = plat_ns();
    SnxStateHeader h;
    StateReader* r = sr_open(path, &h, why, whysize);
    if (!r) return -1;
    AgbBoard* b = (AgbBoard*)malloc(sizeof *b);
    uint8_t* memory = (uint8_t*)malloc(MEMORY_BYTES);
    AgbCpu c;
    int refused = 1;
    if (!b || !memory)
        snprintf(why, whysize, "out of memory for the state's board and memory (%zu bytes)",
                 sizeof *b + (size_t)MEMORY_BYTES);
    else if (h.title_id != agb_board_hash(board))
        snprintf(why, whysize, "it is another game's: its ROM's hash is %016llX, this game's %016llX",
                 (unsigned long long)h.title_id, (unsigned long long)agb_board_hash(board));
    else if (h.abi != SNX_AGB_STATE_VERSION)
        snprintf(why, whysize, "it is a GBA state of version %u, and this ScarletNX reads version %u", (unsigned)h.abi,
                 (unsigned)SNX_AGB_STATE_VERSION);
    else if (!get_section(r, "BOARD", board_schema(board), b, sizeof *b, why, whysize) &&
             !get_section(r, "CPU", cpu_schema(cpu), &c, sizeof c, why, whysize) &&
             !get_section(r, "MEMORY", memory_schema(), memory, MEMORY_BYTES, why, whysize))
        refused = 0;
    sr_close(r);
    if (refused) {
        free(b);
        free(memory);
        return -1;
    }
    /* everything read and checked: the state takes the running game's place, the process's own parts kept */
    b->cartridge = board->cartridge;
    b->memory = board->memory;
    b->ppu.lcd = board->ppu.lcd;
    b->save.type = board->save.type;
    b->save.dirty = board->save.dirty;
    memcpy(board, b, sizeof *board);
    c.board = board;
    c.arm.mem = cpu->arm.mem;
    c.arm.io = cpu->arm.io;
    c.arm.rt = cpu->arm.rt;
    memcpy(cpu, &c, sizeof *cpu);
    uint8_t* at[REGIONS];
    regions(&board->memory, at);
    for (size_t i = 0, from = 0; i < REGIONS; from += region_size[i], ++i) memcpy(at[i], memory + from, region_size[i]);
    /* the cartridge's GPIO registers over its header in the flat window, as the state left the port: its view once a
       write reached it, else the image's bytes there */
    if (board->memory.rom) {
        uint8_t view[sizeof board->gpio.view];
        for (uint32_t i = 0; i < sizeof view; ++i) {
            uint32_t offset = AGB_GPIO_DATA + i;
            view[i] = board->gpio.touched              ? board->gpio.view[i]
                      : offset < board->cartridge.size ? board->cartridge.data[offset]
                                                       : agb_rom_open_bus8(offset);
        }
        for (uint32_t window = 0; window < board->memory.rom_windows; ++window)
            memcpy(board->memory.rom + window * AGB_ROM_MAX + AGB_GPIO_DATA, view, sizeof view);
    }
    free(b);
    free(memory);
    printf("Loaded the state %s in %.1f ms\n", path, (double)(plat_ns() - t0) / 1e6);
    return 0;
}
