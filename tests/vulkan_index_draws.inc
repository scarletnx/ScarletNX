/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Draws whose indices Vulkan can't read as they are, which the renderer makes on the GPU (render.c's index_convert and
   quad_indices): quads of vertex numbers and of 8, 16 and 32-bit and inline indices, 8-bit indices widened, and
   restart indices other than all ones, in strips. Vertices 0 to 3 are a red quad over the whole target and 4 to 7 a
   green one, so a quad drawn from the wrong vertices shows red, and a strip that takes its restart index (2, the red
   quad's top right corner) for a vertex draws a second triangle with red in it */

/* a program at address of count instructions (a scheduling word before every three), a fragment program's header
   taking a screen-linear RGBA input when fragment is set, or one with perspective when it is 2 */
static int index_program(u64 address, const u64* ops, u32 count, int fragment) {
    u64 code[64] = {0};
    u32 at = 10;
    if (fragment) {
        code[3] = fragment == 2 ? 0xaa : 0xff; /* the RGBA input's interpolation */
        code[9] = 15;
    }
    for (u32 i = 0; i < count; i++) {
        if (!(i % 3)) code[at++] = 0x001f8000fc0007e0ull;
        code[at++] = ops[i];
    }
    return invalidate_write_code(address, code, 64);
}

/* the 4x4 target's pixels after a draw call: how many are green and blue (the clear), and whether any has red */
typedef struct {
    u32 green, blue, red;
} IndexPixels;

/* the 4x4 target's pixels once the work recorded so far is done (the rendering pass ended), under vk_lock */
static int index_pixels(u64 target, IndexPixels* px) {
    vk_render_end();
    Surf* surface = vk_rt_find(target);
    VkBuffer buffer;
    u8* data = vk_readback_buffer(64, &buffer);
    CHECK(surface && data);
    vk_surf_use(vk_cmd(), surface, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT);
    VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {4, 4, 1}};
    rec_CmdCopyImageToBuffer(vk_cmd(), surface->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &copy);
    vk_finish();
    vk_readback_ready();
    *px = (IndexPixels){0};
    for (u32 i = 0; i < 16; i++) {
        const u8* p = data + i * 4;
        px->green += !p[0] && p[1] == 255 && !p[2];
        px->blue += !p[0] && !p[1] && p[2] == 255;
        px->red += p[0] != 0;
    }
    return 0;
}

/* the word of the predicate pred (query.c's slot pred - 1, which a draw under it reads: 0 skips the draw, else it
   draws), as the GPU left it, for a failure's message; ~0u when it can't be read */
static u32 index_pred_word(u32 pred) {
    VkBuffer buffer;
    u32 word = ~0u;
    vk_lock();
    vk_render_end();
    u8* data = vk_readback_buffer(4, &buffer);
    if (data) {
        VkBufferCopy copy = {(VkDeviceSize)((pred & ~VK_PRED_EQUAL) - 1) * 4, 0, 4};
        rec_CmdCopyBuffer(vk_cmd(), vk_query_pred_buffer(), buffer, 1, &copy);
        vk_finish();
        vk_readback_ready();
        word = snx_ld_le32(data);
    }
    vk_unlock();
    return word;
}

/* the 4x4 target's first pixel, for a failure's message */
static int index_first_pixel(u64 target, u8 out[4]) {
    Surf* surface = vk_rt_find(target);
    VkBuffer buffer;
    u8* data;
    vk_lock();
    vk_render_end();
    data = vk_readback_buffer(64, &buffer);
    if (!surface || !data) {
        vk_unlock();
        return -1;
    }
    vk_surf_use(vk_cmd(), surface, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT);
    VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {4, 4, 1}};
    rec_CmdCopyImageToBuffer(vk_cmd(), surface->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &copy);
    vk_finish();
    vk_readback_ready();
    memcpy(out, data, 4);
    vk_unlock();
    return 0;
}
/* the render predicate index_draw's draw runs on (its clear doesn't), 0 for none */
static u32 g_index_pred;
static int index_draw(GpuChannel* channel, u64 target, const DrawCall* call, IndexPixels* px) {
    u64 drawn = g_vk_drawn;
    vk_lock();
    rt_clear(channel, target, 4, 4, 0, 1);
    channel->r3d_dirty = ~0u; /* the registers were set directly */
    mx_draw_state_sync(channel);
    channel->owner.pred = g_index_pred;
    vk_render_draw(&channel->ds, call, &channel->owner);
    channel->owner.pred = 0;
    CHECK(g_vk_drawn == drawn + 1);
    CHECK(!index_pixels(target, px));
    vk_unlock();
    return 0;
}

/* indices n of size isz (1, 2 or 4) at the index buffer's address, from index first on, and the restart index ri
   (restart on when ri isn't ~0) */
static int index_buffer(GpuChannel* channel, u64 at, const u32* idx, u32 n, u32 isz, u32 ri) {
    u8 bytes[64];
    u32* r = channel->r3d;
    for (u32 i = 0; i < n; i++)
        if (isz == 1)
            bytes[i] = (u8)idx[i];
        else if (isz == 2)
            snx_st_le16(bytes + i * 2, (u16)idx[i]);
        else
            snx_st_le32(bytes + i * 4, idx[i]);
    CHECK(!gmem_write(at, bytes, (size_t)n * isz));
    r[0x5F2] = (u32)(at >> 32);
    r[0x5F3] = (u32)at;
    r[0x5F4] = (u32)((at + (u64)n * isz - 1) >> 32);
    r[0x5F5] = (u32)(at + (u64)n * isz - 1);
    r[0x5F6] = isz == 1 ? 0 : isz == 2 ? 1 : 2;
    r[0x591] = ri != ~0u;
    r[0x592] = ri;
    return 0;
}

/* a whole green quad: every pixel green */
static int index_all_green(const IndexPixels* px) { return px->green == 16 && !px->red; }
/* one green triangle of the strip, half the target, and nothing red: a restart index taken for vertex 2 draws red */
static int index_half_green(const IndexPixels* px) {
    return px->green >= 6 && px->blue >= 6 && px->green + px->blue == 16 && !px->red;
}

/* A list whose restart index (0, the red vertex at green vertex 4's corner) comes between its two triangles. Where the
   device restarts lists (VK_EXT_primitive_topology_list_restart, g_vk.has_list_restart) the index ends the first
   triangle's run and the second draws: the whole quad green, as the 3D engine draws it. Without it, the way before the
   extension: the index is a vertex, and the triangle (0, 5, 6) shows red. Not 3, the red vertex at 7's corner: lavapipe
   (Mesa 26.2.6) draws 4, 5, 7, 3, 5, 6 all green with no restart at all, where RADV draws (3, 5, 6) red as the rule
   says, and draws 3, 5, 6, 4, 5, 7 right. 8-bit indices without restart are widened
   with the restart index made 0xFFFF, a vertex out of the buffer, which draws no red: only 16 and 32-bit ones are
   checked that way */
static int index_list_restart_cases(GpuChannel* channel, u64 target, u64 indices) {
    IndexPixels px;
    const u32 list[7] = {4, 5, 7, 0, 5, 6, 7};
    const int restarts = g_vk.has_list_restart;
    for (u32 isz = 1; isz <= 4; isz *= 2) {
        if (!restarts && isz == 1) continue;
        CHECK(!index_buffer(channel, indices, list, 7, isz, 0));
        DrawCall call = {.topology = 4, .indexed = 1, .count = 7, .instances = 1};
        CHECK(!index_draw(channel, target, &call, &px));
        if (restarts ? !index_all_green(&px) : !px.red)
            fprintf(stderr, "list of %u byte indices restarting at 0 (list restart %s): %u green, %u blue, %u red\n",
                    isz, restarts ? "on" : "off", px.green, px.blue, px.red);
        CHECK(restarts ? index_all_green(&px) : px.red > 0);
    }
    channel->r3d[0x591] = 1;
    channel->r3d[0x592] = 0;
    DrawCall call = {.topology = 4, .indexed = 3, .count = 7, .instances = 1, .inline_indices = list};
    CHECK(!index_draw(channel, target, &call, &px));
    if (restarts ? !index_all_green(&px) : !px.red)
        fprintf(stderr, "inline list restarting at 0 (list restart %s): %u green, %u blue, %u red\n",
                restarts ? "on" : "off", px.green, px.blue, px.red);
    CHECK(restarts ? index_all_green(&px) : px.red > 0);
    if (restarts) { /* Vulkan's own restart index, read as it is */
        const u32 list_ff[7] = {4, 5, 7, 0xFFFF, 5, 6, 7};
        CHECK(!index_buffer(channel, indices, list_ff, 7, 2, 0xFFFF));
        call = (DrawCall){.topology = 4, .indexed = 1, .count = 7, .instances = 1};
        CHECK(!index_draw(channel, target, &call, &px));
        CHECK(index_all_green(&px));
    }
    channel->r3d[0x591] = 0;
    return 0;
}

static int index_cases(GpuChannel* channel, u64 target, u64 indices) {
    IndexPixels px;
    const u32 quad[5] = {9, 4, 5, 6, 7}, quad0[4] = {0, 1, 2, 3}, strip[5] = {4, 5, 7, 2, 4};
    /* quads of vertex numbers from vertex 4 */
    DrawCall call = {.topology = 7, .first = 4, .count = 4, .instances = 1};
    CHECK(!index_draw(channel, target, &call, &px));
    CHECK(index_all_green(&px));
    /* indexed quads from index 1 (a word apart, or not, for every size), and through the base vertex */
    for (u32 isz = 1; isz <= 4; isz *= 2) {
        CHECK(!index_buffer(channel, indices, quad, 5, isz, ~0u));
        call = (DrawCall){.topology = 7, .indexed = 1, .first = 1, .count = 4, .instances = 1};
        CHECK(!index_draw(channel, target, &call, &px));
        if (!index_all_green(&px)) fprintf(stderr, "indexed quads of %u bytes: %u green, %u red\n", isz, px.green, px.red);
        CHECK(index_all_green(&px));
        CHECK(!index_buffer(channel, indices, quad0, 4, isz, ~0u));
        call = (DrawCall){.topology = 7, .indexed = 1, .count = 4, .base_vertex = 4, .instances = 1};
        CHECK(!index_draw(channel, target, &call, &px));
        CHECK(index_all_green(&px));
    }
    /* inline indices: quads, and a strip with its restart index */
    call = (DrawCall){.topology = 7, .indexed = 3, .first = 1, .count = 4, .instances = 1, .inline_indices = quad};
    CHECK(!index_draw(channel, target, &call, &px));
    CHECK(index_all_green(&px));
    channel->r3d[0x591] = 1;
    channel->r3d[0x592] = 2;
    call = (DrawCall){.topology = 5, .indexed = 3, .count = 5, .instances = 1, .inline_indices = strip};
    CHECK(!index_draw(channel, target, &call, &px));
    CHECK(index_half_green(&px));
    /* strips of every index size whose restart index is 2, an odd number of indices, and 8-bit ones restarting at
       0xFF, Vulkan's own */
    for (u32 isz = 1; isz <= 4; isz *= 2) {
        CHECK(!index_buffer(channel, indices, strip, 5, isz, 2));
        call = (DrawCall){.topology = 5, .indexed = 1, .count = 5, .instances = 1};
        CHECK(!index_draw(channel, target, &call, &px));
        if (!index_half_green(&px))
            fprintf(stderr, "strip of %u byte indices: %u green, %u blue, %u red\n", isz, px.green, px.blue, px.red);
        CHECK(index_half_green(&px));
    }
    const u32 strip_ff[4] = {4, 5, 7, 0xFF}, list[6] = {4, 5, 7, 5, 6, 7};
    CHECK(!index_buffer(channel, indices, strip_ff, 4, 1, 0xFF));
    call = (DrawCall){.topology = 5, .indexed = 1, .count = 4, .instances = 1};
    CHECK(!index_draw(channel, target, &call, &px));
    CHECK(index_half_green(&px));
    /* 8-bit indices of a list, without restart */
    CHECK(!index_buffer(channel, indices, list, 6, 1, ~0u));
    call = (DrawCall){.topology = 4, .indexed = 1, .count = 6, .instances = 1};
    CHECK(!index_draw(channel, target, &call, &px));
    CHECK(index_all_green(&px));
    channel->r3d[0x591] = 0;
    CHECK(!index_list_restart_cases(channel, target, indices));
    return 0;
}

/* Indirect draws (DrawCall.indirect): the arguments, and a multi-draw's count, are guest memory the GPU reads, in
   VkDrawIndirectCommand's and VkDrawIndexedIndirectCommand's layouts, and nothing of them is read on the CPU. A fan of
   vertices 4 to 7 (the green quad), a list through a base vertex, and two records under a count: the second draws the
   red quad over the green one only when the count says two */
static int put_words(u64 at, const u32* w, u32 n) {
    u8 bytes[64];
    for (u32 i = 0; i < n; i++) snx_st_le32(bytes + 4 * i, w[i]);
    CHECK(!gmem_write(at, bytes, 4 * (size_t)n));
    return 0;
}
static int indirect_cases(GpuChannel* channel, u64 target, u64 indices, u64 args) {
    IndexPixels px;
    const u32 list0[6] = {0, 1, 3, 1, 2, 3};
    const u32 fan[4] = {4, 1, 4, 0}; /* count, instances, first vertex, base instance */
    CHECK(!put_words(args, fan, 4));
    DrawCall call = {.topology = 6, .count = 1, .instances = 1, .indirect = args, .draw_count = 1, .stride = 16};
    CHECK(!index_draw(channel, target, &call, &px));
    if (!index_all_green(&px)) fprintf(stderr, "indirect fan: %u green, %u red\n", px.green, px.red);
    CHECK(index_all_green(&px));
    /* indices 0..3 through base vertex 4 (16-bit), then the same records under a count of 1 and of 2, the second
       record's base vertex 0 the red quad */
    CHECK(!index_buffer(channel, indices, list0, 6, 2, ~0u));
    const u32 two[10] = {6, 1, 0, 4, 0, 6, 1, 0, 0, 0}; /* count, instances, first index, base vertex, base instance */
    CHECK(!put_words(args, two, 10));
    call = (DrawCall){.topology = 4, .indexed = 1, .count = 1, .instances = 1, .indirect = args, .draw_count = 1,
                      .stride = 20};
    CHECK(!index_draw(channel, target, &call, &px));
    if (!index_all_green(&px)) fprintf(stderr, "indirect indexed: %u green, %u red\n", px.green, px.red);
    CHECK(index_all_green(&px));
    if (g_vk.has_draw_indirect_count) {
        const u32 one = 1, both = 2;
        CHECK(!put_words(args + 0x40, &one, 1));
        call.indirect_count = args + 0x40;
        call.draw_count = 2;
        CHECK(!index_draw(channel, target, &call, &px));
        CHECK(index_all_green(&px));
        CHECK(!put_words(args + 0x40, &both, 1));
        CHECK(!index_draw(channel, target, &call, &px));
        if (px.red != 16) fprintf(stderr, "indirect count of 2: %u green, %u red\n", px.green, px.red);
        CHECK(px.red == 16);
    }
    /* a render enable comparing two 64-bit values 16 bytes apart on the GPU (vk_query_pred_compare): equal draws under
       IF_EQUAL (VK_PRED_EQUAL), different doesn't */
    vk_lock();
    vk_query_reset(0x7ff0); /* an owner of its own: the query pool predicates are made with */
    vk_unlock();
    if (g_vk.has_cond_render && vk_query_pred_ok()) {
        const u32 values[6] = {7, 9, 0, 0, 7, 9};
        u32 slot = 0;
        for (int differ = 0; differ < 2; differ++) {
            const u32 changed[2] = {7, 10};
            CHECK(!put_words(args + 0x80, values, 6));
            if (differ) CHECK(!put_words(args + 0x90, changed, 2));
            call = (DrawCall){.topology = 6, .count = 4, .first = 4, .instances = 1};
            vk_lock();
            CHECK(vk_query_pred_compare(args + 0x80, &slot));
            vk_unlock();
            g_index_pred = (slot + 1) | VK_PRED_EQUAL;
            CHECK(!index_draw(channel, target, &call, &px));
            g_index_pred = 0;
            vk_lock();
            vk_query_pred_drop(slot);
            vk_unlock();
            if (differ ? px.blue != 16 : !index_all_green(&px))
                fprintf(stderr, "render enable on values %s: %u green, %u blue\n", differ ? "different" : "equal",
                        px.green, px.blue);
            CHECK(differ ? px.blue == 16 : index_all_green(&px));
        }
        puts("render enable: two values in guest memory compared on the GPU");
        /* a render enable comparing two samples-passed reports both still queued (release.c's compare_queued), around
           a draw: one that passes samples, and the two differ; then one predicated off, which counts none, and they
           are equal, as only the GPU knows. IF_NOT_EQUAL draws the green quad over the clear in the first alone */
        const u64 counts = args + 0x100;
        const u32 owner = channel->ds.owner;
        g_renderer->samples_reset(owner);
        for (int counted = 1; counted >= 0; counted--) {
            u32 off = 0, pred = 0;
            CHECK(!put_words(args + 0x80, values, 6));
            call = (DrawCall){.topology = 6, .count = 4, .first = 4, .instances = 1};
            vk_lock();
            CHECK(vk_query_pred_compare(args + 0x80, &off)); /* 0: the values don't differ */
            CHECK(vk_query_report(owner, counts, 16, 0));
            rt_clear(channel, target, 4, 4, 0, 1);
            channel->r3d_dirty = ~0u; /* the registers were set directly */
            mx_draw_state_sync(channel);
            channel->owner.pred = counted ? 0 : off + 1;
            vk_render_draw(&channel->ds, &call, &channel->owner);
            channel->owner.pred = 0;
            CHECK(vk_query_report(owner, counts + 16, 16, 0));
            vk_unlock();
            CHECK(g_renderer->render_predicate(counts, 0, &pred) == 2);
            g_index_pred = pred;
            CHECK(!index_draw(channel, target, &call, &px));
            g_index_pred = 0;
            /* a wrong frame says which side was wrong: the draw between the reports ran past its predicate (the
               reports landed apart), or the predicate over the counts' segments said they differ when they don't */
            int wrong = counted ? !index_all_green(&px) : px.blue != 16;
            u32 inner = wrong ? index_pred_word(off + 1) : 0, outer = wrong ? index_pred_word(pred) : 0;
            g_renderer->render_predicate_drop(pred);
            vk_lock();
            vk_query_pred_drop(off);
            vk_unlock();
            if (wrong) {
                u8 landed[24] = {0};
                vk_release_drain();
                CHECK(!gmem_read(counts, landed, sizeof landed));
                fprintf(stderr,
                        "render enable on two queued counts %s: %u green, %u blue; the draw between the reports had "
                        "predicate %u (%s), the render enable's predicate is %u, the reports landed %llu and %llu\n",
                        counted ? "apart" : "equal", px.green, px.blue, inner, counted ? "not used" : "0 skips it",
                        outer, (unsigned long long)snx_ld_le64(landed), (unsigned long long)snx_ld_le64(landed + 16));
            }
            CHECK(!wrong);
        }
        g_renderer->samples_enable(owner, 0);
        puts("render enable: two queued samples-passed counts compared on the GPU");
    }
    printf("indirect draws: a fan, indexed through a base vertex%s checked on the GPU\n",
           g_vk.has_draw_indirect_count ? ", and a count of one and two records" : "");
    return 0;
}

/* Vertex colors a copy engine wrote, which the GPU holds (docs/layers/gpu/ownership.md), read by a draw after another in
   the same pass: the red quad from the colors the CPU wrote, then the green one from the copy's. The second draw reads
   them where their owner holds them, inside the pass the first one opened, and nothing lands. Copied into staging,
   they ended the pass and a second one began over the same target */
static int owned_stream_case(GpuChannel* channel, u64 target, u64 colors, u64 copy, u64 copy_cpu) {
    const u32 bytes = 8 * 16;
    GpuDmaOp o;
    IndexPixels px;
    u32* r = channel->r3d;
    memset(&o, 0, sizeof o);
    o.width = o.line_in = o.line_out = bytes;
    o.lines = 1;
    o.src = pitch_side(colors, 0);
    o.dst = pitch_side(copy, 0);
    /* GPU-owned where the host traps; with unified memory, over an import, copied there on the GPU (vk_uma_publish) */
    vk_lock();
    int inplace = vk_uma_held(copy_cpu, bytes), owned = own_traps() && !inplace;
    vk_unlock();
    CHECK(g_renderer->dma_copy(&o));
    if (owned) CHECK(own_lookup(copy_cpu, bytes, NULL, 0) == 1);
    if (inplace) CHECK(own_lookup(copy_cpu, bytes, NULL, 0) == 0 && vk_uma_pending_cpu(copy_cpu, bytes));
    u64 landings = own_landings();
    vk_lock();
    rt_clear(channel, target, 4, 4, 0, 1);
    channel->r3d_dirty = ~0u; /* the registers were set directly */
    mx_draw_state_sync(channel);
    u64 passes = g_vk_passes, submits = vk_submit_number(), finishes = g_vk_finishes;
    u64 waits = 0;
    for (u32 i = 0; i < VK_WAIT_N; i++) waits += g_vk_waits[i];
    DrawCall red = {.topology = 6, .count = 4, .instances = 1},
             green = {.topology = 6, .first = 4, .count = 4, .instances = 1};
    vk_render_draw(&channel->ds, &red, &channel->owner);
    /* the color stream from the copy, which the CPU doesn't read: that would land it */
    r[0x705] = (u32)(copy >> 32);
    r[0x706] = (u32)copy;
    r[0x7c2] = (u32)((copy + bytes - 1) >> 32);
    r[0x7c3] = (u32)(copy + bytes - 1);
    channel->r3d_dirty = ~0u;
    mx_draw_state_sync(channel);
    vk_render_draw(&channel->ds, &green, &channel->owner);
    u64 begun = g_vk_passes - passes, submitted = vk_submit_number() - submits;
    u64 waited = 0;
    for (u32 i = 0; i < VK_WAIT_N; i++) waited += g_vk_waits[i];
    int no_wait = g_vk_finishes == finishes && waited == waits;
    CHECK(!index_pixels(target, &px));
    r[0x705] = (u32)(colors >> 32);
    r[0x706] = (u32)colors;
    r[0x7c2] = (u32)((colors + bytes - 1) >> 32);
    r[0x7c3] = (u32)(colors + bytes - 1);
    vk_unlock();
    if (!index_all_green(&px)) fprintf(stderr, "owned colors: %u green, %u red\n", px.green, px.red);
    CHECK(index_all_green(&px));
    if (owned || inplace) {
        /* Periodic asynchronous submissions can split the pass. The draws must still cause no CPU wait, and each
           extra pass must have a submission that explains it. */
        if (begun > 1 + submitted || !no_wait)
            fprintf(stderr, "owned colors: %llu passes, %llu submissions, no-wait=%d\n",
                    (unsigned long long)begun, (unsigned long long)submitted, no_wait);
        CHECK(begun >= 1 && begun <= 1 + submitted && no_wait);
        CHECK(own_landings() == landings && own_lookup(copy_cpu, bytes, NULL, 0) == owned);
    }
    printf("owned colors: a draw read vertex colors the GPU holds %s\n",
           owned     ? "where their owner holds them, inside the pass"
           : inplace ? "in guest memory, copied there on the GPU, inside the pass"
                     : "landed (no traps here)");
    return 0;
}

/* Indices a copy engine wrote, which the GPU holds, drawn after the red quad in the same pass: the green quad through
   them. The draw neither reads them on the CPU for the vertices they reach (render.c draw_last_vertex, which then
   takes the streams' limits) nor copies them: it reads them where their owner holds them, and nothing lands. Read on
   the CPU, they landed, a submit and a wait for the GPU, and the pass ended */
static int owned_index_case(GpuChannel* channel, u64 target, u64 indices, u64 indices_cpu, u64 staged) {
    const u32 list[6] = {4, 5, 6, 4, 6, 7};
    u8 bytes[12];
    GpuDmaOp o;
    IndexPixels px;
    u32* r = channel->r3d;
    for (u32 i = 0; i < 6; i++) snx_st_le16(bytes + i * 2, (u16)list[i]);
    CHECK(!gmem_write(staged, bytes, sizeof bytes));
    memset(&o, 0, sizeof o);
    o.width = o.line_in = o.line_out = sizeof bytes;
    o.lines = 1;
    o.src = pitch_side(staged, 0);
    o.dst = pitch_side(indices, 0);
    vk_lock();
    int inplace = vk_uma_held(indices_cpu, sizeof bytes), owned = own_traps() && !inplace;
    vk_unlock();
    CHECK(g_renderer->dma_copy(&o));
    /* the index registers over the copy's bytes, which the CPU doesn't read: that would land them, or wait for them
       with unified memory */
    r[0x5F2] = (u32)(indices >> 32);
    r[0x5F3] = (u32)indices;
    r[0x5F4] = (u32)((indices + sizeof bytes - 1) >> 32);
    r[0x5F5] = (u32)(indices + sizeof bytes - 1);
    r[0x5F6] = 1; /* 16-bit */
    r[0x591] = 0;
    if (owned) CHECK(own_lookup(indices_cpu, sizeof bytes, NULL, 0) == 1);
    if (inplace)
        CHECK(own_lookup(indices_cpu, sizeof bytes, NULL, 0) == 0 && vk_uma_pending_cpu(indices_cpu, sizeof bytes));
    u64 landings = own_landings();
    vk_lock();
    rt_clear(channel, target, 4, 4, 0, 1);
    channel->r3d_dirty = ~0u; /* the registers were set directly */
    mx_draw_state_sync(channel);
    u64 passes = g_vk_passes, submits = vk_submit_number(), finishes = g_vk_finishes;
    u64 waits = 0;
    for (u32 i = 0; i < VK_WAIT_N; i++) waits += g_vk_waits[i];
    DrawCall red = {.topology = 6, .count = 4, .instances = 1},
             green = {.topology = 4, .indexed = 1, .count = 6, .instances = 1};
    vk_render_draw(&channel->ds, &red, &channel->owner);
    vk_render_draw(&channel->ds, &green, &channel->owner);
    u64 begun = g_vk_passes - passes, submitted = vk_submit_number() - submits;
    u64 waited = 0;
    for (u32 i = 0; i < VK_WAIT_N; i++) waited += g_vk_waits[i];
    int no_wait = g_vk_finishes == finishes && waited == waits;
    CHECK(!index_pixels(target, &px));
    vk_unlock();
    if (!index_all_green(&px)) fprintf(stderr, "owned indices: %u green, %u red\n", px.green, px.red);
    CHECK(index_all_green(&px));
    if (owned || inplace) {
        /* As above, periodic submissions are allowed; a CPU index read or an unexplained pass break is not. */
        if (begun > 1 + submitted || !no_wait || own_landings() != landings)
            fprintf(stderr, "owned indices: %llu passes, %llu submissions, %llu landings, no-wait=%d\n",
                    (unsigned long long)begun, (unsigned long long)submitted,
                    (unsigned long long)(own_landings() - landings), no_wait);
        CHECK(begun >= 1 && begun <= 1 + submitted && no_wait && own_landings() == landings);
        CHECK(own_lookup(indices_cpu, sizeof bytes, NULL, 0) == owned);
    }
    printf("owned indices: a draw read the indices the GPU holds %s\n",
           owned     ? "where their owner holds them, inside the pass"
           : inplace ? "in guest memory, copied there on the GPU, inside the pass"
                     : "landed (no traps here)");
    return 0;
}

/* NVN's draw parameters in an indirect draw (DrawCall.params): the game's driver writes each draw's base vertex and
   base instance into constant buffer 0 at 0x640 and 0x644 from the record, on the GPU, and instanced vertex programs
   add the base instance to their instance ID to find their instance's data. nvn's bank 0 holds 0 there for an
   indirect draw, so one game drew every instance of its street props from the first records of its instance data,
   out of sight. Here a vertex program replaces the green component of the green quad's color with c[0][0x644], with
   bank 0 all zeros, and the record's base instance is 1.0f's bits: the quad is green only when the read takes the
   record's base instance */
static int draw_params_case(GpuChannel* channel, u64 target, u64 indices, u64 address) {
    const u64 program = address + 0x2000, cb0 = address + 0x4000, args = address + 0x16100;
    const u64 vertex[] = {0xefd9ff800807ff00ull, /* ALD R0..3, a[0x80] */
                          0xeff1ff800707ff00ull, /* position */
                          0xefd9ff800907ff00ull, /* ALD R0..3, a[0x90]: the green quad's 0, 1, 0, 1 */
                          0x4c98078019170001ull, /* MOV R1, c[0x0][0x644] */
                          0xeff1ff800807ff00ull, /* color varying */
                          0xe30000000007000full};
    IndexPixels px;
    u8 zeros[0x1000] = {0};
    CHECK(!gmem_write(cb0, zeros, sizeof zeros));
    CHECK(index_program(program, vertex, 6, 0));
    const u32 list0[6] = {0, 1, 3, 1, 2, 3};
    CHECK(!index_buffer(channel, indices, list0, 6, 2, ~0u));
    const u32 record[5] = {6, 1, 0, 4, 0x3f800000}; /* count, instances, first index, base vertex, base instance */
    CHECK(!put_words(args, record, 5));
    u32* r = channel->r3d;
    r[0x811] = 0x2000; /* VertexB's offset */
    channel->ds.bind.cb_addr[0][0] = cb0;
    channel->ds.bind.cb_size[0][0] = 0x1000;
    DrawCall call = {.topology = 4, .indexed = 1, .count = 1, .instances = 1, .indirect = args, .draw_count = 1,
                     .stride = 20, .params = SH_PUSH_GPU_BASE_VERTEX | SH_PUSH_GPU_BASE_INSTANCE};
    int ok = !index_draw(channel, target, &call, &px) && index_all_green(&px);
    if (!ok) fprintf(stderr, "indirect draw parameters: %u green, %u blue, %u red\n", px.green, px.blue, px.red);
    r[0x811] = 0;
    channel->ds.bind.cb_addr[0][0] = 0;
    channel->ds.bind.cb_size[0][0] = 0;
    CHECK(ok);
    puts("indirect draws: a vertex program's read of the base instance in bank 0 takes the record's");
    return 0;
}

/* A texture only a TXQ names has no type of its own: the program's variant takes it from the header (a racing
   game's lightmap programs query a volume through a bound handle and fetch it through a bindless one). Without
   that, the query is a 2D one, whose third component is the constant 1: a 4x2x2 volume's depth less one is then 0
   instead of 1, and the quad drawn with it as its green is black. Vertex programs and fragment programs alike */
static const u32 g_query_volume_tic_words = 1 | 1u << 16 | 1u << 31; /* height 2, depth 2, normalized */
static int query_volume_header(u64 pool, u64 samplers, u64 cbuf, u64 texels) {
    const u32 tic[8] = {0x08 /* A8B8G8R8 */ | 2u << 7 | 2u << 10 | 2u << 13 | 2u << 16 | 2u << 19 | 3u << 22 |
                            4u << 25 | 5u << 28,
                        (u32)texels,
                        (u32)(texels >> 32) | 3u << 21, /* block linear */
                        2u << 28,
                        3 | 2u << 23, /* width 4, 3D */
                        g_query_volume_tic_words,
                        0,
                        2u << 4};
    u8 bytes[256] = {0};
    for (u32 i = 0; i < 8; i++) snx_st_le32(bytes + i * 4, tic[i]);
    CHECK(!gmem_write(pool, bytes, 32));
    memset(bytes, 0, 256);
    CHECK(!gmem_write(samplers, bytes, 32)); /* a sampler of zeros */
    CHECK(!gmem_write(cbuf, bytes, 256));    /* the handle at 0x70: 0, the header above; an offset no other program
                                                of this test queries or samples, which would type the query */
    CHECK(!gmem_write(texels, bytes, 256));  /* 4x2x2 texels of nothing: only the size is asked */
    return 0;
}
static int vertex_query_volume_case(GpuChannel* channel, u64 target, u64 address) {
    const u64 program = address + 0x3800, pool = address + 0x1F000, samplers = pool + 0x100, cbuf = pool + 0x200,
              texels = pool + 0x400;
    const u64 vertex[] = {0xefd9ff800807ff00ull, /* ALD R0..3, a[0x80] */
                          0xeff1ff800707ff00ull, /* position */
                          0xefd9ff800907ff08ull, /* ALD R8..11, a[0x90]: the green quad's 0, 1, 0, 1 */
                          0x0100000000071005ull, /* MOV32I R5, 0: the level */
                          0x010ffffffff7f006ull, /* MOV32I R6, -1 */
                          0xdf48010200470500ull | 0xcull << 36, /* TXQ.DIMENSION R0, R5, 0x1c, depth only */
                          0x5c10000000670009ull, /* IADD R9, R0, R6 */
                          0x5cb8000000970a09ull, /* I2F.F32.U32 R9, R9: the green */
                          0xeff1ff800807ff08ull, /* color varying from R8..11 */
                          0xe30000000007000full};
    IndexPixels px;
    CHECK(!query_volume_header(pool, samplers, cbuf, texels));
    CHECK(index_program(program, vertex, 10, 0));
    u32* r = channel->r3d;
    r[0x55D] = (u32)(pool >> 32), r[0x55E] = (u32)pool, r[0x55F] = 0;
    r[0x557] = (u32)(samplers >> 32), r[0x558] = (u32)samplers, r[0x559] = 0;
    r[0x48D] = 0;
    r[0x982] = 2;
    r[0x811] = (u32)(program - address); /* VertexB's offset */
    channel->ds.bind.cb_addr[0][2] = cbuf;
    channel->ds.bind.cb_size[0][2] = 256;
    DrawCall call = {.topology = 7, .first = 4, .count = 4, .instances = 1};
    int ok = !index_draw(channel, target, &call, &px) && index_all_green(&px);
    if (!ok) {
        u8 first[4] = {0};
        index_first_pixel(target, first);
        fprintf(stderr, "vertex volume query: %u green, %u blue, %u red; first pixel %u %u %u %u\n", px.green, px.blue,
                px.red, first[0], first[1], first[2], first[3]);
    }
    r[0x811] = 0;
    channel->ds.bind.cb_addr[0][2] = 0;
    channel->ds.bind.cb_size[0][2] = 0;
    CHECK(ok);
    puts("vertex textures: a vertex program's size query of a volume it reads nowhere else answers its depth");
    return 0;
}
static int fragment_query_volume_case(GpuChannel* channel, u64 target, u64 address) {
    const u64 program = address + 0x3C00, pool = address + 0x1F000, samplers = pool + 0x100, cbuf = pool + 0x200,
              texels = pool + 0x400;
    u64 fragment[11];
    IndexPixels px;
    /* the varyings, then red = depth - 1 and no green: a quad the harness's own program (green) or a 2D query
       (black) can't draw */
    for (u32 i = 0; i < 4; i++) fragment[i] = 0xe000000000070000ull | (0x20ull + i) << 30 | i;
    fragment[4] = 0x0100000000071005ull; /* MOV32I R5, 0: the level */
    fragment[5] = 0x010ffffffff7f006ull; /* MOV32I R6, -1 */
    fragment[6] = 0x0100000000071001ull; /* MOV32I R1, 0: no green */
    fragment[7] = 0xdf48010200470504ull | 0xcull << 36; /* TXQ.DIMENSION R4, R5, 0x1c, depth only */
    fragment[8] = 0x5c10000000670400ull; /* IADD R0, R4, R6 */
    fragment[9] = 0x5cb8000000070a00ull; /* I2F.F32.U32 R0, R0: the red */
    fragment[10] = 0xe30000000007000full;
    CHECK(!query_volume_header(pool, samplers, cbuf, texels));
    u32* r = channel->r3d;
    r[0x55D] = (u32)(pool >> 32), r[0x55E] = (u32)pool, r[0x55F] = 0;
    r[0x557] = (u32)(samplers >> 32), r[0x558] = (u32)samplers, r[0x559] = 0;
    r[0x48D] = 0;
    r[0x982] = 2;
    channel->ds.bind.cb_addr[4][2] = cbuf;
    channel->ds.bind.cb_size[4][2] = 256;
    DrawCall call = {.topology = 7, .first = 4, .count = 4, .instances = 1};
    CHECK(index_program(program, fragment, 11, 1));
    r[0x851] = (u32)(program - address); /* the fragment program's offset */
    int ok = !index_draw(channel, target, &call, &px) && px.red == 16 && !px.green;
    if (!ok) {
        u8 first[4] = {0};
        index_first_pixel(target, first);
        fprintf(stderr, "fragment volume query: %u green, %u blue, %u red; first pixel %u %u %u %u (%llu draws, %llu skipped)\n",
                px.green, px.blue, px.red, first[0], first[1], first[2], first[3], (unsigned long long)g_vk_drawn,
                (unsigned long long)g_vk_skipped);
    }
    r[0x851] = 0x10000; /* the harness's fragment program again */
    channel->ds.bind.cb_addr[4][2] = 0;
    channel->ds.bind.cb_size[4][2] = 0;
    CHECK(ok);
    puts("fragment textures: a fragment program's size query of a volume it reads nowhere else answers its depth");
    return 0;
}

/* A sampler's min/max reduction on a device whose sampler has none (samplerFilterMinmax off): the fragment program
   takes the least or the greatest of the texels itself (vk_tex_minmax_in_program, shader_minmax_sample), where the
   sampler would average them. Edenprobe's sampling case: a 2x1 R32F texture of 0 and 1, linear both ways, no mip
   filter, clamped, looked up halfway between them, (0.5, 0.5): the least is 0, the greatest 1, the average a half.
   The fragment program writes the lookup's RGBA, so the first pixel's red is 0 or 255 (0.5 would be 127 or 128). The
   device's own filter first, where it has one for R32F */
static int minmax_fragment_case(GpuChannel* channel, u64 target, u64 address) {
    const u64 program = address + 0x6000, pool = address + 0x1F800, samplers = pool + 0x100, cbuf = pool + 0x200,
              texels = pool + 0x400;
    const u32 half = 0x3f000000;
    const u64 fragment[4] = {0x0100000000071004ull | (u64)half << 20, /* MOV32I R4, 0.5 */
                             0x0100000000071005ull | (u64)half << 20, /* MOV32I R5, 0.5 */
                             /* TEX R0, R4, c[0x40], 2D, RGBA */
                             0xc000000000000000ull | 0x10ull << 36 | 7ull << 51 | 0xfull << 31 | 2ull << 28 |
                                 0xffull << 20 | 7ull << 16 | 4ull << 8,
                             0xe30000000007000full};
    VkFormatProperties r32f;
    vkGetPhysicalDeviceFormatProperties(g_vk.phys, VK_FORMAT_R32_SFLOAT, &r32f);
    if (!(r32f.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)) {
        puts("min/max in a fragment program: R32F has no linear filter, skipped");
        return 0;
    }
    /* R32F, 2x1 texels, one level, block linear, normalized */
    const u32 tic[8] = {0x0f | 7u << 7 | 7u << 10 | 7u << 13 | 7u << 16 | 2u << 19 | 3u << 22 | 4u << 25 | 5u << 28,
                        (u32)texels,
                        (u32)(texels >> 32) | 3u << 21,
                        0,
                        1 | 1u << 23,
                        1u << 31,
                        0,
                        0};
    u8 bytes[256] = {0};
    for (u32 i = 0; i < 8; i++) snx_st_le32(bytes + i * 4, tic[i]);
    CHECK(!gmem_write(pool, bytes, 32));
    memset(bytes, 0, sizeof bytes);
    CHECK(!gmem_write(cbuf, bytes, 256)); /* the handle at 0x40: 0, the header above and sampler 0 */
    snx_st_le32(bytes + 4, 0x3f800000);   /* texel 1 is 1.0, texel 0 nought */
    CHECK(!gmem_write(texels, bytes, 256));
    CHECK(index_program(program, fragment, 4, 1));
    u32* r = channel->r3d;
    r[0x55D] = (u32)(pool >> 32), r[0x55E] = (u32)pool, r[0x55F] = 0;
    r[0x557] = (u32)(samplers >> 32), r[0x558] = (u32)samplers, r[0x559] = 0;
    r[0x48D] = 0;
    r[0x982] = 2;
    channel->ds.bind.cb_addr[4][2] = cbuf;
    channel->ds.bind.cb_size[4][2] = 256;
    r[0x851] = (u32)(program - address); /* the fragment program's offset */
    int minmax = vk_format_minmax_filter(VK_FORMAT_R32_SFLOAT), ok = 1;
    VkBool32 had = g_vk.feat12.samplerFilterMinmax;
    for (int device = minmax ? 1 : 0; device >= 0 && ok; device--) {
        g_vk.feat12.samplerFilterMinmax = device ? had : VK_FALSE;
        for (u32 reduction = 1; reduction <= 2 && ok; reduction++) {
            u32 tsc[8] = {2u | 2u << 3 | 2u << 6, 2u | 2u << 4 | 1u << 6 | reduction << 10};
            for (u32 i = 0; i < 8; i++) snx_st_le32(bytes + i * 4, tsc[i]);
            CHECK(!gmem_write(samplers, bytes, 32));
            DrawCall call = {.topology = 7, .first = 4, .count = 4, .instances = 1};
            IndexPixels px;
            u8 first[4] = {0};
            ok = !index_draw(channel, target, &call, &px) && !index_first_pixel(target, first) &&
                 first[0] == (reduction == 1 ? 0 : 255);
            if (!ok)
                fprintf(stderr, "min/max %s: the %s of a texel of 0 and one of 1 reads red %u, not %u\n",
                        device ? "by the device's sampler" : "in a fragment program",
                        reduction == 1 ? "least" : "greatest", first[0], reduction == 1 ? 0 : 255);
        }
    }
    g_vk.feat12.samplerFilterMinmax = had;
    r[0x851] = 0x10000; /* the harness's fragment program again */
    channel->ds.bind.cb_addr[4][2] = 0;
    channel->ds.bind.cb_size[4][2] = 0;
    CHECK(ok);
    printf("min/max in a fragment program: the least and the greatest of two texels taken by the program on a device "
           "without the filter (the device's own %s)\n",
           minmax ? "agrees" : "has none for R32F");
    return 0;
}

/* A fragment program that queries a volume, samples it, then samples a 2D texture through another handle: its own
   slots are the query's (2D until the header says otherwise), the volume's and the 2D texture's, and its variant
   keyed on the volume's header samples and queries the volume through one slot, so the 2D texture's slot is the
   second there. The variant's second slot took the program's second texture, the volume, which a 2D sample can't view
   (zeros: one game's dungeon lost its light mask so, and drew dark). Here the 2D texture is green, its green is the
   quad's green, and the query's depth less 2 its blue: the quad is green only when each slot reads its own texture */
static int fragment_query_slots_case(GpuChannel* channel, u64 target, u64 address) {
    const u64 program = address + 0x4000, pool = address + 0x1F000, samplers = pool + 0x100, cbuf = pool + 0x200,
              texels = pool + 0x400, green = pool + 0x600;
    const u64 fragment[] = {0x0100000000071005ull,                /* MOV32I R5, 0: the level */
                            0x010fffffffe71006ull,                /* MOV32I R6, -2 */
                            0x0100000000071008ull,                /* MOV32I R8, 0: the coordinates */
                            0x0100000000071009ull,                /* MOV32I R9, 0 */
                            0x010000000007100aull,                /* MOV32I R10, 0 */
                            0xdf48010200470504ull | 0xcull << 36, /* TXQ.DIMENSION R4, R5, 0x1c, depth only */
                            /* TEXS 3D R0 (red), R8, R10: the volume, handle at 0x70 */
                            0xd000000000000000ull | 1ull << 59 | 10ull << 53 | 0x1cull << 36 | 0xffull << 28 |
                                10ull << 20 | 7ull << 16 | 8ull << 8,
                            /* TEXS 2D.LZ R1 (green), R8, R9: the 2D texture, handle at 0x74 */
                            0xd000000000000000ull | 1ull << 59 | 2ull << 53 | 1ull << 50 | 0x1dull << 36 |
                                0xffull << 28 | 9ull << 20 | 7ull << 16 | 8ull << 8 | 1,
                            0x5c10000000670402ull, /* IADD R2, R4, R6 */
                            0x5cb8000000270a02ull, /* I2F.F32.U32 R2, R2: the blue */
                            0x0103f80000071003ull, /* MOV32I R3, 1.0 */
                            0xe30000000007000full};
    /* the 2D texture: header 1, 1x1 A8B8G8R8, block linear, its texel green */
    const u32 tic[8] = {0x08 | 2u << 7 | 2u << 10 | 2u << 13 | 2u << 16 | 2u << 19 | 3u << 22 | 4u << 25 | 5u << 28,
                        (u32)green,
                        (u32)(green >> 32) | 3u << 21,
                        0,
                        1u << 23, /* width 1, 2D */
                        1u << 31, /* height 1, depth 1, normalized */
                        0,
                        0};
    u8 bytes[512] = {0};
    IndexPixels px;
    CHECK(!query_volume_header(pool, samplers, cbuf, texels));
    for (u32 i = 0; i < 8; i++) snx_st_le32(bytes + i * 4, tic[i]);
    CHECK(!gmem_write(pool + 32, bytes, 32));
    memset(bytes, 0, sizeof bytes);
    bytes[1] = bytes[3] = 0xFF;
    CHECK(!gmem_write(green, bytes, sizeof bytes));
    memset(bytes, 0, sizeof bytes);
    snx_st_le32(bytes, 1); /* the 2D texture's handle at 0x74: header 1, sampler 0 */
    CHECK(!gmem_write(cbuf + 0x74, bytes, 4));
    u32* r = channel->r3d;
    r[0x55D] = (u32)(pool >> 32), r[0x55E] = (u32)pool, r[0x55F] = 1;
    r[0x557] = (u32)(samplers >> 32), r[0x558] = (u32)samplers, r[0x559] = 0;
    r[0x48D] = 0;
    r[0x982] = 2;
    channel->ds.bind.cb_addr[4][2] = cbuf;
    channel->ds.bind.cb_size[4][2] = 256;
    DrawCall call = {.topology = 7, .first = 4, .count = 4, .instances = 1};
    CHECK(index_program(program, fragment, sizeof fragment / sizeof fragment[0], 1));
    r[0x851] = (u32)(program - address);
    int ok = !index_draw(channel, target, &call, &px) && index_all_green(&px);
    if (!ok) {
        u8 first[4] = {0};
        index_first_pixel(target, first);
        fprintf(stderr, "fragment query slots: %u green, %u blue, %u red; first pixel %u %u %u %u\n", px.green, px.blue,
                px.red, first[0], first[1], first[2], first[3]);
    }
    r[0x851] = 0x10000;
    r[0x55F] = 0;
    channel->ds.bind.cb_addr[4][2] = 0;
    channel->ds.bind.cb_size[4][2] = 0;
    CHECK(ok);
    puts("fragment textures: a variant that samples and queries a volume through one slot reads its next texture "
         "from that texture's own handle");
    return 0;
}

/* A vertex program's texture of an integer format keys the program's variant, as a fragment program's does: its texels
   come back as their bits, where a program keyed float got no view of the image and read zeros (one game's virtual
   texture page table, drawn by a vertex program from an unsigned buffer texture, pointed past its cache). Here a
   vertex program fetches texel 0 of an R32_UINT buffer texture, 1.0f's bits, into the green quad's green: the quad
   is green only when the fetch keeps them. The header is entry 0 of the pool at address + 0x1F000, its handle (header 0,
   sampler 0) in the vertex stage's constant buffer 2 at 0x40 */
static int vertex_int_texture_case(GpuChannel* channel, u64 target, u64 address) {
    const u64 program = address + 0x3000, pool = address + 0x1F000, samplers = pool + 0x100, cbuf = pool + 0x200,
              texels = pool + 0x400;
    const u32 tic[8] = {0x0F | 4u << 7 | 4u << 10 | 4u << 13 | 4u << 16 | 2u << 19 | 3u << 22 | 4u << 25 | 5u << 28,
                        (u32)texels,
                        (u32)(texels >> 32),
                        0,
                        3 | 6u << 23,
                        0,
                        0,
                        0}; /* R32_UINT, a buffer of 4 */
    const u32 words[4] = {0x3f800000, 0, 0, 0};
    const u64 vertex[] = {0xefd9ff800807ff00ull, /* ALD R0..3, a[0x80] */
                          0xeff1ff800707ff00ull, /* position */
                          0xefd9ff800907ff08ull, /* ALD R8..11, a[0x90]: the green quad's 0, 1, 0, 1 */
                          0x0100000000071005ull, /* MOV32I R5, 0 */
                          /* TLDS.1D R0..R3 (a register a component), the texel in R5 */
                          0xd000000000000000ull | 1ull << 59 | 1ull << 57 | 4ull << 50 | 0x10ull << 36 | 2ull << 28 |
                              0xffull << 20 | 7ull << 16 | 5ull << 8,
                          0x5c98078000070009ull, /* MOV R9, R0 */
                          0xeff1ff800807ff08ull, /* color varying from R8..11 */
                          0xe30000000007000full};
    u8 bytes[256] = {0};
    IndexPixels px;
    for (u32 i = 0; i < 8; i++) snx_st_le32(bytes + i * 4, tic[i]);
    CHECK(!gmem_write(pool, bytes, 32));
    memset(bytes, 0, 32);
    CHECK(!gmem_write(samplers, bytes, 32)); /* a sampler of zeros: a texel fetch doesn't filter */
    CHECK(!gmem_write(cbuf, bytes, 256));    /* the handle at 0x40: 0 */
    CHECK(!put_words(texels, words, 4));
    CHECK(index_program(program, vertex, 8, 0));
    u32* r = channel->r3d;
    r[0x55D] = (u32)(pool >> 32), r[0x55E] = (u32)pool, r[0x55F] = 0;
    r[0x557] = (u32)(samplers >> 32), r[0x558] = (u32)samplers, r[0x559] = 0;
    r[0x48D] = 0;
    r[0x982] = 2;
    r[0x811] = (u32)(program - address); /* VertexB's offset */
    channel->ds.bind.cb_addr[0][2] = cbuf;
    channel->ds.bind.cb_size[0][2] = 256;
    DrawCall call = {.topology = 7, .first = 4, .count = 4, .instances = 1};
    int ok = !index_draw(channel, target, &call, &px) && index_all_green(&px);
    if (!ok) fprintf(stderr, "vertex integer texture: %u green, %u blue, %u red\n", px.green, px.blue, px.red);
    r[0x811] = 0;
    channel->ds.bind.cb_addr[0][2] = 0;
    channel->ds.bind.cb_size[0][2] = 0;
    CHECK(ok);
    puts("vertex textures: a vertex program reads an unsigned buffer texture's texel as its bits");
    return 0;
}

/* The highest index a draw reads (render.c's draw_last_vertex) bounds its streams' ranges, and is kept for an index
   range until the range's memory changes. In a stream of 512 vertices, 0 to 3 a red quad and 508 to 511 a green one,
   indices of the red quad bound the range to its first 4 KiB. The same indices rewritten to the green quad's must
   bound it again: a kept bound of 3 leaves vertices 508 to 511 past the range, which strict bounds read as zeros */
static int index_bound_cases(GpuChannel* channel, u64 target, u64 indices, u64 positions, u64 colors) {
    const float corner[8] = {-1, -1, 1, -1, 1, 1, -1, 1};
    u32* r = channel->r3d;
    u8 bytes[16];
    for (u32 v = 0; v < 512; v++) {
        int green = v >= 508;
        const float p[4] = {corner[(v & 3) * 2], corner[(v & 3) * 2 + 1], 0, 1};
        const float c[4] = {green ? 0.0f : 1.0f, green ? 1.0f : 0.0f, 0, 1};
        if (v >= 4 && !green) continue;
        for (u32 k = 0; k < 2; k++) {
            for (u32 i = 0; i < 4; i++) {
                u32 word;
                memcpy(&word, k ? c + i : p + i, 4);
                snx_st_le32(bytes + i * 4, word);
            }
            CHECK(!gmem_write((k ? colors : positions) + v * 16, bytes, 16));
        }
    }
    for (u32 stream = 0; stream < 2; stream++) {
        u64 source = stream ? colors : positions;
        r[0x701 + stream * 4] = (u32)(source >> 32);
        r[0x702 + stream * 4] = (u32)source;
        r[0x7c0 + stream * 2] = (u32)((source + 512 * 16 - 1) >> 32);
        r[0x7c1 + stream * 2] = (u32)(source + 512 * 16 - 1);
    }
    const u32 red[4] = {0, 1, 2, 3}, green[4] = {508, 509, 510, 511};
    DrawCall call = {.topology = 7, .indexed = 1, .count = 4, .instances = 1};
    IndexPixels px;
    for (u32 isz = 2; isz <= 4; isz *= 2) {
        CHECK(!index_buffer(channel, indices, red, 4, isz, ~0u));
        CHECK(!index_draw(channel, target, &call, &px));
        CHECK(px.red == 16);
        CHECK(!index_buffer(channel, indices, green, 4, isz, ~0u));
        CHECK(!index_draw(channel, target, &call, &px));
        if (g_vk.has_vertex_robustness2 && !index_all_green(&px))
            fprintf(stderr, "rewritten %u byte indices: %u green, %u blue, %u red\n", isz, px.green, px.blue, px.red);
        CHECK(!g_vk.has_vertex_robustness2 || index_all_green(&px));
    }
    return 0;
}

/* shadows=0 skips the depth-only passes into a shadow map, which is a depth target a draw samples with a compare, and
   no other depth-only pass: a depth prepass draws at any size. Here an 8x8 Z32F target (the color target is 4x4) takes a
   depth-only quad at depth 0.5 while nothing samples it: it draws. A fragment program then samples it with a compare
   (TEX.LZ.DC, 0.9 LEQUAL the texel, the result its green): no green under the quad's depth. From then on the target is
   a shadow map: the next pass into it is skipped and the target cleared to far, so the same sample is green (lit).
   Left unsampled for more presents than the renderer keeps a shadow map for, it takes its passes again */
static int shadow_map_depth(u64 map, float* depth) {
    vk_render_end();
    Surf* surface = vk_rt_find(map);
    VkBuffer buffer;
    u8* data = vk_readback_buffer(4, &buffer);
    CHECK(surface && data);
    vk_surf_use(vk_cmd(), surface, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT);
    VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1}, .imageExtent = {1, 1, 1}};
    rec_CmdCopyImageToBuffer(vk_cmd(), surface->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &copy);
    vk_finish();
    vk_readback_ready();
    memcpy(depth, data, 4);
    return 0;
}
/* a depth-only pass of the green quad into map: the depth of its first pixel afterwards, and whether the draw ran (1)
   or was skipped (0) */
static int shadow_map_pass(GpuChannel* channel, u64 map, float* depth, int* ran) {
    u32* r = channel->r3d;
    const DrawCall call = {.topology = 7, .first = 4, .count = 4, .instances = 1};
    u64 drawn = g_vk_drawn, skipped = g_vk_skipped;
    vk_lock();
    r[0x487] = 0; /* no color target */
    r[0x54E] = 1;
    r[0x3F8] = (u32)(map >> 32);
    r[0x3F9] = (u32)map;
    r[0x3FA] = 0x0A;   /* Z32F */
    r[0x3FB] = 0x1000; /* pitch linear */
    r[0x3FC] = 64;
    r[0x48A] = r[0x48B] = 8;
    r[0x48C] = 1 | 1u << 16;
    r[0x4B3] = r[0x4BA] = 1;
    r[0x4C3] = 0x207; /* ALWAYS */
    channel->r3d_dirty = ~0u;
    mx_draw_state_sync(channel);
    vk_render_draw(&channel->ds, &call, &channel->owner);
    r[0x487] = 1;
    r[0x54E] = r[0x4B3] = r[0x4BA] = 0;
    int failed = shadow_map_depth(map, depth);
    vk_unlock();
    CHECK(!failed);
    CHECK(g_vk_drawn - drawn + (g_vk_skipped - skipped) == 1);
    *ran = g_vk_drawn != drawn;
    return 0;
}
static u64 shadow_mov(u32 reg, float value) {
    u32 bits;
    memcpy(&bits, &value, 4);
    return 0x0100000000071000ull | (u64)bits << 20 | reg; /* MOV32I */
}
static int shadow_map_cases(GpuChannel* channel, u64 target, u64 address) {
    const u64 program = address + 0x4800, pool = address + 0x1F000, samplers = pool + 0x100, cbuf = pool + 0x200,
              map = address + 0x19000;
    /* the handle at 0x78 of the fragment stage's constant buffer 2: header 2, sampler 1 */
    const u64 fragment[] = {shadow_mov(8, 0.25f),                       /* the coordinates: a texel under the quad */
                            shadow_mov(9, 0.25f), shadow_mov(10, 0.9f), /* the reference */
                            /* TEX.LZ.DC R1 (green), R8, R10, 2D */
                            0xc000000000000000ull | 1ull << 55 | 7ull << 51 | 1ull << 50 | 0x1eull << 36 | 1ull << 31 |
                                2ull << 28 | 10ull << 20 | 7ull << 16 | 8ull << 8 | 1,
                            shadow_mov(0, 0), shadow_mov(2, 0), shadow_mov(3, 1), 0xe30000000007000full};
    /* the map as a texture: ZF32, 8x8, pitch linear of 32 bytes a row, normalized coordinates */
    const u32 tic[8] = {0x2F | 7u << 7 | 7u << 10 | 7u << 13 | 7u << 16 | 2u << 19 | 3u << 22 | 4u << 25 | 5u << 28,
                        (u32)map,
                        (u32)(map >> 32) | 2u << 21,
                        1,
                        7 | 1u << 23,
                        7 | 1u << 31,
                        0,
                        0};
    const u32 tsc[8] = {1u << 9 | 3u << 10}; /* nearest, a compare: LEQUAL */
    const DrawCall call = {.topology = 7, .first = 4, .count = 4, .instances = 1};
    u8 bytes[256] = {0};
    IndexPixels px;
    float depth;
    int ran, had = g_settings.shadows;
    CHECK(!gmem_write(map, bytes, 256));
    CHECK(!put_words(pool + 64, tic, 8));
    CHECK(!put_words(samplers + 32, tsc, 8));
    CHECK(!gmem_write(cbuf, bytes, 256));
    CHECK(!put_words(cbuf + 0x78, (const u32[1]){2 | 1u << 20}, 1));
    CHECK(index_program(program, fragment, sizeof fragment / sizeof fragment[0], 1));
    u32* r = channel->r3d;
    r[0x55D] = (u32)(pool >> 32), r[0x55E] = (u32)pool, r[0x55F] = 2;
    r[0x557] = (u32)(samplers >> 32), r[0x558] = (u32)samplers, r[0x559] = 1;
    r[0x48D] = 0;
    r[0x982] = 2;
    channel->ds.bind.cb_addr[4][2] = cbuf;
    channel->ds.bind.cb_size[4][2] = 256;
    g_settings.shadows = 0;
    /* a depth-only pass at another size than the color target's, into a target nothing samples: drawn */
    int ok = !shadow_map_pass(channel, map, &depth, &ran) && ran && depth == 0.5f;
    if (!ok)
        fprintf(stderr,
                "shadows=0, a depth-only pass into an 8x8 target no draw samples: %s, depth %g (want drawn, 0.5)\n",
                ran ? "drawn" : "skipped", (double)depth);
    /* sampled with a compare: under the quad's depth, no green */
    r[0x851] = (u32)(program - address);
    int sampled = ok && !index_draw(channel, target, &call, &px) && !px.green && !px.red && !px.blue;
    if (ok && !sampled)
        fprintf(stderr, "a compare sample of the drawn map: %u green, %u blue, %u red (want none: in shadow)\n",
                px.green, px.blue, px.red);
    ok = ok && sampled;
    /* the next frame's pass into what is now a shadow map: skipped, the map cleared to far and read as lit */
    g_vk_present++;
    r[0x851] = 0x10000;
    int skipped = ok && !shadow_map_pass(channel, map, &depth, &ran) && !ran && depth == 1.0f;
    if (ok && !skipped)
        fprintf(stderr,
                "shadows=0, a depth-only pass into a map sampled with a compare: %s, depth %g (want skipped, 1)\n",
                ran ? "drawn" : "skipped", (double)depth);
    ok = ok && skipped;
    r[0x851] = (u32)(program - address);
    int lit = ok && !index_draw(channel, target, &call, &px) && index_all_green(&px);
    if (ok && !lit)
        fprintf(stderr, "a compare sample of the skipped map: %u green, %u blue, %u red (want 16 green: lit)\n",
                px.green, px.blue, px.red);
    ok = ok && lit;
    /* no compare sample for longer than a shadow map is kept: its passes draw again */
    g_vk_present += 61;
    r[0x851] = 0x10000;
    int again = ok && !shadow_map_pass(channel, map, &depth, &ran) && ran && depth == 0.5f;
    if (ok && !again)
        fprintf(stderr,
                "shadows=0, a depth-only pass into a map last sampled 61 presents ago: %s, depth %g (want drawn, "
                "0.5)\n",
                ran ? "drawn" : "skipped", (double)depth);
    ok = ok && again;
    /* shadows=1 draws into a map sampled this frame */
    g_settings.shadows = 1;
    r[0x851] = (u32)(program - address);
    int on = ok && !index_draw(channel, target, &call, &px);
    r[0x851] = 0x10000;
    on = on && !shadow_map_pass(channel, map, &depth, &ran) && ran && depth == 0.5f;
    if (ok && !on)
        fprintf(stderr, "shadows=1, a depth-only pass into a sampled map: %s, depth %g (want drawn, 0.5)\n",
                ran ? "drawn" : "skipped", (double)depth);
    ok = ok && on;
    g_settings.shadows = had;
    r[0x55F] = r[0x559] = 0;
    channel->ds.bind.cb_addr[4][2] = 0;
    channel->ds.bind.cb_size[4][2] = 0;
    CHECK(ok);
    puts("shadows=0: a depth-only pass draws until a draw samples its target with a compare, is skipped while it does, "
         "and draws again once it stops");
    return 0;
}

/* A program that only asks the size of a texture its draw renders to binds the target's copy with none of the
   target's pixels brought into it (ShaderInfo.tex_unread_mask): one game's vertex programs ask the size of the target
   they draw to, in three full-target passes a frame, each of which copied the target first. Here a vertex program's
   TXQ of the 4x4 target is the quad's red, less 4: the quad is green when the query answers 4, and no copy is
   written, before a copy of the target exists and with one left by an earlier draw. Between the two a fragment
   program fetches the target's texel (0, 0) for every pixel: that draw has the pixels it found copied, the clear's
   blue. It fetches it again after the 3D engine's texture data cache invalidation, then after a fragment barrier:
   each of those makes the earlier draws' writes visible to texture reads, so each draw has the target copied again
   (g_vk_texture_epoch) */
static int query_target_case(GpuChannel* channel, u64 target, u64 address) {
    const u64 query = address + 0x5000, fetch = address + 0x5400, pool = address + 0x1F000, samplers = pool + 0x100,
              cbuf = pool + 0x200;
    const u64 vertex[] = {0xefd9ff800807ff00ull, /* ALD R0..3, a[0x80] */
                          0xeff1ff800707ff00ull, /* position */
                          0xefd9ff800907ff08ull, /* ALD R8..11, a[0x90]: the green quad's 0, 1, 0, 1 */
                          0x0100000000071005ull, /* MOV32I R5, 0: the level */
                          0x010fffffffc71006ull, /* MOV32I R6, -4 */
                          /* TXQ.DIMENSION R0, R5, 0x1f, width only */
                          (0xdf48010200470500ull & ~(1ull << 33)) | 1ull << 31 | 0xfull << 36,
                          0x5c10000000670008ull, /* IADD R8, R0, R6 */
                          0x5cb8000000870a08ull, /* I2F.F32.U32 R8, R8: the red */
                          0xeff1ff800807ff08ull, /* color varying from R8..11 */
                          0xe30000000007000full};
    const u64 fragment[] = {0x0100000000071008ull, /* MOV32I R8, 0: the texel's x */
                            0x0100000000071009ull, /* MOV32I R9, 0: and y */
                            /* TLD.LZ R0..3, R8, 2D, the handle at 0x7C */
                            0xdc00000000000000ull | 7ull << 51 | 0xfull << 31 | 2ull << 28 | 0x1full << 36 |
                                0xffull << 20 | 7ull << 16 | 8ull << 8,
                            0xe30000000007000full};
    /* the target as a texture: header 3, 4x4 A8B8G8R8, one level */
    const u32 tic[8] = {0x08 | 2u << 7 | 2u << 10 | 2u << 13 | 2u << 16 | 2u << 19 | 3u << 22 | 4u << 25 | 5u << 28,
                        (u32)target,
                        (u32)(target >> 32) | 3u << 21,
                        0,
                        3 | 1u << 23, /* width 4, 2D */
                        3 | 1u << 31, /* height 4, depth 1, normalized */
                        0,
                        0};
    u8 bytes[256] = {0};
    IndexPixels px = {0};
    for (u32 i = 0; i < 8; i++) snx_st_le32(bytes + i * 4, tic[i]);
    CHECK(!gmem_write(pool + 96, bytes, 32));
    memset(bytes, 0, sizeof bytes);
    CHECK(!gmem_write(samplers, bytes, 32));
    snx_st_le32(bytes + 0x7C, 3); /* the handle at 0x7C: header 3, sampler 0 */
    CHECK(!gmem_write(cbuf, bytes, 256));
    CHECK(index_program(query, vertex, 10, 0));
    CHECK(index_program(fetch, fragment, 4, 1));
    u32* r = channel->r3d;
    r[0x55D] = (u32)(pool >> 32), r[0x55E] = (u32)pool, r[0x55F] = 3;
    r[0x557] = (u32)(samplers >> 32), r[0x558] = (u32)samplers, r[0x559] = 0;
    r[0x48D] = 0;
    r[0x982] = 2;
    DrawCall call = {.topology = 7, .first = 4, .count = 4, .instances = 1};
    int ok = 1;
    static const char* const what[5] = {"a vertex program asks its width",
                                        "a fragment program fetches the target's texel",
                                        "a vertex program asks its width",
                                        "a fragment program fetches the target's texel after a texture data cache "
                                        "invalidation",
                                        "a fragment program fetches the target's texel after a fragment barrier"};
    for (u32 round = 0; ok && round < 5; round++) {
        int fetches = round == 1 || round >= 3, stage = fetches ? 4 : 0;
        if (round >= 3) g_renderer->method_barrier(round == 3 ? 2 : 0);
        u64 written = g_vk_rt_copy_writes, through = g_vk_rt_copies;
        r[0x811] = fetches ? 0 : (u32)(query - address);
        r[0x851] = fetches ? (u32)(fetch - address) : 0x10000;
        channel->ds.bind.cb_addr[stage][2] = cbuf;
        channel->ds.bind.cb_size[stage][2] = 256;
        ok = !index_draw(channel, target, &call, &px);
        channel->ds.bind.cb_addr[stage][2] = 0;
        channel->ds.bind.cb_size[stage][2] = 0;
        written = g_vk_rt_copy_writes - written;
        through = g_vk_rt_copies - through;
        ok = ok && (fetches ? px.blue == 16 && !px.green && !px.red && written == 1 && through == 1
                            : index_all_green(&px) && !written && !through);
        if (!ok)
            fprintf(stderr,
                    "target query: draw %u (%s): %u green, %u blue, %u red, %llu copies of the target written, %llu "
                    "draws through a copy\n",
                    round, what[round], px.green, px.blue, px.red, (unsigned long long)written,
                    (unsigned long long)through);
    }
    r[0x811] = 0;
    r[0x851] = 0x10000; /* the harness's programs again */
    CHECK(ok);
    puts("target queries: a program that only asks the size of the target it draws to has no copy of it written");
    return 0;
}

/* A fragment program that samples the target it draws to at a generic's (x, y) reads its own pixel when the vertex
   program writes its position from that generic, [0, 1] onto the viewport with a w of 1, and the viewport covers the
   target (ShaderInfo.uv_out_mask, tex_uv_mask): the draw samples in place where the device has feedback loops, with
   no copy of the target. One game composes a full-size target over itself so, once a frame. Here each pixel of the
   4x4 target holds its own color, and the fragment program writes back the texel it samples with red and green
   swapped: every pixel ends as its own, swapped. Through a copy it does the same wherever the proof doesn't hold: a
   viewport a pixel to the right (each pixel takes its left neighbour's), a generic counted from the bottom under a
   viewport counted from the top (each row takes its mirror's), and a w stored from an attribute. The generic counted
   from the bottom is in place again under a viewport of negative height. With clip set, the mapping is the fragment
   program's (ShaderInfo.pos_out_mask, screen_ndc): the vertex program writes the generic as its position itself, and
   the sample is at the generic's (x / w / 2 + 1/2, y / w / 2 + 1/2), or -y / w / 2 + 1/2 counted from the bottom. The
   same rounds, the generic loaded apart from the position in place of the stored w. Each round writes the target's
   texels anew and puts a texture barrier before its draw, as the console's texture reads see those writes only after
   one */
#define OWN_FFMA(d, a, c, scale, negc)                                                                                 \
    (0x32a0000000070000ull | (u64)((scale) >> 12 & 0x7ffff) << 20 | (u64)((scale) >> 31) << 56 | (u64)(c) << 39 |      \
     (u64)(negc) << 49 | (u64)(a) << 8 | (u64)(d))
#define OWN_AST(addr, reg, count)                                                                                      \
    (0xeff0000000070000ull | (u64)((count) - 1) << 47 | (u64)(addr) << 20 | 0xff00ull | (u64)(reg))
#define OWN_ALD(d, addr)          (0xefd87f8000070000ull | (u64)(addr) << 20 | 0xff00ull | (u64)(d))
#define OWN_IPA(d, addr)          (0xe000000000070000ull | (u64)(addr) << 28 | 0xff00ull | (u64)(d))
#define OWN_IPA_MUL(d, addr, mul) (OWN_IPA(d, addr) | 1ull << 54 | (u64)(mul) << 20)
#define OWN_RCP(d, a)             (0x5080000000470000ull | (u64)(a) << 8 | (u64)(d))
#define OWN_FMUL(d, a, b)         (0x5c68000000070000ull | (u64)(b) << 20 | (u64)(a) << 8 | (u64)(d))
static int own_target_case(GpuChannel* channel, u64 target, u64 address, int clip) {
    const u64 programs = address + (clip ? 0x6800 : 0x5800), sample = address + (clip ? 0x7400 : 0x6400),
              pool = address + 0x1F000, samplers = pool + 0x100, cbuf = pool + 0x200;
    const u32 half = 0x3f000000u, two = 0x40000000u;
    /* u = x / 2 + 1 / 2 and v = y / 2 + 1 / 2 (or 1 / 2 - y / 2, down) of the corner, then the position from them */
    u64 vertex[11] = {0xefd9ff800807ff00ull, /* ALD R0..3, a[0x80] */
                      0x0103f00000071006ull, /* MOV32I R6, 0.5 */
                      0x0103f80000071007ull, /* MOV32I R7, 1.0 */
                      OWN_FFMA(8, 0, 6, half, 0), OWN_FFMA(9, 1, 6, half, 0), OWN_FFMA(4, 8, 7, two, 1),
                      OWN_FFMA(5, 9, 7, two, 1),  OWN_AST(0x70, 4, 2),        OWN_AST(0x80, 8, 2),
                      0xe30000000007000full,      0xe30000000007000full};
    const u64 fragment[] = {0xe000000000070000ull | 0x20ull << 30 | 8, /* IPA R8, a[0x80] */
                            0xe000000000070000ull | 0x21ull << 30 | 9, /* IPA R9, a[0x84] */
                            /* TEX R0..3, R8, 2D, the handle at 0x7C: the target */
                            0xc000000000000000ull | 0x1full << 36 | 0xfull << 31 | 2ull << 28 | 0xffull << 20 |
                                7ull << 16 | 8ull << 8,
                            0x5c98078000070004ull,              /* MOV R4, R0 */
                            0x5c98078000070000ull | 1ull << 20, /* MOV R0, R1 */
                            0x5c98078000070001ull | 4ull << 20, /* MOV R1, R4 */
                            0xe30000000007000full};
    /* clip: the corner loaded a word at a time, stored as the position and as the generic; the generic's x stored
       again from a load of its own where the proof must not hold (the 8th instruction) */
    u64 clip_vertex[9] = {OWN_ALD(0, 0x80),    OWN_ALD(1, 0x84),      OWN_ALD(2, 0x88),
                          OWN_ALD(3, 0x8c),    OWN_ALD(4, 0x80),      OWN_AST(0x70, 0, 4),
                          OWN_AST(0x80, 0, 4), 0xe30000000007000full, 0xe30000000007000full};
    /* the generic's x / w and y / w, each times 1/2 (y's -1/2, down) plus 1/2, then the sample as above */
    u64 clip_fragment[16] = {OWN_IPA(0, 0x7c),           OWN_RCP(0, 0),
                             OWN_IPA_MUL(1, 0x80, 0),    OWN_IPA_MUL(2, 0x84, 0),
                             OWN_IPA_MUL(3, 0x8c, 0),    OWN_RCP(3, 3),
                             OWN_FMUL(4, 1, 3),          OWN_FMUL(5, 2, 3),
                             0x0103f00000071006ull, /* MOV32I R6, 0.5 */
                             OWN_FFMA(8, 4, 6, half, 0), OWN_FFMA(9, 5, 6, half, 0)};
    const u32 tic[8] = {0x08 | 2u << 7 | 2u << 10 | 2u << 13 | 2u << 16 | 2u << 19 | 3u << 22 | 4u << 25 | 5u << 28,
                        (u32)target,
                        (u32)(target >> 32) | 3u << 21,
                        0,
                        3 | 1u << 23, /* width 4, 2D */
                        3 | 1u << 31, /* height 4, depth 1, normalized */
                        0,
                        0};
    static const char* const names[2][5] = {
        {"the generic mapped onto the whole viewport", "a viewport a pixel to the right",
         "a generic counted from the bottom, a viewport from the top", "a w stored from an attribute",
         "a generic counted from the bottom, a viewport of negative height"},
        {"the position mapped from clip space onto the whole viewport", "clip space, a viewport a pixel to the right",
         "clip space counted from the bottom, a viewport from the top", "the generic loaded apart from the position",
         "clip space counted from the bottom, a viewport of negative height"}};
    u8 bytes[256] = {0}, initial[64], out[64];
    u32* r = channel->r3d;
    float kept[6];
    for (u32 i = 0; i < 8; i++) snx_st_le32(bytes + i * 4, tic[i]);
    CHECK(!gmem_write(pool + 96, bytes, 32));
    memset(bytes, 0, sizeof bytes);
    CHECK(!gmem_write(samplers, bytes, 32));
    snx_st_le32(bytes + 0x7C, 3); /* the handle at 0x7C: header 3, sampler 0 */
    CHECK(!gmem_write(cbuf, bytes, 256));
    for (u32 i = 0; i < 16; i++) {
        const u8 color[4] = {(u8)(10 + 60 * (i & 3)), (u8)(20 + 50 * (i >> 2)), (u8)(100 + i), 255};
        memcpy(initial + i * 4, color, 4);
    }
    if (clip) {
        memcpy(clip_fragment + 11, fragment + 2, 5 * sizeof *fragment);
        CHECK(index_program(sample, clip_fragment, 16, 2));
        clip_fragment[10] = OWN_FFMA(9, 5, 6, half ^ 0x80000000u, 0);
        CHECK(index_program(sample + 0x200, clip_fragment, 16, 2));
    } else
        CHECK(index_program(sample, fragment, 7, 1));
    r[0x55D] = (u32)(pool >> 32), r[0x55E] = (u32)pool, r[0x55F] = 3;
    r[0x557] = (u32)(samplers >> 32), r[0x558] = (u32)samplers, r[0x559] = 0;
    r[0x48D] = 0;
    r[0x982] = 2;
    memcpy(kept, r + 0x280, sizeof kept);
    DrawCall call = {.topology = 7, .first = 4, .count = 4, .instances = 1};
    int ok = 1;
    for (u32 round = 0; ok && round < 5; round++) {
        float transform[6];
        int down = round == 2 || round == 4, in_place = (round == 0 || round == 4) && g_vk.has_feedback;
        u64 program = programs + round * 0x200;
        memcpy(transform, kept, sizeof transform);
        if (round == 1) transform[3] += 1.0f;         /* the translation's x: the viewport from x = 1 */
        if (round == 4) transform[1] = -transform[1]; /* the scale's y: a viewport of negative height */
        vertex[4] = down ? OWN_FFMA(9, 1, 6, half ^ 0x80000000u, 0) : OWN_FFMA(9, 1, 6, half, 0);
        vertex[6] = down ? OWN_FFMA(5, 9, 7, two ^ 0x80000000u, 0) : OWN_FFMA(5, 9, 7, two, 1);
        vertex[9] = round == 3 ? OWN_AST(0x7c, 3, 1) : 0xe30000000007000full;
        clip_vertex[7] = round == 3 ? OWN_AST(0x80, 4, 1) : 0xe30000000007000full;
        ok = clip ? index_program(program, clip_vertex, 9, 0) : index_program(program, vertex, 11, 0);
        if (!ok) break;
        memcpy(r + 0x280, transform, sizeof transform);
        r[0x811] = (u32)(program - address);
        r[0x851] = (u32)(sample + (clip && down ? 0x200 : 0) - address);
        channel->ds.bind.cb_addr[4][2] = cbuf;
        channel->ds.bind.cb_size[4][2] = 256;
        vk_lock();
        rt_clear(channel, target, 4, 4, 0, 1);
        channel->r3d_dirty = ~0u; /* the registers were set directly */
        mx_draw_state_sync(channel);
        Surf* surface = vk_rt_find(target);
        VkBuffer upload, readback;
        u64 offset;
        u8* staged = surface ? vk_stage(sizeof initial, 4, &upload, &offset) : NULL;
        u8* data = NULL;
        u64 loops = g_vk_fb_draws, written = g_vk_rt_copy_writes, through = g_vk_rt_copies, drawn = g_vk_drawn;
        ok = staged != NULL;
        if (ok) {
            memcpy(staged, initial, sizeof initial);
            vk_surf_use(vk_cmd(), surface, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                        VK_ACCESS_2_TRANSFER_WRITE_BIT);
            VkBufferImageCopy region = {.bufferOffset = offset,
                                        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                        .imageExtent = {4, 4, 1}};
            rec_CmdCopyBufferToImage(vk_cmd(), upload, surface->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
            vk_surf_wrote(surface);
            vk_rt_drawn(surface);
            vk_surf_touch(surface);
            g_renderer->method_barrier(0); /* NVN's ORDER_FRAGMENTS | INVALIDATE_TEXTURE */
            vk_render_draw(&channel->ds, &call, &channel->owner);
            loops = g_vk_fb_draws - loops, written = g_vk_rt_copy_writes - written, through = g_vk_rt_copies - through;
            ok = g_vk_drawn == drawn + 1;
            vk_render_end();
            data = vk_readback_buffer(64, &readback);
        }
        if (ok && data) {
            vk_surf_use(vk_cmd(), surface, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                        VK_ACCESS_2_TRANSFER_READ_BIT);
            VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                      .imageExtent = {4, 4, 1}};
            rec_CmdCopyImageToBuffer(vk_cmd(), surface->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback, 1, &copy);
            vk_finish();
            vk_readback_ready();
            memcpy(out, data, sizeof out);
        } else
            ok = 0;
        vk_unlock();
        channel->ds.bind.cb_addr[4][2] = 0;
        channel->ds.bind.cb_size[4][2] = 0;
        if (ok && (in_place ? loops != 1 || written || through : loops || written != 1 || through != 1)) {
            fprintf(
                stderr, "own target, %s: %llu draws in place, %llu through a copy, %llu copies written; expected %s\n",
                names[clip][round], (unsigned long long)loops, (unsigned long long)through, (unsigned long long)written,
                in_place ? "1 in place and no copy" : "1 through a copy, written once");
            ok = 0;
        }
        for (u32 i = 0; ok && i < 16; i++) {
            u32 x = i & 3, y = i >> 2;
            /* the texel a pixel samples: its own, its left neighbour's (none for the first column, which the viewport
               leaves as it was), or its row's mirror */
            int kept_pixel = round == 1 && !x;
            u32 from = kept_pixel ? i : round == 1 ? i - 1 : round == 2 ? (3 - y) * 4 + x : i;
            u8 want[4];
            memcpy(want, initial + from * 4, 4);
            if (!kept_pixel) want[0] = initial[from * 4 + 1], want[1] = initial[from * 4];
            if (memcmp(out + i * 4, want, 4)) {
                fprintf(stderr, "own target, %s: pixel (%u, %u) is %u %u %u %u, expected %u %u %u %u\n",
                        names[clip][round], x, y, out[i * 4], out[i * 4 + 1], out[i * 4 + 2], out[i * 4 + 3], want[0],
                        want[1], want[2], want[3]);
                ok = 0;
            }
        }
    }
    memcpy(r + 0x280, kept, sizeof kept);
    r[0x811] = 0;
    r[0x851] = 0x10000; /* the harness's programs again */
    CHECK(ok);
    printf("own target: a draw that samples its target at a generic %s reads it %s\n",
           clip ? "written as its position, mapped from clip space," : "its position is written from",
           !g_vk.has_feedback ? "through a copy (no attachment feedback loops)"
           : clip             ? "in place, and through a copy under another viewport, sign or generic"
                              : "in place, and through a copy under another viewport, sign or w");
    return 0;
}
#undef OWN_FFMA
#undef OWN_AST
#undef OWN_ALD
#undef OWN_IPA
#undef OWN_IPA_MUL
#undef OWN_RCP
#undef OWN_FMUL

static int check_index_draws(void) {
    const u64 address = 0x478000000ull, cpu = HEAP_BASE + 0x7A0000;
    const u64 positions = address + 0x12000, colors = address + 0x13000, indices = address + 0x14000,
              target = address + 0x18000;
    const u64 vertex[] = {0xefd9ff800807ff00ull, /* ALD R0..3, a[0x80] */
                          0xeff1ff800707ff00ull, /* position */
                          0xefd9ff800907ff00ull, /* ALD R0..3, a[0x90] */
                          0xeff1ff800807ff00ull, /* color varying */
                          0xe30000000007000full};
    u64 fragment[5];
    /* two quads over the whole target, corners in the order quads take them */
    const float corner[8] = {-1, -1, 1, -1, 1, 1, -1, 1};
    const float transform[6] = {2, 2, 0.5f, 2, 2, 0.5f};
    GpuChannel* channel = calloc(1, sizeof *channel);
    CHECK(channel);
    channel->id = 125;
    gmmu_map(address, cpu, 0x20000);
    u8 bytes[8 * 16];
    for (u32 v = 0; v < 8; v++) {
        const float p[4] = {corner[(v & 3) * 2], corner[(v & 3) * 2 + 1], 0, 1};
        const float c[4] = {v < 4 ? 1.0f : 0.0f, v < 4 ? 0.0f : 1.0f, 0, 1};
        for (u32 i = 0; i < 4; i++) {
            u32 word;
            memcpy(&word, p + i, 4);
            snx_st_le32(bytes + v * 16 + i * 4, word);
        }
        CHECK(!gmem_write(positions + v * 16, bytes + v * 16, 16));
        for (u32 i = 0; i < 4; i++) {
            u32 word;
            memcpy(&word, c + i, 4);
            snx_st_le32(bytes + v * 16 + i * 4, word);
        }
        CHECK(!gmem_write(colors + v * 16, bytes + v * 16, 16));
    }
    for (u32 i = 0; i < 4; i++) fragment[i] = 0xe000000000070000ull | (0x20ull + i) << 30 | i;
    fragment[4] = 0xe30000000007000full;
    CHECK(index_program(address, vertex, 5, 0));
    CHECK(index_program(address + 0x10000, fragment, 5, 1));
    u32* r = channel->r3d;
    r[MAXWELL3D_RASTERIZE_ENABLE] = 1; /* as reset_3d starts a channel */
    r[0x582] = (u32)(address >> 32);
    r[0x583] = (u32)address;
    r[0x850] = 1;
    r[0x851] = 0x10000;
    r[0x64b] = 1;
    r[0x286] = 0x6420;
    r[0x3fd] = r[0x3fe] = 4u << 16;
    r[0x680] = 0x1111;
    memcpy(r + 0x280, transform, sizeof transform);
    for (u32 stream = 0; stream < 2; stream++) {
        u64 source = stream ? colors : positions;
        r[0x458 + stream] = stream | 1u << 21 | 7u << 27; /* RGBA32F */
        r[0x700 + stream * 4] = 0x1000 | 16;
        r[0x701 + stream * 4] = (u32)(source >> 32);
        r[0x702 + stream * 4] = (u32)source;
        r[0x7c0 + stream * 2] = (u32)((source + 8 * 16 - 1) >> 32);
        r[0x7c1 + stream * 2] = (u32)(source + 8 * 16 - 1);
    }
    int had8 = g_vk.has_index8;
    CHECK(!index_cases(channel, target, indices));
    g_vk.has_index8 = 0; /* the widening on the GPU, which a device without 8-bit indices takes */
    CHECK(!index_cases(channel, target, indices));
    g_vk.has_index8 = had8;
    int had_list_restart = g_vk.has_list_restart;
    g_vk.has_list_restart = 0; /* the way a device without VK_EXT_primitive_topology_list_restart takes */
    CHECK(!index_list_restart_cases(channel, target, indices));
    g_vk.has_list_restart = had_list_restart;
    CHECK(!indirect_cases(channel, target, indices, address + 0x16000));
    CHECK(!owned_stream_case(channel, target, colors, address + 0x15000, cpu + 0x15000));
    CHECK(!owned_index_case(channel, target, indices, cpu + 0x14000, address + 0x11000));
    if (g_vk.has_shader_draw_parameters) CHECK(!draw_params_case(channel, target, indices, address));
    CHECK(!vertex_int_texture_case(channel, target, address));
    CHECK(!vertex_query_volume_case(channel, target, address));
    CHECK(!fragment_query_volume_case(channel, target, address));
    CHECK(!fragment_query_slots_case(channel, target, address));
    CHECK(!minmax_fragment_case(channel, target, address));
    CHECK(!shadow_map_cases(channel, target, address));
    CHECK(!query_target_case(channel, target, address));
    CHECK(!own_target_case(channel, target, address, 0));
    CHECK(!own_target_case(channel, target, address, 1));
    CHECK(!index_bound_cases(channel, target, indices, address + 0x1A000, address + 0x1C000));
    printf(
        "index draws: quads, widened and native 8-bit indices, restart indices and rewritten indices' bounds checked "
        "(8-bit indices %s, lists restart %s)\n",
        had8 ? "native" : "unavailable", had_list_restart ? "where the 3D engine's do" : "nowhere: the device can't");
    free(channel);
    return 0;
}
