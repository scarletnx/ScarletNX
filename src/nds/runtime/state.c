/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A DS game's save state, in sections each with the schema hash of what it holds:

     BOARD   the NdsBoard as it is, but for what the running process keeps: the memory's window and objects, the card
             image, the devices' pointers, the card's save memory (the game's save file) and the board's notes; and
             but for the lines' registers, which each visible line records again before the frame is composed
     CPU     the NdsCpu as it is, but for its pointers (the board, the block cache, the window, the bus hook, the
             runtime's calls)
     MEMORY  main memory, the shared work RAM, the ITCM, the DTCM, the palettes, the OAM and the nine VRAM banks
     GPU3D, SPU, PXI, SOUND  the 3D engine, the sound hardware, the ARM7's side and its sound driver, each its own
             (gpu3d.c, spu.c, pxi.c, sound.c)
     CAPTURE the display capture the GPU made and the bank has not taken yet (gpu/vk/frame.c): its bank, offset and
             byte count, then its pixels; a count of 0 for none

   The structs go as their bytes, as the Switch's g_hle does: a state whose types had other fields has another schema,
   and is refused. A load reads and checks every section before it changes anything, then maps the VRAM banks and the
   DTCM where the state's registers have them. The compiled blocks are not state: each dispatch compares the code memory
   holds (cpu/recomp.c) */
#include "nds/runtime/state.h"
#include "nds/audio/spu.h"
#include "nds/gpu/gpu3d.h"
#include "nds/gpu/vk/vk.h"
#include "nds/hle/pxi.h"
#include "common/platform/time.h"
#include "common/utilities/bits.h"
#include "common/utilities/build_info.h"
#include "common/utilities/common.h"
#include "common/utilities/state_header.h"
#include "common/utilities/state_file.h"
#include "common/utilities/state_io.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* the console's palettes' and OAM's bytes: the objects behind them are a host page */
#define PALETTE_SIZE 0x800u
#define OAM_SIZE     0x800u
#define REGIONS      (6 + NDS_VRAM_BANKS)

/* the MEMORY section's regions, in their order: where the board has them and their sizes */
static void regions(const NdsMemory* m, unsigned char* at[REGIONS], uint32_t size[REGIONS]) {
    const SnxHostMemory* fixed[6] = {&m->main, &m->wram, &m->itcm, &m->dtcm, &m->palette, &m->oam};
    const uint32_t sizes[6] = {NDS_MAIN_RAM_SIZE, NDS_WRAM_SIZE, NDS_ITCM_SIZE, NDS_DTCM_SIZE, PALETTE_SIZE, OAM_SIZE};
    for (unsigned i = 0; i < 6; ++i) at[i] = fixed[i]->view, size[i] = sizes[i];
    for (unsigned bank = 0; bank < NDS_VRAM_BANKS; ++bank)
        at[6 + bank] = m->vram[bank].view, size[6 + bank] = nds_vram_bank_size[bank];
}
static uint64_t memory_bytes(void) {
    uint64_t n = NDS_MAIN_RAM_SIZE + NDS_WRAM_SIZE + NDS_ITCM_SIZE + NDS_DTCM_SIZE + PALETTE_SIZE + OAM_SIZE;
    for (unsigned bank = 0; bank < NDS_VRAM_BANKS; ++bank) n += nds_vram_bank_size[bank];
    return n;
}

/* the schema hashes: the walk of each struct's fields and their types, and its size. The walk names an array's type
   and length but not its elements' fields: the structs the board holds arrays of are walked too */
static uint64_t board_schema(const NdsBoard* board) {
    uint64_t h = SNX_FNV_BASIS ^ sizeof *board;
    __builtin_dump_struct(board, snx_state_schema_out, &h);
    __builtin_dump_struct(&board->dma[0], snx_state_schema_out, &h);
    __builtin_dump_struct(&board->timer[0], snx_state_schema_out, &h);
    __builtin_dump_struct(&board->lines[0][0], snx_state_schema_out, &h);
    return h;
}
static uint64_t cpu_schema(const NdsCpu* cpu) {
    uint64_t h = SNX_FNV_BASIS ^ sizeof *cpu;
    __builtin_dump_struct(cpu, snx_state_schema_out, &h);
    return h;
}
static uint64_t memory_schema(void) {
    uint64_t n = memory_bytes();
    return snx_fnv1a(SNX_FNV_BASIS, &n, sizeof n);
}

/* a capture: the bank, the offset in it and the bytes, which fit the frame */
#define CAPTURE_MOST (NDS_SCREEN_WIDTH * NDS_SCREEN_HEIGHT * 2u)
typedef struct {
    uint32_t bank, offset, bytes;
    unsigned char pixels[CAPTURE_MOST];
} Capture;

/* the board and the CPU as a state holds them: what the running process keeps zeroed, so it is no part of the file */
static void board_strip(NdsBoard* b) {
    memset(&b->mem, 0, sizeof b->mem);
    b->rom = NULL;
    b->pxi = NULL;
    b->gpu3d = NULL;
    b->sound = NULL;
    b->spu = NULL;
    b->save = NULL;
    b->save_size = 0;
    b->save_dirty = 0;
    memset(b->told, 0, sizeof b->told);
    b->trace = 0;
    memset(b->lines, 0, sizeof b->lines);
}
static void cpu_strip(NdsCpu* c) {
    c->arm.mem = NULL;
    c->arm.io = NULL;
    c->arm.rt = NULL;
    c->board = NULL;
    c->recomp = NULL;
    memset(&c->rt, 0, sizeof c->rt);
}

/* the play time the state holds, from the board's cycles, without the product's overflow */
static uint64_t guest_ns(uint64_t cycles) {
    return cycles / NDS_CLOCK * 1000000000ull + cycles % NDS_CLOCK * 1000000000ull / NDS_CLOCK;
}

/* the state file's streams as the devices write and read theirs */
static int writer_put(void* w, const void* p, u64 n) { return sw_put((StateWriter*)w, p, n); }
static int reader_get(void* r, void* p, u64 n) { return sr_get((StateReader*)r, p, n); }

static int put_section(StateWriter* w, const char* tag, uint64_t schema, const void* p, uint64_t n) {
    return sw_begin(w, tag, schema) || sw_put(w, p, n) || sw_end(w) ? -1 : 0;
}

int snx_nds_state_save(NdsBoard* board, NdsCpu* cpu, const char* path, const uint8_t* thumb, char* why,
                       size_t whysize) {
    uint64_t t0 = plat_ns();
    why[0] = 0;
    SnxStateHeader h = {0};
    h.format = SNX_STATE_FORMAT;
    h.abi = SNX_NDS_STATE_VERSION;
    h.title_id = board->rom->identity;
    h.created = (uint64_t)time(NULL);
    h.guest_ns = guest_ns(board->cycles);
    h.thumb_w = thumb ? SNX_STATE_THUMB_W : 0;
    h.thumb_h = thumb ? SNX_STATE_THUMB_H : 0;
    snprintf(h.runtime, sizeof h.runtime, "%s", snx_build_info());
    snprintf(h.game, sizeof h.game, "%s", board->rom->title);
    char* folder = snx_dirname(path);
    if (folder && folder[0] && snx_mkdirs(folder)) {
        char os[160];
        snprintf(why, whysize, "its folder %s can't be made: %s", folder, snx_os_error(os, sizeof os));
        free(folder);
        return -1;
    }
    free(folder);
    NdsBoard* b = (NdsBoard*)malloc(sizeof *b);
    NdsCpu* c = (NdsCpu*)malloc(sizeof *c);
    StateWriter* w = b && c ? sw_open(path, &h, thumb, why, whysize) : NULL;
    int r = -1;
    if (!b || !c) snprintf(why, whysize, "out of memory for a copy of the board (%zu bytes)", sizeof *b + sizeof *c);
    if (w) {
        memcpy(b, board, sizeof *b); /* its padding too, which goes in the file */
        board_strip(b);
        memcpy(c, cpu, sizeof *c);
        cpu_strip(c);
        StateIo io = {w, writer_put, NULL};
        unsigned char* at[REGIONS];
        uint32_t size[REGIONS];
        regions(&board->mem, at, size);
        int failed = put_section(w, "BOARD", board_schema(board), b, sizeof *b) ||
                     put_section(w, "CPU", cpu_schema(cpu), c, sizeof *c) || sw_begin(w, "MEMORY", memory_schema());
        for (unsigned i = 0; !failed && i < REGIONS; ++i) failed = sw_put(w, at[i], size[i]);
        failed = failed || sw_end(w);
        if (!failed && (sw_begin(w, "GPU3D", nds_gpu3d_state_schema(board)) ||
                        nds_gpu3d_state_save(board, &io, why, whysize) || sw_end(w)))
            failed = 1;
        failed = failed || sw_begin(w, "SPU", nds_spu_state_schema(board)) || nds_spu_state_save(board, &io) ||
                 sw_end(w) || sw_begin(w, "PXI", nds_pxi_state_schema(board)) || nds_pxi_state_save(board, &io) ||
                 sw_end(w) || sw_begin(w, "SOUND", nds_sound_state_schema()) || nds_sound_state_save(&io) || sw_end(w);
        unsigned bank = 0;
        uint32_t offset = 0;
        const unsigned char* pixels = NULL;
        uint32_t bytes = failed ? 0 : nds_vk_capture_pending(&bank, &offset, &pixels);
        failed = failed || sw_begin(w, "CAPTURE", 1) || io_put32(&io, bank) || io_put32(&io, offset) ||
                 io_put32(&io, bytes) || (bytes && sw_put(w, pixels, bytes)) || sw_end(w);
        if (failed) {
            sw_abort(w);
            if (!why[0]) snprintf(why, whysize, "the file couldn't be written");
        } else
            r = sw_finish(w, why, whysize);
    }
    free(b);
    free(c);
    if (!r) printf("Saved the state %s in %.1f ms\n", path, (double)(plat_ns() - t0) / 1e6);
    return r;
}

/* the section tagged tag begun, with the schema this runtime writes: 0, or -1 with why */
static int begin(StateReader* r, const char* tag, uint64_t schema, char* why, size_t whysize) {
    uint64_t had = 0;
    if (sr_begin(r, tag, &had)) {
        snprintf(why, whysize, "it has no %s section", tag);
        return -1;
    }
    if (had != schema) {
        snprintf(why, whysize, "it holds the DS's %s in another layout (schema %016llX, this ScarletNX's %016llX)", tag,
                 (unsigned long long)had, (unsigned long long)schema);
        return -1;
    }
    return 0;
}
/* the section begun read whole into p, n bytes: 0, or -1 with why */
static int get_whole(StateReader* r, const char* tag, void* p, uint64_t n, char* why, size_t whysize) {
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
/* a device's section parsed: what it made, or NULL with why, also when bytes are left over */
static void* parsed(StateReader* r, const char* tag, void* staged, char* why, size_t whysize) {
    if (staged && sr_left(r)) {
        snprintf(why, whysize, "its %s section holds %llu bytes past what this ScarletNX reads of it", tag,
                 (unsigned long long)sr_left(r));
        return NULL;
    }
    return staged;
}

int snx_nds_state_load(NdsBoard* board, NdsCpu* cpu, const char* path, char* why, size_t whysize) {
    uint64_t t0 = plat_ns();
    SnxStateHeader h;
    why[0] = 0;
    StateReader* r = sr_open(path, &h, why, whysize);
    if (!r) return -1;
    StateIo io = {r, NULL, reader_get};
    NdsBoard* b = (NdsBoard*)malloc(sizeof *b);
    NdsCpu* c = (NdsCpu*)malloc(sizeof *c);
    unsigned char* memory = (unsigned char*)malloc(memory_bytes());
    void *gpu3d = NULL, *spu = NULL, *pxi = NULL, *sound = NULL;
    Capture* capture = (Capture*)malloc(sizeof *capture);
    int refused = 1;
    if (!b || !c || !memory || !capture)
        snprintf(why, whysize, "out of memory for the state's board and memory (%zu bytes)",
                 sizeof *b + sizeof *c + (size_t)memory_bytes());
    else if (h.title_id != board->rom->identity)
        snprintf(why, whysize, "it is another game's: its card's identity is %016llX, this game's %016llX",
                 (unsigned long long)h.title_id, (unsigned long long)board->rom->identity);
    else if (h.abi != SNX_NDS_STATE_VERSION)
        snprintf(why, whysize, "it is a DS state of version %u, and this ScarletNX reads version %u", (unsigned)h.abi,
                 (unsigned)SNX_NDS_STATE_VERSION);
    else if (!begin(r, "BOARD", board_schema(board), why, whysize) &&
             !get_whole(r, "BOARD", b, sizeof *b, why, whysize) && !begin(r, "CPU", cpu_schema(cpu), why, whysize) &&
             !get_whole(r, "CPU", c, sizeof *c, why, whysize) && !begin(r, "MEMORY", memory_schema(), why, whysize) &&
             !get_whole(r, "MEMORY", memory, memory_bytes(), why, whysize) &&
             !begin(r, "GPU3D", nds_gpu3d_state_schema(board), why, whysize) &&
             (gpu3d = parsed(r, "GPU3D", nds_gpu3d_state_parse(&io, why, whysize), why, whysize)) &&
             !begin(r, "SPU", nds_spu_state_schema(board), why, whysize) &&
             (spu = parsed(r, "SPU", nds_spu_state_parse(&io, why, whysize), why, whysize)) &&
             !begin(r, "PXI", nds_pxi_state_schema(board), why, whysize) &&
             (pxi = parsed(r, "PXI", nds_pxi_state_parse(&io, why, whysize), why, whysize)) &&
             !begin(r, "SOUND", nds_sound_state_schema(), why, whysize) &&
             (sound = parsed(r, "SOUND", nds_sound_state_parse(&io, why, whysize), why, whysize)) &&
             !begin(r, "CAPTURE", 1, why, whysize)) {
        if (io_get32(&io, &capture->bank) || io_get32(&io, &capture->offset) || io_get32(&io, &capture->bytes) ||
            capture->bytes != sr_left(r))
            snprintf(why, whysize, "its CAPTURE section is damaged or cut short");
        else if (capture->bank >= 4 || capture->offset >= 0x20000 || capture->bytes > CAPTURE_MOST)
            snprintf(why, whysize,
                     "its display capture is %u bytes at 0x%X of bank %u, past the %u at most in banks A "
                     "to D",
                     capture->bytes, capture->offset, capture->bank, CAPTURE_MOST);
        else if (capture->bytes && sr_get(r, capture->pixels, capture->bytes))
            snprintf(why, whysize, "its CAPTURE section is damaged: its data or its checksum doesn't read back");
        else if (b->ipc_recv_at >= 64 || b->ipc_recv_count > 64)
            snprintf(why, whysize, "its board holds %u FIFO words from %u, past the 64 it has", b->ipc_recv_count,
                     b->ipc_recv_at);
        else
            refused = 0;
    }
    sr_close(r);
    if (refused) {
        nds_gpu3d_state_drop(gpu3d);
        free(spu);
        free(pxi);
        free(sound);
        free(capture);
        free(b);
        free(c);
        free(memory);
        if (!why[0]) snprintf(why, whysize, "it couldn't be read");
        return -1;
    }
    /* everything read and checked: the state takes the running game's place, the process's own parts kept */
    b->mem = board->mem;
    b->rom = board->rom;
    b->pxi = board->pxi;
    b->gpu3d = board->gpu3d;
    b->sound = board->sound;
    b->spu = board->spu;
    b->save = board->save;
    b->save_size = board->save_size;
    b->save_dirty = board->save_dirty;
    memcpy(b->told, board->told, sizeof b->told);
    b->trace = board->trace;
    memcpy(b->lines, board->lines, sizeof b->lines);
    memcpy(board, b, sizeof *board);
    c->arm.mem = cpu->arm.mem;
    c->arm.io = cpu->arm.io;
    c->board = board;
    c->recomp = cpu->recomp;
    c->rt = cpu->rt;
    memcpy(cpu, c, sizeof *cpu);
    cpu->arm.rt = &cpu->rt;
    unsigned char* at[REGIONS];
    uint32_t size[REGIONS];
    regions(&board->mem, at, size);
    for (unsigned i = 0, from = 0; i < REGIONS; from += size[i], ++i) memcpy(at[i], memory + from, size[i]);
    nds_vram_remap(board);
    nds_dtcm_remap(board);
    nds_gpu3d_state_install(board, gpu3d);
    nds_spu_state_install(board, spu);
    nds_pxi_state_install(board, pxi);
    nds_sound_state_install(board, sound);
    /* the state's capture waits for the frame as the GPU's would; with no device to hold it, it lands now */
    if (!nds_vk_capture_pend(capture->bank, capture->offset, capture->pixels, capture->bytes)) {
        unsigned char* into = board->mem.vram[capture->bank].view;
        uint32_t first = capture->bytes < 0x20000 - capture->offset ? capture->bytes : 0x20000 - capture->offset;
        memcpy(into + capture->offset, capture->pixels, first);
        memcpy(into, capture->pixels + first, capture->bytes - first);
    }
    free(capture);
    free(b);
    free(c);
    free(memory);
    printf("Loaded the state %s in %.1f ms\n", path, (double)(plat_ns() - t0) / 1e6);
    return 0;
}
