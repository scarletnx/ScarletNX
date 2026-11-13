/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The system's word that memory runs short (a low memory warning, Android's onTrimMemory): at the renderer's next
   present the caches free the textures the two presents before didn't use, which memory gives again, and keep the ones
   they did; before the word nothing goes. No extracted data or game files are used */
#define MR_CPU  (HEAP_BASE + 0x10c00000)
#define MR_GVA  0x8a8000000ull
#define MR_TEX  8
#define MR_SPAN 0x40000ull /* a 256x256 RGBA8 texture's bytes */
static int mr_texture(u32 i, u32 tic[8]) {
    NvnTexture t = {.gpu = MR_GVA + i * MR_SPAN};
    t.d.target = NVN_TEXTURE_TARGET_2D;
    t.d.format = NVN_FORMAT_RGBA8;
    t.d.width = t.d.height = 256;
    t.d.depth = t.d.levels = 1;
    CHECK(nvn_tic_encode(&t, NULL, 0, 0, tic));
    for (u32 k = 0; k < MR_SPAN; k += 4) snx_st_le32(GP(MR_CPU + i * MR_SPAN + k), 0xff000000u | (i << 16) | k);
    return 0;
}
static int check_memory_release(void) {
    u32 tic[MR_TEX + 1][8], count0, count, parked;
    u64 bytes0, bytes, by[3];
    CHECK(!mem_map(MR_CPU, (MR_TEX + 1) * MR_SPAN, MS_NORMAL, PERM_RW));
    gmmu_map(MR_GVA, MR_CPU, (MR_TEX + 1) * MR_SPAN);
    for (u32 i = 0; i <= MR_TEX; i++) CHECK(!mr_texture(i, tic[i]));
    vk_surf_stats(VK_LRU_TEX, &count0, &bytes0, &parked);
    g_vk_present++;
    vk_lock();
    for (u32 i = 0; i < MR_TEX; i++) CHECK(vk_tex_from_tic(tic[i]));
    vk_unlock();
    vk_surf_stats(VK_LRU_TEX, &count, &bytes, &parked);
    CHECK(count >= count0 + MR_TEX && bytes >= bytes0 + MR_TEX * MR_SPAN);
    u32 runs = vk_mem_release_stats(by);
    /* no word, no release */
    g_vk_present += 3;
    vk_lock();
    vk_mem_release_run();
    vk_unlock();
    CHECK(vk_mem_release_stats(by) == runs);
    /* one texture used this present, the eight two presents back: the word frees those, keeps it */
    vk_lock();
    Surf* kept = vk_tex_from_tic(tic[MR_TEX]);
    vk_unlock();
    CHECK(kept);
    vk_mem_release_ask("a test's word");
    vk_lock();
    vk_mem_release_run();
    vk_unlock();
    CHECK(vk_mem_release_stats(by) == runs + 1);
    u32 after;
    u64 after_bytes;
    vk_surf_stats(VK_LRU_TEX, &after, &after_bytes, &parked);
    if (by[0] < MR_TEX * MR_SPAN || after_bytes > bytes + MR_SPAN - MR_TEX * MR_SPAN)
        fprintf(stderr, "memory release: %llu bytes of textures freed, %llu held after, not %llu and at most %llu\n",
                (unsigned long long)by[0], (unsigned long long)after_bytes, (unsigned long long)(MR_TEX * MR_SPAN),
                (unsigned long long)(bytes + MR_SPAN - MR_TEX * MR_SPAN));
    CHECK(by[0] >= MR_TEX * MR_SPAN && after_bytes <= bytes + MR_SPAN - MR_TEX * MR_SPAN);
    vk_lock();
    CHECK(vk_tex_from_tic(tic[MR_TEX]) == kept); /* the one the present used is still there */
    vk_unlock();
    /* four presents later the line says what went and the blocks then */
    g_vk_present += 4;
    vk_lock();
    vk_mem_release_run();
    vk_unlock();
    gmmu_space_unmap(MR_GVA, (MR_TEX + 1) * MR_SPAN);
    mem_unmap(MR_CPU, (MR_TEX + 1) * MR_SPAN);
    printf("memory release: the system's word freed %llu KiB of textures two presents unused and kept the one in use\n",
           (unsigned long long)(by[0] >> 10));
    return 0;
}
