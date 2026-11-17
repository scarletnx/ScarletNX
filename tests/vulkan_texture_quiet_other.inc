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
    u64 c0, q0, w0, c1, q1, w1, f0[5], f1[5];
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
