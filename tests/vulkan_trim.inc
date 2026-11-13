/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The budget's trim (surface.c vk_mem_trim) frees the working set only to stay under 95% of the budget, and of it only
   what has gone unused for eight presents: with two, it took textures a game uses every few frames, which came back at
   once, an ASTC one encoded again each time. Two 64x64 textures, one last used eight presents ago and one nine, go
   through a trim that wants more than everything held: the first stays, the second goes. It runs alone
   (SNX_TEST_TRIM_ONLY, the vulkan_trim entry), as such a trim would take what the other checks hold */
#define TRIM_GVA 0x4f2000000ull
#define TRIM_CPU (HEAP_BASE + 0x5600000)

/* a 64x64 pitch linear RGBA8 header (2) at gva, rows of 256 bytes */
static void trim_tic(u32* tic, u64 gva) {
    const u32 rgba8 = 0x08 | 2u << 7 | 2u << 10 | 2u << 13 | 2u << 16;
    const u32 words[8] = {rgba8, (u32)gva, (u32)(gva >> 32) | 2u << 21, 256 >> 5, 63 | 1u << 23, 63, 0, 0};
    memcpy(tic, words, sizeof words);
}

static u32 trim_textures(void) {
    u32 count, parked;
    u64 bytes;
    vk_surf_stats(VK_LRU_TEX, &count, &bytes, &parked);
    return count;
}

static int check_trim_working_set(void) {
    u32 nine_tic[8], eight_tic[8];
    u8* pixels = malloc(0x4000);
    CHECK(pixels);
    CHECK(mem_map(TRIM_CPU, 0x10000, MS_NORMAL, PERM_RW) == 0); /* past the 8 MiB the run maps at its start */
    gmmu_map(TRIM_GVA, TRIM_CPU, 0x10000);
    for (u32 i = 0; i < 0x4000; i++) pixels[i] = (u8)(i * 13 + 5);
    CHECK(!gmem_write(TRIM_GVA, pixels, 0x4000));
    CHECK(!gmem_write(TRIM_GVA + 0x8000, pixels, 0x4000));
    free(pixels);
    trim_tic(nine_tic, TRIM_GVA);
    trim_tic(eight_tic, TRIM_GVA + 0x8000);
    vk_lock();
    const u32 held = trim_textures();
    g_vk_present = 100;
    Surf* nine = vk_tex_from_tic(nine_tic);
    g_vk_present = 101;
    Surf* eight = vk_tex_from_tic(eight_tic);
    CHECK(nine && eight && nine != eight && trim_textures() == held + 2);
    g_vk_present = 109;
    u64 usage, budget;
    vk_mem_use(&usage, &budget);
    vk_mem_trim(budget, 0); /* past 95% of the budget whatever is freed: each step frees all it may */
    const u32 left = trim_textures();
    /* the one still held is found again, not made: the count stays */
    Surf* again = vk_tex_from_tic(eight_tic);
    const u32 after = trim_textures();
    vk_unlock();
    if (left != held + 1 || !again || after != left) {
        fprintf(stderr,
                "trim: %u textures held after a trim at present 109, %u before the two were made (one more expected: "
                "the one last used at 101), and %u after finding that one again (as many expected)\n",
                left, held, after);
        return 1;
    }
    printf("trim: a texture unused for eight presents stays, one unused for nine goes\n");
    return 0;
}

/* A texture whose texture object the game ended goes first (surface.c ended_reclaim): two textures made in one
   present, with an object each at its address (gpu/guest_textures.h), unused for six presents, fewer than the eight
   that the working set is kept for. One's object ends. A trim that wants everything takes that one and leaves the
   other, and takes neither when an object is made at the address again before it */
static int check_trim_ended(void) {
    const u64 ended_gva = TRIM_GVA + 0x4000, live_gva = TRIM_GVA + 0xC000;
    u32 ended_tic[8], live_tic[8];
    u8* pixels = malloc(0x4000);
    CHECK(pixels);
    for (u32 i = 0; i < 0x4000; i++) pixels[i] = (u8)(i * 7 + 3);
    CHECK(!gmem_write(ended_gva, pixels, 0x4000));
    CHECK(!gmem_write(live_gva, pixels, 0x4000));
    free(pixels);
    trim_tic(ended_tic, ended_gva);
    trim_tic(live_tic, live_gva);
    u64 usage, budget;
    int failed = 0;
    for (u32 again = 0; again < 2 && !failed; again++) {
        gpu_texture_add(ended_gva, 0x4000);
        gpu_texture_add(live_gva, 0x4000);
        vk_lock();
        const u32 held = trim_textures();
        g_vk_present = 200 + again * 100;
        Surf* ended = vk_tex_from_tic(ended_tic);
        Surf* live = vk_tex_from_tic(live_tic);
        CHECK(ended && live && ended != live && trim_textures() == held + 2);
        g_vk_present += 3;
        gpu_texture_remove(ended_gva);
        vk_surf_ended_frame(); /* the cache learns of the end; with memory to spare it frees nothing */
        CHECK(trim_textures() == held + 2);
        if (again) gpu_texture_add(ended_gva, 0x4000); /* the game makes an object there again */
        g_vk_present += 3;
        vk_mem_use(&usage, &budget);
        vk_mem_trim(budget, 0);
        /* the one kept is found, not made: the count stays. The one that went is made again: one more */
        const u32 left = trim_textures();
        CHECK(vk_tex_from_tic(live_tic));
        const u32 with_live = trim_textures();
        CHECK(vk_tex_from_tic(ended_tic));
        const u32 with_ended = trim_textures();
        if (with_live != left || with_ended != with_live + !again) {
            fprintf(stderr,
                    "trim: six presents after two textures were made and three after one's object ended%s, a trim "
                    "left %u textures; %u with the live one found again (as many expected) and %u with the other "
                    "(%u expected)\n",
                    again ? " and was made again" : "", left, with_live, with_ended, with_live + !again);
            failed = 1;
        }
        /* for the next round and the checks after: both gone, and no object at either address */
        g_vk_present += 20;
        vk_mem_trim(budget, 0);
        vk_unlock();
        if (again) gpu_texture_remove(ended_gva);
        gpu_texture_remove(live_gva);
    }
    if (failed) return 1;
    printf("trim: a texture whose object the game ended goes first, and stays when an object is made there again\n");
    return 0;
}
