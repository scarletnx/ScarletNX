/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The adaptive production reader must produce exactly the former full-window hash and length. Include forward
   targets, dynamic calls, BRX's global reaching-definition proof, scheduling boundaries and adjacent programs. */
static int window_brx_table(u32 bank, u32 offset, u32* value) {
    if (bank || offset > 4) return 0;
    *value = 24;
    return 1;
}
static u64 window_branch(u32 from, u32 to, int call) {
    u32 displacement = (to - from - 1) * 8;
    return (call ? 0xe26000000007000full : 0xe24000000007000full) |
           (u64)(displacement & 0xffffffu) << 20;
}
static int check_shader_read_windows(void) {
    const u32 bytes = 0x20000;
    const u64 gpu = 0x840000000ull, cpu = HEAP_BASE + 0x10a00000;
    CHECK(!mem_map(cpu, bytes, MS_NORMAL, PERM_RW));
    gmmu_map(gpu, cpu, bytes);
    u64* program = calloc(1, bytes);
    u8 *actual = malloc(SH_CODE), *full = malloc(bytes); /* vk_shader_read_code pads its whole window */
    CHECK(program && actual && full);
    vk_lock();
    for (u32 test = 0; test < 9; test++) {
        if (test == 8 && !own_traps()) {
            puts("SKIP shader read windows GPU-owned BRX: memory traps are unavailable");
            continue;
        }
        memset(program, 0, bytes);
        u32 end = test == 1 ? 132 : test == 2 ? 140 : test == 3 ? 100 : test == 4 ? 32 : test == 5 ? 64 : 100;
        for (u32 i = 10; i < end; i++)
            program[i] = i % 4 == 2 ? 0x001f8000fc0007e0ull : 0x50b0000000070f00ull;
        if (test == 0) {
            program[11] = window_branch(11, 67, 0);
            program[12] = program[67] = 0xe30000000007000full;
        } else if (test == 1) {
            program[11] = window_branch(11, 131, 1);
            program[12] = 0xe30000000007000full;
            program[131] = 0xe32000000007000full;
        } else if (test == 2) {
            program[11] = 0xe220001000070020ull; /* JCAL: retain code after an early main EXIT */
            program[12] = program[139] = 0xe30000000007000full;
        } else if (test == 3 || test == 8) {
            program[11] = 0x3820000000170b0bull;
            program[12] = 0x3848000000270b0dull;
            program[13] = 0xef94000000070d0dull;
            program[15] = 0xe250000000070d0full;
            program[16] = program[19] = 0xe30000000007000full;
            program[67] = window_branch(67, 15, 0); /* changes BRX proof outside the first prefix */
        } else if (test == 4 || test == 5)
            program[end - 1] = 0xe30000000007000full;
        else if (test == 6) {
            program[11] = 0xe220001000070020ull;
            program[12] = 0xe30000000007000full;
            program[19] = 0; /* real compiler padding before another program */
            program[41] = 0xe30000000007000full;
        } else
            program[11] = 0xe30000000007000full;
        snx_le64_array(program, bytes / 8);
        CHECK(!gmem_write(gpu, program, bytes));
        gmem_read_code(gpu, full, bytes);
        u32 expected_length = 0, length = 0;
        VkCbReader reader = test == 3 || test == 8 ? window_brx_table : NULL;
        vk_shader_hash(full, bytes, &expected_length, reader);
        VkOwnRes* owner = NULL;
        if (test == 8) {
            /* The coherent read first exposes BRX. Its global proof must include the incoming branch at word67,
               beyond the initial256-byte prefix. A zero-padded prefix alone incorrectly accepts the EXIT at19. */
            memset(actual, 0, bytes);
            memcpy(actual, full, 256);
            u32 prefix_length = 0;
            vk_shader_hash(actual, bytes, &prefix_length, reader);
            CHECK(prefix_length + 16 < 256 && expected_length > 256);
            VkBuffer upload;
            u64 offset;
            u8* staging = vk_stage_upload(bytes, 16, &upload, &offset);
            owner = vk_own_res_new(bytes);
            CHECK(staging && owner);
            memcpy(staging, full, bytes);
            VkBufferCopy copy = {offset, 0, bytes};
            rec_CmdCopyBuffer(vk_cmd(), upload, owner->buf, 1, &copy);
            vk_own_publish_owned(cpu, bytes, owner, 0);
            CHECK(own_lookup(cpu, bytes, NULL, 0) == 1);
            CHECK(gmem_read_code_cpu(gpu, actual, 256) == 0);
            CHECK(!shader_code_full_window(actual, 256));
            CHECK(own_lookup(cpu, bytes, NULL, 0) == 1);
        }
        vk_shader_read_code(gpu, actual, &length, reader);
        CHECK(length == expected_length && !memcmp(actual, full, length));
        CHECK(vk_hash(actual, length) == vk_hash(full, expected_length));
        if (owner) {
            vk_own_res_unref(owner);
            puts("PASS shader read windows GPU-owned BRX: coherent-read discovery retains the full program exact=1");
        }
        if (test == 0 || test == 1 || test == 2 || test == 3 || test == 8) CHECK(length > 256);
        if (test == 7) CHECK(length == 96);
    }
    vk_unlock();
    free(program); free(actual); free(full);
    puts("PASS shader read windows: forward/indirect flow, BRX lookahead, boundaries and adjacent programs exact=1");
    return 0;
}
