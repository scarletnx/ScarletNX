/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */

/* A draw's read-only storage buffer over guest memory that a region of unified memory holds (gpu/vk/buffer.c's
   vk_buf_get_storage). Where imports are dear (amdgpu), such a buffer was copied into a buffer of its own, and each
   write in place on its pages made the CPU copy them again (patch_dirty). Now it is bound in place where the checks
   of vk_uma_read pass. A dispatch writes a word of it in place, a vertex program loads the word and stores it through
   a second buffer: in the submission of the write (the read's barrier, uma.c's order, puts the load after the store),
   and in the next submission, while the store is pending. Then a third load once the store is done, also in place
   where imports are dear. No extracted shaders or game data are used */
#define USR_GRO  0x6b0000000ull /* the read-only buffer's window */
#define USR_RO   (HEAP_BASE + 0x38000000)
#define USR_CODE (0x4e1000000ull + 0x200) /* the dispatch's program, beside the other dispatch checks' */
/* its constant buffer, in no region: the descriptor at 0x110, the word at 0x120. In the buffer's window, the runtime's
   write of the word waited for the store before, and the program went to the GPU at once from then on, in a
   submission of its own (vk_uma_read_soon) */
#define USR_CB   (0x4e1000000ull + 0xc000)
/* one dispatch that stores the word c[0][0x120] at the descriptor's address, recorded */
static int usr_dispatch_at(GpuChannel* channel, u64 code, u32 word) {
    u32 qmd[64] = {0};
    DispatchState d;
    CHECK(!storage_write_words(USR_CB + 0x120, &word, 1));
    qmd[QMD_GRID_DIM_X] = 1;
    qmd[QMD_GRID_DIM_YZ] = 1 | 1u << 16;
    qmd[QMD_BLOCK_DIM_X] = 1u << 16;
    qmd[QMD_BLOCK_DIM_YZ] = 1 | 1u << 16;
    qmd[QMD_CONST_BUFFER_ENABLE] = 1u;
    qmd[QMD_CONST_BUFFER_CONFIG] = (u32)USR_CB;
    qmd[QMD_CONST_BUFFER_CONFIG + 1] = (u32)(USR_CB >> 32) | 0x200u << 15;
    const u64 dispatches = g_vk_dispatches;
    vk_lock();
    mx_dispatch_state(qmd, channel->rcomp, code, &d);
    vk_dispatch(&d);
    vk_unlock();
    CHECK(g_vk_dispatches == dispatches + 1);
    return 0;
}
static int usr_dispatch(GpuChannel* channel, u32 word) { return usr_dispatch_at(channel, USR_CODE, word); }
/* one draw of the program at ALIAS_GCODE, recorded with nothing between it and the work before: no barrier of a program
   change (alias_program_draw's), so only the read's own barrier orders it after the dispatch's store */
static int usr_draw(GpuChannel* channel) {
    const u64 drawn = g_vk_drawn;
    DrawCall call = {.topology = 0, .count = 1, .instances = 1};
    vk_lock();
    vk_render_draw(&channel->ds, &call, &channel->owner);
    vk_unlock();
    CHECK(g_vk_drawn == drawn + 1);
    return 0;
}
/* about 4 GiB of fills in two submissions, which the GPU runs while the next store and load are recorded: with two
   submissions not done, recording makes no submission each millisecond (cmd.c's frame_check and gpu_short_of_work),
   which would put a submission between them */
static int usr_busy(void) {
    VkBuffer b;
    u64 at;
    vk_lock();
    vk_render_end();
    CHECK(vk_stage_device(16u << 20, 16, &b, &at));
    for (u32 i = 0; i < 256; i++) {
        vk_all_barrier(vk_cmd()); /* each fill after the one before */
        rec_CmdFillBuffer(vk_cmd(), b, at, 16u << 20, i);
        if (i == 127) vk_submit(VK_NULL_HANDLE, 0, VK_NULL_HANDLE);
    }
    vk_submit(VK_NULL_HANDLE, 0, VK_NULL_HANDLE);
    vk_unlock();
    return 0;
}
/* the load: one draw of the reader, and the word it stored once the GPU is done */
static int usr_load(GpuChannel* channel, u32* word) {
    CHECK(!usr_draw(channel));
    alias_words(word, 1);
    return 0;
}
static int check_uma_storage_region(void) {
    const u64 ro = USR_GRO + 0x1000;
    const u64 writer[] = {
        0x001f8000fc0007e0ull, 0x4c98078004470000ull, /* MOV R0, c[0][0x110]: the buffer's address, NVN's descriptor */
        0x4c98078004570001ull,                        /* MOV R1, c[0][0x114] */
        0x4c98078004870002ull,                        /* MOV R2, c[0][0x120]: the word */
        0x001f8000fc0007e0ull, 0xeedc200000070002ull, /* STG.E [R0], R2 */
        0xe30000000007000full};
    const u64 reader[] = {ALIAS_MOV_R0,
                          ALIAS_MOV_R1,
                          ALIAS_MOV_R4,
                          ALIAS_MOV_R5,
                          0xeed4200000070402ull, /* LDG.E R2, [R4]: the read-only buffer */
                          0xeedc200000070002ull, /* STG.E [R0], R2: buffer 0, written */
                          ALIAS_ALD,
                          ALIAS_AST,
                          ALIAS_EXIT};
    const u32 desc[8] = {(u32)ALIAS_STORED, (u32)(ALIAS_STORED >> 32), 0x100, 0, (u32)ro, (u32)(ro >> 32), 0x100, 0};
    const u32 wdesc[3] = {(u32)ro, (u32)(ro >> 32), 0x100};
    if (!vk_uma()) {
        printf("skip: guest memory is copied on this device (vk_memory)\n");
        return 0;
    }
    const int dear = vk_uma_imports_dear();
    GpuChannel* channel = calloc(1, sizeof *channel);
    CHECK(channel && !alias_channel(channel, 0));
    CHECK(mem_map(USR_RO, 0x200000, MS_NORMAL, PERM_RW) == 0);
    gmmu_map(USR_GRO, USR_RO, 0x200000);
    gmmu_map(0x4e1000000ull, HEAP_BASE + 0x780000, 0x10000);
    CHECK(invalidate_write_code(USR_CODE, writer, sizeof writer / sizeof *writer));
    CHECK(!storage_write_words(ALIAS_CBUF + 0x510, desc, 8));
    CHECK(!storage_write_words(USR_CB + 0x110, wdesc, 3));
    CHECK(mesh_program(ALIAS_GCODE, reader, sizeof reader / sizeof *reader, 0));
    g_renderer->method_barrier(5); /* the program, once: no barrier for it between a store and a load below */
    u32 warm;
    CHECK(!usr_dispatch(channel, 0)); /* the pipelines made before the rounds, so a round records quickly */
    CHECK(!usr_load(channel, &warm));
    u32 got[3] = {0};
    u64 bound[VK_SREAD_N] = {0}, done[VK_SREAD_N] = {0};
    u64 c0[2][VK_SREAD_N], c1[2][VK_SREAD_N], c2[2][VK_SREAD_N];
    VkBuffer r;
    u64 o;
    memset(GP(USR_RO) + 0x1000, 0, 0x100);
    memset(GP(ALIAS_DATA) + 0x1000, 0xcd, 0x100);
    g_renderer->mem_may_have_changed();
    vk_lock();
    CHECK(vk_buf_get_strict(ro, 0x100, &r, &o)); /* a region over its window, also where imports are dear */
    vk_finish();
    vk_buf_storage_counts(c0);
    vk_unlock();
    const u32 validation = g_vk_validation_messages;
    /* the same submission: the dispatch's store in place, then the load, with the GPU busy */
    CHECK(!usr_busy());
    const u64 sub = vk_submit_number();
    CHECK(!usr_dispatch(channel, 0x5eed));
    CHECK(!usr_draw(channel));
    const int same = vk_submit_number() == sub;
    alias_words(&got[0], 1);
    /* the next submission: the store submitted alone, then the load */
    memset(GP(ALIAS_DATA) + 0x1000, 0xcd, 0x100);
    g_renderer->mem_may_have_changed();
    CHECK(!usr_dispatch(channel, 0x6eef));
    g_renderer->stream_end(); /* the store's submission goes */
    CHECK(!usr_load(channel, &got[1]));
    vk_lock();
    vk_buf_storage_counts(c1);
    vk_unlock();
    /* the store done (alias_words waited for it): a load with nothing pending over the buffer */
    memset(GP(ALIAS_DATA) + 0x1000, 0xcd, 0x100);
    g_renderer->mem_may_have_changed();
    CHECK(!usr_load(channel, &got[2]));
    const u32 messages = g_vk_validation_messages - validation;
    vk_lock();
    vk_buf_storage_counts(c2);
    vk_unlock();
    for (int k = 0; k < VK_SREAD_N; k++) {
        bound[k] = c1[0][k] - c0[0][k];
        done[k] = c2[0][k] - c1[0][k];
    }
    /* in place over the pending store, and once it is done */
    const int ok = bound[VK_SREAD_IN_PLACE] && !bound[VK_SREAD_DEAR] && done[VK_SREAD_IN_PLACE] &&
                   !done[VK_SREAD_DEAR] && same && got[0] == 0x5eed && got[1] == 0x6eef && got[2] == 0x6eef &&
                   !messages;
    CHECK(!alias_channel_end());
    CHECK(mem_unmap(USR_RO, 0x200000) == 0);
    g_renderer->mem_may_have_changed();
    free(channel);
    if (!ok)
        fprintf(stderr,
                "read-only storage over a write in place (imports dear %d, the store and the first load in one "
                "submission %d): loaded %08x, %08x and %08x (want 00005eed, 00006eef and 00006eef); over the "
                "pending store %llu in place, %llu copied as dear, %llu refused; once done %llu in place, %llu "
                "copied as dear, %llu refused; %u validation messages\n",
                dear, same, got[0], got[1], got[2], (unsigned long long)bound[VK_SREAD_IN_PLACE],
                (unsigned long long)bound[VK_SREAD_DEAR], (unsigned long long)bound[VK_SREAD_REFUSED],
                (unsigned long long)done[VK_SREAD_IN_PLACE], (unsigned long long)done[VK_SREAD_DEAR],
                (unsigned long long)done[VK_SREAD_REFUSED], messages);
    CHECK(ok);
    printf("in place: a draw's read-only storage reads a dispatch's store in place, in its submission and the next\n");
    return 0;
}

/* The order of a store in place and a later draw's load of it, where only the load's barrier (uma.c's order, from
   vk_uma_read) can give it. The dispatch counts a float down from USR_SPIN in a loop before it stores its word, about
   12 ms of the GPU's time on the Deck (it stores the count too, or the compiler drops the loop). The draw that loads
   the word is recorded right after it in the same submission: with no barrier between them, the GPU starts the draw
   while the dispatch still runs, and the draw loads the word from before the store (10 runs of 10 on the Deck). The
   validation layer's sync check reports nothing there. Each of USR_ROUNDS rounds stores another word, and every load
   must give it */
#define USR_CODE_SPIN (0x4e1000000ull + 0x400)
#define USR_SPIN      400000.0f
#define USR_ROUNDS    4
/* the word of op i of a compute program as invalidate_write_code lays the ops out here, a scheduling word first and
   before each third op after it */
static u32 usr_word(u32 op) { return 1 + op + op / 3; }
static int check_uma_storage_order(void) {
    const u64 ro = USR_GRO + 0x1000;
    const float spin = USR_SPIN, minus_one = -1.0f;
    u32 spin_bits, minus_bits;
    memcpy(&spin_bits, &spin, 4);
    memcpy(&minus_bits, &minus_one, 4);
    u64 ops[16];
    u32 n = 0;
    ops[n++] = 0x4c98078004470000ull;     /* MOV R0, c[0][0x110]: the buffer's address, NVN's descriptor */
    ops[n++] = 0x4c98078004570001ull;     /* MOV R1, c[0][0x114] */
    ops[n++] = 0x4c98078004870002ull;     /* MOV R2, c[0][0x120]: the word */
    ops[n++] = storage_mov(6, spin_bits); /* MOV32I R6, USR_SPIN */
    const u32 head = n;
    ops[n++] = 0x0800000000070000ull | (u64)minus_bits << 20 | 6u << 8 | 6u; /* FADD32I R6, R6, -1.0 */
    ops[n++] = 0x5bb403800007000full | 0xffull << 20 | 6u << 8;              /* FSETP.GT P1, R6, RZ */
    const u32 bytes = (usr_word(head) - (usr_word(n) + 1)) * 8;
    ops[n] = 0xe24000000000000full | (u64)(bytes & 0xffffff) << 20 | 1ull << 16; /* @P1 BRA head */
    n++;
    ops[n++] = 0xeedc200000070002ull; /* STG.E [R0], R2 */
    ops[n++] = 0xeedc200000470006ull; /* STG.E [R0 + 4], R6: the count used, so the compiler keeps the loop */
    ops[n++] = 0xe30000000007000full; /* EXIT */
    u64 code[32];
    u32 at = 0;
    for (u32 i = 0; i < n; i++) {
        if (!(at % 4)) code[at++] = 0x001f8000fc0007e0ull;
        code[at++] = ops[i];
    }
    const u64 reader[] = {ALIAS_MOV_R0,
                          ALIAS_MOV_R1,
                          ALIAS_MOV_R4,
                          ALIAS_MOV_R5,
                          0xeed4200000070402ull, /* LDG.E R2, [R4]: the read-only buffer */
                          0xeedc200000070002ull, /* STG.E [R0], R2: buffer 0, written */
                          ALIAS_ALD,
                          ALIAS_AST,
                          ALIAS_EXIT};
    const u32 desc[8] = {(u32)ALIAS_STORED, (u32)(ALIAS_STORED >> 32), 0x100, 0, (u32)ro, (u32)(ro >> 32), 0x100, 0};
    const u32 wdesc[3] = {(u32)ro, (u32)(ro >> 32), 0x100};
    if (!vk_uma()) {
        printf("skip: guest memory is copied on this device (vk_memory)\n");
        return 0;
    }
    GpuChannel* channel = calloc(1, sizeof *channel);
    CHECK(channel && !alias_channel(channel, 0));
    CHECK(mem_map(USR_RO, 0x200000, MS_NORMAL, PERM_RW) == 0);
    gmmu_map(USR_GRO, USR_RO, 0x200000);
    gmmu_map(0x4e1000000ull, HEAP_BASE + 0x780000, 0x10000);
    CHECK(invalidate_write_code(USR_CODE_SPIN, code, at));
    CHECK(!storage_write_words(ALIAS_CBUF + 0x510, desc, 8));
    CHECK(!storage_write_words(USR_CB + 0x110, wdesc, 3));
    CHECK(mesh_program(ALIAS_GCODE, reader, sizeof reader / sizeof *reader, 0));
    g_renderer->method_barrier(5); /* the program, once: no barrier for it between the store and the load */
    u32 warm;
    CHECK(!usr_dispatch_at(channel, USR_CODE_SPIN, 0)); /* the pipelines made before the rounds */
    CHECK(!usr_load(channel, &warm));
    u32 got[USR_ROUNDS] = {0}, same = 0, right = 0;
    const u32 validation = g_vk_validation_messages;
    for (u32 round = 0; round < USR_ROUNDS; round++) {
        VkBuffer r;
        u64 o;
        memset(GP(USR_RO) + 0x1000, 0, 0x100);
        memset(GP(ALIAS_DATA) + 0x1000, 0xcd, 0x100);
        g_renderer->mem_may_have_changed();
        vk_lock();
        CHECK(vk_buf_get_strict(ro, 0x100, &r, &o)); /* a region over its window */
        vk_finish();
        vk_unlock();
        CHECK(!usr_busy());
        const u64 sub = vk_submit_number();
        CHECK(!usr_dispatch_at(channel, USR_CODE_SPIN, 0x7e00 + round));
        CHECK(!usr_draw(channel));
        same += vk_submit_number() == sub;
        alias_words(&got[round], 1);
        right += got[round] == 0x7e00 + round;
    }
    const u32 messages = g_vk_validation_messages - validation;
    CHECK(!alias_channel_end());
    CHECK(mem_unmap(USR_RO, 0x200000) == 0);
    g_renderer->mem_may_have_changed();
    free(channel);
    if (same != USR_ROUNDS || right != USR_ROUNDS || messages)
        fprintf(stderr,
                "a draw's load after a long store in place: %u of %u rounds in one submission, %u loads gave the "
                "stored word (the first gave %08x, want %08x), %u validation messages\n",
                same, USR_ROUNDS, right, got[0], 0x7e00u, messages);
    CHECK(same == USR_ROUNDS && right == USR_ROUNDS && !messages);
    printf("in place: a draw's load waits for a long store in place before it in its submission\n");
    return 0;
}
