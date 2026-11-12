/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The whole machine: a hand assembled ROM goes through the AOT recompiler and runs on the board, through
   a BIOS call and an interrupt, and the board's memory shows what happened. */
#include "agb/bios/bios.h"
#include "agb/board/board.h"
#include "agb/cpu/agb_cpu.h"
#include "agb/cpu/memmap.h"
#include "agb/cpu/recomp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* the most blocks a lookup compares with its pc through the index: the slot its pc hashes to and the few after it,
   with at most half of the slots taken */
#define AGB_LOOKUP_COMPARES 4
#define CHECK(expression)                                                                                              \
    do {                                                                                                               \
        if (!(expression)) {                                                                                           \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expression);                                           \
            return 1;                                                                                                  \
        }                                                                                                              \
    } while (0)

static void put32(uint8_t* p, uint32_t value) {
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}
static void put16(uint8_t* p, uint16_t value) {
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

/* The ROM: an ARM stub that swaps a word in through SWP and sets the IRQ mode's stack through MSR, which
   the recompiler refuses and the machine interprets, then branches into Thumb code, which enables the vblank interrupt, installs a handler at
   0x03007FFC, waits for a vblank through the HLE BIOS and writes two markers on its way. The handler
   acknowledges the interrupt flag and writes a marker of its own. */
static uint8_t* make_rom(size_t* size) {
    uint8_t* rom = (uint8_t*)calloc(1, 0x200);
    *size = 0x200;
    put32(rom + 0x00, 0xEA00002E); /* B 0xC0, the header's entry branch */
    put32(rom + 0xC0, 0xEA00001E); /* b 0x140 */
    rom[0xB2] = 0x96;
    {
        unsigned sum = 0;
        for (int i = 0xA0; i <= 0xBC; ++i) sum += rom[i];
        rom[0xBD] = (uint8_t)(0u - sum - 0x19u);
    }
    /* ARM at 0x140: 9 at marker 3, swapped for 5 with the 9 kept at marker 4; the IRQ mode's stack set and
       system mode's, back in it, at marker 5; then bx to 0x080000CC in Thumb. The block that traps there calls the
       runtime library (snx_arm_undefined), which the module must link */
    put32(rom + 0x140, 0xE3A02009); /* mov r2, #9 */
    put32(rom + 0x144, 0xE59F0024); /* ldr r0, [pc, #36]: marker 3 */
    put32(rom + 0x148, 0xE5802000); /* str r2, [r0] */
    put32(rom + 0x14C, 0xE3A01005); /* mov r1, #5 */
    put32(rom + 0x150, 0xE1002091); /* swp r2, r1, [r0] */
    put32(rom + 0x154, 0xE5802004); /* str r2, [r0, #4]: marker 4 */
    put32(rom + 0x158, 0xE321F0D2); /* msr cpsr_c, #0xD2: IRQ mode, interrupts off */
    put32(rom + 0x15C, 0xE59FD014); /* ldr sp, [pc, #20]: the IRQ stack */
    put32(rom + 0x160, 0xE321F01F); /* msr cpsr_c, #0x1F: system mode */
    put32(rom + 0x164, 0xE580D008); /* str sp, [r0, #8]: marker 5 */
    put32(rom + 0x168, 0xEA000004); /* b 0x180 */
    put32(rom + 0x16C, 0xE12FFF13); /* bx r3 */
    put32(rom + 0x170, 0x02000010); /* marker 3 */
    put32(rom + 0x174, 0x080000CD); /* the Thumb code */
    put32(rom + 0x178, 0x03007FA0);
    /* ARM at 0x180: an STMDB with an empty list, which the machine interprets: R15 goes where the first of 16
       registers would, 0x40 below the base, and the base moves down by 0x40 (marker 6) */
    put32(rom + 0x180, 0xE3A04402); /* mov r4, #0x02000000 */
    put32(rom + 0x184, 0xE2844C01); /* add r4, r4, #0x100 */
    put32(rom + 0x188, 0xE1A05004); /* mov r5, r4 */
    put32(rom + 0x18C, 0xE9250000); /* stmdb r5!, {} */
    put32(rom + 0x190, 0xE5845004); /* str r5, [r4, #4]: marker 6 */
    put32(rom + 0x194, 0xE59F3000); /* ldr r3, [pc, #0] */
    put32(rom + 0x198, 0xE12FFF13); /* bx r3 */
    put32(rom + 0x19C, 0x080000CD); /* the Thumb code */
    /* Thumb at 0xCC: the vblank interrupt on, the handler installed, one BIOS wait, then the markers */
    put16(rom + 0xCC, 0x4808); /* ldr r0, [pc, #32]: DISPSTAT */
    put16(rom + 0xCE, 0x2108); /* movs r1, #8 */
    put16(rom + 0xD0, 0x6001); /* str r1, [r0]: vblank interrupt on */
    put16(rom + 0xD2, 0x4808); /* ldr r0, [pc, #32]: IME */
    put16(rom + 0xD4, 0x2101); /* movs r1, #1 */
    put16(rom + 0xD6, 0x6001); /* str r1, [r0] */
    put16(rom + 0xD8, 0x4807); /* ldr r0, [pc, #28]: IE */
    put16(rom + 0xDA, 0x2101); /* movs r1, #1 */
    put16(rom + 0xDC, 0x6001); /* str r1, [r0] */
    put16(rom + 0xDE, 0x4807); /* ldr r0, [pc, #28]: 0x03007FFC */
    put16(rom + 0xE0, 0x4907); /* ldr r1, [pc, #28]: the handler */
    put16(rom + 0xE2, 0x6001); /* str r1, [r0] */
    put16(rom + 0xE4, 0xDF05); /* swi 0x05: VBlankIntrWait */
    put16(rom + 0xE6, 0x4807); /* ldr r0, [pc, #28]: marker 1 */
    put16(rom + 0xE8, 0x212A); /* movs r1, #42 */
    put16(rom + 0xEA, 0x6001); /* str r1, [r0] */
    put16(rom + 0xEC, 0xE7FE); /* b 0xEC: spin */
    put16(rom + 0xEE, 0x46C0); /* nop */
    put32(rom + 0xF0, 0x04000004);
    put32(rom + 0xF4, 0x04000208);
    put32(rom + 0xF8, 0x04000200);
    put32(rom + 0xFC, 0x03007FFC);
    put32(rom + 0x100, 0x08000121); /* the handler, Thumb */
    put32(rom + 0x104, 0x02000000); /* marker 1 */
    /* the interrupt handler at 0x120: it acknowledges the vblank and flags it for the BIOS's wait, as a game does */
    put16(rom + 0x120, 0x2101); /* movs r1, #1 */
    put16(rom + 0x122, 0x4804); /* ldr r0, [pc, #16]: IF */
    put16(rom + 0x124, 0x8001); /* strh r1, [r0]: acknowledge */
    put16(rom + 0x126, 0x4804); /* ldr r0, [pc, #16]: the BIOS's flags */
    put16(rom + 0x128, 0x8001); /* strh r1, [r0]: the vblank VBlankIntrWait waits for */
    put16(rom + 0x12A, 0x2107); /* movs r1, #7 */
    put16(rom + 0x12C, 0x4803); /* ldr r0, [pc, #12]: marker 2 */
    put16(rom + 0x12E, 0x6001); /* str r1, [r0] */
    put16(rom + 0x130, 0x4770); /* bx lr */
    put16(rom + 0x132, 0x46C0);
    put32(rom + 0x134, 0x04000202);
    put32(rom + 0x138, 0x03007FF8);
    put32(rom + 0x13C, 0x02000004); /* marker 2 */
    return rom;
}

/* a block run as the machine runs it, from its own address in its own state: its r0 */
static uint32_t run_block(AgbCpu* cpu, const AgbBlock* block) {
    cpu->arm.r[15] = block->pc;
    cpu->arm.cpsr = block->thumb ? cpu->arm.cpsr | SNX_ARM_T : cpu->arm.cpsr & ~(uint32_t)SNX_ARM_T;
    cpu->arm.exit = SNX_ARM_OK;
    block->fn(&cpu->arm);
    return cpu->arm.exit == SNX_ARM_OK ? cpu->arm.r[0] : 0xFFFFFFFFu;
}

int main(void) {
    AgbBoard board;
    AgbMap map;
    AgbCpu cpu;
    AgbRecomp recomp;
    char error[512] = {0};
    size_t size = 0;
    uint8_t* rom = make_rom(&size);
    int16_t* audio = (int16_t*)calloc(2 * AGB_APU_RATE, sizeof(int16_t));
    int result = 1;
    CHECK(rom && audio);
    agb_board_init(&board);
    CHECK(!agb_board_insert(&board, rom, size, error, sizeof error));
    free(rom);
    CHECK(!agb_map_init(&map, &board, error, sizeof error));
    agb_cpu_init(&cpu, &board, map.base);
    agb_recomp_init(&recomp, "agb_cpu_test.code");
    if (agb_recomp_build(&recomp, &board, error, sizeof error)) {
        fprintf(stderr, "agb_cpu: recompiling the ROM: %s\n", error);
        goto done;
    }
    for (int frame = 0; frame < 5; ++frame) {
        uint32_t written = 0;
        if (agb_cpu_run(&board, &cpu, &recomp, AGB_SCANLINE_CYCLES * AGB_FRAME_LINES, audio, AGB_APU_RATE, &written,
                        error, sizeof error)) {
            fprintf(stderr, "agb_cpu: %s\n", error);
            goto done;
        }
        CHECK(written >= 540 && written <= 560); /* the mixer filled the frame's audio at 32768 Hz */
    }
    /* the recompiled code ran: the swap went through the machine, the BIOS call waited for a vblank and the
       markers landed */
    CHECK(agb_read32(&board, 0x02000010) == 5 && agb_read32(&board, 0x02000014) == 9);
    CHECK(agb_read32(&board, 0x02000018) == 0x03007F00); /* the IRQ mode's stack stayed in its bank */
    CHECK(agb_read32(&board, 0x02000000) == 42);
    CHECK(agb_read32(&board, 0x02000004) == 7);
    /* the interrupt handler acknowledged the flag and left the machine running */
    CHECK(agb_read16(&board, 0x04000202) == 0);
    CHECK(agb_read16(&board, 0x04000208) == 1);
    /* the empty list stored the PC + 12 at the base - 0x40 and moved the base there */
    CHECK(agb_read32(&board, 0x02000104) == 0x020000C0);
    CHECK(agb_read32(&board, 0x020000C0) == 0x08000198);
    /* past the image every cartridge window reads the address / 2 pattern, in the flat map as on the board */
    CHECK(map.base[0x08000400] == 0x00 && map.base[0x08000401] == 0x02 && map.base[0x0C000402] == 0x01);
    CHECK(agb_read32(&board, 0x0A000400) == 0x02010200);
    {
        /* a region and its mirrors are one memory in the flat map: EWRAM's every 256 KiB and past bit 31, VRAM's last
           32 KiB of each 128 KiB the 32 KiB above the bitmap. volatile: the compiler sees two addresses where the
           host maps one page, and may read the mirror before the store */
        volatile uint8_t* flat = map.base;
        flat[0x0203FFF0] = 0x5A;
        CHECK(flat[0x0207FFF0] == 0x5A && flat[0x8203FFF0] == 0x5A);
        flat[0x06010004] = 0xA5;
        CHECK(flat[0x06018004] == 0xA5 && flat[0x06038004] == 0xA5);
    }
    {
        /* a block that ends past a frame's cycles, at each frame: 200 moves and the branch back in IWRAM, 203 cycles (a
           cycle each and a refill of two), run for frames of 280896, 147 more than 1383 of them. Each run ends where the
           machine's frame does, before line 0 is drawn, not 56 cycles later at each frame */
        for (uint32_t i = 0; i < 200; ++i) agb_write32(&board, 0x03001000 + i * 4, 0xE1A00000); /* mov r0, r0 */
        agb_write32(&board, 0x03001320, 0xEAFFFF36);                                            /* b 0x03001000 */
        agb_write16(&board, 0x04000208, 0);                                                     /* IME off */
        cpu.arm.r[15] = 0x03001000;
        cpu.arm.cpsr = 0x1F;
        for (int frame = 0; frame < 20; ++frame) {
            uint32_t written = 0;
            if (agb_cpu_run(&board, &cpu, &recomp, AGB_SCANLINE_CYCLES * AGB_FRAME_LINES, audio, AGB_APU_RATE, &written,
                            error, sizeof error)) {
                fprintf(stderr, "agb_cpu: %s\n", error);
                goto done;
            }
            if (board.ppu.vcount || board.ppu.dot >= 203) {
                fprintf(stderr, "agb_cpu: frame %d ended at line %u, cycle %u of it\n", frame, board.ppu.vcount,
                        board.ppu.dot);
                goto done;
            }
        }
    }
    {
        /* the CPU waits out the cycles a DMA held the bus for: a run as long as them runs none of its instructions */
        agb_write32(&board, 0x03000800, 0xE2800001); /* add r0, r0, #1 */
        agb_write32(&board, 0x03000804, 0xEAFFFFFD); /* b 0x03000800 */
        cpu.arm.r[0] = 0;
        cpu.arm.r[15] = 0x03000800;
        cpu.arm.cpsr = 0x9F;
        cpu.over = 0;
        board.cpu_halted = cpu.wait = 0;
        board.held_cycles = 1000;
        CHECK(!agb_cpu_run(&board, &cpu, &recomp, 1000, audio, AGB_APU_RATE, NULL, error, sizeof error));
        CHECK(cpu.arm.r[0] == 0 && board.held_cycles == 0);
        CHECK(!agb_cpu_run(&board, &cpu, &recomp, 1000, audio, AGB_APU_RATE, NULL, error, sizeof error));
        CHECK(cpu.arm.r[0] > 0);
        /* a hold longer than the run: the run ends at its own end, with no cycles over and a run's sound, 32 samples
           of 512 cycles, and the rest of the hold waits in the next run */
        {
            uint32_t written = 0;
            cpu.arm.r[0] = 0;
            cpu.over = 0;
            board.held_cycles = 30000;
            CHECK(!agb_cpu_run(&board, &cpu, &recomp, 16384, audio, AGB_APU_RATE, &written, error, sizeof error));
            CHECK(cpu.over == 0 && board.held_cycles == 30000 - 16384 && written == 32 && cpu.arm.r[0] == 0);
            CHECK(!agb_cpu_run(&board, &cpu, &recomp, 16384, audio, AGB_APU_RATE, &written, error, sizeof error));
            CHECK(board.held_cycles == 0 && cpu.arm.r[0] > 0);
        }
    }
    {
        /* a BIOS call takes its entry and return, 45 cycles, and the memory it streams through: CpuSet of 64 words from
           EWRAM to EWRAM, 64 reads and writes of 6 cycles, holds the CPU 813 cycles before the add after it runs */
        agb_write32(&board, 0x03000900, 0xEF0B0000); /* swi 0x0B0000: CpuSet */
        agb_write32(&board, 0x03000904, 0xE2855001); /* add r5, r5, #1 */
        agb_write32(&board, 0x03000908, 0xEAFFFFFD); /* b 0x03000904 */
        cpu.arm.r[0] = 0x02001000;
        cpu.arm.r[1] = 0x02002000;
        cpu.arm.r[2] = 64 | 1u << 26;
        cpu.arm.r[5] = 0;
        cpu.arm.r[15] = 0x03000900;
        cpu.arm.cpsr = 0x9F;
        cpu.over = 0;
        board.cpu_halted = cpu.wait = 0;
        board.held_cycles = 0;
        CHECK(!agb_cpu_run(&board, &cpu, &recomp, 800, audio, AGB_APU_RATE, NULL, error, sizeof error));
        CHECK(cpu.arm.r[5] == 0);
        CHECK(!agb_cpu_run(&board, &cpu, &recomp, 100, audio, AGB_APU_RATE, NULL, error, sizeof error));
        CHECK(cpu.arm.r[5] > 0);
    }
    {
        /* an instruction costs its fetch, by region: an add and the branch back are 2 + 2 cycles from IWRAM (32 bits,
           no wait state) and 12 + 12 from EWRAM (16 bits, 2 wait states, a word two halfwords), so 2400 cycles run the
           loop 600 times from IWRAM and 100 from EWRAM */
        for (int ram = 0; ram < 2; ++ram) {
            uint32_t base = ram ? 0x02003000 : 0x03000A00;
            agb_write32(&board, base, 0xE2800001);     /* add r0, r0, #1 */
            agb_write32(&board, base + 4, 0xEAFFFFFD); /* b base */
            cpu.arm.r[0] = 0;
            cpu.arm.r[15] = base;
            cpu.arm.cpsr = 0x9F;
            cpu.over = 0;
            board.cpu_halted = cpu.wait = 0;
            board.held_cycles = 0;
            CHECK(!agb_cpu_run(&board, &cpu, &recomp, 2400, audio, AGB_APU_RATE, NULL, error, sizeof error));
            CHECK(cpu.arm.r[0] == (ram ? 100u : 600u));
        }
    }
    /* the instructions after the interpreted ones start blocks the discovery step did not see: they were compiled as
       the run reached them, and saving the run keeps them, in the map as hints (the module as it was, at once) */
    CHECK(recomp.compiled > 0 && recomp.dirty);
    if (agb_recomp_save(&recomp, &board, error, sizeof error)) {
        fprintf(stderr, "agb_cpu: saving the module: %s\n", error);
        goto done;
    }
    CHECK(!recomp.dirty);
    {
        /* the cache holds every block of the run, each with its size: a block of the module its function, a hint
           none until a run reaches it, which compiles it again and counts it as no new one */
        AgbRecomp cached;
        size_t count = recomp.count, hints = 0;
        agb_recomp_init(&cached, "agb_cpu_test.code");
        if (agb_recomp_load_cache(&cached, &board, error, sizeof error)) {
            fprintf(stderr, "agb_cpu: loading the cache: %s\n", error);
            agb_recomp_free(&cached);
            goto done;
        }
        result = cached.count == count ? 0 : 1;
        for (size_t i = 1; i <= cached.count; ++i) {
            const AgbBlock* block = &cached.blocks[i];
            if (!block->instructions || (block->fn == NULL) != (block->hint != 0)) result = 1;
            hints += block->hint;
        }
        if (!hints) result = 1;
        for (size_t i = 1; i <= cached.count; ++i)
            if (cached.blocks[i].hint && (!agb_recomp_lookup(&cached, &board, cached.blocks[i].pc,
                                                             cached.blocks[i].thumb, error, sizeof error) ||
                                          cached.compiled))
                result = 1;
        agb_recomp_free(&cached);
        CHECK(!result);
        result = 1;
    }
    {
        /* the ARM7TDMI's NV condition: an instruction with it never runs, which ARMv5 made BLX and later cores CPS */
        const AgbBlock* block;
        agb_write32(&board, 0x03000100, 0xE3A00001); /* mov r0, #1 */
        agb_write32(&board, 0x03000104, 0xF1000000); /* cond NV */
        agb_write32(&board, 0x03000108, 0xFA000003); /* cond NV: ARMv5's BLX */
        agb_write32(&board, 0x0300010C, 0xE2800001); /* add r0, r0, #1 */
        agb_write32(&board, 0x03000110, 0xE12FFF1E); /* bx lr */
        block = agb_recomp_lookup(&recomp, &board, 0x03000100, 0, error, sizeof error);
        CHECK(block);
        cpu.arm.r[14] = 0x08000200;
        CHECK(run_block(&cpu, block) == 2 && cpu.arm.r[15] == 0x08000200);
        /* its Thumb BX takes no notice of the bits under the register, which should be zero */
        agb_write16(&board, 0x03000120, 0x2003); /* movs r0, #3 */
        agb_write16(&board, 0x03000122, 0x4775); /* bx lr, bits 2 to 0 set */
        block = agb_recomp_lookup(&recomp, &board, 0x03000120, 1, error, sizeof error);
        CHECK(block);
        CHECK(run_block(&cpu, block) == 3 && cpu.arm.r[15] == 0x08000200);
    }
    {
        /* code the game puts in RAM runs as RAM holds it: a block built from it goes once the game writes over it,
           this run or the next one, which the cache hands it */
        const AgbBlock* block;
        AgbRecomp next;
        agb_write16(&board, 0x03000000, 0x2001); /* movs r0, #1 */
        agb_write16(&board, 0x03000002, 0x4770); /* bx lr */
        block = agb_recomp_lookup(&recomp, &board, 0x03000000, 1, error, sizeof error);
        CHECK(block);
        CHECK(run_block(&cpu, block) == 1);
        agb_write16(&board, 0x03000000, 0x2002); /* movs r0, #2 */
        block = agb_recomp_lookup(&recomp, &board, 0x03000000, 1, error, sizeof error);
        CHECK(block);
        CHECK(run_block(&cpu, block) == 2);
        {
            size_t compiled = recomp.compiled; /* while RAM holds the same code, its block is the one already built */
            CHECK(agb_recomp_lookup(&recomp, &board, 0x03000000, 1, error, sizeof error) == block &&
                  recomp.compiled == compiled);
        }
        agb_write16(&board, 0x03000002, 0x4708); /* bx r1: the block's last word alone */
        cpu.arm.r[1] = 0x08000201;
        block = agb_recomp_lookup(&recomp, &board, 0x03000000, 1, error, sizeof error);
        CHECK(block && run_block(&cpu, block) == 2 && cpu.arm.r[15] == 0x08000200);
        CHECK(!agb_recomp_save(&recomp, &board, error, sizeof error));
        agb_recomp_init(&next, "agb_cpu_test.code");
        CHECK(!agb_recomp_load_cache(&next, &board, error, sizeof error));
        /* the same code in RAM: the cached block runs, no new one compiled */
        block = agb_recomp_lookup(&next, &board, 0x03000000, 1, error, sizeof error);
        result = block && !next.compiled && run_block(&cpu, block) == 2 ? 0 : 1;
        agb_write16(&board, 0x03000000, 0x2003); /* movs r0, #3 */
        block = agb_recomp_lookup(&next, &board, 0x03000000, 1, error, sizeof error);
        if (!block || run_block(&cpu, block) != 3) result = 1;
        agb_recomp_free(&next);
        CHECK(!result);
        result = 1;
    }
    {
        /* a sound driver function SoundDriverGetJumpList named returns at once, as the BIOS's bx lr: here to Thumb
           code that puts 7 in r0 and spins, interrupts off */
        uint32_t written = 0;
        agb_write16(&board, 0x03000200, 0x2007); /* movs r0, #7 */
        agb_write16(&board, 0x03000202, 0xE7FE); /* b . */
        cpu.arm.r[0] = 0;
        cpu.arm.r[14] = 0x03000201;
        cpu.arm.r[15] = AGB_BIOS_NOP_CALL;
        cpu.arm.cpsr = 0x9F;
        board.cpu_halted = cpu.wait = 0;
        CHECK(!agb_cpu_run(&board, &cpu, &recomp, 2000, audio, AGB_APU_RATE, &written, error, sizeof error));
        CHECK(cpu.arm.r[0] == 7 && cpu.arm.r[15] == 0x03000202 && (cpu.arm.cpsr & SNX_ARM_T));
    }
    {
        /* a Thumb BX PC two bytes past a word lands in ARM state at an address with bit 1 set, as a compiler's
           interworking stub does: the ARM7TDMI holds R15's bits 1 and 0 at zero there, so the word below runs and
           reads the PC from it. Here r0 = 42 and r2 = the first ARM word's address + 8, stored at 0x02000300 */
        uint32_t written = 0;
        agb_write16(&board, 0x03000502, 0x4778);     /* bx pc: ARM at 0x03000506 */
        agb_write32(&board, 0x03000504, 0xE1A0200F); /* mov r2, pc */
        agb_write32(&board, 0x03000508, 0xE3A0002A); /* mov r0, #42 */
        agb_write32(&board, 0x0300050C, 0xE59F1004); /* ldr r1, [pc, #4]: 0x02000300 */
        agb_write32(&board, 0x03000510, 0xE8810005); /* stmia r1, {r0, r2} */
        agb_write32(&board, 0x03000514, 0xEAFFFFFE); /* b . */
        agb_write32(&board, 0x03000518, 0x02000300);
        cpu.arm.r[15] = 0x03000502;
        cpu.arm.cpsr = 0x9F | SNX_ARM_T;
        board.cpu_halted = cpu.wait = 0;
        CHECK(!agb_cpu_run(&board, &cpu, &recomp, 2000, audio, AGB_APU_RATE, &written, error, sizeof error));
        CHECK(agb_read32(&board, 0x02000300) == 42 && agb_read32(&board, 0x02000304) == 0x0300050C);
        CHECK(cpu.arm.r[15] == 0x03000514 && !(cpu.arm.cpsr & SNX_ARM_T));
    }
    {
        /* a software interrupt whose number the BIOS does not have returns at once, as mGBA's HLE BIOS answers it, and
           the machine keeps which and where for the runner to say: here swi 0xFF0099, then 7 stored at 0x02000310 */
        uint32_t written = 0;
        agb_write32(&board, 0x03000600, 0xEFFF0099); /* swi 0xFF0099 */
        agb_write32(&board, 0x03000604, 0xE3A00007); /* mov r0, #7 */
        agb_write32(&board, 0x03000608, 0xE59F1004); /* ldr r1, [pc, #4]: 0x02000310 */
        agb_write32(&board, 0x0300060C, 0xE5810000); /* str r0, [r1] */
        agb_write32(&board, 0x03000610, 0xEAFFFFFE); /* b . */
        agb_write32(&board, 0x03000614, 0x02000310);
        cpu.arm.r[15] = 0x03000600;
        cpu.arm.cpsr = 0x9F;
        board.cpu_halted = cpu.wait = 0;
        CHECK(!agb_cpu_run(&board, &cpu, &recomp, 2000, audio, AGB_APU_RATE, &written, error, sizeof error));
        CHECK(agb_read32(&board, 0x02000310) == 7);
        CHECK(cpu.unknown_swi == 0xFF && cpu.unknown_swi_at == 0x03000600 && cpu.unknown_swi_count == 1);
    }
    {
        /* a recompile (-r) builds every block of the map into the module, the hints too: here from a copy of the map
           under another name, as Windows keeps a module this process has loaded from being written */
        AgbRecomp made, after;
        FILE *from = fopen("agb_cpu_test.code.map", "rb"), *to = fopen("agb_cpu_made.code.map", "wb");
        char bytes[4096];
        size_t n;
        result = !from || !to;
        while (!result && (n = fread(bytes, 1, sizeof bytes, from)) > 0) result = fwrite(bytes, 1, n, to) != n;
        if (from) fclose(from);
        if (to) fclose(to);
        CHECK(!result);
        agb_recomp_init(&made, "agb_cpu_made.code");
        result = agb_recomp_load_map(&made, &board, error, sizeof error) ||
                 agb_recomp_build(&made, &board, error, sizeof error) || made.count < 2;
        if (result) fprintf(stderr, "agb_cpu: the recompile from the map: %s\n", error);
        for (size_t i = 1; !result && i <= made.count; ++i)
            if (!made.blocks[i].fn || made.blocks[i].hint || !made.blocks[i].symbol) {
                fprintf(stderr, "agb_cpu: block %zu at 0x%08X after the recompile: fn %p, hint %u, symbol %u\n", i,
                        made.blocks[i].pc, (void*)made.blocks[i].fn, made.blocks[i].hint, made.blocks[i].symbol);
                result = 1;
            }
        agb_recomp_free(&made);
        CHECK(!result);
        /* a save after another process's recompile, the module file not the one this run loaded (its size here):
           every block is kept as a hint, that module's functions numbered its own way */
        recomp.module_size += 1;
        recomp.dirty = 1;
        CHECK(!agb_recomp_save(&recomp, &board, error, sizeof error));
        recomp.module_size -= 1;
        agb_recomp_init(&after, "agb_cpu_test.code");
        result = agb_recomp_load_map(&after, &board, error, sizeof error) || !after.count;
        for (size_t i = 1; !result && i <= after.count; ++i)
            if (after.blocks[i].symbol || !after.blocks[i].hint) result = 1;
        agb_recomp_free(&after);
        CHECK(!result);
        result = 1;
    }
    {
        /* an LDM the console's bus wraps past 4 GiB lands in the window's guard (memmap.c) instead of aborting */
        const AgbBlock* block;
        agb_write32(&board, 0x03000100, 0xE3E00017); /* mvn r0, #23: 0xFFFFFFE8 */
        agb_write32(&board, 0x03000104, 0xE89001FE); /* ldmia r0, {r1-r8}: to 0x00000007 */
        agb_write32(&board, 0x03000108, 0xE12FFF1E); /* bx lr */
        block = agb_recomp_lookup(&recomp, &board, 0x03000100, 0, error, sizeof error);
        CHECK(block && run_block(&cpu, block) == 0xFFFFFFE8);
    }
    {
        /* the ARM7TDMI's BL is two halves: the code walk pairs them, so the callee is a block before it runs; a low half
           alone calls through LR, which only the run knows: the walk adds the block after it and no other */
        int callee = 0, after = 0;
        size_t before;
        agb_write16(&board, 0x03000200, 0xF000); /* bl 0x03000300: the high half, LR = 0x03000204 */
        agb_write16(&board, 0x03000202, 0xF87E); /* the low half: LR + 0xFC */
        agb_write16(&board, 0x03000300, 0x4770); /* bx lr */
        agb_write16(&board, 0x03000400, 0xF810); /* a low half alone */
        CHECK(agb_recomp_lookup(&recomp, &board, 0x03000200, 1, error, sizeof error));
        before = recomp.count;
        CHECK(agb_recomp_lookup(&recomp, &board, 0x03000400, 1, error, sizeof error) && recomp.count == before + 2);
        for (size_t i = 1; i <= recomp.count; ++i) {
            callee |= recomp.blocks[i].pc == 0x03000300 && recomp.blocks[i].thumb;
            after |= recomp.blocks[i].pc == 0x03000402 && recomp.blocks[i].thumb;
        }
        CHECK(callee && after);
    }
    {
        /* finding a block costs the same however many the map names: one named after 32768 others, as a run that once
           went through 32 MiB of zeros left, against the same one named first, each found 1000 times. A search of the
           index compares the pc with the block or two its slots name, where a scan of the blocks compared it with
           every block before it: 32768 for the one named last. Counted, not timed, so a loaded machine changes
           nothing */
        uint64_t compared[2] = {0, 0};
        for (int last = 0; last < 2; ++last) {
            AgbRecomp many;
            FILE *from = fopen("agb_cpu_test.code.map", "r"), *to = fopen("agb_cpu_index.code.map", "w");
            char header[128];
            const AgbBlock* block;
            result = !from || !to || !fgets(header, sizeof header, from) || fputs(header, to) < 0;
            if (!result && !last) result = fputs("080000C0 0 1 0 -\n", to) < 0;
            for (uint32_t i = 0; !result && i < 32768; ++i)
                result = fprintf(to, "%08X 0 1 0 -\n", 0x0A000000u + i * 4) < 0;
            if (!result && last) result = fputs("080000C0 0 1 0 -\n", to) < 0;
            if (from) fclose(from);
            if (to && fclose(to)) result = 1;
            CHECK(!result);
            agb_recomp_init(&many, "agb_cpu_index.code");
            CHECK(!agb_recomp_load_map(&many, &board, error, sizeof error) && many.count == 32769);
            block = agb_recomp_lookup(&many, &board, 0x080000C0, 0, error, sizeof error); /* compiled here, once */
            result = !block;
            many.compared = 0;
            for (int i = 0; !result && i < 1000; ++i)
                result = agb_recomp_lookup(&many, &board, 0x080000C0, 0, error, sizeof error) != block;
            compared[last] = many.compared;
            agb_recomp_free(&many);
            CHECK(!result);
        }
        remove("agb_cpu_index.code.map");
        result = 1;
        if (compared[0] > 1000 * AGB_LOOKUP_COMPARES || compared[1] > 1000 * AGB_LOOKUP_COMPARES) {
            fprintf(stderr,
                    "agb_cpu: 1000 lookups compared the pc with %llu blocks for the block named after 32768 others, "
                    "%llu named first: more than %d a lookup\n",
                    (unsigned long long)compared[1], (unsigned long long)compared[0], AGB_LOOKUP_COMPARES);
            goto done;
        }
    }
    {
        /* the BIOS's IntrWait waits with the I/O base in r12 and returns with it there, and its interrupt stub calls the
           game's handler with that base in r0: a handler that flags the vblank through r12 (0x03FFFFF8, the flags'
           mirror) ends the wait. Here ARM code with r12 = 0 calls VBlankIntrWait, then stores 42 and its r12 at
           0x02000320; the handler stores its r0 at 0x02000328 */
        static const uint32_t code[] = {
            0xE3A0C000, 0xEF050000, 0xE3A0502A, 0xE59F1008, 0xE5815000, 0xE581C004, 0xEAFFFFFE, 0x02000320,
        };
        static const uint32_t handler[] = {
            0xE59F2014, 0xE5820000, 0xE3A01001, 0xE14C10B8, 0xE2802C02, 0xE1C210B2, 0xE12FFF1E, 0x02000328,
        };
        uint32_t written = 0;
        for (uint32_t i = 0; i < 8; ++i) {
            agb_write32(&board, 0x03000700 + i * 4, code[i]);
            agb_write32(&board, 0x03000740 + i * 4, handler[i]);
        }
        agb_write32(&board, 0x03007FFC, 0x03000740);
        agb_write16(&board, 0x03007FF8, 0);
        agb_write16(&board, 0x04000004, 0x0008); /* the vblank interrupt on */
        agb_write16(&board, 0x04000200, 0x0001);
        agb_write16(&board, 0x04000208, 0x0001);
        board.irq_if = 0;
        board.cpu_halted = cpu.wait = 0;
        cpu.arm.r[15] = 0x03000700;
        cpu.arm.cpsr = 0x1F;
        for (int frame = 0; frame < 2; ++frame)
            CHECK(!agb_cpu_run(&board, &cpu, &recomp, AGB_SCANLINE_CYCLES * AGB_FRAME_LINES, audio, AGB_APU_RATE,
                               &written, error, sizeof error));
        CHECK(agb_read32(&board, 0x02000328) == 0x04000000);
        CHECK(agb_read32(&board, 0x02000320) == 42 && agb_read32(&board, 0x02000324) == 0x04000000);
        agb_write16(&board, 0x04000208, 0);
    }
    {
        /* an MSR that sets the T bit switches to Thumb: the word after it, fetched as ARM already, runs as a no-op and
           Thumb starts two words on, as mGBA has it. Here the ARM word would set r0, the Thumb code stores 7 at r3 */
        static const uint32_t code[] = {0xE129F002, 0xE3A00001, 0x60192107, 0xE7FE};
        uint32_t written = 0;
        for (uint32_t i = 0; i < 4; ++i) agb_write32(&board, 0x03000900 + i * 4, code[i]);
        agb_write16(&board, 0x04000208, 0); /* IME off */
        cpu.arm.r[0] = 0;
        cpu.arm.r[2] = 0x3F;
        cpu.arm.r[3] = 0x02000400;
        cpu.arm.r[15] = 0x03000900;
        cpu.arm.cpsr = 0x1F;
        board.cpu_halted = cpu.wait = 0;
        CHECK(!agb_cpu_run(&board, &cpu, &recomp, AGB_SCANLINE_CYCLES * AGB_FRAME_LINES, audio, AGB_APU_RATE, &written,
                           error, sizeof error));
        CHECK(agb_read32(&board, 0x02000400) == 7 && cpu.arm.r[0] == 0);
        CHECK((cpu.arm.cpsr & 0x3F) == 0x3F && cpu.arm.r[15] == 0x0300090C);
    }
    {
        /* the cache is the ROM's it was built from: another one's, named the same, recompiles and says why */
        AgbRecomp other;
        uint8_t* changed = make_rom(&size);
        CHECK(changed);
        changed[0x1F0] = 1;
        CHECK(!agb_board_insert(&board, changed, size, error, sizeof error));
        free(changed);
        agb_recomp_init(&other, "agb_cpu_test.code");
        result = agb_recomp_load_cache(&other, &board, error, sizeof error);
        agb_recomp_free(&other);
        CHECK(result && strstr(error, "agb_cpu_test.code.map is another ROM's ("));
        result = 1;
    }
    result = 0;
done:
    agb_recomp_free(&recomp);
    agb_map_destroy(&map, &board);
    agb_board_destroy(&board);
    free(audio);
    remove("agb_cpu_test.code.map");
    remove("agb_cpu_test.code.so");
    remove("agb_cpu_made.code.map");
    remove("agb_cpu_made.code.so");
    if (!result) puts("agb_cpu: ok");
    return result;
}
