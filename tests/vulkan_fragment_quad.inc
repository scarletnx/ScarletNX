/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A fragment program that shuffles within its 2x2 quad and votes, on a device whose subgroups leave the fragment stage
   out (Turnip on an Adreno 610 lists compute alone): it runs each pixel as one lane alone at its place in its quad,
   and a SHFL within the quad reads the other pixel's register through fine derivatives of its 16-bit halves
   (emit_thread.c's quad_read). Such a program was not translated there, and its draws were skipped: a game's dFdx,
   SHFL.BFLY 1 then FSWZADD 0x99, lost 13% of a scene's draws.

   A triangle over half of an 8x8 target, its edge along the diagonal so that quads there have pixels outside it
   (helpers, which run the program and give their registers), loads for each pixel a 32-bit word of a table by its
   place (NaNs with payloads, infinities, subnormals, signed zeros, halves of all zeros and all ones among them) and a
   float of a second, then stores for each pixel what twelve SHFL within the quad read (IDX, UP, DOWN and BFLY of
   lanes 0 to 3) with their predicates, dFdx and dFdy of the float as the console computes them, and its lane. A
   warp-aggregated atomic counts the pixels. The draw is made with the device's subgroups and with the fragment stage
   taken out of them: every pixel's words must be what the console's lanes of that quad read. The lane and the count
   are the lone lane's (its place in the quad, its own bit) and checked in the second draw only. No extracted shaders
   or game data are used. */
#define QUAD_SIDE  8
#define QUAD_WORDS 32 /* each pixel's words in the result buffer */
#define QUAD_FORMS 12
#define QUAD_UNSET 0xa5a5a5a5u

static const struct {
    u32 mode, index; /* 0 IDX, 1 UP, 2 DOWN, 3 BFLY; the immediate lane */
} quad_forms[QUAD_FORMS] = {{3, 1}, {3, 2}, {3, 3}, {0, 0}, {0, 1}, {0, 2},
                            {0, 3}, {1, 1}, {1, 2}, {2, 1}, {2, 3}, {3, 0}};

/* the console's SHFL with mask 0x1c03 (segments of 4 lanes) at quad lane q: the lane it reads, and whether it is valid
   (PTX's shfl: an invalid read keeps the lane's own value) */
static u32 quad_source(u32 mode, u32 index, u32 q, int* valid) {
    u32 source = mode == 0 ? index & 3 : mode == 1 ? q - index : mode == 2 ? q + index : q ^ index;
    *valid = mode == 1 ? q >= index : source <= 3;
    return *valid ? source : q;
}

static u32 quad_word(u32 k) {
    static const u32 special[] = {0x7fc00001u, 0xffffffffu, 0x00000000u, 0x00000001u, 0x80000000u, 0x7f800000u,
                                  0x0000ffffu, 0xffff0000u, 0x00010000u, 0xff800000u, 0x7f7fffffu, 0x807fffffu};
    if (k < sizeof special / sizeof *special) return special[k];
    u32 x = 0x9e3779b9u * (k + 1);
    x ^= x >> 15;
    x *= 0x2c1b3c6du;
    return x ^ x >> 12;
}
static float quad_float(u32 k) { return (float)((int)(k * 37 % 61) - 30) * 0.375f + (float)(k % 7) * 1.0625f; }

static u64 quad_shfl(u32 mode, u32 index, u32 d, u32 a, u32 predicate) {
    return 0xEF10000000070000ull | 0x1c03ull << 34 | 1ull << 29 | 1ull << 28 | (u64)mode << 30 | (u64)predicate << 48 |
           (u64)index << 20 | a << 8 | d;
}
static u64 quad_stg(u32 word, u32 r) { return 0xEEDC200000070600ull | (u64)(word * 4) << 20 | r; } /* STG.E [R6+] */

/* the program at address: marker makes each pass's program a program of its own */
static int quad_program(u64 address, u32 marker) {
    u64 ops[128];
    u32 n = 0;
    ops[n++] = 0xe000000000070000ull | 0x1cull << 30 | 8; /* IPA R8, a[0x70]: the fragment's x */
    ops[n++] = 0xe000000000070000ull | 0x1dull << 30 | 9; /* IPA R9, a[0x74]: its y */
    ops[n++] = 0x5cb0018000870a00ull;                     /* F2I.U32.F32.TRUNC R0, R8 */
    ops[n++] = 0x5cb0018000970a01ull;                     /* F2I.U32.F32.TRUNC R1, R9 */
    ops[n++] = 0x3848000000370102ull;                     /* SHL R2, R1, 3 */
    ops[n++] = 0x5c10000000070202ull;                     /* IADD R2, R2, R0: the pixel's index */
    ops[n++] = 0x3848000000270203ull;                     /* SHL R3, R2, 2 */
    ops[n++] = 0xef9400200007030aull;                     /* LDC R10, c[2][R3]: its word */
    ops[n++] = 0xef9400201007030bull;                     /* LDC R11, c[2][R3 + 0x100]: its float */
    ops[n++] = 0x3848000000770206ull;                     /* SHL R6, R2, 7: its 32 words' offset */
    ops[n++] = 0x4C10800004C70606ull;                     /* IADD.CC R6, R6, c[0x0][0x130] */
    ops[n++] = 0x4C10080004D7FF07ull;                     /* IADD.X R7, RZ, c[0x0][0x134] */
    ops[n++] = storage_mov(15, marker);
    for (u32 f = 0; f < QUAD_FORMS; f++) {
        ops[n++] = quad_shfl(quad_forms[f].mode, quad_forms[f].index, 12, 10, 1); /* SHFL P1, R12, R10 */
        ops[n++] = quad_stg(2 * f, 12);
        ops[n++] = storage_mov(12, 0);
        ops[n++] = (storage_mov(12, 1) & ~(7ull << 16)) | 1ull << 16; /* @P1 MOV32I R12, 1 */
        ops[n++] = quad_stg(2 * f + 1, 12);
    }
    for (u32 axis = 0; axis < 2; axis++) { /* dFdx and dFdy: nouveau's handleDFDX */
        ops[n++] = quad_shfl(3, axis + 1, 12, 11, 7);
        ops[n++] = 0x50F8000000070000ull | (u64)(axis ? 0xa5 : 0x99) << 28 | 11ull << 20 | 12ull << 8 | 12;
        ops[n++] = quad_stg(2 * QUAD_FORMS + axis, 12);
    }
    ops[n++] = 0xF0C800000007000Dull; /* S2R R13, SR_LANEID */
    ops[n++] = quad_stg(2 * QUAD_FORMS + 2, 13);
    ops[n++] = 0x50D8E3800007000Eull; /* VOTE.ALL R14, PT, PT: the live lanes */
    ops[n++] = 0x5C30000000E70010ull; /* FLO.U32 R16, R14: the highest */
    ops[n++] = 0x5C08000000E70011ull; /* POPC R17, R14: how many */
    ops[n++] = 0x5B64038000D71007ull; /* ISETP.EQ.U32.AND P0, PT, R16, R13, PT */
    ops[n++] = 0x4C98078004870004ull; /* MOV R4, c[0x0][0x120] */
    ops[n++] = 0x4C98078004970005ull; /* MOV R5, c[0x0][0x124] */
    ops[n++] = 0xEBF9000000000411ull; /* @P0 RED.E.ADD [R4], R17 */
    ops[n++] = 0xE30000000007000Full;
    u64 code[200] = {0};
    u32 at = 10;
    code[3] = 0xff; /* screen-linear RGBA input */
    code[9] = 15;
    for (u32 i = 0; i < n; i++) {
        if (!(i % 3)) code[at++] = 0x001f8000fc0007e0ull;
        code[at++] = ops[i];
    }
    u8 bytes[sizeof code];
    for (u32 i = 0; i < at; i++) snx_st_le64(bytes + i * 8, code[i]);
    return !gmem_write(address, bytes, (u64)at * 8);
}

/* one draw of the program at offset with stages as the stages that have subgroups: 0 when every pixel the triangle
   covers holds what the console reads. lone: the fragment stage has none, and its lane and count are checked too */
static int quad_pass(GpuChannel* channel, u64 gpu, u64 cpu, u32 offset, VkShaderStageFlags stages, int lone,
                     const char* what) {
    const u64 cbuf = gpu + 0x3000, counter = gpu + 0x4000, results = gpu + 0x20000, target = gpu + 0x40000;
    static u8 unset[QUAD_SIDE * QUAD_SIDE * QUAD_WORDS * 4];
    memset(unset, 0xa5, sizeof unset);
    CHECK(quad_program(gpu + offset, 0x7300 + offset));
    CHECK(!gmem_write(counter, (const u32[1]){0}, 4));
    CHECK(!gmem_write(results, unset, sizeof unset));
    const u32 count_desc[3] = {(u32)counter, (u32)(counter >> 32), 4};
    const u32 result_desc[3] = {(u32)results, (u32)(results >> 32), sizeof unset};
    CHECK(!storage_write_words(cbuf + 0x120, count_desc, 3));
    CHECK(!storage_write_words(cbuf + 0x130, result_desc, 3));
    g_renderer->method_barrier(5);
    channel->r3d[0x851] = offset;
    vk_lock();
    rt_clear_format(channel, target, QUAD_SIDE, QUAD_SIDE, 0, 0, 0xc0); /* RGBA32F */
    channel->r3d_dirty = ~0u;
    mx_draw_state_sync(channel);
    DsState* state = &channel->ds;
    state->bind.cb_addr[ST_FS][0] = cbuf;
    state->bind.cb_size[ST_FS][0] = 0x1000;
    state->bind.cb_addr[ST_FS][2] = gpu + 0xb000;
    state->bind.cb_size[ST_FS][2] = 0x200;
    state->blend.en[0] = 0;
    state->blend.mask[0] = 15;
    VkShaderStageFlags device_stages = g_vk_subgroup_stages;
    g_vk_subgroup_stages = stages;
    u64 drawn = g_vk_drawn, skipped = g_vk_skipped;
    DrawCall draw = {.topology = 4, .count = 3, .instances = 1};
    vk_render_draw(state, &draw, &channel->owner);
    vk_render_end();
    vk_finish();
    g_vk_subgroup_stages = device_stages;
    int drew = g_vk_drawn == drawn + 1 && g_vk_skipped == skipped;
    vk_unlock();
    if (!drew) fprintf(stderr, "fragment quad, %s: the draw was skipped\n", what);
    CHECK(drew);
    u32 covered = 0, partial = 0, wrong = 0;
    for (u32 qy = 0; qy < QUAD_SIDE; qy += 2)
        for (u32 qx = 0; qx < QUAD_SIDE; qx += 2) {
            u32 inside = 0;
            for (u32 q = 0; q < 4; q++) {
                u32 x = qx + (q & 1), y = qy + (q >> 1), p = y * QUAD_SIDE + x;
                const u8* words = GP(cpu + 0x20000 + (u64)p * QUAD_WORDS * 4);
                if (snx_ld_le32(words) == QUAD_UNSET) continue;
                inside++;
                u32 expected[QUAD_WORDS], checked = 2 * QUAD_FORMS + 2 + (lone != 0);
                for (u32 f = 0; f < QUAD_FORMS; f++) {
                    int valid;
                    u32 source = quad_source(quad_forms[f].mode, quad_forms[f].index, q, &valid);
                    expected[2 * f] = quad_word((qy + (source >> 1)) * QUAD_SIDE + qx + (source & 1));
                    expected[2 * f + 1] = (u32)valid;
                }
                for (u32 axis = 0; axis < 2; axis++) {
                    u32 first = qy * QUAD_SIDE + qx + (axis ? x - qx : (y - qy) * QUAD_SIDE);
                    float derivative = quad_float(first + (axis ? QUAD_SIDE : 1)) - quad_float(first);
                    memcpy(&expected[2 * QUAD_FORMS + axis], &derivative, 4);
                }
                expected[2 * QUAD_FORMS + 2] = q;
                for (u32 k = 0; k < checked; k++) {
                    u32 got = snx_ld_le32(words + 4 * k);
                    if (got == expected[k]) continue;
                    if (!wrong++)
                        fprintf(stderr, "fragment quad, %s: pixel (%u, %u) word %u is %#x, expected %#x\n", what, x, y,
                                k, got, expected[k]);
                }
            }
            covered += inside;
            partial += inside && inside < 4;
        }
    u32 count = snx_ld_le32(GP(cpu + 0x4000));
    if (wrong || !partial || (lone && count != covered))
        fprintf(stderr, "fragment quad, %s: %u words wrong, %u pixels in %u quads the edge cuts, %u counted of %u\n",
                what, wrong, covered, partial, count, covered);
    CHECK(!wrong && partial && (!lone || count == covered));
    return 0;
}

static int check_fragment_quad(void) {
    if (!g_vk.feat.fragmentStoresAndAtomics) {
        puts("SKIP fragment quad: the device has no stores or atomics in fragment programs");
        return 0;
    }
    const u64 gpu = 0x8ec000000ull, cpu = HEAP_BASE + 0x12900000;
    const u64 stream = gpu + 0x8000, tables = gpu + 0xb000;
    CHECK(mem_map(cpu, 0x80000, MS_NORMAL, PERM_RW) == 0);
    gmmu_map(gpu, cpu, 0x80000);
    GpuChannel* channel = calloc(1, sizeof *channel);
    CHECK(channel);
    channel->id = 158;
    const u64 vertex[] = {0xefd9ff800807ff00ull, 0xeff1ff800707ff00ull, 0xe30000000007000full};
    const float position[12] = {-1, -1, 0, 1, 1, -1, 0, 1, -1, 1, 0, 1};
    CHECK(!gmem_write(stream, position, sizeof position));
    CHECK(mesh_program(gpu, vertex, sizeof vertex / sizeof *vertex, 0));
    u32 table[2 * QUAD_SIDE * QUAD_SIDE];
    for (u32 k = 0; k < QUAD_SIDE * QUAD_SIDE; k++) {
        float value = quad_float(k);
        table[k] = quad_word(k);
        memcpy(&table[QUAD_SIDE * QUAD_SIDE + k], &value, 4);
    }
    CHECK(!storage_write_words(tables, table, 2 * QUAD_SIDE * QUAD_SIDE));
    u32* r = channel->r3d;
    r[0x582] = (u32)(gpu >> 32);
    r[0x583] = (u32)gpu;
    r[0x810] = r[0x850] = 1;
    r[0x64b] = 1;
    r[0x286] = 0x6420;
    r[0x3fd] = r[0x3fe] = QUAD_SIDE << 16;
    r[0x680] = 0x1111;
    r[MAXWELL3D_RASTERIZE_ENABLE] = 1;
    all_samples(r);
    const float transform[6] = {QUAD_SIDE / 2, QUAD_SIDE / 2, 0.5f, QUAD_SIDE / 2, QUAD_SIDE / 2, 0.5f};
    memcpy(r + 0x280, transform, sizeof transform);
    r[0x458] = 1u << 21 | 7u << 27;
    r[0x700] = 0x1000 | 16;
    r[0x701] = (u32)(stream >> 32);
    r[0x702] = (u32)stream;
    r[0x7c0] = (u32)((stream + sizeof position - 1) >> 32);
    r[0x7c1] = (u32)(stream + sizeof position - 1);
    const VkShaderStageFlags without = g_vk_subgroup_stages & ~(VkShaderStageFlags)VK_SHADER_STAGE_FRAGMENT_BIT;
    int has_fragment = (g_vk_subgroup_stages & VK_SHADER_STAGE_FRAGMENT_BIT) != 0;
    CHECK(!quad_pass(channel, gpu, cpu, 0x1000, g_vk_subgroup_stages, !has_fragment,
                     has_fragment ? "the device's fragment subgroups" : "a device without fragment subgroups"));
    CHECK(!quad_pass(channel, gpu, cpu, 0x2000, without, 1, "the fragment stage taken out of the subgroups"));
    free(channel);
    printf("PASS fragment quad: twelve SHFL within a quad, dFdx and dFdy read what the console's lanes read, helpers "
           "on the triangle's edge included, %s and as one lane alone at its place in the quad\n",
           has_fragment ? "with the device's fragment subgroups" : "on a device without fragment subgroups");
    return 0;
}
