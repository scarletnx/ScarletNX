/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Dynamic handle tables preserve multisample image types. Pixel x selects table entry x (a 16-byte stride),
   alternating two different textures, and TLD.B.MS fetches each of their four samples without filtering. */
static int ms_indexed_fetch(GpuChannel* ch, u64 base, const u32 tic[8], const u8* samples) {
    if (!g_shader_texture_arrays) {
        puts("SKIP indexed multisample fetch: non-uniform sampled image arrays are unavailable");
        return 0;
    }
    u32* r = ch->r3d;
    const u64 pool = base + 0x1c000, samplers = pool + 0x100, cbuf = pool + 0x200;
    const u64 target = base + 0x1d000, second = base + 0x1e000;
    u8 bytes[512] = {0}, pixels[64], alternate[256];
    int block_linear = ((tic[2] >> 21) & 7) == 3;
    for (u32 i = 0; i < 256; i++) alternate[i] = samples[i] ^ 0xff;
    for (u32 y = 0; y < 8; y++)
        for (u32 x = 0; x < 8; x++) memcpy((u8*)ms_texel(bytes, block_linear, x, y), alternate + y * 32 + x * 4, 4);
    CHECK(!gmem_write(second, bytes, block_linear ? 512 : 256));
    u32 other_tic[8];
    memcpy(other_tic, tic, sizeof other_tic);
    other_tic[1] = (u32)second;
    other_tic[2] = (other_tic[2] & 0xffffff00u) | (u32)(second >> 32);
    for (u32 i = 0; i < 8; i++) {
        snx_st_le32(bytes + i * 4, tic[i]);
        snx_st_le32(bytes + 32 + i * 4, other_tic[i]);
    }
    CHECK(!gmem_write(pool, bytes, 64));
    memset(bytes, 0, 256);
    CHECK(!gmem_write(samplers, bytes, 32));
    for (u32 i = 0; i < 4; i++) snx_st_le32(bytes + 0x40 + i * 16, i & 1);
    CHECK(!gmem_write(cbuf, bytes, 256));
    r[0x55d] = (u32)(pool >> 32), r[0x55e] = (u32)pool, r[0x55f] = 1;
    r[0x557] = (u32)(samplers >> 32), r[0x558] = (u32)samplers, r[0x559] = 0;
    r[0x48d] = 0;
    r[0x982] = 2;
    ch->ds.bind.cb_addr[4][2] = cbuf;
    ch->ds.bind.cb_size[4][2] = 256;
    r[0x574] = 0;
    r[0x200] = (u32)(target >> 32), r[0x201] = (u32)target, r[0x202] = 16, r[0x203] = 4, r[0x204] = 0xd5;
    r[0x205] = 0x1000, r[0x206] = 1, r[0x207] = 0;
    for (u32 sample = 0; sample < 4; sample++) {
        u64 code = base + 0x11200 + sample * 0x200;
        const u64 ops[] = {0xe000000000070002ull | 0x1cull << 30, /* IPA R2, a[0x70] */
                           0xe000000000070003ull | 0x1dull << 30, /* IPA R3, a[0x74] */
                           0x5cb0000000070002ull | 3ull << 39 | 2ull << 20 | 2ull << 10 | 2ull << 8,
                           0x5cb0000000070003ull | 3ull << 39 | 3ull << 20 | 2ull << 10 | 2ull << 8,
                           0x3848000000470205ull, /* SHL R5, R2, 4 */
                           0xef94002004070504ull, /* LDC R4, c[2][R5 + 0x40] */
                           0x0100000000071005ull | (u64)sample << 20,
                           0xdd380007a0470200ull | 1ull << 50, /* TLD.B.MS R0..R3, R2, handle R4, sample R5 */
                           0xe30000000007000full};
        CHECK(index_program(code, ops, sizeof ops / sizeof *ops, 1));
        r[0x851] = (u32)(code - base);
        DrawCall call = {.topology = 4, .first = 3, .count = 3, .instances = 1};
        u64 drawn = g_vk_drawn;
        vk_lock();
        ch->r3d_dirty = ~0u;
        mx_draw_state_sync(ch);
        vk_render_draw(&ch->ds, &call, &ch->owner);
        vk_render_end();
        CHECK(g_vk_drawn == drawn + 1);
        Surf* output = vk_rt_find(target);
        CHECK(output && !output->msaa && !ms_read_pixels(output, 4, 4, pixels));
        vk_unlock();
        for (u32 y = 0; y < 4; y++)
            for (u32 x = 0; x < 4; x++) {
                const u8* want = (x & 1 ? alternate : samples) + (2 * y + sample / 2) * 32 + (2 * x + sample % 2) * 4;
                if (memcmp(pixels + (y * 4 + x) * 4, want, 4)) {
                    fprintf(stderr, "indexed multisample fetch: table entry %u sample %u pixel (%u,%u) differs\n", x,
                            sample, x, y);
                    return 1;
                }
            }
    }
    r[0x851] = 0x10000;
    puts("PASS indexed multisample fetch: two dynamic textures, four table entries and four samples exact=1");
    return 0;
}
