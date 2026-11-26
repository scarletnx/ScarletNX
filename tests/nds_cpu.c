/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The NDS machine end to end, on a program the test makes (support/nds_image.c): its blocks compiled at run time, then
   compiled ahead of time into a module and run from it, the host's native back end's, whose index and results are
   those of the module LLVM alone made (NDS_FROZEN_*). No game's data. */
#include "nds/aot/nds_module.h"
#include "nds/board/nds.h"
#include "nds/cpu/nds_cpu.h"
#include "nds/cpu/recomp.h"
#include "common/cpu/recompiler/mach/native.h"
#include "common/platform/dl.h"
#include "common/utilities/bits.h"
#include "common/utilities/common.h"
#include "common/utilities/endian.h"
#include "support/nds_image.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x)                                                         \
    do {                                                                 \
        if (!(x)) {                                                      \
            fprintf(stderr, "%s:%d: %s failed\n", __FILE__, __LINE__, #x); \
            exit(1);                                                     \
        }                                                                \
    } while (0)

static uint32_t result(const NdsBoard* board, unsigned index) {
    return snx_ld_le32(board->mem.main.view + 0x100000 + index * 4);
}

/* the program for frames frames, on the module at path when it is there: the blocks it took from the module */
static uint64_t run(const NdsRom* rom, const char* module, unsigned frames) {
    char error[512] = "", note[512] = "";
    NdsBoard* board = calloc(1, sizeof *board);
    NdsCpu* cpu = calloc(1, sizeof *cpu);
    CHECK(board && cpu);
    CHECK(nds_board_init(board, (NdsRom*)rom, error, sizeof error));
    NdsRecomp* recomp = nds_recomp_new(rom, module, note, sizeof note);
    CHECK(recomp && !note[0]);
    CHECK(nds_recomp_has_module(recomp) == (module != NULL));
    nds_cpu_init(cpu, board, recomp);
    for (unsigned i = 0; i < frames; ++i) {
        if (nds_cpu_run_frame(cpu, error, sizeof error)) {
            fprintf(stderr, "the machine stopped: %s\n", error);
            exit(1);
        }
    }
    CHECK(result(board, 0) == 23);          /* CLZ */
    CHECK(result(board, 1) == 0x7FFFFFFFu); /* QADD saturates */
    CHECK(result(board, 2) == 0x1234);      /* BLX to Thumb through a register, and back */
    CHECK(result(board, 3) == 14);          /* the BIOS's Div */
    CHECK(result(board, 4) == 2);
    CHECK(result(board, 7) == 42);          /* BLX to Thumb, whose BL is two halves, and POP {pc} back to ARM */
    /* a frame counted at each vertical blank's interrupt. A run ends as its last frame's blank begins, before the
       handler has run for it */
    CHECK(result(board, 6) == frames - 1);
    CHECK(result(board, 5) == frames - 1);
    CHECK(board->frames == frames);
    CHECK(cpu->unknown_swi_count == 0);
    uint64_t compiled, hinted, from_module;
    nds_recomp_counts(recomp, &compiled, &hinted, &from_module);
    CHECK(compiled + from_module > 0);
    if (!module) CHECK(hinted == compiled); /* every block is the image's own code */
    nds_recomp_free(recomp);
    nds_board_free(board);
    free(cpu);
    free(board);
    return from_module;
}

/* the index of the module at path, hashed (little-endian, the same on any host): the image it is of, and each region's
   place, size and code's hash and every slot's units (0 where it has no block); in *blocks the blocks, in *longer those
   of more than a unit */
static uint64_t index_hash(const char* path, unsigned* blocks, unsigned* longer) {
    void* library = plat_dl_open(path);
    CHECK(library);
    const NdsModule* module = plat_dl_sym(library, NDS_MODULE_SYMBOL);
    CHECK(module);
    uint8_t head[12];
    snx_st_le64(head, module->identity), snx_st_le32(head + 8, module->region_count);
    uint64_t hash = snx_fnv1a(SNX_FNV_BASIS, head, sizeof head);
    *blocks = *longer = 0;
    for (uint32_t r = 0; r < module->region_count; ++r) {
        const NdsModuleRegion* m = &module->regions[r];
        uint8_t place[16];
        snx_st_le32(place, m->address), snx_st_le32(place + 4, m->size), snx_st_le64(place + 8, m->hash);
        hash = snx_fnv1a(hash, place, sizeof place);
        for (uint32_t i = 0; i < m->size / 2; ++i) {
            CHECK(!m->thumb[i] == !m->thumb_units[i] && m->thumb_units[i] <= NDS_BLOCK_UNITS);
            const uint8_t thumb = m->thumb_units[i];
            hash = snx_fnv1a(hash, &thumb, 1);
            *blocks += thumb != 0, *longer += thumb > 1;
            if (i >= m->size / 4) continue;
            CHECK(!m->arm[i] == !m->arm_units[i] && m->arm_units[i] <= NDS_BLOCK_UNITS);
            const uint8_t arm = m->arm_units[i];
            hash = snx_fnv1a(hash, &arm, 1);
            *blocks += arm != 0, *longer += arm > 1;
        }
    }
    plat_dl_close(library);
    return hash;
}
/* the module LLVM alone made, the reference: its index, its blocks, those of more than a unit, and how many of them the
   run took */
#define NDS_FROZEN_INDEX       0xbe4e820462d633b2ull
#define NDS_FROZEN_BLOCKS      11u
#define NDS_FROZEN_LONGER      9u
#define NDS_FROZEN_FROM_MODULE 11ull

/* LDM with its base in the list and writeback, as the ARM946E-S (ARMv5) runs it: the base written back when it is the
   list's only register or not its last, the loaded value when it is the last. Assembled at 0x02000000:
           mov r4, #0x02100000
           add r0, r4, #0x40
           mov r1, #0x11
           str r1, [r0]
           mov r1, #0x22
           str r1, [r0, #4]
           ldmia r0!, {r0, r1}      @ r0 not last: 0x02100048
           str r0, [r4]
           str r1, [r4, #4]         @ 0x22
           add r2, r4, #0x40
           ldmia r2!, {r2}          @ the only one: 0x02100044
           str r2, [r4, #8]
           add r3, r4, #0x40
           ldmia r3!, {r1, r3}      @ r3 last: what it loaded, 0x22
           str r3, [r4, #12]
   loop:   b loop */
static const uint32_t ldm_program[] = {0xE3A04621, 0xE2840040, 0xE3A01011, 0xE5801000, 0xE3A01022, 0xE5801004,
                                       0xE8B00003, 0xE5840000, 0xE5841004, 0xE2842040, 0xE8B20004, 0xE5842008,
                                       0xE2843040, 0xE8B3000A, 0xE584300C, 0xEAFFFFFE};
static void test_ldm_base_in_list(const char* dir) {
    char error[512] = "", note[512] = "";
    char* image = snx_path_join(dir, "ldm.nds");
    CHECK(image && nds_test_image(image, ldm_program, sizeof ldm_program / sizeof *ldm_program));
    NdsRom rom;
    CHECK(nds_rom_open(&rom, image, NULL, error, sizeof error));
    NdsBoard* board = calloc(1, sizeof *board);
    NdsCpu* cpu = calloc(1, sizeof *cpu);
    CHECK(board && cpu && nds_board_init(board, &rom, error, sizeof error));
    NdsRecomp* recomp = nds_recomp_new(&rom, NULL, note, sizeof note);
    CHECK(recomp && !note[0]);
    nds_cpu_init(cpu, board, recomp);
    if (nds_cpu_run_frame(cpu, error, sizeof error)) {
        fprintf(stderr, "the machine stopped: %s\n", error);
        exit(1);
    }
    CHECK(result(board, 0) == 0x02100048u && result(board, 1) == 0x22);
    CHECK(result(board, 2) == 0x02100044u);
    CHECK(result(board, 3) == 0x22);
    nds_recomp_free(recomp);
    nds_board_free(board);
    nds_rom_close(&rom);
    free(cpu);
    free(board);
    free(image);
}

int main(void) {
    char error[512] = "";
    char* dir = snx_temp_dir(NULL);
    CHECK(dir);
    char *image = snx_path_join(dir, "test.nds"), *module = snx_path_join(dir, "test" NDS_MODULE_SUFFIX);
    CHECK(image && module);
    CHECK(nds_test_image(image, nds_test_program, nds_test_program_words));
    NdsRom rom;
    if (!nds_rom_open(&rom, image, NULL, error, sizeof error)) {
        fprintf(stderr, "%s\n", error);
        return 1;
    }
    CHECK(rom.region_count == 1 && rom.regions[0].address == NDS_TEST_ADDRESS);
    CHECK(!strcmp(rom.title, "SNXTEST") && !strcmp(rom.code, "TEST"));
    CHECK(!nds_module_current(module, &rom));
    CHECK(run(&rom, NULL, 12) == 0);
    /* ahead of time: every block the run needs is in the module */
    SnxArmAotCount made = {0};
    if (nds_aot_compile(&rom, module, NULL, 1, 0, NULL, &made, error, sizeof error)) {
        fprintf(stderr, "%s\n", error);
        return 1;
    }
    CHECK(nds_module_current(module, &rom));
    const uint64_t from_module = run(&rom, module, 12);
    CHECK(from_module >= 8);
    /* every block the native back end's where the host has one, LLVM's where it has none */
    CHECK(snx_native_arm_available() ? made.native && !made.llvm : !made.native && made.llvm);
    unsigned blocks, longer;
    const uint64_t index = index_hash(module, &blocks, &longer);
    if (index != NDS_FROZEN_INDEX || blocks != NDS_FROZEN_BLOCKS || longer != NDS_FROZEN_LONGER ||
        made.native + made.llvm != NDS_FROZEN_BLOCKS || from_module != NDS_FROZEN_FROM_MODULE) {
        fprintf(stderr,
                "nds_cpu: the module's index is %016llx, of %u blocks (%zu made), %u of more than a unit, %llu of them "
                "run; LLVM alone's was %016llx, of %u blocks, %u of more than a unit, %llu of them run\n",
                (unsigned long long)index, blocks, made.native + made.llvm, longer, (unsigned long long)from_module,
                (unsigned long long)NDS_FROZEN_INDEX, NDS_FROZEN_BLOCKS, NDS_FROZEN_LONGER, NDS_FROZEN_FROM_MODULE);
        return 1;
    }
    nds_rom_close(&rom);
    test_ldm_base_in_list(dir);
    (void)snx_remove_tree(dir);
    free(module);
    free(image);
    free(dir);
    puts("nds_cpu: ok");
    return 0;
}
