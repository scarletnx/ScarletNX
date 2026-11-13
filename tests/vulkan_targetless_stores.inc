/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */

/* draws a triangle over the 4x4 viewport with four color targets of format 0 and no depth, the fragment program
   at code + 0x1000; *drawn and *skipped are whether the renderer recorded or skipped it */
static void targetless_draw(GpuChannel* channel, int* drawn, int* skipped) {
    u64 recorded = g_vk_drawn, dropped = g_vk_skipped;
    vk_lock();
    mx_draw_state_sync(channel);
    DrawCall call = {.topology = 4, .count = 3, .instances = 1};
    vk_render_draw(&channel->ds, &call, &channel->owner);
    vk_render_end();
    vk_unlock();
    *drawn = g_vk_drawn == recorded + 1;
    *skipped = g_vk_skipped == dropped + 1;
}

/* A per-pixel pass may keep only what its fragment program stores, every color target's format 0 and depth off:
   the console runs it, and skipping it leaves the memory as the CPU wrote it. A program that stores nothing there
   still has nothing to draw */
static int check_targetless_stores(void) {
    const u64 address = 0x490000000ull, cpu = HEAP_BASE + 0x760000;
    const u64 positions = address + 0x2000, cbuf = address + 0x3000, stored = address + 0x4000;
    const u64 vertex[] = {0xefd9ff800807ff00ull, /* ALD R0..3, a[0x80] */
                          0xeff1ff800707ff00ull, /* position */
                          0xe30000000007000full};
    const u64 store[] = {0x4c98078014470000ull,  /* MOV R0, c[0][0x510]: the buffer's address, NVN's descriptor */
                         0x4c98078014570001ull,  /* MOV R1, c[0][0x514] */
                         storage_mov(2, 0x5eed), /* MOV32I R2, 0x5eed */
                         0xeedc200000070002ull,  /* STG.E [R0], R2 */
                         0xe30000000007000full};
    const u64 nothing[] = {0xe30000000007000full};
    const float position[12] = {-1, -1, 0, 1, 3, -1, 0, 1, -1, 3, 0, 1};
    const float transform[6] = {2, 2, 0.5f, 2, 2, 0.5f};
    const u32 descriptor[3] = {(u32)stored, (u32)(stored >> 32), 16};
    u8 bytes[48];
    int drawn, skipped;
    GpuChannel* channel = calloc(1, sizeof *channel);
    CHECK(channel);
    channel->id = 125;
    gmmu_map(address, cpu, 0x8000);
    for (u32 i = 0; i < 12; i++) {
        u32 word;
        memcpy(&word, position + i, 4);
        snx_st_le32(bytes + 4 * i, word);
    }
    CHECK(!gmem_write(positions, bytes, sizeof bytes));
    CHECK(!storage_write_words(cbuf + 0x510, descriptor, 3));
    CHECK(!storage_write_words(stored, (const u32[1]){0xdeadbeef}, 1));
    CHECK(mesh_program(address, vertex, 3, 0));
    /* These fragment programs have no inputs or color outputs: their effects are only storage writes. */
    CHECK(mesh_program(address + 0x1000, store, 5, 0));
    u32* r = channel->r3d;
    r[0x582] = (u32)(address >> 32);
    r[0x583] = (u32)address;
    r[0x850] = 1;
    r[0x851] = 0x1000;
    r[0x64b] = 1;
    r[0x286] = 0x6420;
    r[0x3fd] = r[0x3fe] = 4u << 16;
    r[0x680] = 0x1111;
    r[MAXWELL3D_RASTERIZE_ENABLE] = 1; /* as reset_3d starts a channel */
    all_samples(r);
    memcpy(r + 0x280, transform, sizeof transform);
    r[0x458] = 1u << 21 | 7u << 27; /* RGBA32F */
    r[0x700] = 0x1000 | 16;
    r[0x701] = (u32)(positions >> 32);
    r[0x702] = (u32)positions;
    r[0x7c0] = (u32)((positions + 47) >> 32);
    r[0x7c1] = (u32)(positions + 47);
    r[0x487] = 0x0fac6884; /* four targets, slot i on target i */
    for (u32 i = 0; i < 4; i++) {
        u64 a = address + 0x5000 + i * 0x400;
        r[0x200 + i * 0x10] = (u32)(a >> 32);
        r[0x201 + i * 0x10] = (u32)a;
        r[0x202 + i * 0x10] = 4;
        r[0x203 + i * 0x10] = 4;
        r[0x204 + i * 0x10] = 0; /* no format: nothing is written there */
    }
    r[0x54e] = 0; /* depth off */
    channel->ds.bind.cb_addr[4][0] = cbuf;
    channel->ds.bind.cb_size[4][0] = 0x1000;
    channel->r3d_dirty = ~0u;
    targetless_draw(channel, &drawn, &skipped);
    u8 word[4];
    CHECK(!gmem_read(stored, word, sizeof word));
    if (!drawn || skipped || snx_ld_le32(word) != 0x5eed)
        fprintf(stderr, "targetless store: drawn %d, skipped %d, the buffer holds %08x, expected 00005eed\n", drawn,
                skipped, snx_ld_le32(word));
    CHECK(drawn && !skipped && snx_ld_le32(word) == 0x5eed);
    CHECK(mesh_program(address + 0x1000, nothing, 1, 0));
    maxwell3d_method(channel, 0x369, 1, 1); /* the program's memory changed */
    channel->r3d_dirty = ~0u;
    targetless_draw(channel, &drawn, &skipped);
    CHECK(!drawn && skipped);
    /* A store into a buffer image, as a base pass writes its texture feedback: SUST.P on dimension 1 through header 0
       of the pool (R32_UINT, 8 texels), its handle in c[0][0x120]. Only texel 2 changes */
    const u64 pool = address + 0x6000, texels = address + 0x6100;
    const u64 buffer_store[] = {storage_mov(15, 2),      /* MOV32I R15, 2: the texel */
                                storage_mov(24, 0x5eed), /* MOV32I R24, 0x5eed */
                                0x4c9807800487001cull,   /* MOV R28, c[0][0x120] */
                                0xeb200e0200f70f18ull,   /* SUST.P.RGBA [R15], R24, R28 */
                                0xe30000000007000full};
    const u32 uint_word = 0x0f | 4u << 7 | 4u << 10 | 4u << 13 | 4u << 16;
    const u32 header[8] = {uint_word, (u32)texels, (u32)(texels >> 32), 0, 7u | 6u << 23, 0, 0, 0};
    u32 before[8];
    u8 after[32];
    for (u32 i = 0; i < 8; i++) before[i] = 0xdead0000u | i;
    CHECK(!storage_write_words(pool, header, 8));
    CHECK(!storage_write_words(cbuf + 0x120, (const u32[1]){0}, 1));
    CHECK(!storage_write_words(texels, before, 8));
    CHECK(mesh_program(address + 0x1000, buffer_store, 5, 0));
    maxwell3d_method(channel, 0x369, 1, 1);
    r[0x55D] = (u32)(pool >> 32), r[0x55E] = (u32)pool, r[0x55F] = 0;
    channel->r3d_dirty = ~0u;
    targetless_draw(channel, &drawn, &skipped);
    CHECK(!gmem_read(texels, after, sizeof after));
    int same = 1;
    for (u32 i = 0; i < 8; i++) same &= snx_ld_le32(after + 4 * i) == (i == 2 ? 0x5eed : before[i]);
    if (!drawn || skipped || !same)
        fprintf(stderr,
                "buffer image store: drawn %d, skipped %d, texel 2 holds %08x, expected 00005eed; texel 3 %08x, "
                "expected %08x\n",
                drawn, skipped, snx_ld_le32(after + 8), snx_ld_le32(after + 12), before[3]);
    CHECK(drawn && !skipped && same);
    /* The same store beside a 2D image slot with nothing bound, header 1 of the pool empty, whose store no fragment
       takes (c[0][0x128] is 0): the draw runs with an empty image there, as a dispatch does */
    const u64 beside_empty[] = {storage_mov(15, 2),      /* MOV32I R15, 2: the texel */
                                storage_mov(24, 0x5eed), /* MOV32I R24, 0x5eed */
                                0x4c9807800487001cull,   /* MOV R28, c[0][0x120] */
                                0x4c98078004a70005ull,   /* MOV R5, c[0][0x128] */
                                0x5b6b03800ff70507ull,   /* ISETP.NE P0, R5, RZ */
                                0x4c98078004970006ull,   /* MOV R6, c[0][0x124] */
                                0xeb200e0200f70f18ull,   /* SUST.P.RGBA [R15], R24, R28 */
                                0xeb20030600f00f18ull,   /* @P0 SUST.P.RGBA.2D [R15], R24, R6 */
                                0xe30000000007000full};
    CHECK(!storage_write_words(pool + 0x20, (const u32[8]){0}, 8));
    CHECK(!storage_write_words(cbuf + 0x124, (const u32[2]){1, 0}, 2));
    CHECK(!storage_write_words(texels, before, 8));
    CHECK(mesh_program(address + 0x1000, beside_empty, 9, 0));
    maxwell3d_method(channel, 0x369, 1, 1);
    r[0x55F] = 1;
    channel->r3d_dirty = ~0u;
    targetless_draw(channel, &drawn, &skipped);
    CHECK(!gmem_read(texels, after, sizeof after));
    same = 1;
    for (u32 i = 0; i < 8; i++) same &= snx_ld_le32(after + 4 * i) == (i == 2 ? 0x5eed : before[i]);
    if (!drawn || skipped || !same)
        fprintf(stderr,
                "store beside an empty image slot: drawn %d, skipped %d, texel 2 holds %08x, expected 00005eed\n",
                drawn, skipped, snx_ld_le32(after + 8));
    CHECK(drawn && !skipped && same);
    puts("targetless draws: a fragment program's store runs, into memory or a buffer image, also beside an empty image "
         "slot; a program with no store is skipped");
    free(channel);
    return 0;
}

/* deko3d's vertex stage keeps its storage buffers' descriptors {address, size} in c[0] from 0xb0 (driver_constbuf.h:
   the draw's 16 bytes, then the stage's 32 texture and 8 image handles), below NVN's 0x110. A vertex program that reads
   word 32 of the first and stores it into the second, as uam compiles them: its draw has no target, so the store is
   all it leaves. The vertex program wasn't translated and the draw was skipped */
static int check_deko_vertex_storage(void) {
    const u64 address = 0x491000000ull, cpu = HEAP_BASE + 0x5480000;
    const u64 positions = address + 0x2000, cbuf = address + 0x3000, source = address + 0x4000;
    const u64 stored = address + 0x4800;
    const u64 vertex[] = {0xefd9ff800807ff00ull, /* ALD R0..3, a[0x80] */
                          0xef9500000b07ff08ull, /* LDC.64 R8, c[0][0xb0]: the first storage buffer */
                          0xeed420000807080cull, /* LDG.E R12, [R8+0x80]: its word 32 */
                          0xef9500000c07ff0aull, /* LDC.64 R10, c[0][0xc0]: the second */
                          0xeedc200000070a0cull, /* STG.E [R10], R12 */
                          0xeff1ff800707ff00ull, /* position */
                          0xe30000000007000full};
    const u64 nothing[] = {0xe30000000007000full};
    const float position[12] = {-1, -1, 0, 1, 3, -1, 0, 1, -1, 3, 0, 1};
    const float transform[6] = {2, 2, 0.5f, 2, 2, 0.5f};
    const u32 descriptors[8] = {(u32)source, (u32)(source >> 32), 0x100, 0, (u32)stored, (u32)(stored >> 32), 16, 0};
    u32 words[64];
    u8 bytes[48];
    int drawn, skipped;
    GpuChannel* channel = calloc(1, sizeof *channel);
    CHECK(channel);
    channel->id = 126;
    CHECK(mem_map(cpu, 0x8000, MS_NORMAL, PERM_RW) == 0); /* past the heap */
    gmmu_map(address, cpu, 0x8000);
    for (u32 i = 0; i < 12; i++) {
        u32 word;
        memcpy(&word, position + i, 4);
        snx_st_le32(bytes + 4 * i, word);
    }
    for (u32 i = 0; i < 64; i++) words[i] = 0xdead0000u | i;
    words[32] = 0x5eedb0b0;
    CHECK(!gmem_write(positions, bytes, sizeof bytes));
    CHECK(!storage_write_words(cbuf + 0xb0, descriptors, 8));
    CHECK(!storage_write_words(source, words, 64));
    CHECK(!storage_write_words(stored, (const u32[1]){0xdeadbeef}, 1));
    CHECK(mesh_program(address, vertex, 7, 0));
    CHECK(mesh_program(address + 0x1000, nothing, 1, 0));
    u32* r = channel->r3d;
    r[0x582] = (u32)(address >> 32);
    r[0x583] = (u32)address;
    r[0x850] = 1;
    r[0x851] = 0x1000;
    r[0x64b] = 1;
    r[0x286] = 0x6420;
    r[0x3fd] = r[0x3fe] = 4u << 16;
    r[0x680] = 0x1111;
    r[MAXWELL3D_RASTERIZE_ENABLE] = 1;
    all_samples(r);
    memcpy(r + 0x280, transform, sizeof transform);
    r[0x458] = 1u << 21 | 7u << 27; /* RGBA32F */
    r[0x700] = 0x1000 | 16;
    r[0x701] = (u32)(positions >> 32);
    r[0x702] = (u32)positions;
    r[0x7c0] = (u32)((positions + 47) >> 32);
    r[0x7c1] = (u32)(positions + 47);
    r[0x487] = 0x0fac6884;
    for (u32 i = 0; i < 4; i++) {
        u64 a = address + 0x5000 + i * 0x400;
        r[0x200 + i * 0x10] = (u32)(a >> 32);
        r[0x201 + i * 0x10] = (u32)a;
        r[0x202 + i * 0x10] = 4;
        r[0x203 + i * 0x10] = 4;
        r[0x204 + i * 0x10] = 0; /* no format: nothing is written there */
    }
    r[0x54e] = 0; /* depth off */
    channel->ds.bind.cb_addr[0][0] = cbuf;
    channel->ds.bind.cb_size[0][0] = 0x1000;
    channel->r3d_dirty = ~0u;
    targetless_draw(channel, &drawn, &skipped);
    u8 word[4];
    CHECK(!gmem_read(stored, word, sizeof word));
    if (!drawn || skipped || snx_ld_le32(word) != 0x5eedb0b0)
        fprintf(stderr,
                "deko3d vertex storage: drawn %d, skipped %d, the second buffer holds %08x, expected 5eedb0b0\n", drawn,
                skipped, snx_ld_le32(word));
    CHECK(drawn && !skipped && snx_ld_le32(word) == 0x5eedb0b0);
    puts("deko3d vertex storage: a vertex program reads word 32 of its buffer at c[0][0xb0] and stores it through the "
         "one at c[0][0xc0]");
    free(channel);
    return 0;
}
