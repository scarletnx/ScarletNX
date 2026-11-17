/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The quiet texture check (gpu/vk/texture.c tex_check): a draw's texture with no present, write tracking epoch with a
   CPU write on its pages or GPU-side write on its watched granules since its last full check is taken with one
   compare. Each case draws a DrawTexture that samples texel (0, 0) of a 64x64 RGBA8 texture twice (the
   second draw's check must be quiet), writes the texture one way, and draws again: the pixel and every texel of the
   image must be memory's. The ways: the guest CPU (then a new epoch, as a submission starts), a DMA copy, a compute
   dispatch's store, and a draw into the texture's memory as a render target. The writes other than the CPU's come in
   the same epoch and present, where only the GPU-side record can tell. Each sequence runs as it is, and with
   draw_state_check=1, whose full check of each quiet texture must find nothing the quick one missed. Synthetic data
   and programs only */
#define TQ_GVA    0xd40000000ull
#define TQ_CPU    (HEAP_BASE + 0x2f000000)
#define TQ_SPAN   0x400000ull
#define TQ_TIC    (TQ_GVA)            /* the header pool */
#define TQ_TSC    (TQ_GVA + 0x1000)   /* the sampler pool */
#define TQ_CODE   (TQ_GVA + 0x2000)   /* the compute program */
#define TQ_CONST  (TQ_GVA + 0x3000)   /* its constant buffer 0: the store's address at 0x110 */
#define TQ_SRC    (TQ_GVA + 0x4000)   /* the DMA copy's source */
#define TQ_TARGET (TQ_GVA + 0x100000) /* the draws' 4x4 target */
#define TQ_TEX    (TQ_GVA + 0x200000) /* the texture, in a 2 MiB granule of its own */
#define TQ_SIDE   64u

/* the texture's description, as tex_partial's cases (vulkan_tex_partial.inc) take it, to compare its image */
static const TpCase g_tq_case = {"quiet 64x64 RGBA8",      NVN_FORMAT_RGBA8, 4,       1,
                                 VK_FORMAT_R8G8B8A8_UNORM, TQ_SIDE,          TQ_SIDE, 0};

/* every texel of the texture's memory the color c (RGBA, its bytes in that order) */
static void tq_fill(const TpTex* x, u32 c) {
    u8* p = GP(x->cpu);
    guest_done(x->cpu, x->lay.size);
    for (u64 i = 0; i < x->lay.size; i += 4) snx_st_le32(p + i, c);
}
/* n draws of the texture's texel (0, 0) into the target, cleared first, then its pixel (0, 0) into px; the checks and
   quiet checks they made into *checks and *quiet */
static int tq_draws(GpuChannel* ch, u32 n, u8 px[4], u64* checks, u64* quiet) {
    u64 c0, q0, w0, c1, q1, w1;
    vk_lock();
    ch->r3d[0x574] = 0;
    rt_clear(ch, TQ_TARGET, 4, 4, 0, 0);
    ch->r3d_dirty = ~0u;
    mx_draw_state_sync(ch);
    DsState* state = &ch->ds;
    state->bind.tex_pool = TQ_TIC;
    state->bind.smp_pool = TQ_TSC;
    state->bind.tex_max = state->bind.smp_max = 0;
    state->blend = (DsBlend){0};
    state->blend.mask[0] = 15;
    state->rkey.alpha_to_coverage = state->rkey.alpha_to_one = 0;
    state->rkey.sample_mask = ~0u;
    state->dyn.alpha_test = 0;
    vk_tex_check_counts(&c0, &q0, &w0);
    u64 drawn = g_vk_drawn;
    for (u32 i = 0; i < n; i++) {
        DrawTexture rectangle = {.w = 4, .h = 4, .sx0 = 0.5f, .sy0 = 0.5f};
        rectangle.state.multisample_flags = DS_MS_A2C_DITHER;
        vk_render_draw_texture(state, &rectangle);
    }
    vk_tex_check_counts(&c1, &q1, &w1);
    vk_render_end();
    Surf* target = vk_rt_find(TQ_TARGET);
    int ok = g_vk_drawn == drawn + n && target && !read_pixel_at(target, 0, 0, px);
    vk_unlock();
    *checks = c1 - c0;
    *quiet = q1 - q0;
    return ok ? 0 : 1;
}
/* the compute dispatch that stores the word c at gva (STG through c[0][0x110] of the word at c[0][0x118]): one
   program for every word, made once (tq_compute_ready) */
static int tq_compute(GpuChannel* ch, u64 gva, u32 c) {
    const u64 ops[] = {0x4c98078004470000ull,  /* MOV R0, c[0][0x110] */
                       0x4c98078004570001ull,  /* MOV R1, c[0][0x114] */
                       0x4c98078004670002ull,  /* MOV R2, c[0][0x118] */
                       0xeedc200000070002ull,  /* STG.E [R0], R2 */
                       0xe30000000007000full}; /* EXIT */
    u8 code[256] = {0};
    u32 at = 0;
    for (u32 i = 0; i < sizeof ops / sizeof *ops; i++) {
        if (!(i % 3)) snx_st_le64(code + 8 * at++, 0x001f8000fc0007e0ull);
        snx_st_le64(code + 8 * at++, ops[i]);
    }
    CHECK(!gmem_write(TQ_CODE, code, sizeof code));
    CHECK(!storage_write_words(TQ_CONST + 0x110, (const u32[3]){(u32)gva, (u32)(gva >> 32), c}, 3));
    u32 qmd[64] = {0};
    qmd[QMD_GRID_DIM_X] = 1;
    qmd[QMD_GRID_DIM_YZ] = 1 | 1u << 16;
    qmd[QMD_BLOCK_DIM_X] = 1u << 16;
    qmd[QMD_BLOCK_DIM_YZ] = 1 | 1u << 16;
    qmd[QMD_CONST_BUFFER_ENABLE] = 1;
    qmd[QMD_CONST_BUFFER_CONFIG] = (u32)TQ_CONST;
    qmd[QMD_CONST_BUFFER_CONFIG + 1] = (u32)(TQ_CONST >> 32) | 0x200u << 15;
    u64 before = g_vk_dispatches;
    vk_lock();
    DispatchState d;
    mx_dispatch_state(qmd, ch->rcomp, TQ_CODE, &d);
    vk_dispatch(&d);
    /* its result landed, in the same epoch and present: the published bytes' GPU-side stamp is what the texture's
       checks have to see */
    const u32 epoch = vk_ww_epoch();
    vk_compute_land();
    vk_finish(); /* the submission's harvest of its write log */
    if (vk_ww_epoch() != epoch) fprintf(stderr, "texture quiet: the dispatch's landing started a new epoch\n");
    vk_unlock();
    return g_vk_dispatches == before + 1;
}
/* the program made and run once into the DMA source's page, until its word is there: a dispatch whose pipeline a
   worker still makes doesn't run yet */
static int tq_compute_ready(GpuChannel* ch) {
    for (u32 c = 1; c <= 8; c++) {
        u8 w[4] = {0};
        CHECK(tq_compute(ch, TQ_SRC + 0x800, 0x5eed0000u + c));
        CHECK(!gmem_read(TQ_SRC + 0x800, w, sizeof w));
        if (snx_ld_le32(w) == 0x5eed0000u + c) return 1;
        vk_pipeline_quiesce();
    }
    return 0;
}
enum { TQ_CPU_WRITE, TQ_DMA, TQ_COMPUTE, TQ_DRAW, TQ_WAYS };
static const char* const g_tq_ways[TQ_WAYS] = {"the guest CPU", "a DMA copy", "a compute store", "a draw over it"};
/* one way's sequence with the settings given: 1 when the pixel or the image isn't memory's, or the quiet check never
   ran where it may (the second of two draws with nothing between); *wrong takes draw_state_check's count */
static int tq_way(GpuChannel* ch, TpTex* x, int way, int check, u64* wrong) {
    static const u32 colors[TQ_WAYS] = {0xff3366ccu, 0xff22aa44u, 0xff1188ddu, 0};
    const u32 before = 0xff102030u, after = colors[way];
    int saved_check = g_settings.draw_state_check;
    u64 c0, q0, w0, c1, q1, w1, checks, quiet_n;
    u8 px[4];
    g_settings.draw_state_check = check;
    vk_tex_check_counts(&c0, &q0, &w0);
    tq_fill(x, before);
    g_renderer->mem_may_have_changed(); /* the CPU's fill, as a submission takes it */
    g_vk_present++;
    int failed = tq_draws(ch, 1, px, &checks, &quiet_n);
    failed |= snx_ld_le32(px) != before;
    /* two draws with nothing between: the second one's check is quiet */
    failed |= tq_draws(ch, 2, px, &checks, &quiet_n);
    int quiet_ok = quiet_n >= 1;
    switch (way) {
        case TQ_CPU_WRITE:
            snx_st_le32(GP(x->cpu), after); /* texel (0, 0) */
            vk_ww_new_epoch();              /* a submission: what the CPU wrote before it is the stream's */
            break;
        case TQ_DMA: {
            u32 src[16];
            for (u32 i = 0; i < 16; i++) src[i] = after;
            CHECK(!gmem_write(TQ_SRC, src, sizeof src));
            GpuDmaOp o = {.width = 64, .line_in = 64, .line_out = 64, .lines = 1};
            o.src = pitch_side(TQ_SRC, 0);
            o.dst = pitch_side(x->gva, 0);
            CHECK(g_renderer->dma_copy(&o));
            break;
        }
        case TQ_COMPUTE: CHECK(tq_compute(ch, x->gva, after)); break;
        case TQ_DRAW: {
            /* a clear of the texture's memory as a 64x64 target in its block height: blue. The next lookups take
               the target itself (the surface table at the address moved), with no texture check */
            unsigned th = 0, td = 0;
            nvn_tex_level_tiles(&x->tex.d, 0, &th, &td);
            rt_clear(ch, x->gva, TQ_SIDE, TQ_SIDE, th, 1);
            break;
        }
    }
    const u32 want = way == TQ_DRAW ? 0xffff0000u : after; /* blue, in RGBA byte order */
    failed |= tq_draws(ch, 2, px, &checks, &quiet_n);
    u32 got = snx_ld_le32(px);
    /* the image against memory, through a lookup with nothing between (quiet, as the draws' were) */
    vk_lock();
    Surf* t = vk_tex_from_tic(x->tic);
    vk_unlock();
    u32 first[4] = {0}, image_has = 0, memory_has = 0;
    /* a draw's pixels stay on the GPU, not in memory: the pixel alone */
    int differ = way == TQ_DRAW ? 0 : t ? tp_differ(x, t, first, &image_has, &memory_has) : -1;
    vk_tex_check_counts(&c1, &q1, &w1);
    *wrong = w1 - w0;
    g_settings.draw_state_check = saved_check;
    if (failed || got != want || differ != 0 || !quiet_ok || *wrong) {
        fprintf(stderr,
                "texture quiet, %s%s: pixel %08x (expected %08x), %d texels differ from memory, "
                "%llu checks of which %llu quiet in the last draws, %llu changes the quick check missed%s\n",
                g_tq_ways[way], check ? " with draw_state_check=1" : "", got, want, differ, (unsigned long long)checks,
                (unsigned long long)quiet_n, (unsigned long long)*wrong,
                quiet_ok ? "" : "; the second of two draws wasn't quiet");
        if (differ > 0)
            fprintf(stderr, "  the first, (%u, %u): image %08x, memory %08x\n", first[2], first[3], image_has,
                    memory_has);
        return 1;
    }
    printf("texture quiet, %s%s: the pixel and the image memory's (%llu quiet checks; after the write %llu checks, "
           "%llu quiet)\n",
           g_tq_ways[way], check ? " with draw_state_check=1" : "", (unsigned long long)(q1 - q0),
           (unsigned long long)checks, (unsigned long long)quiet_n);
    return 0;
}
static int check_texture_quiet(void) {
    GpuChannel* ch = calloc(1, sizeof *ch);
    CHECK(ch);
    ch->id = 141;
    TpTex x = {0};
    x.c = &g_tq_case;
    x.cpu = TQ_CPU + 0x200000;
    x.layers = 1;
    x.tex.d.target = NVN_TEXTURE_TARGET_2D;
    x.tex.d.format = NVN_FORMAT_RGBA8;
    x.tex.d.levels = 1;
    x.tex.d.width = x.tex.d.height = TQ_SIDE;
    x.tex.d.depth = 1;
    nvn_tex_layout(&x.tex.d, &x.lay);
    CHECK(x.lay.size <= 0x10000);
    CHECK(mem_map(TQ_CPU, TQ_SPAN, MS_NORMAL, PERM_RW) == 0);
    gmmu_map(TQ_GVA, TQ_CPU, TQ_SPAN);
    CHECK(!storage_write_words(TQ_TSC, (const u32[8]){2u | 2u << 3 | 2u << 6}, 8)); /* nearest, clamp to edge */
    CHECK(tq_compute_ready(ch));
    int failed = 0;
    /* each run a texture of its own, 64 KiB apart in the texture's granule: a target a draw left over one is not the
       next run's */
    for (int way = 0; way < TQ_WAYS; way++)
        for (int run = 0; run < 2; run++) {
            const u64 at = (u64)(way * 2 + run) * 0x10000;
            u64 wrong = 0;
            x.gva = x.tex.gpu = TQ_TEX + at;
            x.cpu = TQ_CPU + (TQ_TEX - TQ_GVA) + at;
            CHECK(nvn_tic_encode(&x.tex, NULL, 0, 0, x.tic));
            CHECK(!storage_write_words(TQ_TIC, x.tic, 8));
            failed |= tq_way(ch, &x, way, run, &wrong);
        }
    free(ch);
    if (!failed)
        puts("PASS texture quiet: a texture written by the CPU, DMA, compute or a draw is drawn as memory has it");
    return failed;
}

#define TQ_DECOY      (TQ_GVA + TQ_SPAN) /* a texture in a granule of its own, past the others */
#define TQ_DECOY_SPAN 0x200000ull
/* the decoy texture looked up and checked, which watches its granule */
static int tq_decoy(void) {
    NvnTexture d = {0};
    d.d.target = NVN_TEXTURE_TARGET_2D;
    d.d.format = NVN_FORMAT_RGBA8;
    d.d.levels = 1;
    d.d.width = d.d.height = TQ_SIDE;
    d.d.depth = 1;
    d.gpu = TQ_DECOY;
    u32 tic[8];
    CHECK(nvn_tic_encode(&d, NULL, 0, 0, tic));
    vk_lock();
    Surf* t = vk_tex_from_tic(tic);
    CHECK(t && vk_tex_from_tic(tic) == t); /* the second lookup checks it, and watches its granule */
    vk_unlock();
    return 0;
}
