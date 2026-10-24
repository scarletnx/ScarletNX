/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The whole machine: a hand assembled ROM goes through the AOT recompiler and runs on the board, through
   a BIOS call and an interrupt, and the board's memory shows what happened. */
#include "agb/board/board.h"
#include "agb/cpu/agb_cpu.h"
#include "agb/cpu/memmap.h"
#include "agb/cpu/recomp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
    /* the interrupt handler at 0x120 */
    put16(rom + 0x120, 0x2101); /* movs r1, #1 */
    put16(rom + 0x122, 0x4803); /* ldr r0, [pc, #12]: IF */
    put16(rom + 0x124, 0x8001); /* strh r1, [r0]: acknowledge */
    put16(rom + 0x126, 0x2107); /* movs r1, #7 */
    put16(rom + 0x128, 0x4802); /* ldr r0, [pc, #8]: marker 2 */
    put16(rom + 0x12A, 0x6001); /* str r1, [r0] */
    put16(rom + 0x12C, 0x4770); /* bx lr */
    put16(rom + 0x12E, 0x46C0);
    put32(rom + 0x130, 0x04000202);
    put32(rom + 0x134, 0x02000004); /* marker 2 */
    return rom;
}

/* a Thumb block run as the machine runs it, from its own address: its r0 */
static uint32_t run_block(AgbCpu* cpu, const AgbBlock* block) {
    cpu->arm.r[15] = block->pc;
    cpu->arm.cpsr |= SNX_ARM_T;
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
    agb_recomp_init(&recomp, "agb_cpu_test.code", SNX_AGB_TEST_CC);
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
    /* the instructions after the interpreted ones start blocks the discovery step did not see: they were compiled as
       the run reached them, and saving the run folds them into the cached module */
    CHECK(recomp.compiled > 0 && recomp.dirty);
    if (agb_recomp_save(&recomp, &board, error, sizeof error)) {
        fprintf(stderr, "agb_cpu: saving the module: %s\n", error);
        goto done;
    }
    CHECK(!recomp.patch_count && !recomp.dirty); /* no module file left */
    {
        /* the cache holds every block of the run, each with its function and size */
        AgbRecomp cached;
        size_t count = recomp.count;
        agb_recomp_init(&cached, "agb_cpu_test.code", SNX_AGB_TEST_CC);
        if (agb_recomp_load_cache(&cached, &board, error, sizeof error)) {
            fprintf(stderr, "agb_cpu: loading the cache: %s\n", error);
            agb_recomp_free(&cached);
            goto done;
        }
        result = cached.count == count ? 0 : 1;
        for (size_t i = 1; i <= cached.count; ++i)
            if (!cached.blocks[i].fn || !cached.blocks[i].instructions) result = 1;
        agb_recomp_free(&cached);
        CHECK(!result);
        result = 1;
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
        agb_recomp_init(&next, "agb_cpu_test.code", SNX_AGB_TEST_CC);
        CHECK(!agb_recomp_load_cache(&next, &board, error, sizeof error));
        /* the same code in RAM: the cached block runs, nothing compiled */
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
        /* the cache is the ROM's it was built from: another one's, named the same, recompiles and says why */
        AgbRecomp other;
        uint8_t* changed = make_rom(&size);
        CHECK(changed);
        changed[0x1F0] = 1;
        CHECK(!agb_board_insert(&board, changed, size, error, sizeof error));
        free(changed);
        agb_recomp_init(&other, "agb_cpu_test.code", SNX_AGB_TEST_CC);
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
    if (!result) puts("agb_cpu: ok");
    return result;
}
