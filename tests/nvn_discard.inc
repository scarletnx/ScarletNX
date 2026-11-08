/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Real command recording and copied-record replay, with a renderer that observes discard hints but keeps pixels. */
static struct {
    u64 event[16][4]; /* kind/mask, color 0, color 1, depth */
    u32 count;
} discard_test;

static void discard_event(const DsState* ds, u32 kind, u32 mask) {
    if (discard_test.count >= 16) {
        check(0, "discard callback count stays bounded");
        return;
    }
    u64* e = discard_test.event[discard_test.count++];
    e[0] = (u64)kind << 32 | mask;
    e[1] = ds->tgt.color[0].gva;
    e[2] = ds->tgt.color[1].gva;
    e[3] = ds->tgt.depth.gva;
}
static void discard_draw(const DsState* ds, const DrawCall* dc, DrawOwner* owner) {
    (void)dc;
    (void)owner;
    discard_event(ds, 0, 0);
}
static void discard_hint(const DsState* ds, u32 mask) { discard_event(ds, 1, mask); }

static void test_discard_commands(void) {
    enum { CB_A = 0xa400, CB_B = 0xa600, OBJECTS = 0xb000, COLORS = 0xc000, VIEW = 0xc100, VIEWS = 0xc120 };
    enum { A, B, Z, ARRAY, PADDED, LINEAR, LINEAR_PADDED, VOLUME, MS_COLOR, MS_DEPTH, TEXTURES };
    const u64 gpu[TEXTURES] = {0x600000, 0x610000, 0x620000, 0x630000, 0x640000,
                              0x650000, 0x660000, 0x670000, 0x680000, 0x690000};
    for (u32 i = 0; i < TEXTURES; i++) {
        NvnTexture t = {.magic = NVN_MAGIC('T', 'E', 'X', 0), .gpu = gpu[i]};
        t.d = (NvnTexDesc){.format = NVN_FORMAT_RGBA8,
                           .flags = NVN_TEXTURE_FLAGS_MINIMAL_LAYOUT,
                           .target = NVN_TEXTURE_TARGET_2D,
                           .samples = 1,
                           .levels = 1,
                           .width = 32,
                           .height = 16,
                           .depth = 1};
        if (i == Z || i == MS_DEPTH) t.d.format = NVN_FORMAT_DEPTH32F;
        if (i == ARRAY) t.d.target = NVN_TEXTURE_TARGET_2D_ARRAY, t.d.depth = 3;
        if (i == PADDED) t.d.width = 31;
        if (i == LINEAR || i == LINEAR_PADDED) t.d.flags = NVN_TEX_LINEAR, t.d.stride = i == LINEAR ? 128 : 256;
        if (i == VOLUME) t.d.target = NVN_TEXTURE_TARGET_3D, t.d.depth = 3;
        if (i == MS_COLOR || i == MS_DEPTH) {
            t.d.target = NVN_TEXTURE_TARGET_2D_MULTISAMPLE;
            t.d.samples = i == MS_COLOR ? 2 : 4;
        }
        memcpy(GP(OBJECTS + 0x100 * i), &t, sizeof t);
    }
    g_test_renderer.draw = discard_draw;
    g_test_renderer.discard = discard_hint;
    discard_test.count = 0;
    nvn_CommandBufferInitialize(NULL, CB_A, DEVICE);
    nvn_CommandBufferAddCommandMemory(NULL, CB_A, POOL, 0xd0000, 0x10000);
    nvn_CommandBufferAddControlMemory(NULL, CB_A, 0xd000, 0x800);
    nvn_CommandBufferBeginRecording(NULL, CB_A);
    u64 before = nvn_CommandBufferGetCommandMemoryUsed(NULL, CB_A);
    nvn_CommandBufferDiscardColor(NULL, CB_A, -1);
    nvn_CommandBufferDiscardColor(NULL, CB_A, 8);
    nvn_CommandBufferDiscardColor(NULL, CB_A, INT32_MIN);
    nvn_CommandBufferDiscardColor(NULL, CB_A, INT32_MAX);
    check(nvn_CommandBufferGetCommandMemoryUsed(NULL, CB_A) == before, "invalid discard indices record no command");
    gw64(COLORS, OBJECTS + 0x100 * A);
    gw64(COLORS + 8, OBJECTS + 0x100 * B);
    nvn_CommandBufferSetRenderTargets(NULL, CB_A, 2, COLORS, 0, OBJECTS + 0x100 * Z, 0);
    nvn_CommandBufferDrawArrays(NULL, CB_A, NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 3);
    nvn_CommandBufferDiscardColor(NULL, CB_A, 1);
    nvn_CommandBufferSetScissor(NULL, CB_A, 1, 0, 31, 16);
    nvn_CommandBufferDiscardColor(NULL, CB_A, 0);
    nvn_CommandBufferSetScissor(NULL, CB_A, -5, -7, 37, 23);
    nvn_CommandBufferDiscardColor(NULL, CB_A, 0);
    nvn_CommandBufferSetScissor(NULL, CB_A, 0, 0, 32, 15);
    nvn_CommandBufferDiscardDepthStencil(NULL, CB_A);
    nvn_CommandBufferSetScissor(NULL, CB_A, 0, 0, 32, 16);
    nvn_CommandBufferDiscardDepthStencil(NULL, CB_A);
    gw64(COLORS, OBJECTS + 0x100 * B);
    nvn_CommandBufferSetRenderTargets(NULL, CB_A, 1, COLORS, 0, 0, 0);
    nvn_CommandBufferDiscardColor(NULL, CB_A, 0);
    nvn_CommandBufferDiscardDepthStencil(NULL, CB_A);
    nvn_CommandBufferDiscardColor(NULL, CB_A, 7);
    const i32 narrow[4] = {0, 0, 31, 16}, full[4] = {0, 0, 32, 16};
    memcpy(GP(SCRATCH), narrow, sizeof narrow);
    nvn_CommandBufferSetScissors(NULL, CB_A, 1, 1, SCRATCH);
    nvn_CommandBufferDiscardColor(NULL, CB_A, 0);
    memcpy(GP(SCRATCH), full, sizeof full);
    nvn_CommandBufferSetScissors(NULL, CB_A, 1, 1, SCRATCH);
    NvnView view = {.set = NVN_VIEW_LAYERS, .min_layer = 1, .layers = 1};
    memcpy(GP(VIEW), &view, sizeof view);
    gw64(VIEWS, VIEW);
    gw64(COLORS, OBJECTS + 0x100 * ARRAY);
    nvn_CommandBufferSetRenderTargets(NULL, CB_A, 1, COLORS, VIEWS, 0, 0);
    nvn_CommandBufferDiscardColor(NULL, CB_A, 0);
    nvn_CommandBufferSetRenderTargets(NULL, CB_A, 1, COLORS, 0, 0, 0);
    nvn_CommandBufferDiscardColor(NULL, CB_A, 0);
    for (u32 i = PADDED; i <= VOLUME; i++) {
        gw64(COLORS, OBJECTS + 0x100 * i);
        nvn_CommandBufferSetRenderTargets(NULL, CB_A, 1, COLORS, 0, 0, 0);
        nvn_CommandBufferDiscardColor(NULL, CB_A, 0);
    }
    gw64(COLORS, OBJECTS + 0x100 * MS_COLOR);
    nvn_CommandBufferSetRenderTargets(NULL, CB_A, 1, COLORS, 0, OBJECTS + 0x100 * MS_DEPTH, 0);
    nvn_CommandBufferDiscardColor(NULL, CB_A, 0);
    nvn_CommandBufferDiscardDepthStencil(NULL, CB_A);
    nvn_CommandBufferSetScissor(NULL, CB_A, 0, 0, 31, 16);
    nvn_CommandBufferDiscardColor(NULL, CB_A, 0);
    nvn_CommandBufferSetScissor(NULL, CB_A, INT32_MIN, 0, INT32_MAX, 16);
    nvn_CommandBufferDiscardColor(NULL, CB_A, 0);
    nvn_CommandBufferSetScissor(NULL, CB_A, 1, 0, INT32_MAX, 16);
    nvn_CommandBufferDiscardColor(NULL, CB_A, 0);
    nvn_CommandBufferSetScissor(NULL, CB_A, 0, 0, 0, 16);
    nvn_CommandBufferDiscardColor(NULL, CB_A, 0);
    nvn_CommandBufferSetScissor(NULL, CB_A, 0, 0, 32, -1);
    nvn_CommandBufferDiscardColor(NULL, CB_A, 0);
    nvn_CommandBufferSetScissor(NULL, CB_A, -1, -1, INT32_MAX, INT32_MAX);
    nvn_CommandBufferDiscardColor(NULL, CB_A, 0);
    nvn_CommandBufferSetRenderTargets(NULL, CB_A, 1, 0, 0, 0, 0);
    nvn_CommandBufferDiscardColor(NULL, CB_A, 0);
    u64 original = nvn_CommandBufferEndRecording(NULL, CB_A);
    check(discard_test.count == 0, "discard does not execute at command record time");
    nvn_CommandBufferInitialize(NULL, CB_B, DEVICE);
    nvn_CommandBufferAddCommandMemory(NULL, CB_B, POOL, 0xe0000, 0x10000);
    nvn_CommandBufferAddControlMemory(NULL, CB_B, 0xe000, 0x800);
    nvn_CommandBufferBeginRecording(NULL, CB_B);
    gw64(SCRATCH, original);
    nvn_CommandBufferCopyCommands(NULL, CB_B, 1, SCRATCH);
    u64 copied = nvn_CommandBufferEndRecording(NULL, CB_B);
    memset(GP(OBJECTS), 0, 0x100 * TEXTURES);
    memset(GP(VIEW), 0, sizeof view);
    static NvnDraw draw;
    const u64 expected[][4] = {{0, gpu[A], gpu[B], gpu[Z]},
                              {0x100000002ull, gpu[A], gpu[B], gpu[Z]},
                              {0x100000001ull, gpu[A], gpu[B], gpu[Z]},
                              {0x100000100ull, gpu[A], gpu[B], gpu[Z]},
                              {0x100000001ull, gpu[B], 0, 0},
                              {0x100000001ull, gpu[ARRAY] + 2048, 0, 0},
                              {0x100000001ull, gpu[LINEAR], 0, 0},
                              {0x100000001ull, gpu[MS_COLOR], 0, gpu[MS_DEPTH]},
                              {0x100000100ull, gpu[MS_COLOR], 0, gpu[MS_DEPTH]},
                              {0x100000001ull, gpu[MS_COLOR], 0, gpu[MS_DEPTH]}};
    for (u32 i = 0; i < 2; i++) {
        memset(&draw, 0, sizeof draw);
        nvn_draw_device(i ? NVN_WINDOW_ORIGIN_MODE_UPPER_LEFT : NVN_WINDOW_ORIGIN_MODE_LOWER_LEFT,
                        NVN_DEPTH_MODE_NEAR_IS_ZERO);
        nvn_draw_init(&draw, 1);
        discard_test.count = 0;
        check(replay(&draw, i ? copied : original, OP_DISCARD) == 24, "all valid discard records survive copy/replay");
        check(discard_test.count == sizeof expected / sizeof *expected, "only full, unpadded views reach discard");
        check(!memcmp(discard_test.event, expected, sizeof expected),
              "discard follows draw/scissor/target state and retained texture snapshots in either window origin");
    }
    /* Predicates do not reach the callback as an unconditional discard. The same record works once it is safe. */
    draw.ds.tgt.color[0] = (DsSurf){.gva = gpu[A], .width = 32, .height = 16, .layers = 1};
    draw.ds.tgt.count = 1;
    draw.ds.tgt.msaa = DS_MSAA_1X1;
    draw.discard_width[0] = 32;
    for (u32 i = 0; i < DS_VIEWPORTS_MAX; i++) memcpy(draw.scissor[i], full, sizeof full);
    NvnPDiscard hint = {1, 0};
    memcpy(GP(SCRATCH), &hint, sizeof hint);
    u32 count = discard_test.count;
    draw.own.pred = 1;
    nvn_draw_record(&draw, OP_DISCARD, SCRATCH, sizeof hint);
    draw.own.pred = 0;
    draw.render_off = 1;
    nvn_draw_record(&draw, OP_DISCARD, SCRATCH, sizeof hint);
    draw.render_off = 0;
    nvn_draw_record(&draw, OP_DISCARD, SCRATCH, sizeof hint - 1);
    check(discard_test.count == count, "predicates and truncated payloads retain target contents");
    nvn_draw_record(&draw, OP_DISCARD, SCRATCH, sizeof hint);
    check(discard_test.count == count + 1, "a full-view discard works after conditional rendering ends");
    nvn_CommandBufferFinalize(NULL, CB_A);
    nvn_CommandBufferFinalize(NULL, CB_B);
    g_test_renderer.draw = r_draw;
    g_test_renderer.discard = NULL;
}
