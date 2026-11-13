/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The flat window the compiled blocks run on (as the GBA's, agb/cpu/memmap.c): each of the console's memories is a
   host memory object mapped at its address and its mirrors, so a block reaches memory with one pointer add. The VRAM
   banks move as the game writes VRAMCNT, and the DTCM as it writes CP15: their pages are mapped again where they go.
   Where the banks go is GBATEK's table, as melonDS maps them (GPU.cpp's MapVRAM_*). */
#include "nds/board/nds.h"
#include "common/utilities/common.h"
#include "common/utilities/endian.h"
#include <stdio.h>
#include <string.h>

const uint32_t nds_vram_bank_size[NDS_VRAM_BANKS] = {0x20000, 0x20000, 0x20000, 0x20000, 0x10000,
                                                     0x4000,  0x4000,  0x8000,  0x4000};
const uint32_t nds_space_size[NDS_SPACE_COUNT] = {0x80000, 0x20000, 0x40000, 0x20000, 0xA4000, 0x8000,
                                                  0x8000,  0x2000,  0x2000,  0x80000, 0x18000};
/* a space's page: 16 KiB, an extended palette's slot 8 KiB */
static uint32_t space_page(unsigned space) {
    return space >= NDS_SPACE_BG_EXTPAL_A && space <= NDS_SPACE_OBJ_EXTPAL_B ? 0x2000u : 0x4000u;
}
/* where the ARM9 has the first five spaces */
static const uint32_t space_address[5] = {0x06000000, 0x06200000, 0x06400000, 0x06600000, 0x06800000};
static const uint32_t lcdc_offset[NDS_VRAM_BANKS] = {0x00000, 0x20000, 0x40000, 0x60000, 0x80000,
                                                     0x90000, 0x94000, 0x98000, 0xA0000};

/* [at, at + size) of the window as memory's bytes from offset on, in place of what it showed */
static bool place(NdsBoard* board, uint64_t at, uint64_t size, const SnxHostMemory* memory, uint64_t offset) {
    (void)snx_host_unmap(board->mem.base, at, size);
    return snx_host_map(memory, board->mem.base, at, offset, size, SNX_HOST_READ | SNX_HOST_WRITE);
}

/* a bank's bytes from offset on at a space's pages from page on */
static void bank_at(NdsBoard* board, unsigned space, unsigned page, unsigned bank, uint32_t offset, uint32_t size) {
    uint32_t unit = space_page(space), pages = nds_space_size[space] / unit;
    for (uint32_t done = 0; done < size && page < pages; done += unit, ++page)
        board->mem.space[space][page] = (uint32_t)bank << 24 | (offset + done);
}

void nds_vram_remap(NdsBoard* board) {
    NdsMemory* m = &board->mem;
    memset(m->space, 0xFF, sizeof m->space);
    for (unsigned bank = 0; bank < NDS_VRAM_BANKS; ++bank) {
        unsigned control = board->vramcnt[bank], mode = control & 7, offset = control >> 3 & 3;
        uint32_t size = nds_vram_bank_size[bank];
        if (!(control & 0x80)) continue;
        if (bank != 2 && bank != 3) mode &= bank < 2 || bank >= 7 ? 3 : 7; /* A, B, H and I have two mode bits */
        if (!mode) {
            bank_at(board, NDS_SPACE_LCDC, lcdc_offset[bank] / 0x4000, bank, 0, size);
            continue;
        }
        switch (bank) {
            case 0:
            case 1:
            case 2:
            case 3:
                if (mode == 1)
                    bank_at(board, NDS_SPACE_BG_A, offset * 8, bank, 0, size);
                else if (mode == 2 && bank < 2)
                    bank_at(board, NDS_SPACE_OBJ_A, (offset & 1) * 8, bank, 0, size);
                else if (mode == 3)
                    bank_at(board, NDS_SPACE_TEXTURE, offset * 8, bank, 0, size);
                else if (mode == 4)
                    bank_at(board, bank == 2 ? NDS_SPACE_BG_B : NDS_SPACE_OBJ_B, 0, bank, 0, size);
                break; /* C's and D's mode 2 is the ARM7's memory */
            case 4:
                if (mode == 1)
                    bank_at(board, NDS_SPACE_BG_A, 0, bank, 0, size);
                else if (mode == 2)
                    bank_at(board, NDS_SPACE_OBJ_A, 0, bank, 0, size);
                else if (mode == 3)
                    bank_at(board, NDS_SPACE_TEXPAL, 0, bank, 0, size);
                else if (mode == 4)
                    bank_at(board, NDS_SPACE_BG_EXTPAL_A, 0, bank, 0, 0x8000);
                break;
            case 5:
            case 6: {
                unsigned page = (offset & 1) + (offset & 2) * 2; /* 0, 1, 4, 5: and again 32 KiB on */
                if (mode == 1 || mode == 2) {
                    unsigned space = mode == 1 ? NDS_SPACE_BG_A : NDS_SPACE_OBJ_A;
                    bank_at(board, space, page, bank, 0, size);
                    bank_at(board, space, page + 2, bank, 0, size);
                } else if (mode == 3)
                    bank_at(board, NDS_SPACE_TEXPAL, page, bank, 0, size);
                else if (mode == 4)
                    bank_at(board, NDS_SPACE_BG_EXTPAL_A, (offset & 1) * 2, bank, 0, size);
                else if (mode == 5)
                    bank_at(board, NDS_SPACE_OBJ_EXTPAL_A, 0, bank, 0, 0x2000);
                break;
            }
            case 7:
                if (mode == 1) {
                    bank_at(board, NDS_SPACE_BG_B, 0, bank, 0, size);
                    bank_at(board, NDS_SPACE_BG_B, 4, bank, 0, size);
                } else if (mode == 2)
                    bank_at(board, NDS_SPACE_BG_EXTPAL_B, 0, bank, 0, size);
                break;
            default:
                if (mode == 1) {
                    for (unsigned page = 2; page < 8; page += 4) {
                        bank_at(board, NDS_SPACE_BG_B, page, bank, 0, size);
                        bank_at(board, NDS_SPACE_BG_B, page + 1, bank, 0, size);
                    }
                } else if (mode == 2) {
                    for (unsigned page = 0; page < 8; ++page) bank_at(board, NDS_SPACE_OBJ_B, page, bank, 0, size);
                } else if (mode == 3)
                    bank_at(board, NDS_SPACE_OBJ_EXTPAL_B, 0, bank, 0, 0x2000);
                break;
        }
    }
    /* the window: the pages whose bank changed */
    for (unsigned space = 0; space < 5; ++space) {
        uint32_t pages = nds_space_size[space] / 0x4000;
        for (uint32_t page = 0; page < pages; ++page) {
            uint32_t now = m->space[space][page];
            if (now == m->seen[space][page] && m->seen[space][page] != 0xFFFFFFFEu) continue;
            uint64_t at = (uint64_t)space_address[space] + page * 0x4000u;
            if (now == NDS_VRAM_NONE)
                (void)place(board, at, 0x4000, &m->scratch, 0);
            else
                (void)place(board, at, 0x4000, &m->vram[now >> 24], now & 0xFFFFFF);
            m->seen[space][page] = now;
        }
    }
}

void nds_vram_copy(const NdsBoard* board, NdsVramSpace space, unsigned char* out) {
    uint32_t unit = space_page(space), pages = nds_space_size[space] / unit;
    for (uint32_t page = 0; page < pages; ++page) {
        uint32_t from = board->mem.space[space][page];
        if (from == NDS_VRAM_NONE)
            memset(out + page * unit, 0, unit);
        else
            memcpy(out + page * unit, board->mem.vram[from >> 24].view + (from & 0xFFFFFF), unit);
    }
}

/* what the window shows at [at, at + size) with no DTCM there: main memory's mirrors, the shared work RAM's, the
   ITCM's. False for a place that is none of them */
static bool place_default(NdsBoard* board, uint32_t at, uint32_t size) {
    NdsMemory* m = &board->mem;
    for (uint32_t done = 0; done < size; done += 0x4000) {
        uint32_t address = at + done;
        if (address < 0x02000000u) {
            if (!place(board, address, 0x4000, &m->itcm, address & (NDS_ITCM_SIZE - 1))) return false;
        } else if (address < 0x03000000u) {
            if (!place(board, address, 0x4000, &m->main, address & (NDS_MAIN_RAM_SIZE - 1))) return false;
        } else if (address < 0x04000000u) {
            if (!place(board, address, 0x4000, &m->wram, address & (NDS_WRAM_SIZE - 1))) return false;
        } else
            return false;
    }
    return true;
}

void nds_dtcm_remap(NdsBoard* board) {
    NdsMemory* m = &board->mem;
    uint32_t base = board->cp15_dtcm & 0xFFFFF000u, size = 0x200u << (board->cp15_dtcm >> 1 & 0x1F);
    if (!(board->cp15_control & (1u << 16)) || size < 0x1000) size = 0;
    /* the 16 KiB again and again through its range, as far as 16 MiB of it */
    if (size > 0x1000000u) size = 0x1000000u;
    if (size && size < NDS_DTCM_SIZE) size = NDS_DTCM_SIZE;
    base &= ~(NDS_DTCM_SIZE - 1);
    if (base == m->dtcm_base && size == m->dtcm_size) return;
    if (m->dtcm_size) (void)place_default(board, m->dtcm_base, m->dtcm_size);
    m->dtcm_base = base, m->dtcm_size = 0;
    if (!size) return;
    if (base >= 0x04000000u || (uint64_t)base + size > 0x04000000u) {
        if (nds_once(board, 1))
            fprintf(stderr, "scarletnx-nds: the game puts the DTCM at 0x%08X, where the window cannot have it\n", base);
        return;
    }
    for (uint32_t done = 0; done < size; done += NDS_DTCM_SIZE)
        if (!place(board, base + done, NDS_DTCM_SIZE, &m->dtcm, 0)) return;
    m->dtcm_size = size;
}

bool nds_memory_init(NdsBoard* board, char* error, size_t error_size) {
    NdsMemory* m = &board->mem;
    char why[160];
    m->page = snx_host_page();
    if (!m->page || m->page > 0x4000) {
        snprintf(error, error_size, "the host's pages are %zu bytes: the NDS's memory needs views of 16 KiB", m->page);
        return false;
    }
    size_t small = m->page > 0x1000 ? m->page : 0x1000;
    struct {
        SnxHostMemory* memory;
        uint64_t size;
    } objects[] = {{&m->main, NDS_MAIN_RAM_SIZE}, {&m->wram, NDS_WRAM_SIZE}, {&m->itcm, NDS_ITCM_SIZE},
                   {&m->dtcm, NDS_DTCM_SIZE},     {&m->palette, small},      {&m->oam, small},
                   {&m->scratch, 0x4000}};
    for (size_t i = 0; i < sizeof objects / sizeof *objects; ++i)
        if (!snx_host_memory_new(objects[i].memory, objects[i].size)) goto host;
    for (unsigned bank = 0; bank < NDS_VRAM_BANKS; ++bank)
        if (!snx_host_memory_new(&m->vram[bank], nds_vram_bank_size[bank])) goto host;
    m->base = snx_host_reserve(NDS_MAP_SIZE + NDS_MAP_GUARD);
    if (!m->base) {
        snprintf(error, error_size, "cannot reserve the guest memory window, %llu bytes of address space: %s",
                 (unsigned long long)(NDS_MAP_SIZE + NDS_MAP_GUARD), snx_os_error(why, sizeof why));
        return false;
    }
    bool ok = true;
    for (uint64_t at = 0; ok && at < 0x02000000u; at += NDS_ITCM_SIZE)
        ok = snx_host_map(&m->itcm, m->base, at, 0, NDS_ITCM_SIZE, SNX_HOST_READ | SNX_HOST_WRITE);
    for (uint64_t at = 0x02000000u; ok && at < 0x03000000u; at += NDS_MAIN_RAM_SIZE)
        ok = snx_host_map(&m->main, m->base, at, 0, NDS_MAIN_RAM_SIZE, SNX_HOST_READ | SNX_HOST_WRITE);
    for (uint64_t at = 0x03000000u; ok && at < 0x04000000u; at += NDS_WRAM_SIZE)
        ok = snx_host_map(&m->wram, m->base, at, 0, NDS_WRAM_SIZE, SNX_HOST_READ | SNX_HOST_WRITE);
    ok = ok && snx_host_map(&m->palette, m->base, 0x05000000u, 0, small, SNX_HOST_READ | SNX_HOST_WRITE) &&
         snx_host_map(&m->oam, m->base, 0x07000000u, 0, small, SNX_HOST_READ | SNX_HOST_WRITE);
    /* the rest is memory of zeros: the registers' window, which no block reads (the bus hook has it), the mirrors of
       the palettes, the OAM and the VRAM spaces, the cartridge slot with nothing in it, and the BIOS */
    struct {
        uint64_t at, end;
    } gaps[] = {{0x04000000u, 0x05000000u},
                {0x05000000u + small, 0x06000000u},
                {0x06080000u, 0x06200000u},
                {0x06220000u, 0x06400000u},
                {0x06440000u, 0x06600000u},
                {0x06620000u, 0x06800000u},
                {0x068A4000u, 0x07000000u},
                {0x07000000u + small, NDS_MAP_SIZE + NDS_MAP_GUARD}};
    for (size_t i = 0; ok && i < sizeof gaps / sizeof *gaps; ++i)
        ok = snx_host_commit(m->base, gaps[i].at, gaps[i].end - gaps[i].at);
    if (!ok) {
        snprintf(error, error_size, "cannot map the guest's memory into its window: %s", snx_os_error(why, sizeof why));
        return false;
    }
    memset(m->seen, 0xFE, sizeof m->seen); /* nothing shown yet: every page is placed */
    nds_vram_remap(board);
    return true;
host:
    snprintf(error, error_size, "cannot make the guest's memory: %s", snx_os_error(why, sizeof why));
    return false;
}

void nds_memory_free(NdsBoard* board) {
    NdsMemory* m = &board->mem;
    if (m->base) snx_host_release(m->base, NDS_MAP_SIZE + NDS_MAP_GUARD);
    SnxHostMemory* objects[] = {&m->main, &m->wram, &m->itcm, &m->dtcm, &m->palette, &m->oam, &m->scratch};
    for (size_t i = 0; i < sizeof objects / sizeof *objects; ++i)
        if (objects[i]->view) snx_host_memory_free(objects[i]);
    for (unsigned bank = 0; bank < NDS_VRAM_BANKS; ++bank)
        if (m->vram[bank].view) snx_host_memory_free(&m->vram[bank]);
    *m = (NdsMemory){0};
}

static inline bool is_io(uint32_t address) { return address >> 24 == 0x04; }
uint8_t nds_read8(NdsBoard* board, uint32_t address) {
    return is_io(address) ? (uint8_t)nds_io_read(board, address, 1) : board->mem.base[address];
}
uint16_t nds_read16(NdsBoard* board, uint32_t address) {
    address &= ~1u;
    return is_io(address) ? (uint16_t)nds_io_read(board, address, 2) : snx_ld_le16(board->mem.base + address);
}
uint32_t nds_read32(NdsBoard* board, uint32_t address) {
    address &= ~3u;
    return is_io(address) ? nds_io_read(board, address, 4) : snx_ld_le32(board->mem.base + address);
}
void nds_write8(NdsBoard* board, uint32_t address, uint8_t value) {
    if (is_io(address))
        nds_io_write(board, address, 1, value);
    else
        board->mem.base[address] = value;
}
void nds_write16(NdsBoard* board, uint32_t address, uint16_t value) {
    address &= ~1u;
    if (is_io(address))
        nds_io_write(board, address, 2, value);
    else
        snx_st_le16(board->mem.base + address, value);
}
void nds_write32(NdsBoard* board, uint32_t address, uint32_t value) {
    address &= ~3u;
    if (is_io(address))
        nds_io_write(board, address, 4, value);
    else
        snx_st_le32(board->mem.base + address, value);
}
