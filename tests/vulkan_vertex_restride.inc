/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */

/* the three vertices of a triangle over the whole target, each a position and a color (RGBA32F at 0 and 16), stride
   bytes apart from start, with poison in the bytes between them that a read at the wrong offset takes */
static void restride_stream(u8* start, u32 stride, const float color[4]) {
    static const float position[12] = {-1, -1, 0, 1, 3, -1, 0, 1, -1, 3, 0, 1};
    memset(start, 0xFF, 3 * stride); /* NaNs: a misplaced vertex covers no pixel, a misplaced color isn't the one */
    for (u32 v = 0; v < 3; v++)
        for (u32 i = 0; i < 8; i++) {
            u32 word;
            memcpy(&word, i < 4 ? &position[4 * v + i] : &color[i - 4], 4);
            snx_st_le32(start + v * stride + 4 * i, word);
        }
}

/* A vertex stream whose stride (69) isn't a multiple of its attributes' 4-byte components, which Vulkan can't read
   (RADV's fetches hung the GPU on one game's): the draw reads a copy restrided on the GPU (render.c's restride), as the
   CPU's copy before it did. From a stream that starts in the middle of a word, after the CPU changed it, and from a
   stream the DMA engine wrote on the GPU, which the copy reads where it is */
static int check_vertex_restride(void) {
    const u64 address = 0x488000000ull, cpu = HEAP_BASE + 0x740000;
    const u64 stream = address + 0x12001, twin = address + 0x13000, target = address + 0x18000;
    const u32 stride = 69, len = 3 * stride;
    const u64 vertex[] = {0xefd9ff800807ff00ull, /* ALD R0..3, a[0x80] */
                          0xeff1ff800707ff00ull, /* position */
                          0xefd9ff800907ff00ull, /* ALD R0..3, a[0x90] */
                          0xeff1ff800807ff00ull, /* color varying */
                          0xe30000000007000full};
    u64 fragment[5];
    const float transform[6] = {2, 2, 0.5f, 2, 2, 0.5f};
    const float green[4] = {0, 1, 0, 1}, blue[4] = {0, 0, 1, 1}, white[4] = {1, 1, 1, 1};
    GpuChannel* channel = calloc(1, sizeof *channel);
    CHECK(channel);
    channel->id = 124;
    gmmu_map(address, cpu, 0x20000);
    for (u32 i = 0; i < 4; i++) fragment[i] = 0xe000000000070000ull | (0x20ull + i) << 30 | i;
    fragment[4] = 0xe30000000007000full;
    CHECK(mesh_program(address, vertex, 5, 0));
    CHECK(mesh_program(address + 0x10000, fragment, 5, 1));
    u32* r = channel->r3d;
    r[0x582] = (u32)(address >> 32);
    r[0x583] = (u32)address;
    r[0x850] = 1;
    r[0x851] = 0x10000;
    r[0x64b] = 1;
    r[0x286] = 0x6420;
    r[0x3fd] = r[0x3fe] = 4u << 16;
    r[0x680] = 0x1111;
    r[MAXWELL3D_RASTERIZE_ENABLE] = 1;
    memcpy(r + 0x280, transform, sizeof transform);
    r[0x458] = 0 | 0u << 7 | 1u << 21 | 7u << 27;  /* stream 0 at 0: RGBA32F */
    r[0x459] = 0 | 16u << 7 | 1u << 21 | 7u << 27; /* stream 0 at 16: RGBA32F */
    r[0x700] = 0x1000 | stride;
    r[0x701] = (u32)(stream >> 32);
    r[0x702] = (u32)stream;
    r[0x7c0] = (u32)((stream + len - 1) >> 32);
    r[0x7c1] = (u32)(stream + len - 1);
    u8 bytes[3 * 69];
    restride_stream(bytes, stride, green);
    CHECK(!gmem_write(stream, bytes, len));
    channel->r3d_dirty = ~0u;
    CHECK(!mesh_pixel(channel, target, 0, (const u8[4]){0, 255, 0, 255}));
    /* the CPU changes the stream: the next draw's copy has the new colors */
    restride_stream(bytes, stride, blue);
    CHECK(!gmem_write(stream, bytes, len));
    g_renderer->mem_may_have_changed();
    channel->r3d_dirty = ~0u;
    CHECK(!mesh_pixel(channel, target, 0, (const u8[4]){0, 0, 255, 255}));
    /* the DMA engine copies another stream over it on the GPU */
    restride_stream(bytes, stride, white);
    CHECK(!gmem_write(twin, bytes, len));
    GpuDmaOp o = {.width = len, .line_in = len, .line_out = len, .lines = 1};
    o.src = pitch_side(twin, 0);
    o.dst = pitch_side(stream, 0);
    CHECK(g_renderer->dma_copy(&o));
    channel->r3d_dirty = ~0u;
    CHECK(!mesh_pixel(channel, target, 0, (const u8[4]){255, 255, 255, 255}));
    CHECK(!memcmp(GP(cpu + 0x12001), bytes, len)); /* and the copy lands when the CPU reads it */
    printf("vertex restride: a stride of %u drawn from CPU and GPU writes\n", stride);
    free(channel);
    return 0;
}
