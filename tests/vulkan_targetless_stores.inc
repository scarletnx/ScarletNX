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
    CHECK(mesh_program(address + 0x1000, store, 5, 1));
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
    CHECK(mesh_program(address + 0x1000, nothing, 1, 1));
    maxwell3d_method(channel, 0x369, 1, 1); /* the program's memory changed */
    channel->r3d_dirty = ~0u;
    targetless_draw(channel, &drawn, &skipped);
    CHECK(!drawn && skipped);
    puts("targetless draws: a fragment program's store runs, a program with no store is skipped");
    free(channel);
    return 0;
}
