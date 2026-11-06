/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */

/* What the command buffers hold of the vertex input, read at the driver's door: volk's pointers to the pipeline
   creation, the command buffers' begins, the graphics binds, vkCmdSetVertexInputEXT and every draw call go through
   these first. A pipeline made with VK_DYNAMIC_STATE_VERTEX_INPUT_EXT draws with the layout last set in its command
   buffer, which a begin or the bind of a pipeline that holds its own layout takes away; any other draws with its own.
   Every draw of every check is counted: one with a dynamic layout and none set reads the driver's, which is wrong */
#define VIW_PIPES 4096
#define VIW_CBS   64
#define VIW_DRAWS 16
typedef struct {
    VkCommandBuffer cb;
    u64 used; /* when it was last used, in the watch's calls: the least recent one's slot goes to a new one */
    VkPipeline pipe;
    int dynamic;  /* the bound graphics pipeline takes the layout as dynamic state */
    int set;      /* a layout is set and holds for it */
    u32 format1;  /* the format of the layout's attribute at location 1, 0 for none */
    u32 nb, na;   /* the layout set: its bindings and attributes, field by field (the structs have padding) */
    u32 b[33][4]; /* binding, stride, input rate, divisor */
    u32 a[32][4]; /* location, binding, format, offset */
} VIWBuffer;
static struct {
    PMutex lock;
    PFN_vkCreateGraphicsPipelines create;
    PFN_vkBeginCommandBuffer begin;
    PFN_vkCmdBindPipeline bind;
    PFN_vkCmdSetVertexInputEXT set;
    PFN_vkCmdDraw draw;
    PFN_vkCmdDrawIndexed draw_indexed;
    PFN_vkCmdDrawIndirect draw_indirect;
    PFN_vkCmdDrawIndexedIndirect draw_indexed_indirect;
    PFN_vkCmdDrawIndirectCount draw_indirect_count;
    PFN_vkCmdDrawIndexedIndirectCount draw_indexed_indirect_count;
    struct {
        VkPipeline p;
        int dynamic;
        u32 format1; /* a static layout's attribute at location 1 */
    } pipes[VIW_PIPES];
    u32 npipes, pipes_over;
    VIWBuffer cbs[VIW_CBS];
    u64 calls;
    u32 layouts_over; /* what the watch had no room for: a failure */
    /* every draw so far: those with a dynamic layout, those of them without one set (wrong), the sets, the sets of the
       layout the command buffer already had, and the binds of a graphics pipeline made before the watch */
    u64 draws, dynamic_draws, unset, sets, redundant, unknown_binds;
    /* the draws while capture is on, as each found the vertex input */
    int capture;
    u32 ncap;
    struct {
        VkCommandBuffer cb;
        VkPipeline pipe;
        int dynamic;
        u32 format1;
        u64 sets; /* the sets before it */
    } cap[VIW_DRAWS];
} g_viw = {.lock = PMUTEX_INIT};

/* cb's state, a new one in the slot used least recently when the watch has none (lock held). The checks make many
   more command buffers than the slots, each recorded and done with; the frame's, in use all along, keeps its slot */
static VIWBuffer* viw_buffer(VkCommandBuffer cb) {
    u32 old = 0;
    for (u32 i = 0; i < VIW_CBS; i++) {
        if (g_viw.cbs[i].cb == cb) {
            g_viw.cbs[i].used = ++g_viw.calls;
            return &g_viw.cbs[i];
        }
        if (g_viw.cbs[i].used < g_viw.cbs[old].used) old = i;
    }
    g_viw.cbs[old] = (VIWBuffer){.cb = cb, .used = ++g_viw.calls};
    return &g_viw.cbs[old];
}
static u32 viw_format1(u32 n, const VkVertexInputAttributeDescription* a) {
    for (u32 i = 0; i < n; i++)
        if (a[i].location == 1) return (u32)a[i].format;
    return 0;
}
static VKAPI_ATTR VkResult VKAPI_CALL viw_create(VkDevice dev, VkPipelineCache cache, u32 n,
                                                 const VkGraphicsPipelineCreateInfo* info,
                                                 const VkAllocationCallbacks* alloc, VkPipeline* out) {
    VkResult r = g_viw.create(dev, cache, n, info, alloc, out);
    pm_lock(&g_viw.lock);
    for (u32 i = 0; i < n && r == VK_SUCCESS; i++) {
        int dynamic = 0;
        for (u32 d = 0; info[i].pDynamicState && d < info[i].pDynamicState->dynamicStateCount; d++)
            dynamic |= info[i].pDynamicState->pDynamicStates[d] == VK_DYNAMIC_STATE_VERTEX_INPUT_EXT;
        const VkPipelineVertexInputStateCreateInfo* vi = info[i].pVertexInputState;
        if (g_viw.npipes == VIW_PIPES) {
            g_viw.pipes_over++;
            continue;
        }
        g_viw.pipes[g_viw.npipes].p = out[i];
        g_viw.pipes[g_viw.npipes].dynamic = dynamic;
        g_viw.pipes[g_viw.npipes++].format1 =
            dynamic || !vi ? 0 : viw_format1(vi->vertexAttributeDescriptionCount, vi->pVertexAttributeDescriptions);
    }
    pm_unlock(&g_viw.lock);
    return r;
}
static VKAPI_ATTR VkResult VKAPI_CALL viw_begin(VkCommandBuffer cb, const VkCommandBufferBeginInfo* info) {
    pm_lock(&g_viw.lock);
    VIWBuffer* b = viw_buffer(cb);
    if (b) *b = (VIWBuffer){.cb = cb};
    pm_unlock(&g_viw.lock);
    return g_viw.begin(cb, info);
}
static VKAPI_ATTR void VKAPI_CALL viw_bind(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipeline p) {
    if (bp == VK_PIPELINE_BIND_POINT_GRAPHICS) {
        pm_lock(&g_viw.lock);
        VIWBuffer* b = viw_buffer(cb);
        u32 i = 0;
        while (i < g_viw.npipes && g_viw.pipes[i].p != p) i++;
        if (i == g_viw.npipes) g_viw.unknown_binds++;
        if (b) {
            b->pipe = p;
            b->dynamic = i < g_viw.npipes && g_viw.pipes[i].dynamic;
            if (!b->dynamic) b->set = 0; /* a pipeline's own layout takes the dynamic one's place */
            if (!b->dynamic) b->format1 = i < g_viw.npipes ? g_viw.pipes[i].format1 : 0;
        }
        pm_unlock(&g_viw.lock);
    }
    g_viw.bind(cb, bp, p);
}
static VKAPI_ATTR void VKAPI_CALL viw_set(VkCommandBuffer cb, u32 nb, const VkVertexInputBindingDescription2EXT* bd,
                                          u32 na, const VkVertexInputAttributeDescription2EXT* ad) {
    pm_lock(&g_viw.lock);
    VIWBuffer* b = viw_buffer(cb);
    g_viw.sets++;
    if (b && nb <= 33 && na <= 32) {
        u32 nbv[33][4], nav[32][4], format1 = 0;
        for (u32 i = 0; i < nb; i++) {
            nbv[i][0] = bd[i].binding;
            nbv[i][1] = bd[i].stride;
            nbv[i][2] = (u32)bd[i].inputRate;
            nbv[i][3] = bd[i].divisor;
        }
        for (u32 i = 0; i < na; i++) {
            nav[i][0] = ad[i].location;
            nav[i][1] = ad[i].binding;
            nav[i][2] = (u32)ad[i].format;
            nav[i][3] = ad[i].offset;
            if (ad[i].location == 1) format1 = (u32)ad[i].format;
        }
        if (b->set && b->nb == nb && b->na == na && !memcmp(b->b, nbv, nb * sizeof *nbv) &&
            !memcmp(b->a, nav, na * sizeof *nav))
            g_viw.redundant++;
        b->set = 1;
        b->nb = nb;
        b->na = na;
        memcpy(b->b, nbv, nb * sizeof *nbv);
        memcpy(b->a, nav, na * sizeof *nav);
        b->format1 = format1;
    } else if (b)
        g_viw.layouts_over++;
    pm_unlock(&g_viw.lock);
    g_viw.set(cb, nb, bd, na, ad);
}
static void viw_draw(VkCommandBuffer cb) {
    pm_lock(&g_viw.lock);
    VIWBuffer* b = viw_buffer(cb);
    g_viw.draws++;
    if (b && b->dynamic) {
        g_viw.dynamic_draws++;
        if (!b->set) g_viw.unset++;
    }
    if (b && g_viw.capture && g_viw.ncap < VIW_DRAWS) {
        g_viw.cap[g_viw.ncap].cb = cb;
        g_viw.cap[g_viw.ncap].pipe = b->pipe;
        g_viw.cap[g_viw.ncap].dynamic = b->dynamic;
        g_viw.cap[g_viw.ncap].format1 = b->dynamic && !b->set ? ~0u : b->format1;
        g_viw.cap[g_viw.ncap++].sets = g_viw.sets;
    }
    pm_unlock(&g_viw.lock);
}
static VKAPI_ATTR void VKAPI_CALL viw_draw0(VkCommandBuffer cb, u32 a, u32 b, u32 c, u32 d) {
    viw_draw(cb);
    g_viw.draw(cb, a, b, c, d);
}
static VKAPI_ATTR void VKAPI_CALL viw_draw1(VkCommandBuffer cb, u32 a, u32 b, u32 c, i32 d, u32 e) {
    viw_draw(cb);
    g_viw.draw_indexed(cb, a, b, c, d, e);
}
static VKAPI_ATTR void VKAPI_CALL viw_draw2(VkCommandBuffer cb, VkBuffer buf, VkDeviceSize off, u32 n, u32 stride) {
    viw_draw(cb);
    g_viw.draw_indirect(cb, buf, off, n, stride);
}
static VKAPI_ATTR void VKAPI_CALL viw_draw3(VkCommandBuffer cb, VkBuffer buf, VkDeviceSize off, u32 n, u32 stride) {
    viw_draw(cb);
    g_viw.draw_indexed_indirect(cb, buf, off, n, stride);
}
static VKAPI_ATTR void VKAPI_CALL viw_draw4(VkCommandBuffer cb, VkBuffer buf, VkDeviceSize off, VkBuffer cbuf,
                                            VkDeviceSize coff, u32 n, u32 stride) {
    viw_draw(cb);
    g_viw.draw_indirect_count(cb, buf, off, cbuf, coff, n, stride);
}
static VKAPI_ATTR void VKAPI_CALL viw_draw5(VkCommandBuffer cb, VkBuffer buf, VkDeviceSize off, VkBuffer cbuf,
                                            VkDeviceSize coff, u32 n, u32 stride) {
    viw_draw(cb);
    g_viw.draw_indexed_indirect_count(cb, buf, off, cbuf, coff, n, stride);
}
/* once the device is made (volkLoadDevice), before any graphics pipeline: every one is then known */
static void viw_install(void) {
    g_viw.create = vkCreateGraphicsPipelines;
    g_viw.begin = vkBeginCommandBuffer;
    g_viw.bind = vkCmdBindPipeline;
    g_viw.set = vkCmdSetVertexInputEXT;
    g_viw.draw = vkCmdDraw;
    g_viw.draw_indexed = vkCmdDrawIndexed;
    g_viw.draw_indirect = vkCmdDrawIndirect;
    g_viw.draw_indexed_indirect = vkCmdDrawIndexedIndirect;
    g_viw.draw_indirect_count = vkCmdDrawIndirectCount;
    g_viw.draw_indexed_indirect_count = vkCmdDrawIndexedIndirectCount;
    vkCreateGraphicsPipelines = viw_create;
    vkBeginCommandBuffer = viw_begin;
    vkCmdBindPipeline = viw_bind;
    if (g_viw.set) vkCmdSetVertexInputEXT = viw_set;
    vkCmdDraw = viw_draw0;
    vkCmdDrawIndexed = viw_draw1;
    vkCmdDrawIndirect = viw_draw2;
    vkCmdDrawIndexedIndirect = viw_draw3;
    if (g_viw.draw_indirect_count) vkCmdDrawIndirectCount = viw_draw4;
    if (g_viw.draw_indexed_indirect_count) vkCmdDrawIndexedIndirectCount = viw_draw5;
}
/* what the checks' draws found, said and checked once every check ran */
static int viw_report(void) {
    printf("vertex input: %llu draws, %llu with the layout as dynamic state, %llu layouts set, %llu of them the one "
           "the command buffer had, %llu draws with none set\n",
           (unsigned long long)g_viw.draws, (unsigned long long)g_viw.dynamic_draws, (unsigned long long)g_viw.sets,
           (unsigned long long)g_viw.redundant, (unsigned long long)g_viw.unset);
    CHECK(!g_viw.pipes_over && !g_viw.layouts_over && !g_viw.unknown_binds);
    CHECK(!g_viw.unset);
    CHECK(g_vk.vertex_input_dynamic ? g_viw.dynamic_draws > 0 : !g_viw.dynamic_draws && !g_viw.sets);
    return 0;
}

/* One vertex program drawn with three layouts of its color attribute, the pairs of formats one vertex program is most
   often drawn with in one game's pipelines (RGBA32F, RGBA16F and RGBA8 at offsets 0, 16 and 24 of one stream), in one
   command buffer: A A, a clear of some color components (a draw of the renderer's own, with no vertex input), then
   A B A C, each to a target of its own, then C again in the next command buffer. Each draw finds its own layout in
   effect, which the target's color shows. With dynamic vertex input one pipeline serves the six draws, the layout set
   before a draw only where it differs from the command buffer's (the clear's empty one in between, none in a new
   one); without it each layout has a pipeline of its own */
static int check_vertex_input_layouts(void) {
    const u64 address = 0x4A0000000ull, cpu = HEAP_BASE + 0x740000;
    const u64 positions = address + 0x11000, colors = address + 0x12000, targets = address + 0x18000;
    const u64 vertex[] = {0xefd9ff800807ff00ull, /* ALD R0..3, a[0x80] */
                          0xeff1ff800707ff00ull, /* position */
                          0xefd9ff800907ff00ull, /* ALD R0..3, a[0x90] */
                          0xeff1ff800807ff00ull, /* color varying */
                          0xe30000000007000full};
    const float position[12] = {-1, -1, 0, 1, 3, -1, 0, 1, -1, 3, 0, 1};
    const float transform[6] = {2, 2, 0.5f, 2, 2, 0.5f};
    static const struct {
        u32 reg; /* the attribute's register: stream 1, its offset, size and type */
        VkFormat vk;
        u8 rgba[4];
    } layout[3] = {{1 | 0u << 7 | 0x01u << 21 | 7u << 27, VK_FORMAT_R32G32B32A32_SFLOAT, {255, 0, 0, 255}},
                   {1 | 16u << 7 | 0x03u << 21 | 7u << 27, VK_FORMAT_R16G16B16A16_SFLOAT, {0, 255, 0, 255}},
                   {1 | 24u << 7 | 0x0Au << 21 | 2u << 27, VK_FORMAT_R8G8B8A8_UNORM, {0, 0, 255, 255}}};
    static const int order[6] = {0, 0, 0, 1, 0, 2};
    const u32 drawn = 6, captured = 7; /* the clear's draw after the second */
    u64 fragment[5];
    GpuChannel* channel = calloc(1, sizeof *channel);
    CHECK(channel);
    channel->id = 127;
    gmmu_map(address, cpu, 0x20000);
    u8 bytes[96];
    for (u32 i = 0; i < 12; i++) {
        u32 word;
        memcpy(&word, position + i, 4);
        snx_st_le32(bytes + 4 * i, word);
    }
    CHECK(!gmem_write(positions, bytes, 48));
    /* each vertex's record: red as RGBA32F, green as RGBA16F, blue as RGBA8 */
    memset(bytes, 0, sizeof bytes);
    for (u32 v = 0; v < 3; v++) {
        u8* rec = bytes + 32 * v;
        snx_st_le32(rec, 0x3f800000);
        snx_st_le32(rec + 12, 0x3f800000);
        snx_st_le16(rec + 18, 0x3c00);
        snx_st_le16(rec + 22, 0x3c00);
        rec[26] = rec[27] = 255;
    }
    CHECK(!gmem_write(colors, bytes, sizeof bytes));
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
    r[0x458] = 0 | 0x01u << 21 | 7u << 27; /* the position, RGBA32F of stream 0 */
    r[0x459] = layout[0].reg;
    for (u32 stream = 0; stream < 2; stream++) {
        u64 source = stream ? colors : positions;
        u32 stride = stream ? 32 : 16;
        r[0x700 + stream * 4] = 0x1000 | stride;
        r[0x701 + stream * 4] = (u32)(source >> 32);
        r[0x702 + stream * 4] = (u32)source;
        r[0x7c0 + stream * 2] = (u32)((source + 3 * stride - 1) >> 32);
        r[0x7c1 + stream * 2] = (u32)(source + 3 * stride - 1);
    }
    vk_lock();
    for (u32 i = 0; i < drawn; i++) { /* to black: a draw that misses its target leaves no layout's color */
        const float black[4] = {0, 0, 0, 0};
        u64 target = targets + 0x1000 * i;
        r[0x487] = 1;
        r[0x200] = (u32)(target >> 32);
        r[0x201] = (u32)target;
        r[0x202] = 4;
        r[0x203] = 4;
        r[0x204] = 0xD5;
        r[0x205] = 0;
        r[0x206] = 1;
        r[0x207] = 0;
        memcpy(&r[0x360], black, sizeof black);
        engine_clear(channel, 15 << 2);
        vk_render_end();
    }
    g_viw.ncap = 0;
    g_viw.capture = 1;
    u64 sets = g_viw.sets, redundant = g_viw.redundant, unset = g_viw.unset;
    for (u32 i = 0; i < drawn; i++) {
        u64 target = targets + 0x1000 * i;
        r[0x487] = 1;
        r[0x200] = (u32)(target >> 32);
        r[0x201] = (u32)target;
        r[0x202] = 4;
        r[0x203] = 4;
        r[0x204] = 0xD5;
        r[0x205] = 0;
        r[0x206] = 1;
        r[0x207] = 0;
        if (i == 2) { /* red, green and blue cleared to white: the renderer's own draw */
            const float white[4] = {1, 1, 1, 1};
            memcpy(&r[0x360], white, sizeof white);
            engine_clear(channel, 7 << 2);
        }
        r[0x459] = layout[order[i]].reg;
        channel->r3d_dirty = ~0u;
        mx_draw_state_sync(channel);
        DrawCall call = {.topology = 4, .count = 3, .instances = 1};
        u64 before = g_vk_drawn;
        vk_render_draw(&channel->ds, &call, &channel->owner);
        CHECK(g_vk_drawn == before + 1);
    }
    vk_render_end();
    VkBuffer buffer;
    u8* data = vk_readback_buffer(4 * drawn, &buffer);
    CHECK(data);
    for (u32 i = 0; i < drawn; i++) {
        Surf* surface = vk_rt_find(targets + 0x1000 * i);
        CHECK(surface);
        vk_surf_use(vk_cmd(), surface, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                    VK_ACCESS_2_TRANSFER_READ_BIT);
        VkBufferImageCopy copy = {.bufferOffset = 4 * i,
                                  .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                  .imageOffset = {1, 1, 0},
                                  .imageExtent = {1, 1, 1}};
        rec_CmdCopyImageToBuffer(vk_cmd(), surface->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &copy);
    }
    vk_finish(); /* the ring drained: every draw went through the watch */
    vk_readback_ready();
    g_viw.capture = 0;
    for (u32 i = 0; i < drawn; i++) {
        const u8* want = layout[order[i]].rgba;
        if (memcmp(data + 4 * i, want, 4))
            fprintf(stderr, "vertex input draw %u: got %u,%u,%u,%u, expected %u,%u,%u,%u\n", i, data[4 * i],
                    data[4 * i + 1], data[4 * i + 2], data[4 * i + 3], want[0], want[1], want[2], want[3]);
        CHECK(!memcmp(data + 4 * i, want, 4));
    }
    /* the last draw again, in the next command buffer: its layout is set there again */
    g_viw.capture = 1;
    channel->r3d_dirty = ~0u;
    mx_draw_state_sync(channel);
    DrawCall again = {.topology = 4, .count = 3, .instances = 1};
    vk_render_draw(&channel->ds, &again, &channel->owner);
    vk_render_end();
    vk_finish();
    g_viw.capture = 0;
    vk_unlock();
    for (u32 c = 0; g_viw.ncap != captured + 1 && c < g_viw.ncap; c++)
        fprintf(stderr, "vertex input capture %u: command buffer %p, pipeline %p, dynamic %d, format %u, sets %llu\n",
                c, (void*)g_viw.cap[c].cb, (void*)g_viw.cap[c].pipe, g_viw.cap[c].dynamic, g_viw.cap[c].format1,
                (unsigned long long)g_viw.cap[c].sets);
    CHECK(g_viw.ncap == captured + 1);
    CHECK(g_viw.cap[captured].cb != g_viw.cap[0].cb && g_viw.cap[captured].format1 == (u32)layout[2].vk &&
          g_viw.cap[captured].dynamic == g_vk.vertex_input_dynamic);
    VkPipeline pipes[VIW_DRAWS];
    u32 npipes = 0, want[VIW_DRAWS], kept = 0;
    for (u32 c = 0, i = 0; c <= captured; c++) {
        int clear = c == 2;
        want[c] = clear ? 0 : c == captured ? (u32)layout[2].vk : (u32)layout[order[i++]].vk;
        if (g_viw.cap[c].format1 != want[c])
            fprintf(stderr, "vertex input draw %u: format %u at location 1, expected %u (dynamic %d)\n", c,
                    g_viw.cap[c].format1, want[c], g_viw.cap[c].dynamic);
        CHECK(g_viw.cap[c].format1 == want[c] && g_viw.cap[c].dynamic == g_vk.vertex_input_dynamic);
        if (clear || c == captured) continue;
        u32 p = 0;
        while (p < npipes && pipes[p] != g_viw.cap[c].pipe) p++;
        if (p == npipes) pipes[npipes++] = g_viw.cap[c].pipe;
    }
    /* A layout is set before a draw exactly when its command buffer has another or none: a new command buffer (the
       renderer submits one a millisecond of recording at most, and the last draw follows a finish), or another layout
       before it, the clear's empty one among them. Draws 0 and 1 share theirs where no submit fell between them */
    for (u32 c = 1; g_vk.vertex_input_dynamic && c <= captured; c++) {
        int need = g_viw.cap[c].cb != g_viw.cap[c - 1].cb || want[c] != want[c - 1];
        u64 set = g_viw.cap[c].sets - g_viw.cap[c - 1].sets;
        if (set != (u64)need)
            fprintf(stderr, "vertex input draw %u: %llu layouts set before it, expected %d\n", c,
                    (unsigned long long)set, need);
        CHECK(set == (u64)need);
        kept += !need;
    }
    CHECK(g_viw.cap[captured].cb != g_viw.cap[captured - 1].cb);
    CHECK(npipes == (g_vk.vertex_input_dynamic ? 1u : 3u));
    CHECK(g_viw.redundant == redundant && g_viw.unset == unset);
    CHECK(g_vk.vertex_input_dynamic ? g_viw.sets > sets : g_viw.sets == sets);
    printf("vertex input layouts: 3 layouts of one program in 7 draws, %u pipeline%s, %llu layouts set, %u draw%s with "
           "the one before it (%s)\n",
           npipes, npipes == 1 ? "" : "s", (unsigned long long)(g_viw.sets - sets), kept, kept == 1 ? "" : "s",
           g_vk.vertex_input_dynamic ? "dynamic vertex input" : "a pipeline per layout");
    free(channel);
    return 0;
}

/* The file of earlier runs' pipelines holds the choice in its key's guest part, which its pipelines depend on: a file
   made with the layout as dynamic state, whose keys hold none, is never read by a run that puts it in the key, nor the
   other way. The rest of the key is the same. The key is said, to compare with a file another build wrote */
static int check_vertex_input_file_key(void) {
    char with[1024], without[1024];
    size_t gwith = 0, gwithout = 0;
    int was = g_vk.vertex_input_dynamic;
    g_vk.vertex_input_dynamic = 1;
    vk_pipeline_file_key(with, sizeof with, &gwith);
    g_vk.vertex_input_dynamic = 0;
    vk_pipeline_file_key(without, sizeof without, &gwithout);
    g_vk.vertex_input_dynamic = was;
    CHECK(gwith && gwithout && (gwith != gwithout || memcmp(with, without, gwith)));
    CHECK(!strcmp(with + gwith, without + gwithout));
    printf("vertex input file key: with the layout dynamic %.*s\n", (int)gwith, with);
    printf("vertex input file key: with it in the key %.*s\n", (int)gwithout, without);
    return 0;
}
