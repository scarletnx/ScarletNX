/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "support/3ds_cro.h"
#include "3ds/aot/ctr_jit.h"
#include "3ds/aot/ctr_module.h"
#include "common/cpu/arm/exec.h"
#include "common/utilities/endian.h"

#define TEXT_ADDRESS  0x100000u
#define WINDOW_TEXT   0x400u /* the static text of the window scenarios: eight 128-byte windows */
#define CYCLES        128u
#define SVC_SEND_SYNC (0xef000000u | SVC_SEND_SYNC_REQUEST)
#define MOV_R0(value) (0xe3a00000u | (value))
#define BRANCH_HERE   0xeafffffeu
#define PLAIN_TEXT    0x10u /* the static text of the scenarios without windows: four ARM words */

static void run(const SnxCtrModule* module, SnxCtrProcess* p, uint32_t pc, bool thumb, uint32_t expected) {
    SnxArmCPU* cpu = &p->cpu.state.cpu;
    cpu->r[15] = pc;
    cpu->r[0] = 0;
    cpu->cpsr = thumb ? cpu->cpsr | SNX_ARM_T : cpu->cpsr & ~SNX_ARM_T;
    cpu->exit = SNX_ARM_OK;
    cpu->syndrome = 0;
    uint64_t executed = 0;
    assert(snx_ctr_dispatch(module, p, 1, &executed) == SNX_CTR_RUN_BUDGET);
    assert(executed == 1 && cpu->r[0] == expected);
}
static SnxArmBlock aot_block(SnxArmExec* aot, const unsigned char* code, uint32_t offset, uint32_t count) {
    uint32_t words[8];
    size_t consumed;
    for (uint32_t i = 0; i < count; ++i) words[i] = snx_ld_le32(code + offset + i * 4);
    SnxArmBlock block = snx_arm_exec_compile(aot, words, count, TEXT_ADDRESS + offset, false, SNX_ARM_PROFILE_ARM11,
                                             snx_ctr_module_memory() == SNX_CTR_MODULE_CHECKED_MODE, &consumed);
    assert(block);
    return block;
}
int main(int argc, char** argv) {
    assert(argc == 2 || argc == 3);
    const char* scenario = argv[1];
    bool cycles = !strncmp(scenario, "cycles", 6), self_unload = !strcmp(scenario, "svc-unload");
    bool thumb = !strcmp(scenario, "thumb") || !strcmp(scenario, "cycles-thumb");
    bool window = !strcmp(scenario, "static-window"), stat = window || !strcmp(scenario, "static");
    uint32_t text = window ? WINDOW_TEXT : PLAIN_TEXT;
    unsigned char code[0x3000] = {0};
    snx_st_le32(code, MOV_R0(1));
    snx_st_le32(code + 4, BRANCH_HERE);
    if (window) {
        /* B holds the word the CRO rewrites, E reaches it from the window before, and A and C are clear of it */
        const uint32_t placed[][2] = {{0xf4, MOV_R0(5)},   {0xf8, 0xe1a00000u},  {0xfc, 0xe1a00000u},
                                      {0x100, MOV_R0(5)},  {0x104, BRANCH_HERE}, {0x180, MOV_R0(2)},
                                      {0x184, BRANCH_HERE}};
        for (uint32_t i = 0; i < sizeof placed / sizeof *placed; ++i) snx_st_le32(code + placed[i][0], placed[i][1]);
    }
    SnxCtrImage executable = {.segment = {{TEXT_ADDRESS, 1, text}, {0x200000, 1, 4}, {0x300000, 1, 4}},
                              .stack_size = 0x4000,
                              .main_priority = 0x30,
                              .bss_size = 4096,
                              .code = code,
                              .code_size = sizeof code};
    SnxCtrProcess* p = snx_ctr_process_new(&executable, argc == 3 ? SNX_CTR_NEW : SNX_CTR_OLD);
    assert(p);
    cro_map_heap(p, 0xa000);
    uint32_t session = cro_service(p);
    char error[512];
    SnxCtrJit* jit = snx_ctr_jit_new(p, error, sizeof error);
    if (!jit) {
        fprintf(stderr, "CRO execution requires the runtime compiler: %s\n", error);
        snx_ctr_process_free(p);
        return 77;
    }
    SnxArmBlock arms[WINDOW_TEXT / 4] = {0};
    SnxArmBlock thumbs[WINDOW_TEXT / 2] = {0};
    SnxArmExec* aot = snx_arm_exec_new(error, sizeof error);
    assert(aot);
    arms[0] = aot_block(aot, code, 0, 4);
    if (window) {
        arms[0xf4 / 4] = aot_block(aot, code, 0xf4, 6);
        arms[0x100 / 4] = aot_block(aot, code, 0x100, 2);
        arms[0x180 / 4] = aot_block(aot, code, 0x180, 2);
    }
    SnxCtrModule module = {.magic = SNX_CTR_MODULE_MAGIC,
                           .abi = SNX_CTR_MODULE_ABI,
                           .descriptor_size = sizeof(SnxCtrModule),
                           .cpu_size = sizeof(SnxArmCPU),
                           .checked_size = sizeof(SnxArmCheckedCPU),
                           .arm_abi = SNX_ARM_ABI,
                           .checked_abi = SNX_ARM_CHECKED_ABI,
                           .codegen_abi = SNX_CTR_MODULE_CODEGEN_ABI,
                           .profile = SNX_ARM_PROFILE_ARM11,
                           .memory_mode = snx_ctr_module_memory(),
                           .pointer_size = sizeof(void*),
                           .text_address = TEXT_ADDRESS,
                           .text_bytes = text,
                           .arm_count = text / 4,
                           .thumb_count = text / 2,
                           .arm_blocks = arms,
                           .thumb_blocks = thumbs};
    assert(snx_ctr_module_compatible(&module, &executable));
    if (stat) run(&module, p, TEXT_ADDRESS, false, 1); /* the AOT entry runs before the CRO rewrites it */
    unsigned char cro[CRO_IMAGE_SIZE];
    cro_image(cro, true);
    assert(snx_ctr_write_ram(p->space, CRO_CRS_SOURCE, cro, sizeof cro) == SNX_CTR_MEM_OK);
    const uint32_t init[] = {CRO_CRS_SOURCE, CRO_IMAGE_SIZE, CRO_CRS};
    assert(!cro_call(p, session, 1, init, 3, CTR_PROCESS_HANDLE, NULL));
    cro_image(cro, false);
    if (thumb) cro_set(cro, IMG_CODE, 0xe7fe2001u); /* movs r0, #1 and b . */
    if (self_unload) {
        cro_set(cro, IMG_CODE + 0x10, SVC_SEND_SYNC);
        cro_set(cro, IMG_CODE + 0x14, BRANCH_HERE);
    }
    if (stat) {
        /* the word at the tag of the static text becomes mov r0, #7 */
        uint32_t tag = window ? CRO_TAG(0, 0x100) : 0;
        cro_add_static_relocation(cro, tag, MOV_R0(7) - (CRO_MODULE + IMG_CODE + 0x44));
    }
    assert(snx_ctr_write_ram(p->space, CRO_SOURCE, cro, sizeof cro) == SNX_CTR_MEM_OK);
    uint32_t load[LOAD_WORDS] = CRO_LOAD_DEFAULT;
    uint32_t fixed;
    assert(!cro_call(p, session, 9, load, LOAD_WORDS, CTR_PROCESS_HANDLE, &fixed));
    if (window) {
        /* Only the windows a rewritten word can reach leave the AOT tables: B, E and the block between them */
        uint64_t compiled = snx_ctr_jit_compiled(jit);
        run(&module, p, TEXT_ADDRESS, false, 1);
        run(&module, p, TEXT_ADDRESS + 0x180, false, 2);
        assert(snx_ctr_jit_compiled(jit) == compiled);
        run(&module, p, TEXT_ADDRESS + 0x100, false, 7);
        assert(snx_ctr_jit_compiled(jit) == compiled + 1);
        run(&module, p, TEXT_ADDRESS + 0xf4, false, 7);
        assert(snx_ctr_jit_compiled(jit) == compiled + 2);
    } else {
        run(&module, p, stat ? TEXT_ADDRESS : CRO_MODULE + IMG_CODE, thumb, stat ? 7 : 1);
    }
    assert(snx_ctr_jit_compiled(jit));
    if (cycles) {
        for (uint32_t i = 0; i < CYCLES; ++i) {
            const uint32_t unload[] = {load[LOAD_ADDRESS], 0, CRO_SOURCE};
            assert(!cro_call(p, session, 5, unload, 3, CTR_PROCESS_HANDLE, NULL));
            assert(p->reclaim);                      /* the IPC leaves the native code to the dispatcher */
            run(&module, p, TEXT_ADDRESS, false, 1); /* the next block releases it, even when it needs no compile */
            assert(!p->reclaim);
            uint32_t offset = IMG_CODE + (i % 4) * 16;
            cro_image(cro, false);
            cro_set(cro, offset, (thumb ? 0xe7fe2000u : MOV_R0(0)) | (i + 2));
            if (!thumb) cro_set(cro, offset + 4, BRANCH_HERE);
            load[LOAD_ADDRESS] = CRO_MODULE + (i % 2) * 0x100000;
            assert(snx_ctr_write_ram(p->space, CRO_SOURCE, cro, sizeof cro) == SNX_CTR_MEM_OK);
            assert(!cro_call(p, session, 9, load, LOAD_WORDS, CTR_PROCESS_HANDLE, &fixed));
            run(&module, p, load[LOAD_ADDRESS] + offset, thumb, i + 2);
        }
        /* the first load compiled one block, and each cycle one more */
        assert(snx_ctr_jit_compiled(jit) == 1 + CYCLES);
    }
    if (self_unload) {
        uint32_t cmd = cro_command_buffer(p);
        ctr_put(p, cmd, IPC_HEADER(5, 3, 2)); /* UnloadCRO, with the process handle */
        ctr_put(p, cmd + 4, CRO_MODULE);
        ctr_put(p, cmd + 8, 0);
        ctr_put(p, cmd + 12, CRO_SOURCE);
        ctr_put(p, cmd + 16, 0);
        ctr_put(p, cmd + 20, CTR_PROCESS_HANDLE);
        SnxArmCPU* cpu = &p->cpu.state.cpu;
        cpu->r[0] = session;
        cpu->r[15] = CRO_MODULE + IMG_CODE + 0x10;
        cpu->exit = SNX_ARM_OK;
        uint64_t executed = 0;
        assert(snx_ctr_dispatch(&module, p, 1, &executed) == SNX_CTR_RUN_BUDGET);
        assert(executed == 1 && !cpu->r[0] && !ctr_word(p, cmd + 4));
        assert(snx_ctr_query(p->space, CRO_MODULE, NULL, NULL) == SNX_CTR_MEM_UNMAPPED);
        assert(p->reclaim); /* the block that made the SVC is still running its native code */
        run(&module, p, TEXT_ADDRESS, false, 1);
        assert(!p->reclaim);
    }
    if (!stat && !cycles && !self_unload) {
        const uint32_t unload[] = {CRO_MODULE, 0, CRO_SOURCE};
        assert(!cro_call(p, session, 5, unload, 3, CTR_PROCESS_HANDLE, NULL));
        cro_image(cro, false);
        cro_set(cro, IMG_CODE, thumb ? 0xe7fe2002u : 0xe3a00002u);
        assert(snx_ctr_write_ram(p->space, CRO_SOURCE, cro, sizeof cro) == SNX_CTR_MEM_OK);
        assert(!cro_call(p, session, 9, load, LOAD_WORDS, CTR_PROCESS_HANDLE, &fixed));
        run(&module, p, CRO_MODULE + IMG_CODE, thumb, 2);
        assert(snx_ctr_jit_compiled(jit) >= 2);
    }
    snx_ctr_jit_free(jit);
    snx_arm_exec_free(aot);
    snx_ctr_process_free(p);
    puts(scenario);
    return 0;
}
