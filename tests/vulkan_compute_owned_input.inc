/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A dispatch that reads a storage buffer whose bytes the GPU owns (memory/own.c), where the host traps guest memory
   and the CPU read the dispatch's output lately, so that the output isn't kept on the GPU (compute.c's resident). On
   the compute queue its read of the input (gmem_read into the arena) landed the owner first, a wait for the GPU and a
   copy back to the CPU, and copied the bytes up again. It runs on the graphics queue now, where the buffer cache
   copies the owner's bytes on the GPU: no landing, the input still GPU-owned after the dispatch is recorded, no batch
   of the compute queue, and the word the first dispatch stored in the input is what the second one stores in its
   output. A first dispatch, whose buffer is kept on the GPU, makes the input GPU-owned. No extracted shaders or game
   data are used */
#define COI_GPU 0xa10000000ull
#define COI_CPU (HEAP_BASE + 0x3c800000)
#define COI_LEN 0x30000u
/* the programs at code, each op after its scheduling word as the arena check lays them out */
static int coi_code(u64 code_at, const u64* ops, u32 nops) {
    u8 code[256] = {0};
    u32 at = 0;
    for (u32 i = 0; i < nops; i++) {
        if (!(at % 4)) snx_st_le64(code + 8 * at++, 0x001f8000fc0007e0ull);
        snx_st_le64(code + 8 * at++, ops[i]);
    }
    CHECK(at * 8 <= sizeof code);
    CHECK(!gmem_write(code_at, code, sizeof code));
    return 0;
}
/* one dispatch of the program at code with constant buffer 0 at cb, recorded */
static int coi_dispatch(GpuChannel* ch, u64 code, u64 cb) {
    u32 qmd[64] = {0};
    DispatchState d;
    qmd[QMD_GRID_DIM_X] = 1;
    qmd[QMD_GRID_DIM_YZ] = 1 | 1u << 16;
    qmd[QMD_BLOCK_DIM_X] = 1u << 16;
    qmd[QMD_BLOCK_DIM_YZ] = 1 | 1u << 16;
    qmd[QMD_CONST_BUFFER_ENABLE] = 1u;
    qmd[QMD_CONST_BUFFER_CONFIG] = (u32)cb;
    qmd[QMD_CONST_BUFFER_CONFIG + 1] = (u32)(cb >> 32) | 0x200u << 15;
    const u64 dispatches = g_vk_dispatches;
    vk_lock();
    mx_dispatch_state(qmd, ch->rcomp, code, &d);
    vk_dispatch(&d);
    vk_unlock();
    CHECK(g_vk_dispatches == dispatches + 1);
    return 0;
}
static int check_compute_owned_input(void) {
    const u64 code_w = COI_GPU, code_r = COI_GPU + 0x200, cb_w = COI_GPU + 0x1000, cb_r = COI_GPU + 0x1200;
    /* the input and the output in 64 KiB granules of their own, as the CPU's reads are kept (vk_own_cpu_read) */
    const u64 in = COI_GPU + 0x10000, out = COI_GPU + 0x20000;
    const u32 word = 0x5eed0c0du, unset = 0xcdcdcdcdu;
    if (vk_uma()) {
        puts("compute owned input: memory is shared with the CPU, where every dispatch binds in place: not checked");
        return 0;
    }
    if (!own_traps()) {
        puts("compute owned input: the host traps no guest memory, so the GPU owns none: not checked");
        return 0;
    }
    const u64 writer[] = {0x4c98078004470000ull,  /* MOV R0, c[0][0x110]: buffer 0, the input to be */
                          0x4c98078004570001ull,  /* MOV R1, c[0][0x114] */
                          0x4c98078004870002ull,  /* MOV R2, c[0][0x120]: the word */
                          0xeedc200000070002ull,  /* STG.E [R0], R2 */
                          0xe30000000007000full}; /* EXIT */
    const u64 reader[] = {0x4c98078004470000ull,  /* MOV R0, c[0][0x110]: buffer 0, the output */
                          0x4c98078004570001ull,  /* MOV R1, c[0][0x114] */
                          0x4c98078004870004ull,  /* MOV R4, c[0][0x120]: buffer 1, the input */
                          0x4c98078004970005ull,  /* MOV R5, c[0][0x124] */
                          0xeed4200000070402ull,  /* LDG.E R2, [R4] */
                          0xeedc200000070002ull,  /* STG.E [R0], R2 */
                          0xe30000000007000full}; /* EXIT */
    CHECK(mem_map(COI_CPU, COI_LEN, MS_NORMAL, PERM_RW) == 0);
    gmmu_map(COI_GPU, COI_CPU, COI_LEN);
    CHECK(!coi_code(code_w, writer, sizeof writer / sizeof *writer));
    CHECK(!coi_code(code_r, reader, sizeof reader / sizeof *reader));
    const u32 desc_w[5] = {(u32)in, (u32)(in >> 32), 0x1000, 0, word};
    const u32 desc_r[8] = {(u32)out, (u32)(out >> 32), 0x100, 0, (u32)in, (u32)(in >> 32), 0x1000, 0};
    CHECK(!gmem_write(cb_w + 0x110, desc_w, sizeof desc_w));
    CHECK(!gmem_write(cb_r + 0x110, desc_r, sizeof desc_r));
    CHECK(!gmem_write(out, &unset, 4));
    GpuChannel* ch = calloc(1, sizeof *ch);
    CHECK(ch);
    /* the writer: its buffer kept on the GPU, the input's bytes GPU-owned from then on */
    CHECK(!coi_dispatch(ch, code_w, cb_w));
    vk_lock();
    vk_finish();
    vk_unlock();
    if (!own_lookup(COI_CPU + 0x10000, 0x1000, NULL, 0)) {
        puts("compute owned input: the writer's buffer was not kept on the GPU here: not checked");
        free(ch);
        CHECK(mem_unmap(COI_CPU, COI_LEN) == 0);
        return 0;
    }
    /* the reader, whose output the CPU read lately */
    vk_lock();
    vk_own_cpu_touched(COI_CPU + 0x20000, 0x1000);
    vk_unlock();
    const u64 lands = own_landings(), batches = g_vk_cs_batches;
    CHECK(!coi_dispatch(ch, code_r, cb_r));
    const u64 landed = own_landings() - lands;
    const int owned = own_lookup(COI_CPU + 0x10000, 0x1000, NULL, 0) != 0;
    vk_lock();
    vk_finish();
    vk_compute_land();
    vk_unlock();
    const u64 compute_batches = g_vk_cs_batches - batches;
    u32 got = 0;
    CHECK(!gmem_read(out, &got, 4));
    free(ch);
    CHECK(mem_unmap(COI_CPU, COI_LEN) == 0);
    if (landed || !owned || compute_batches || got != word)
        fprintf(stderr,
                "compute owned input: recording the reader landed GPU-owned bytes %llu times (none expected) and left "
                "its input %s, ran %llu batches on the compute queue (none expected), and it stored %#x, not %#x\n",
                (unsigned long long)landed, owned ? "GPU-owned" : "the CPU's", (unsigned long long)compute_batches, got,
                word);
    CHECK(!landed && owned && !compute_batches && got == word);
    puts("compute owned input: a dispatch read GPU-owned bytes on the graphics queue, without landing them");
    return 0;
}

/* Two dispatches in a chain: the writer's output is GPU-owned, and the reader reads it while nobody on the CPU read
   either output lately. The reader's output stays on the GPU as the writer's does (compute.c's resident), so the next
   pass reads it there. The rule that moves a reader of GPU-owned bytes to the graphics queue is for one whose output
   isn't kept: applied to this one too, its output came back through a staged result */
static int check_compute_owned_chain(void) {
    /* a range of its own: the check before this one had the CPU read the bytes it wrote */
    const u64 gpu = COI_GPU + 0x100000, cpu = COI_CPU + 0x100000;
    const u64 code_w = gpu, code_r = gpu + 0x200, cb_w = gpu + 0x1000, cb_r = gpu + 0x1200;
    const u64 in = gpu + 0x10000, out = gpu + 0x20000;
    const u32 word = 0x5eed0c0du, unset = 0xcdcdcdcdu;
    if (vk_uma()) {
        puts("compute owned chain: memory is shared with the CPU, where every dispatch binds in place: not checked");
        return 0;
    }
    if (!own_traps()) {
        puts("compute owned chain: the host traps no guest memory, so the GPU owns none: not checked");
        return 0;
    }
    CHECK(mem_map(cpu, COI_LEN, MS_NORMAL, PERM_RW) == 0);
    gmmu_map(gpu, cpu, COI_LEN);
    CHECK(!coi_code(code_w,
                    (const u64[]){0x4c98078004470000ull, 0x4c98078004570001ull, 0x4c98078004870002ull,
                                  0xeedc200000070002ull, 0xe30000000007000full},
                    5));
    CHECK(!coi_code(code_r,
                    (const u64[]){0x4c98078004470000ull, 0x4c98078004570001ull, 0x4c98078004870004ull,
                                  0x4c98078004970005ull, 0xeed4200000070402ull, 0xeedc200000070002ull,
                                  0xe30000000007000full},
                    7));
    const u32 desc_w[5] = {(u32)in, (u32)(in >> 32), 0x1000, 0, word};
    const u32 desc_r[8] = {(u32)out, (u32)(out >> 32), 0x100, 0, (u32)in, (u32)(in >> 32), 0x1000, 0};
    CHECK(!gmem_write(cb_w + 0x110, desc_w, sizeof desc_w));
    CHECK(!gmem_write(cb_r + 0x110, desc_r, sizeof desc_r));
    CHECK(!gmem_write(out, &unset, 4));
    GpuChannel* ch = calloc(1, sizeof *ch);
    CHECK(ch);
    CHECK(!coi_dispatch(ch, code_w, cb_w));
    vk_lock();
    vk_finish();
    vk_unlock();
    if (!own_lookup(cpu + 0x10000, 0x1000, NULL, 0)) {
        puts("compute owned chain: the writer's buffer was not kept on the GPU here: not checked");
        free(ch);
        CHECK(mem_unmap(cpu, COI_LEN) == 0);
        return 0;
    }
    const u64 lands = own_landings();
    CHECK(!coi_dispatch(ch, code_r, cb_r));
    const int kept = own_lookup(cpu + 0x20000, 0x1000, NULL, 0) != 0;
    const u64 landed = own_landings() - lands;
    vk_lock();
    vk_finish();
    vk_compute_land();
    vk_unlock();
    u32 got = 0;
    CHECK(!gmem_read(out, &got, 4));
    free(ch);
    CHECK(mem_unmap(cpu, COI_LEN) == 0);
    if (!kept || landed || got != word)
        fprintf(stderr,
                "compute owned chain: the reader's output is %s on the GPU (kept expected), %llu landings (none), and "
                "it stored %#x, not %#x\n",
                kept ? "kept" : "not kept", (unsigned long long)landed, got, word);
    CHECK(kept && !landed && got == word);
    puts("compute owned chain: the output of a dispatch over GPU-owned bytes stayed on the GPU");
    return 0;
}
