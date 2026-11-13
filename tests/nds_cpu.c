/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The NDS machine end to end, on a program the test makes (support/nds_image.c): its blocks compiled at run time, then
   compiled ahead of time into a module and run from it, with the same results. No game's data. */
#include "nds/aot/nds_module.h"
#include "nds/board/nds.h"
#include "nds/cpu/nds_cpu.h"
#include "nds/cpu/recomp.h"
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

int main(void) {
    char error[512] = "";
    char* dir = snx_temp_dir(NULL);
    CHECK(dir);
    char *image = snx_path_join(dir, "test.nds"), *module = snx_path_join(dir, "test" NDS_MODULE_SUFFIX);
    CHECK(image && module);
    CHECK(nds_test_image(image, nds_test_program, nds_test_program_words));
    NdsRom rom;
    if (!nds_rom_open(&rom, image, error, sizeof error)) {
        fprintf(stderr, "%s\n", error);
        return 1;
    }
    CHECK(rom.region_count == 1 && rom.regions[0].address == NDS_TEST_ADDRESS);
    CHECK(!strcmp(rom.title, "SNXTEST") && !strcmp(rom.code, "TEST"));
    CHECK(!nds_module_current(module, &rom));
    CHECK(run(&rom, NULL, 12) == 0);
    /* ahead of time: every block the run needs is in the module */
    if (nds_aot_compile(&rom, module, NULL, 1, error, sizeof error)) {
        fprintf(stderr, "%s\n", error);
        return 1;
    }
    CHECK(nds_module_current(module, &rom));
    CHECK(run(&rom, module, 12) >= 8);
    nds_rom_close(&rom);
    (void)snx_remove_tree(dir);
    free(module);
    free(image);
    free(dir);
    puts("nds_cpu: ok");
    return 0;
}
