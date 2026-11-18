/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Constant buffers by address (vk_cb_addr_mask): with descriptor heaps, a slot that a shader reads only at constant
   offsets is mapped by its device address, and an address has no range. The console's GPU reads zeros past a bank's
   bound size, so:
   - a slot read at an offset from a register (LDC c[2][R3]) must keep its descriptor and its robust range. By
     address, cb_pad gives it a 64 KiB block, and a read past 64 KiB reads the block that the draw stages after it
     (bank 3's). The count of slots written by address (cb_addresses) must also leave bank 2 out;
   - a slot read at constant offsets past its bound size (c[3][0x180], bound 0x40) must read zeros, which cb_pad
     gives by a copy of the bound bytes into a block of zeros. Without it, the read gets the bytes after the binding:
     there, bank 4's copy, which the draw stages next (bound 0x400, read at constant offsets inside it).
   Guest memory after each bound bank holds sentinels, so a read with no bound gets one. A fragment program stores
   what it read, through a global store (STG) to a result buffer that c[0][0x130] names. The offsets that bank 2 is
   read at come from bank 1, so no pass of the translator can fold them to constants. No extracted shaders or game
   data are used. */
#define CBA_SIDE  4

static const u32 cba_bank2[] = {0x0,    0x4,    0x80,    0xfc,    0x100,   0x104,   0x140,   0x1fc,   0x200,
                                0x1000, 0x8000, 0xfff0,  0xfffc,  0x10000, 0x10004, 0x10040, 0x10100, 0x18000};
static const u32 cba_bank3[] = {0x0, 0x3c, 0x40, 0x180};
static const u32 cba_bank1[] = {0x0, 0x3c};
static const u32 cba_bank4[] = {0x0, 0x3f0};
#define CBA_N2      (u32)(sizeof cba_bank2 / sizeof *cba_bank2)
#define CBA_N3      (u32)(sizeof cba_bank3 / sizeof *cba_bank3)
#define CBA_N1      (u32)(sizeof cba_bank1 / sizeof *cba_bank1)
#define CBA_N4      (u32)(sizeof cba_bank4 / sizeof *cba_bank4)
#define CBA_WORDS   (CBA_N2 + CBA_N3 + CBA_N1 + CBA_N4)
#define CBA_BY_ADDRESS 4u /* banks 0, 1, 3 and 4: read only at constant offsets. Bank 2 keeps its descriptor */
#define CBA_TABLE   0x40u /* where bank 1 holds the offsets that bank 2 is read at */
#define CBA_SPAN    0x20000u /* guest bytes written for bank 2: its bound bytes, then sentinels past 64 KiB */

static const u32 cba_size[5] = {0x1000, 0x100, 0x100, 0x40, 0x400}; /* the bound size of each bank */
/* the reads, in the order the program stores them */
static const struct {
    u32 bank, count;
    const u32* off;
} cba_reads[4] = {{2, CBA_N2, cba_bank2}, {3, CBA_N3, cba_bank3}, {1, CBA_N1, cba_bank1}, {4, CBA_N4, cba_bank4}};

static u32 cba_value(u32 bank, u32 off) { return 0xb0000000u | bank << 24 | off; }
static u32 cba_sentinel(u32 bank, u32 off) { return 0x5e000000u | bank << 20 | (off & 0xfffffu); }
/* what a read of bank at off gives: the guest's word inside its bound size, then the guest's next bytes up to the end
   of the range that the renderer binds (whole robust steps, vk_cb_range), then zeros as on the console */
static u32 cba_expected(u32 bank, u32 off) {
    if (off < cba_size[bank]) return cba_value(bank, off);
    if (off < vk_cb_range(cba_size[bank])) return cba_sentinel(bank, off);
    return 0;
}

static u64 cba_mov_c(u32 d, u32 bank, u32 off) { /* MOV Rd, c[bank][off] */
    return 0x4C98078000070000ull | (u64)bank << 34 | (u64)(off / 4) << 20 | d;
}
static u64 cba_ldc(u32 d, u32 bank, u32 a) { /* LDC.32 Rd, c[bank][Ra] */
    return 0xEF94000000070000ull | (u64)bank << 36 | (u64)a << 8 | d;
}
static u64 cba_stg(u32 word, u32 r) { return 0xEEDC200000070600ull | (u64)(word * 4) << 20 | r; } /* STG.E [R6+] */

static int cba_program(u64 address) {
    u64 ops[96];
    u32 n = 0, word = 0;
    ops[n++] = cba_mov_c(6, 0, 0x130); /* the result buffer's address */
    ops[n++] = cba_mov_c(7, 0, 0x134);
    for (u32 k = 0; k < CBA_N2; k++) {
        ops[n++] = cba_mov_c(3, 1, CBA_TABLE + 4 * k); /* the offset, from bank 1 */
        ops[n++] = cba_ldc(10, 2, 3);
        ops[n++] = cba_stg(word++, 10);
    }
    for (u32 g = 1; g < 4; g++) /* the banks read at constant offsets */
        for (u32 k = 0; k < cba_reads[g].count; k++) {
            ops[n++] = cba_mov_c(10, cba_reads[g].bank, cba_reads[g].off[k]);
            ops[n++] = cba_stg(word++, 10);
        }
    ops[n++] = 0xE30000000007000Full; /* EXIT */
    CHECK(n <= sizeof ops / sizeof *ops);
    u64 code[160] = {0};
    u32 at = 10;
    code[3] = 0xff; /* screen-linear RGBA input */
    code[9] = 15;
    for (u32 i = 0; i < n; i++) {
        if (!(i % 3)) code[at++] = 0x001f8000fc0007e0ull;
        code[at++] = ops[i];
    }
    CHECK(at <= sizeof code / sizeof *code);
    u8 bytes[sizeof code];
    for (u32 i = 0; i < at; i++) snx_st_le64(bytes + i * 8, code[i]);
    CHECK(!gmem_write(address, bytes, (u64)at * 8));
    return 0;
}

/* bank's bound bytes, then sentinels, span bytes in all at address; bank 1 holds the offsets of bank 2 at CBA_TABLE */
static int cba_bank_write(u64 address, u32 bank, u32 span) {
    static u8 bytes[CBA_SPAN];
    CHECK(span <= CBA_SPAN);
    for (u32 off = 0; off < span; off += 4) {
        u32 v = off < cba_size[bank] ? cba_value(bank, off) : cba_sentinel(bank, off);
        if (bank == 1 && off >= CBA_TABLE && off < CBA_TABLE + 4 * CBA_N2) v = cba_bank2[(off - CBA_TABLE) / 4];
        snx_st_le32(bytes + off, v);
    }
    CHECK(!gmem_write(address, bytes, span));
    return 0;
}

static int check_cb_address(void) {
    if (!g_vk.has_descriptor_heaps) {
        puts("SKIP constant buffers by address: selected device backend is not descriptor heaps");
        return 0;
    }
    if (!g_vk.cb_address) {
        puts("SKIP constant buffers by address: the vk_cb_address setting is off");
        return 0;
    }
    if (!g_vk.feat.fragmentStoresAndAtomics) {
        puts("SKIP constant buffers by address: the device has no stores in fragment programs");
        return 0;
    }
    for (u32 bank = 0; bank < 5; bank++)
        if (vk_cbuf_type(bank) != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) {
            printf("SKIP constant buffers by address: slot %u is a storage buffer on this device\n", bank);
            return 0;
        }
    const u64 gpu = 0x8ee000000ull, cpu = HEAP_BASE + 0x12a00000;
    const u64 fs = 0x1000, cb0 = gpu + 0x3000, stream = gpu + 0x8000, results = gpu + 0x48000,
              target = gpu + 0x50000;
    const u64 bank_at[5] = {cb0, gpu + 0x10000, gpu + 0x20000, gpu + 0x12000, gpu + 0x14000};
    CHECK(mem_map(cpu, 0x80000, MS_NORMAL, PERM_RW) == 0);
    gmmu_map(gpu, cpu, 0x80000);
    GpuChannel* channel = calloc(1, sizeof *channel);
    CHECK(channel);
    channel->id = 159;
    const u64 vertex[] = {0xefd9ff800807ff00ull, 0xeff1ff800707ff00ull, 0xe30000000007000full};
    const float position[12] = {-1, -1, 0, 1, 1, -1, 0, 1, -1, 1, 0, 1};
    CHECK(!gmem_write(stream, position, sizeof position));
    CHECK(mesh_program(gpu, vertex, sizeof vertex / sizeof *vertex, 0));
    CHECK(!cba_program(gpu + fs));
    /* bank 2's sentinels go past 64 KiB, so its reads there also find guest sentinels after the bank */
    for (u32 bank = 1; bank < 5; bank++) CHECK(!cba_bank_write(bank_at[bank], bank, bank == 2 ? CBA_SPAN : 0x1000));
    static u8 unset[CBA_WORDS * 4];
    memset(unset, 0xa5, sizeof unset);
    CHECK(!gmem_write(results, unset, sizeof unset));
    const u32 result_desc[3] = {(u32)results, (u32)(results >> 32), sizeof unset};
    CHECK(!storage_write_words(cb0 + 0x130, result_desc, 3));
    u32* r = channel->r3d;
    r[0x582] = (u32)(gpu >> 32);
    r[0x583] = (u32)gpu;
    r[0x810] = r[0x850] = 1;
    r[0x851] = fs;
    r[0x64b] = 1;
    r[0x286] = 0x6420;
    r[0x3fd] = r[0x3fe] = CBA_SIDE << 16;
    r[0x680] = 0x1111;
    r[MAXWELL3D_RASTERIZE_ENABLE] = 1;
    all_samples(r);
    const float transform[6] = {CBA_SIDE / 2, CBA_SIDE / 2, 0.5f, CBA_SIDE / 2, CBA_SIDE / 2, 0.5f};
    memcpy(r + 0x280, transform, sizeof transform);
    r[0x458] = 1u << 21 | 7u << 27;
    r[0x700] = 0x1000 | 16;
    r[0x701] = (u32)(stream >> 32);
    r[0x702] = (u32)stream;
    r[0x7c0] = (u32)((stream + sizeof position - 1) >> 32);
    r[0x7c1] = (u32)(stream + sizeof position - 1);
    g_renderer->method_barrier(5);
    VkDescriptorHeapStats before, after;
    vk_lock();
    rt_clear_format(channel, target, CBA_SIDE, CBA_SIDE, 0, 0, 0xc0); /* RGBA32F */
    channel->r3d_dirty = ~0u;
    mx_draw_state_sync(channel);
    DsState* state = &channel->ds;
    for (u32 bank = 0; bank < 5; bank++) {
        state->bind.cb_addr[ST_FS][bank] = bank_at[bank];
        state->bind.cb_size[ST_FS][bank] = cba_size[bank];
    }
    state->blend.en[0] = 0;
    state->blend.mask[0] = 15;
    vk_descriptor_heaps_get_stats(&before);
    u64 drawn = g_vk_drawn, skipped = g_vk_skipped;
    DrawCall draw = {.topology = 4, .count = 3, .instances = 1};
    vk_render_draw(state, &draw, &channel->owner);
    vk_render_end();
    vk_finish();
    vk_descriptor_heaps_get_stats(&after);
    int drew = g_vk_drawn == drawn + 1 && g_vk_skipped == skipped;
    vk_unlock();
    if (!drew) fprintf(stderr, "constant buffers by address: the draw was skipped\n");
    CHECK(drew);
    u32 wrong = 0, word = 0;
    const u8* got = GP(cpu + (results - gpu));
    for (u32 g = 0; g < 4; g++)
        for (u32 k = 0; k < cba_reads[g].count; k++, word++) {
            u32 bank = cba_reads[g].bank, off = cba_reads[g].off[k];
            u32 value = snx_ld_le32(got + 4 * word), expected = cba_expected(bank, off);
            if (value == expected) continue;
            wrong++;
            fprintf(stderr, "constant buffers by address: c[%u][%#x] (bound %#x) read %#x, expected %#x\n", bank, off,
                    cba_size[bank], value, expected);
        }
    u64 addresses = after.cb_addresses - before.cb_addresses;
    if (addresses != CBA_BY_ADDRESS)
        fprintf(stderr, "constant buffers by address: %llu written by address, expected %u (banks 0, 1, 3 and 4)\n",
                (unsigned long long)addresses, CBA_BY_ADDRESS);
    CHECK(!wrong && addresses == CBA_BY_ADDRESS);
    free(channel);
    printf("PASS constant buffers by address: %llu slots by address, a register-indexed bank read zeros past its "
           "bound size and past 64 KiB, a constant read past a short bank read the zeros of its padded block "
           "(robust step %u)\n",
           (unsigned long long)addresses, vk_cb_step());
    return 0;
}
