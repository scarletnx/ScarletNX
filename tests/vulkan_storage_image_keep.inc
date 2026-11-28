/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A fragment program's store into a 2D storage image: a 64x64 R32_UINT block linear surface of 16 KB, each 4 KB page
   a column of 16 texels, the texel and its value in the program's constant buffer. Where the host traps guest memory,
   the image's span is GPU-owned from the bind on, and nothing of it is copied on the CPU before the draw nor back
   after it. The release leaves the image kept, the next draw joins it, and a read publishes it over the span: the
   CPU's own read too, which lands its owner (own.c's claim hook), and a draw's buffer over it. The first draw's bind
   stages the span once, as memory holds it. A CPU write between the draw and the release lands the span, and the
   release then merges the image with what the CPU wrote by the byte, as the write-back always did: the CPU's bytes and
   the store beside them on the same page both stay. The image's next draws take that write-back. Elsewhere every draw
   takes it, as before. No extracted shaders or game data are used */
#define IMGKEEP_GVA  0x4c0000000ull
#define IMGKEEP_CPU  (HEAP_BASE + 0x5400000)
#define IMGKEEP_SIZE 0x4000u

/* texel (x, y)'s offset in a w x h R32 image of blocks 8 GOBs high */
static u64 imgkeep_at(u32 x, u32 y, u32 w, u32 h) { return bl_offset(x * 4, y, 0, w * 4, h, 3, 0); }
static u32 imgkeep_pattern(u64 at) { return 0xc0de0000u ^ (u32)(at >> 2); }
/* what the CPU writes at at: the pattern's word with every byte changed, as the merge goes by the byte */
static u32 imgkeep_cpu(u32 x, u32 y) { return ~imgkeep_pattern(imgkeep_at(x, y, 64, 64)); }
static u32 imgkeep_word0(void) { return 0x0f | 4u << 7 | 4u << 10 | 4u << 13 | 4u << 16; } /* R32, unsigned */

/* the image at image, w x h texels, filled with the pattern as the CPU writes it, as header 0 of the pool */
static int imgkeep_image(u64 pool, u64 image, u32 w, u32 h) {
    const u32 tic[8] = {
        imgkeep_word0(), (u32)image, (u32)(image >> 32) | 3u << 21, 3u << 3, (w - 1) | 1u << 23, h - 1, 0, 0};
    u64 size = (u64)w * 4 * h;
    u8* bytes = malloc(size);
    CHECK(bytes);
    for (u64 i = 0; i < size; i += 4) snx_st_le32(bytes + i, imgkeep_pattern(i));
    CHECK(!gmem_write(image, bytes, size));
    free(bytes);
    CHECK(!storage_write_words(pool, tic, 8));
    return 0;
}
/* a draw over the viewport whose fragments store value at texel (x, y) */
static int imgkeep_draw(GpuChannel* channel, u64 cbuf, u32 x, u32 y, u32 value) {
    int drawn, skipped;
    CHECK(!storage_write_words(cbuf + 0x124, (const u32[3]){x, y, value}, 3));
    channel->r3d_dirty = ~0u;
    targetless_draw(channel, &drawn, &skipped);
    if (!drawn || skipped) fprintf(stderr, "storage image draw: drawn %d, skipped %d\n", drawn, skipped);
    CHECK(drawn && !skipped);
    return 0;
}
/* the image write-backs' counts since was (vk_gfx_image_stats) */
typedef struct {
    u64 copied, back, published;
} ImgCount;
static ImgCount imgkeep_since(ImgCount was) {
    ImgCount now;
    vk_gfx_image_stats(&now.copied, &now.back, &now.published);
    return (ImgCount){now.copied - was.copied, now.back - was.back, now.published - was.published};
}
/* what a release does with the draws' storage images */
static void imgkeep_release(void) {
    vk_lock();
    vk_gfx_image_put_aside();
    vk_unlock();
}
/* the image's bytes as the CPU reads them: the pattern, but for n texels {x, y, value} */
static int imgkeep_expect(const char* when, u64 image, const u32 (*texel)[3], u32 n) {
    static u8 got[IMGKEEP_SIZE];
    CHECK(!gmem_read(image, got, sizeof got));
    for (u32 i = 0; i < IMGKEEP_SIZE; i += 4) {
        u32 want = imgkeep_pattern(i);
        for (u32 k = 0; k < n; k++)
            if (imgkeep_at(texel[k][0], texel[k][1], 64, 64) == i) want = texel[k][2];
        if (snx_ld_le32(got + i) != want) {
            fprintf(stderr, "storage image %s: the word at %#x is %08x, expected %08x\n", when, i, snx_ld_le32(got + i),
                    want);
            return 1;
        }
    }
    return 0;
}
/* the texel (x, y) of the texture header tic looks up, as its image holds it (its first texel where it is scaled): want,
   or a message saying when. Not the image's own surface, or the check would read the store where it is */
static int imgkeep_view_texel(const u32* tic, const Surf* image, u32 x, u32 y, u32 want, const char* when) {
    vk_lock();
    Surf* t = vk_tex_from_tic(tic);
    CHECK(t && t != image);
    u32* pixels = malloc((size_t)t->sw * t->sh * 4);
    CHECK(pixels);
    CHECK(!sc_pixels(t, pixels));
    u32 got = pixels[(size_t)(y * t->sh / t->h) * t->sw + x * t->sw / t->w];
    free(pixels);
    vk_unlock();
    if (got != want)
        fprintf(stderr,
                "storage image read through a texture of another format %s: texel %u,%u is %08x, expected %08x\n", when,
                x, y, got, want);
    CHECK(got == want);
    return 0;
}
/* pixel (x, y) of the render target at gva, as its image holds it after the read check a texture's lookup of it makes
   (vk_rt_note_read): want, or a message saying when */
static int imgkeep_target_texel(u64 gva, u32 x, u32 y, u32 want, const char* when) {
    vk_lock();
    Surf* rt = vk_rt_find(gva);
    CHECK(rt);
    vk_rt_note_read(rt);
    u32* pixels = malloc((size_t)rt->sw * rt->sh * 4);
    CHECK(pixels);
    CHECK(!sc_pixels(rt, pixels));
    u32 got = pixels[(size_t)(y * rt->sh / rt->h) * rt->sw + x * rt->sw / rt->w];
    free(pixels);
    vk_unlock();
    if (got != want)
        fprintf(stderr,
                "storage image over a render target's memory, the target read %s: pixel %u,%u is %08x, "
                "expected %08x\n",
                when, x, y, got, want);
    CHECK(got == want);
    return 0;
}
/* the channel that draws: a triangle over a 4x4 viewport with four color targets of format 0 and no depth, the
   fragment program storing c[0][0x12c] at texel (c[0][0x124], c[0][0x128]) of the image whose handle is c[0][0x120] */
static GpuChannel* imgkeep_channel(u64 address, u64 pool) {
    const u64 positions = address + 0x2000, cbuf = address + 0x3000;
    const u64 vertex[] = {0xefd9ff800807ff00ull, /* ALD R0..3, a[0x80] */
                          0xeff1ff800707ff00ull, /* position */
                          0xe30000000007000full};
    const u64 store[] = {0x4c9807800497000full,                                /* MOV R15, c[0][0x124] */
                         0x4c98078004a70010ull,                                /* MOV R16, c[0][0x128] */
                         0x4c98078004b70018ull,                                /* MOV R24, c[0][0x12c] */
                         0x4c9807800487001cull,                                /* MOV R28, c[0][0x120] */
                         (0xeb200e0200f70f18ull & ~(7ull << 33)) | 3ull << 33, /* SUST.P.RGBA 2D [R15], R24, R28 */
                         0xe30000000007000full};
    const float position[12] = {-1, -1, 0, 1, 3, -1, 0, 1, -1, 3, 0, 1};
    const float transform[6] = {2, 2, 0.5f, 2, 2, 0.5f};
    u8 bytes[48];
    GpuChannel* channel = calloc(1, sizeof *channel);
    if (!channel) return NULL;
    channel->id = 126;
    for (u32 i = 0; i < 12; i++) {
        u32 word;
        memcpy(&word, position + i, 4);
        snx_st_le32(bytes + 4 * i, word);
    }
    /* The storage-only fragment stage needs neither interpolated inputs nor color outputs. */
    if (gmem_write(positions, bytes, sizeof bytes) || storage_write_words(cbuf + 0x120, (const u32[1]){0}, 1) ||
        !mesh_program(address, vertex, 3, 0) || !mesh_program(address + 0x1000, store, 6, 0)) {
        free(channel);
        return NULL;
    }
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
        u64 a = address + 0x8000 + i * 0x400;
        r[0x200 + i * 0x10] = (u32)(a >> 32);
        r[0x201 + i * 0x10] = (u32)a;
        r[0x202 + i * 0x10] = 4;
        r[0x203 + i * 0x10] = 4;
        r[0x204 + i * 0x10] = 0; /* no format: nothing is written there */
    }
    r[0x54e] = 0;
    r[0x55D] = (u32)(pool >> 32), r[0x55E] = (u32)pool, r[0x55F] = 0;
    channel->ds.bind.cb_addr[4][0] = cbuf;
    channel->ds.bind.cb_size[4][0] = 0x1000;
    channel->r3d_dirty = ~0u;
    return channel;
}

static int check_storage_image_keep(void) {
    const u64 address = IMGKEEP_GVA, cbuf = address + 0x3000, pool = address + 0x6000;
    const u64 first = address + 0x10000, second = address + 0x20000;
    const u64 second_cpu = IMGKEEP_CPU + 0x20000;
    CHECK(mem_map(IMGKEEP_CPU, 0x40000, MS_NORMAL, PERM_RW) == 0); /* past the heap */
    gmmu_map(address, IMGKEEP_CPU, 0x40000);
    vk_lock();
    vk_finish();
    u32 validation = g_vk_validation_messages;
    vk_unlock();
    GpuChannel* channel = imgkeep_channel(address, pool);
    CHECK(channel);

    /* two stores, a release after each: kept on the GPU where the host traps memory, the first bind staging the span.
       A release publishes nothing, and copies nothing back */
    CHECK(!imgkeep_image(pool, first, 64, 64));
    int traps = own_traps(), expect_kept = traps && !vk_uma();
    ImgCount was = imgkeep_since((ImgCount){0}), d;
    CHECK(!imgkeep_draw(channel, cbuf, 2, 3, 0x5eed0001));
    imgkeep_release();
    d = imgkeep_since(was);
    int kept = !d.back && !d.published;
    if (expect_kept && !kept)
        fprintf(stderr,
                "storage image: the host traps memory, but the draw copied %llu bytes of the image before it and %llu "
                "back after the release, and published %llu images, where it keeps the image on the GPU: at most %u, "
                "0 and 0\n",
                (unsigned long long)d.copied, (unsigned long long)d.back, (unsigned long long)d.published,
                IMGKEEP_SIZE);
    CHECK(!expect_kept || kept);
    CHECK(traps || !kept);
    if (kept)
        CHECK(d.copied <= IMGKEEP_SIZE && !d.back);
    else
        CHECK(d.copied == IMGKEEP_SIZE && d.back == IMGKEEP_SIZE);
    was = imgkeep_since((ImgCount){0});
    CHECK(!imgkeep_draw(channel, cbuf, 7, 9, 0x5eed0011));
    imgkeep_release();
    d = imgkeep_since(was);
    if (kept && (d.copied || d.back || d.published))
        fprintf(stderr,
                "storage image kept: its next draw copied %llu bytes before it and %llu back after the release, and "
                "published %llu images: 0, 0 and 0 expected\n",
                (unsigned long long)d.copied, (unsigned long long)d.back, (unsigned long long)d.published);
    CHECK(!kept || (!d.copied && !d.back && !d.published));
    if (kept) CHECK(own_lookup(IMGKEEP_CPU + 0x10000, IMGKEEP_SIZE, NULL, 0) == 1);
    if (kept) { /* the CPU's own read of the span, past the runtime's hooks: its landing publishes the image first */
        u32 word = snx_ld_le32(GP(IMGKEEP_CPU + 0x10000 + imgkeep_at(7, 9, 64, 64)));
        d = imgkeep_since(was);
        if (word != 0x5eed0011 || d.published != 1)
            fprintf(stderr,
                    "storage image kept past its releases: the CPU read %08x and %llu images were published, "
                    "5eed0011 and 1 expected\n",
                    word, (unsigned long long)d.published);
        CHECK(word == 0x5eed0011 && d.published == 1);
    }
    CHECK(!imgkeep_expect("after two releases", first, (const u32[][3]){{2, 3, 0x5eed0001}, {7, 9, 0x5eed0011}}, 2));

    /* a draw's buffer read of a kept image's span after its release: the owner it binds where it is holds the span's
       bytes from before the draw, so the image is published first and the read has the store */
    const u64 third = address + 0x30000;
    CHECK(!imgkeep_image(pool, third, 64, 64));
    was = imgkeep_since((ImgCount){0});
    CHECK(!imgkeep_draw(channel, cbuf, 11, 13, 0x5eed0021));
    imgkeep_release();
    d = imgkeep_since(was);
    CHECK(!kept || (!d.back && !d.published));
    if (kept) {
        VkBuffer buf, rb;
        u64 at;
        const u8* got;
        vk_lock();
        CHECK(vk_buf_get(third, IMGKEEP_SIZE, &buf, &at) && (got = vk_readback_buffer(IMGKEEP_SIZE, &rb)));
        VkBufferCopy c = {at, 0, IMGKEEP_SIZE};
        vk_render_end();
        rec_CmdCopyBuffer(vk_cmd(), buf, rb, 1, &c);
        vk_finish();
        vk_readback_ready();
        u32 word = snx_ld_le32(got + imgkeep_at(11, 13, 64, 64));
        vk_unlock();
        d = imgkeep_since(was);
        if (word != 0x5eed0021 || d.published != 1)
            fprintf(stderr,
                    "storage image kept past its release, read as a buffer: the read has %08x and %llu images were "
                    "published, 5eed0021 and 1 expected\n",
                    word, (unsigned long long)d.published);
        CHECK(word == 0x5eed0021 && d.published == 1);
    }
    CHECK(!imgkeep_expect("read as a buffer after its release", third, (const u32[][3]){{11, 13, 0x5eed0021}}, 1));

    /* the CPU writes between the draw and the release, in place as a guest thread does, past the runtime's hooks: the
       stored texel (its write comes last, as on the console where the GPU had stored), the texel beside it on its
       page and a texel on another page. All of them stay, and the store beside them */
    const u32 cpu_texels[][2] = {{2, 3}, {3, 3}, {40, 10}, {6, 7}};
    u32 cpu_words[4];
    for (u32 i = 0; i < 4; i++) cpu_words[i] = imgkeep_cpu(cpu_texels[i][0], cpu_texels[i][1]);
    CHECK(!imgkeep_image(pool, second, 64, 64));
    CHECK(!imgkeep_draw(channel, cbuf, 2, 3, 0x5eed0002));
    for (u32 i = 0; i < 3; i++)
        snx_st_le32(GP(second_cpu + imgkeep_at(cpu_texels[i][0], cpu_texels[i][1], 64, 64)), cpu_words[i]);
    imgkeep_release();
    CHECK(!imgkeep_expect("written by the CPU before the release", second,
                          (const u32[][3]){{2, 3, cpu_words[0]}, {3, 3, cpu_words[1]}, {40, 10, cpu_words[2]}}, 3));

    /* the CPU wrote that image lately: its next draw takes the write-back with its bytes from before the draw, and a
       CPU write beside the store on the same page keeps both */
    was = imgkeep_since((ImgCount){0});
    CHECK(!imgkeep_draw(channel, cbuf, 5, 7, 0x5eed0003));
    d = imgkeep_since(was);
    if (d.copied != IMGKEEP_SIZE)
        fprintf(stderr, "storage image the CPU wrote: the draw copied %llu bytes before it, expected %u\n",
                (unsigned long long)d.copied, IMGKEEP_SIZE);
    CHECK(d.copied == IMGKEEP_SIZE);
    snx_st_le32(GP(second_cpu + imgkeep_at(6, 7, 64, 64)), cpu_words[3]);
    imgkeep_release();
    CHECK(!imgkeep_expect("written by the CPU beside the store", second,
                          (const u32[][3]){{2, 3, cpu_words[0]},
                                           {3, 3, cpu_words[1]},
                                           {40, 10, cpu_words[2]},
                                           {5, 7, 0x5eed0003},
                                           {6, 7, cpu_words[3]}},
                          5));
    /* a texture of another format over a kept image, looked up after a store: its next store, kept again over the
       owner that the read published, moves nothing that the texture's check asks (memory's GPU write generation, the
       CPU's writes), and shows in the texture when it is looked up again */
    const u64 fourth = address + 0x34000;
    const u32 r32f = 0x0f | 7u << 7 | 7u << 10 | 7u << 13 | 7u << 16, rgba = 2u << 19 | 3u << 22 | 4u << 25 | 5u << 28;
    const u32 view_tic[8] = {
        r32f | rgba, (u32)fourth, (u32)(fourth >> 32) | 3u << 21, 3u << 3, 63 | 1u << 23, 63, 0, 0};
    const u32 image_tic[8] = {
        imgkeep_word0(), (u32)fourth, (u32)(fourth >> 32) | 3u << 21, 3u << 3, 63 | 1u << 23, 63, 0, 0};
    CHECK(!imgkeep_image(pool, fourth, 64, 64));
    CHECK(!imgkeep_draw(channel, cbuf, 2, 3, 0x5eed0031));
    imgkeep_release();
    vk_lock();
    Surf* fourth_image = vk_tex_from_tic_storage(image_tic);
    vk_unlock();
    CHECK(fourth_image);
    CHECK(!imgkeep_view_texel(view_tic, fourth_image, 2, 3, 0x5eed0031, "after a store"));
    vk_lock();
    CHECK(vk_tex_from_tic(view_tic)); /* checked again, with no submission after it: the CPU's writes stay asked */
    vk_unlock();
    CHECK(!imgkeep_draw(channel, cbuf, 7, 9, 0x5eed0032));
    imgkeep_release();
    CHECK(!imgkeep_view_texel(view_tic, fourth_image, 7, 9, 0x5eed0032, "looked up again after the next store"));
    CHECK(!imgkeep_expect("read through a texture of another format", fourth,
                          (const u32[][3]){{2, 3, 0x5eed0031}, {7, 9, 0x5eed0032}}, 2));

    /* the same for a render target of another format over the image's memory, read after each store: an R32 image
       there is a surface of its own, kept as the others */
    const u64 fifth = address + 0x38000;
    GpuChannel* targets = calloc(1, sizeof *targets);
    CHECK(targets);
    targets->id = 125;
    rt_clear_format(targets, fifth, 64, 64, 3, 0, 0xD5);
    CHECK(!storage_write_words(
        pool,
        (const u32[8]){imgkeep_word0(), (u32)fifth, (u32)(fifth >> 32) | 3u << 21, 3u << 3, 63 | 1u << 23, 63, 0, 0},
        8));
    CHECK(!imgkeep_draw(channel, cbuf, 2, 3, 0x5eed0041));
    imgkeep_release();
    CHECK(!imgkeep_target_texel(fifth, 2, 3, 0x5eed0041, "after a store"));
    CHECK(!imgkeep_draw(channel, cbuf, 7, 9, 0x5eed0042));
    imgkeep_release();
    CHECK(!imgkeep_target_texel(fifth, 7, 9, 0x5eed0042, "again after the next store"));
    free(targets);
    vk_lock();
    vk_finish();
    CHECK(g_vk_validation_messages == validation);
    vk_unlock();
    free(channel);
    printf("draws' storage images: %s; the CPU's writes before the release and the stores beside them stay\n",
           kept ? "kept on the GPU, nothing copied before the draw or back after it"
                : "written back with their bytes from before the draw (no traps here)");
    return 0;
}

/* A compute program's store into the same kind of image. A dispatch with a storage image runs on the graphics queue
   (compute.c's vk_dispatch), and its image is kept as a draw's is: nothing is published at the dispatch nor at the
   release, and its next dispatch joins it. A read publishes it then: the CPU's own, a buffer's over it, and a
   texture's through a header of another format, whose fetch has the stores. An image of less than a page is
   published at its dispatch, as each one was before. Before, every dispatch published its image over its span (one
   game: 74 a frame, nearly all of them never read) */
#define CSKEEP_GVA  0x4f3000000ull
#define CSKEEP_CPU  (HEAP_BASE + 0x5700000) /* between vulkan_trim.inc's 64 KiB and vulkan_pass_breaks.inc's */
#define CSKEEP_SPAN 0x60000u

/* MOV Rd, c[2][off] */
static u64 cskeep_mov(u32 rd, u32 off) { return 0x4c98078800070000ull | (u64)(off >> 2) << 20 | rd; }
/* header index of the pool: an R32 image (word0) at image, w x h texels in blocks of block_height GOBs (a power of 2) */
static int cskeep_header(u64 pool, u32 index, u32 word0, u64 image, u32 w, u32 h, u32 block_height) {
    u32 log2 = 0;
    while (1u << log2 < block_height) log2++;
    const u32 tic[8] = {word0, (u32)image, (u32)(image >> 32) | 3u << 21, log2 << 3, (w - 1) | 1u << 23, h - 1, 0, 0};
    CHECK(!storage_write_words(pool + 32ull * index, tic, 8));
    return 0;
}
/* the pattern over a w x h R32 image of size bytes at image, as the CPU writes it */
static int cskeep_fill(u64 image, u64 size) {
    u8* bytes = malloc(size);
    CHECK(bytes);
    for (u64 i = 0; i < size; i += 4) snx_st_le32(bytes + i, imgkeep_pattern(i));
    CHECK(!gmem_write(image, bytes, size));
    free(bytes);
    return 0;
}
/* a block of one thread whose program stores c[2][0x12c] at texel (c[2][0x124], c[2][0x128]) of the image whose
   handle is c[2][0x120]; with fetch, the texel there of the texture whose handle is c[2][0x130] instead (TLD.B) */
static int cskeep_dispatch(GpuChannel* channel, u32 handle, u32 x, u32 y, u32 value, int fetch, u32 texture) {
    const u64 code_at = CSKEEP_GVA + (fetch ? 0x400 : 0), cbuf = CSKEEP_GVA + 0x1000;
    const u64 sust = (0xeb203a0200f77614ull & ~(0xffull | 0xffull << 8 | 0xfull << 16 | 0xffull << 39 | 7ull << 33)) |
                     8ull | 7ull << 16 | 6ull << 39 | 3ull << 33; /* SUST.P [R0, R1], R8, handle R6 */
    u64 ops[8];
    u32 n = 0, at = 0;
    u8 code[128] = {0};
    u32 qmd[64] = {0};
    u64 dispatches = g_vk_dispatches;
    DispatchState d;
    ops[n++] = cskeep_mov(0, 0x124);
    ops[n++] = cskeep_mov(1, 0x128);
    ops[n++] = cskeep_mov(8, 0x12c);
    if (fetch) {
        ops[n++] = cskeep_mov(4, 0x130);
        ops[n++] = 0xdd38000080070008ull | 2ull << 28 | 4ull << 20; /* TLD.B R8, [R0, R1], R4 */
    }
    ops[n++] = cskeep_mov(6, 0x120);
    ops[n++] = sust;
    ops[n++] = 0xe30000000007000full; /* EXIT */
    for (u32 i = 0; i < n; i++) {
        if (!(i % 3)) snx_st_le64(code + 8 * at++, 0x001f8000fc0007e0ull);
        snx_st_le64(code + 8 * at++, ops[i]);
    }
    CHECK(!gmem_write(code_at, code, sizeof code));
    CHECK(!storage_write_words(cbuf + 0x120, (const u32[5]){handle, x, y, value, texture}, 5));
    qmd[QMD_GRID_DIM_X] = 1;
    qmd[QMD_GRID_DIM_YZ] = 1 | 1u << 16;
    qmd[QMD_BLOCK_DIM_X] = 1u << 16;
    qmd[QMD_BLOCK_DIM_YZ] = 1 | 1u << 16;
    qmd[QMD_CONST_BUFFER_ENABLE] = 1u << 2;
    qmd[QMD_CONST_BUFFER_CONFIG + 4] = (u32)cbuf;
    qmd[QMD_CONST_BUFFER_CONFIG + 5] = (u32)(cbuf >> 32) | 0x200u << 15;
    vk_lock();
    mx_dispatch_state(qmd, channel->rcomp, code_at, &d);
    vk_dispatch(&d);
    vk_unlock();
    if (g_vk_dispatches != dispatches + 1) fprintf(stderr, "compute storage image: the dispatch was skipped\n");
    CHECK(g_vk_dispatches == dispatches + 1);
    return 0;
}
/* the images published since was over their spans (VK_PUB_OTHER: a dispatch's, or a kept one's when read) */
static u64 cskeep_published(u64 was) { return g_vk_rt_watch.by[VK_PUB_OTHER] - was; }
/* the w x h R32 image at image as the CPU reads it: the pattern, but for n texels {x, y, value} */
static int cskeep_expect(const char* when, u64 image, u32 w, u32 h, u32 block_height, const u32 (*texel)[3], u32 n) {
    const u64 size = (u64)w * 4 * h;
    u8* got = malloc(size);
    u32 log2 = 0;
    while (1u << log2 < block_height) log2++;
    CHECK(got);
    CHECK(!gmem_read(image, got, size));
    for (u64 i = 0; i < size; i += 4) {
        u32 want = imgkeep_pattern(i);
        for (u32 k = 0; k < n; k++)
            if (bl_offset(texel[k][0] * 4, texel[k][1], 0, w * 4, h, log2, 0) == i) want = texel[k][2];
        if (snx_ld_le32(got + i) != want) {
            fprintf(stderr, "compute storage image %s: the word at %#llx is %08x, expected %08x\n", when,
                    (unsigned long long)i, snx_ld_le32(got + i), want);
            free(got);
            return 1;
        }
    }
    free(got);
    return 0;
}

static int check_compute_storage_image_keep(void) {
    const u64 pool = CSKEEP_GVA + 0x2000;
    const u64 first = CSKEEP_GVA + 0x10000, second = CSKEEP_GVA + 0x20000, third = CSKEEP_GVA + 0x30000,
              copy = CSKEEP_GVA + 0x40000, small = CSKEEP_GVA + 0x50000;
    const u32 r32f = 0x0f | 7u << 7 | 7u << 10 | 7u << 13 | 7u << 16, rgba = 2u << 19 | 3u << 22 | 4u << 25 | 5u << 28;
    GpuChannel* channel = calloc(1, sizeof *channel);
    CHECK(channel);
    if (!g_vk.feat.shaderStorageImageWriteWithoutFormat) {
        puts("compute storage images: unformatted writes unavailable, skipped");
        free(channel);
        return 0;
    }
    CHECK(mem_map(CSKEEP_CPU, CSKEEP_SPAN, MS_NORMAL, PERM_RW) == 0); /* past the heap */
    gmmu_map(CSKEEP_GVA, CSKEEP_CPU, CSKEEP_SPAN);
    channel->id = 127;
    channel->rcomp[KEPLER_COMPUTE_TIC] = (u32)(pool >> 32);
    channel->rcomp[KEPLER_COMPUTE_TIC + 1] = (u32)pool;
    channel->rcomp[KEPLER_COMPUTE_TEX_CB_INDEX] = 2;
    /* 0..2: the three images, 3: the third as an R32_FLOAT texture, 4: the copy, 5: the image of less than a page */
    CHECK(!cskeep_header(pool, 0, imgkeep_word0(), first, 64, 64, 8));
    CHECK(!cskeep_header(pool, 1, imgkeep_word0(), second, 64, 64, 8));
    CHECK(!cskeep_header(pool, 2, imgkeep_word0(), third, 64, 64, 8));
    CHECK(!cskeep_header(pool, 3, r32f | rgba, third, 64, 64, 8));
    CHECK(!cskeep_header(pool, 4, imgkeep_word0(), copy, 64, 64, 8));
    CHECK(!cskeep_header(pool, 5, imgkeep_word0(), small, 8, 8, 1));
    CHECK(!cskeep_fill(first, IMGKEEP_SIZE) && !cskeep_fill(second, IMGKEEP_SIZE) && !cskeep_fill(third, IMGKEEP_SIZE));
    CHECK(!cskeep_fill(copy, IMGKEEP_SIZE) && !cskeep_fill(small, 0x200));
    vk_lock();
    vk_finish();
    u32 validation = g_vk_validation_messages;
    vk_unlock();
    const int traps = own_traps(), expect_kept = traps && !vk_uma();

    /* two stores, a release after each: kept, where the host traps memory, with nothing published or copied back,
       and the second dispatch joins the first's image with nothing copied before it */
    u64 pub = g_vk_rt_watch.by[VK_PUB_OTHER];
    ImgCount was = imgkeep_since((ImgCount){0}), d;
    u64 why_was[VK_IMG_KEEP_N], why[VK_IMG_KEEP_N], read;
    vk_gfx_image_keep_stats(why_was, &read);
    CHECK(!cskeep_dispatch(channel, 0, 2, 3, 0x5eed0101, 0, 0));
    imgkeep_release();
    d = imgkeep_since(was);
    const int kept = !cskeep_published(pub) && !d.back && !d.published;
    if (expect_kept && !kept)
        fprintf(stderr,
                "compute storage image: the host traps memory, but the dispatch and its release published %llu images, "
                "copied %llu bytes back and published %llu kept ones, where the image stays on the GPU: 0, 0 and 0\n",
                (unsigned long long)cskeep_published(pub), (unsigned long long)d.back, (unsigned long long)d.published);
    CHECK(!expect_kept || kept);
    pub = g_vk_rt_watch.by[VK_PUB_OTHER];
    was = imgkeep_since((ImgCount){0});
    CHECK(!cskeep_dispatch(channel, 0, 7, 9, 0x5eed0111, 0, 0));
    imgkeep_release();
    d = imgkeep_since(was);
    if (kept && (cskeep_published(pub) || d.copied || d.back || d.published))
        fprintf(stderr,
                "compute storage image kept: its next dispatch and release published %llu images, copied %llu bytes "
                "before it and %llu back, and published %llu kept ones: 0, 0, 0 and 0 expected\n",
                (unsigned long long)cskeep_published(pub), (unsigned long long)d.copied, (unsigned long long)d.back,
                (unsigned long long)d.published);
    CHECK(!kept || (!cskeep_published(pub) && !d.copied && !d.back && !d.published));
    /* the 600-present log's counts: the first dispatch's image kept, the second's joined to it, and nothing else */
    vk_gfx_image_keep_stats(why, &read);
    for (int i = 0; i < VK_IMG_KEEP_N; i++) {
        const u64 want = i == VK_IMG_KEEP_KEPT || i == VK_IMG_KEEP_JOINED;
        if (kept && why[i] - why_was[i] != want)
            fprintf(stderr, "compute storage images kept: %llu binds counted as reason %d, %llu expected\n",
                    (unsigned long long)(why[i] - why_was[i]), i, (unsigned long long)want);
        CHECK(!kept || why[i] - why_was[i] == want);
    }
    if (kept) CHECK(own_lookup(CSKEEP_CPU + 0x10000, IMGKEEP_SIZE, NULL, 0) == 1);
    if (kept) { /* the CPU's own read of the span, past the runtime's hooks: its landing publishes the image first */
        u32 word = snx_ld_le32(GP(CSKEEP_CPU + 0x10000 + imgkeep_at(7, 9, 64, 64)));
        d = imgkeep_since(was);
        if (word != 0x5eed0111 || d.published != 1)
            fprintf(stderr,
                    "compute storage image kept past its releases: the CPU read %08x and %llu images were published, "
                    "5eed0111 and 1 expected\n",
                    word, (unsigned long long)d.published);
        CHECK(word == 0x5eed0111 && d.published == 1);
        u64 read_now;
        vk_gfx_image_keep_stats(why, &read_now);
        if (read_now - read != 1)
            fprintf(stderr,
                    "compute storage image kept, read by the CPU: %llu kept ones counted as published when read, "
                    "1 expected\n",
                    (unsigned long long)(read_now - read));
        CHECK(read_now - read == 1);
    }
    CHECK(!cskeep_expect("after two releases", first, 64, 64, 8,
                         (const u32[][3]){{2, 3, 0x5eed0101}, {7, 9, 0x5eed0111}}, 2));

    /* a draw's buffer read of a kept image's span after its release: published first, so the read has the store */
    was = imgkeep_since((ImgCount){0});
    CHECK(!cskeep_dispatch(channel, 1, 11, 13, 0x5eed0121, 0, 0));
    imgkeep_release();
    if (kept) {
        VkBuffer buf, rb;
        u64 at;
        const u8* got;
        vk_lock();
        CHECK(vk_buf_get(second, IMGKEEP_SIZE, &buf, &at) && (got = vk_readback_buffer(IMGKEEP_SIZE, &rb)));
        VkBufferCopy c = {at, 0, IMGKEEP_SIZE};
        vk_render_end();
        rec_CmdCopyBuffer(vk_cmd(), buf, rb, 1, &c);
        vk_finish();
        vk_readback_ready();
        u32 word = snx_ld_le32(got + imgkeep_at(11, 13, 64, 64));
        vk_unlock();
        d = imgkeep_since(was);
        if (word != 0x5eed0121 || d.published != 1)
            fprintf(stderr,
                    "compute storage image kept past its release, read as a buffer: the read has %08x and %llu images "
                    "were published, 5eed0121 and 1 expected\n",
                    word, (unsigned long long)d.published);
        CHECK(word == 0x5eed0121 && d.published == 1);
    }
    CHECK(!cskeep_expect("read as a buffer after its release", second, 64, 64, 8,
                         (const u32[][3]){{11, 13, 0x5eed0121}}, 1));

    /* a later dispatch's fetch of the kept image through an R32_FLOAT header over it (the bits of a float that is no
       subnormal), stored into a fourth image: it has the store. Then a second store, kept again over the owner the
       fetch's read published (nothing the texture's check asks moves), and a fetch of it through the same texture */
    CHECK(!cskeep_dispatch(channel, 2, 4, 5, 0x5eed0131, 0, 0));
    imgkeep_release();
    CHECK(!cskeep_dispatch(channel, 4, 4, 5, 0, 1, 3));
    imgkeep_release();
    CHECK(!cskeep_dispatch(channel, 2, 6, 7, 0x5eed0132, 0, 0));
    imgkeep_release();
    CHECK(!cskeep_dispatch(channel, 4, 6, 7, 0, 1, 3));
    imgkeep_release();
    CHECK(!cskeep_expect("fetched through another format", copy, 64, 64, 8,
                         (const u32[][3]){{4, 5, 0x5eed0131}, {6, 7, 0x5eed0132}}, 2));
    CHECK(!cskeep_expect("after the fetches through another format", third, 64, 64, 8,
                         (const u32[][3]){{4, 5, 0x5eed0131}, {6, 7, 0x5eed0132}}, 2));

    /* a render target of another format over a kept image's memory, read after each store: it has the store */
    const u64 target = CSKEEP_GVA + 0x58000;
    rt_clear_format(channel, target, 64, 64, 3, 0, 0xD5);
    CHECK(!cskeep_header(pool, 6, imgkeep_word0(), target, 64, 64, 8));
    CHECK(!cskeep_dispatch(channel, 6, 2, 3, 0x5eed0151, 0, 0));
    imgkeep_release();
    CHECK(!imgkeep_target_texel(target, 2, 3, 0x5eed0151, "after a dispatch's store"));
    CHECK(!cskeep_dispatch(channel, 6, 7, 9, 0x5eed0152, 0, 0));
    imgkeep_release();
    CHECK(!imgkeep_target_texel(target, 7, 9, 0x5eed0152, "again after the next dispatch's store"));

    /* an image of less than a page: published at its dispatch, as before */
    pub = g_vk_rt_watch.by[VK_PUB_OTHER];
    CHECK(!cskeep_dispatch(channel, 5, 1, 2, 0x5eed0141, 0, 0));
    if (kept && cskeep_published(pub) != 1)
        fprintf(stderr,
                "compute storage image of less than a page: %llu images published at its dispatch, 1 expected\n",
                (unsigned long long)cskeep_published(pub));
    CHECK(!kept || cskeep_published(pub) == 1);
    imgkeep_release();
    CHECK(!cskeep_expect("of less than a page", small, 8, 8, 1, (const u32[][3]){{1, 2, 0x5eed0141}}, 1));
    vk_lock();
    vk_finish();
    CHECK(g_vk_validation_messages == validation);
    vk_unlock();
    free(channel);
    printf("compute storage images: %s; a read of one publishes it, and has its stores\n",
           kept ? "kept on the GPU, nothing published at a dispatch or a release"
                : "published at each dispatch (no traps here)");
    return 0;
}

/* --bench-storage-image: a draw into a 1024x1024 R32_UINT image (4 MB) and a release, rounds times, each store landed
   by a CPU read at the end. Times the draws and releases with the GPU done between rounds, as at a frame's fence,
   and counts the bytes copied on the CPU before the draws and back from the GPU after them */
static int bench_storage_image(void) {
    const u64 address = 0x4d0000000ull, cpu = HEAP_BASE + 0x8000000, cbuf = address + 0x3000, pool = address + 0x6000;
    const u64 image = address + 0x10000;
    const u32 w = 1024, h = 1024, rounds = 64;
    CHECK(mem_map(cpu, 0x10000 + (u64)w * 4 * h, MS_NORMAL, PERM_RW) == 0); /* past the heap */
    gmmu_map(address, cpu, 0x10000 + (u64)w * 4 * h);
    GpuChannel* channel = imgkeep_channel(address, pool);
    CHECK(channel);
    CHECK(!imgkeep_image(pool, image, w, h));
    CHECK(!imgkeep_draw(channel, cbuf, 0, 0, 0x5eed0000)); /* the pipeline made, outside the time */
    imgkeep_release();
    vk_lock();
    vk_finish();
    vk_unlock();
    ImgCount was = imgkeep_since((ImgCount){0});
    u64 ns = 0;
    for (u32 i = 1; i <= rounds; i++) {
        u64 t = plat_ns();
        CHECK(!imgkeep_draw(channel, cbuf, i * 13 % w, i * 7 % h, 0x5eed0000 + i));
        imgkeep_release();
        vk_lock();
        vk_finish();
        vk_unlock();
        ns += plat_ns() - t;
    }
    u8 word[4];
    for (u32 i = 0; i <= rounds; i++) {
        CHECK(!gmem_read(image + imgkeep_at(i * 13 % w, i * 7 % h, w, h), word, 4));
        if (snx_ld_le32(word) != 0x5eed0000 + i)
            fprintf(stderr, "storage image bench: round %u's texel holds %08x, expected %08x\n", i, snx_ld_le32(word),
                    0x5eed0000 + i);
        CHECK(snx_ld_le32(word) == 0x5eed0000 + i);
    }
    free(channel);
    ImgCount d = imgkeep_since(was);
    printf("GPU_HOTPATH_BENCH workload=storage_image_draws operations=%u ns=%llu exact=1 bytes=%llu copied_bytes=%llu "
           "read_back_bytes=%llu published=%llu\n",
           rounds, (unsigned long long)ns, (unsigned long long)w * 4 * h, (unsigned long long)d.copied,
           (unsigned long long)d.back, (unsigned long long)d.published);
    return 0;
}
