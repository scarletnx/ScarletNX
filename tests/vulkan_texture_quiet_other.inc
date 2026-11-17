/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* SNX_TEST_NVN_CASE=texture_quiet_other: a GPU-side write on another watched granule leaves a quiet texture quiet; a
   write on its own granule sends it to a full check, and its texel is then the write's. The counts of quiet and full
   checks tell which */
static int check_texture_quiet_other(void) {
    if (mem_map(TQ_CPU, TQ_SPAN, MS_NORMAL, PERM_RW) == 0)
        gmmu_map(TQ_GVA, TQ_CPU, TQ_SPAN); /* else the texture_quiet case's */
    if (mem_map(TQ_CPU + TQ_SPAN, TQ_DECOY_SPAN, MS_NORMAL, PERM_RW) == 0)
        gmmu_map(TQ_DECOY, TQ_CPU + TQ_SPAN, TQ_DECOY_SPAN);
    CHECK(!tq_decoy());
    NvnTexture a = {0};
    a.d.target = NVN_TEXTURE_TARGET_2D;
    a.d.format = NVN_FORMAT_RGBA8;
    a.d.levels = 1;
    a.d.width = a.d.height = TQ_SIDE;
    a.d.depth = 1;
    a.gpu = TQ_TEX;
    u32 tic[8];
    CHECK(nvn_tic_encode(&a, NULL, 0, 0, tic));
    /* without draw_state_check: its full check of each quiet texture takes the quiet away, which the counts ask */
    int saved_check = g_settings.draw_state_check, failed = 0;
    g_settings.draw_state_check = 0;
    g_vk_present++;
    u64 c0, q0, w0, c1, q1, w1, f0[6], f1[6];
    vk_lock();
    Surf* t = vk_tex_from_tic(tic); /* full: a new present */
    CHECK(t && vk_tex_from_tic(tic) == t);
    vk_tex_check_counts(&c0, &q0, &w0);
    vk_tex_full_counts(f0);
    gpu_mem_written(TQ_DECOY + 0x8000, 0x1000); /* another texture's granule */
    CHECK(vk_tex_from_tic(tic) == t);
    vk_tex_check_counts(&c1, &q1, &w1);
    vk_tex_full_counts(f1);
    vk_unlock();
    const int quiet = q1 - q0 == 1, by_write = f1[3] - f0[3] == 1;
    printf("texture quiet other: after a write on another granule the texture's check was %s\n",
           quiet      ? "quiet"
           : by_write ? "full, by the write"
                      : "full");
    if (!quiet) {
        fprintf(stderr, "texture quiet other: a write on another granule sent the texture to a full check\n");
        failed = 1;
    }
    /* its own granule: a full check, and the texel the write's */
    u8 word[4] = {0x11, 0x22, 0x33, 0xff};
    CHECK(!gmem_write(TQ_TEX, word, 4));
    gpu_mem_written(TQ_TEX, 4);
    vk_lock();
    vk_tex_full_counts(f0);
    CHECK(vk_tex_from_tic(tic) == t);
    vk_tex_full_counts(f1);
    vk_unlock();
    const u32 got = ct_texel(t, 0, 0);
    if (f1[3] - f0[3] != 1 || got != 0xff332211u) {
        fprintf(stderr,
                "texture quiet other: a write on its own granule: %llu full checks by it, texel %08x (written "
                "ff332211)\n",
                (unsigned long long)(f1[3] - f0[3]), got);
        failed = 1;
    }
    g_settings.draw_state_check = saved_check;
    if (!failed) puts("PASS texture quiet other: a write on another granule leaves a texture quiet, its own does not");
    return failed;
}
/* SNX_TEST_NVN_CASE=texture_quiet_alias: a view of other guest memory, in another 4 MiB grain, leaves a quiet texture
   quiet over an epoch with no write on it (write tracking asks for the texture's own pages alone, vk_written_quick),
   and a CPU write on it, an epoch, still sends it to a full check with the write's texel. B, in the same leaf of write
   tracking, takes the leaf in for each epoch with its full check first */
#define TQA_SRC  (HEAP_BASE + 0x3b000000)
#define TQA_VIEW (HEAP_BASE + 0x3b400000)
static Surf* tqa_look(const u32* tic, u64* quiet) {
    u64 c0, q0, w0, c1, q1, w1;
    vk_lock();
    vk_tex_check_counts(&c0, &q0, &w0);
    Surf* t = vk_tex_from_tic(tic);
    vk_tex_check_counts(&c1, &q1, &w1);
    vk_unlock();
    *quiet = q1 - q0;
    return t;
}
static int check_texture_quiet_alias(void) {
    if (mem_map(TQ_CPU, TQ_SPAN, MS_NORMAL, PERM_RW) == 0) gmmu_map(TQ_GVA, TQ_CPU, TQ_SPAN); /* else another case's */
    CHECK(mem_map(TQA_SRC, 0x10000, MS_NORMAL, PERM_RW) == 0);
    if (!mem_pool_active() || mem_alias_begin(TQA_SRC, 0x10000)) {
        puts("SKIP texture quiet alias: a view needs the alias pool");
        CHECK(mem_unmap(TQA_SRC, 0x10000) == 0);
        return 0;
    }
    CHECK(mem_alias_map(TQA_VIEW, TQA_SRC, 0x10000, MS_NORMAL, PERM_RW) == 0);
    CHECK(mem_alias_any());
    NvnTexture d = {0};
    d.d.target = NVN_TEXTURE_TARGET_2D;
    d.d.format = NVN_FORMAT_RGBA8;
    d.d.levels = 1;
    d.d.width = d.d.height = TQ_SIDE;
    d.d.depth = 1;
    u32 tic_a[8], tic_b[8];
    d.gpu = TQ_TEX + 0x1a0000;
    CHECK(nvn_tic_encode(&d, NULL, 0, 0, tic_a));
    d.gpu = TQ_TEX + 0x1b0000;
    CHECK(nvn_tic_encode(&d, NULL, 0, 0, tic_b));
    int saved_check = g_settings.draw_state_check, failed = 0;
    g_settings.draw_state_check = 0;
    u64 quiet;
    g_renderer->mem_may_have_changed();
    g_vk_present++;
    Surf* a = tqa_look(tic_a, &quiet);
    CHECK(a && tqa_look(tic_a, &quiet) == a);
    Surf* b = tqa_look(tic_b, &quiet);
    CHECK(b && tqa_look(tic_b, &quiet) == b);
    /* an epoch with no write, in the same present */
    g_renderer->mem_may_have_changed();
    CHECK(tqa_look(tic_b, &quiet) == b);
    CHECK(tqa_look(tic_a, &quiet) == a);
    printf("texture quiet alias: with a view elsewhere, over an epoch with no write the texture was %s\n",
           quiet ? "quiet" : "checked in full");
    if (quiet != 1) failed = 1;
    /* a CPU write on A, then an epoch */
    const u64 cpu = TQ_CPU + (TQ_TEX - TQ_GVA) + 0x1a0000;
    guest_done(cpu, 4);
    snx_st_le32(GP(cpu), 0xff556677u);
    g_renderer->mem_may_have_changed();
    CHECK(tqa_look(tic_b, &quiet) == b);
    CHECK(tqa_look(tic_a, &quiet) == a);
    const u32 got = ct_texel(a, 0, 0);
    if (quiet || got != 0xff556677u) {
        fprintf(stderr,
                "texture quiet alias: after a CPU write and an epoch the check was %s and the texel %08x (written "
                "ff556677)\n",
                quiet ? "quiet" : "full", got);
        failed = 1;
    }
    g_settings.draw_state_check = saved_check;
    CHECK(mem_alias_unmap(TQA_VIEW, 0x10000, TQA_SRC) == 0);
    mem_alias_end(TQA_SRC, 0x10000);
    CHECK(mem_unmap(TQA_SRC, 0x10000) == 0);
    if (!failed) puts("PASS texture quiet alias: a view elsewhere leaves a texture quiet over an epoch with no write");
    return failed;
}
/* A texture across a present (SNX_TEST_NVN_CASE=texture_quiet_present): two textures in one leaf of write tracking.
   After a present and an epoch, B's full check takes the leaf in, and A stays quiet over the present (no write on
   it). After a CPU write on A, a present and an epoch, A takes a full check and its texel is the CPU's */
static int check_texture_quiet_present(void) {
    if (mem_map(TQ_CPU, TQ_SPAN, MS_NORMAL, PERM_RW) == 0) gmmu_map(TQ_GVA, TQ_CPU, TQ_SPAN); /* else another case's */
    NvnTexture d = {0};
    d.d.target = NVN_TEXTURE_TARGET_2D;
    d.d.format = NVN_FORMAT_RGBA8;
    d.d.levels = 1;
    d.d.width = d.d.height = TQ_SIDE;
    d.d.depth = 1;
    u32 tic_a[8], tic_b[8];
    d.gpu = TQ_TEX + 0x180000;
    CHECK(nvn_tic_encode(&d, NULL, 0, 0, tic_a));
    d.gpu = TQ_TEX + 0x190000;
    CHECK(nvn_tic_encode(&d, NULL, 0, 0, tic_b));
    int saved_check = g_settings.draw_state_check, failed = 0;
    g_settings.draw_state_check = 0;
    u64 quiet;
    g_renderer->mem_may_have_changed();
    g_vk_present++;
    Surf* a = tqa_look(tic_a, &quiet);
    CHECK(a && tqa_look(tic_a, &quiet) == a);
    Surf* b = tqa_look(tic_b, &quiet);
    CHECK(b && tqa_look(tic_b, &quiet) == b);
    /* a present and an epoch with no write: B's check takes the leaf in, then A */
    g_renderer->mem_may_have_changed();
    g_vk_present++;
    CHECK(tqa_look(tic_b, &quiet) == b);
    CHECK(tqa_look(tic_a, &quiet) == a);
    printf("texture quiet present: over a present with no write the texture was %s\n",
           quiet ? "quiet" : "checked in full");
    if (quiet != 1) failed = 1;
    /* a CPU write on A, a present and an epoch */
    const u64 cpu = TQ_CPU + (TQ_TEX - TQ_GVA) + 0x180000;
    guest_done(cpu, 4);
    snx_st_le32(GP(cpu), 0xff445567u);
    g_renderer->mem_may_have_changed();
    g_vk_present++;
    CHECK(tqa_look(tic_b, &quiet) == b);
    CHECK(tqa_look(tic_a, &quiet) == a);
    const u32 got = ct_texel(a, 0, 0);
    if (quiet || got != 0xff445567u) {
        fprintf(stderr,
                "texture quiet present: after a CPU write and a present the check was %s and the texel %08x (written "
                "ff445567)\n",
                quiet ? "quiet" : "full", got);
        failed = 1;
    }
    g_settings.draw_state_check = saved_check;
    if (!failed) puts("PASS texture quiet present: quiet over a present with no write, a full check after a CPU write");
    return failed;
}
