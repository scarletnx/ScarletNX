/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A geometry stage's global memory is the draw's as much as the vertex stage's: a writable binding there that shares
   a page with a vertex binding makes the draw alias, and the draw's storage is prepared for the geometry stage's set
   too. No extracted shaders or game data are used. */
static int check_draw_storage_stages(void) {
    const u64 gva = 0x4f0000000ull, cpu = HEAP_BASE + 0x5000000;
    gmmu_map(gva, cpu, 0x10000);
    const int was = g_settings.guest_buffers_copied;
    g_settings.guest_buffers_copied = 2; /* the atlas's layout: in place has no map (vulkan_uma_imports.inc) */
    VkDrawStorage draw = {0};
    draw.n[ST_GS] = 1; /* the geometry stage writes 256 bytes */
    draw.b[ST_GS][0] = (VkDrawGbuf){.addr = gva, .size = 256, .writable = 1};
    draw.n[ST_VS] = 1; /* the vertex stage reads 64 of them */
    draw.b[ST_VS][0] = (VkDrawGbuf){.addr = gva + 64, .size = 64};
    vk_lock();
    CHECK(vk_draw_storage_any(&draw));
    CHECK(vk_buf_draw_has_alias(&draw) == 1);
    CHECK(vk_buf_draw_prepare(&draw));
    /* the page table of the aliased binding is in its map */
    CHECK(draw.map_buf[ST_GS] && draw.map_size[ST_GS] > 48 * 4);
    CHECK(draw.b[ST_GS][0].aliased && draw.b[ST_VS][0].aliased);
    CHECK(draw.map_buf[ST_VS] && !draw.map_buf[ST_FS]); /* the fragment stage has no global memory */
    vk_buf_writable_abort();
    vk_unlock();
    g_settings.guest_buffers_copied = was;
    /* the same storage only read, by the geometry stage alone: nothing is written, so nothing aliases */
    VkDrawStorage read = {0};
    read.n[ST_GS] = 1;
    read.b[ST_GS][0] = (VkDrawGbuf){.addr = gva, .size = 256};
    vk_lock();
    CHECK(vk_draw_storage_any(&read) && vk_buf_draw_has_alias(&read) == 0);
    /* with nothing aliased, every stage of every draw of the frame binds one map, where the draw before bound it: a
       map of its own for each draw gave each a descriptor set of its own. An aliased draw's map stays its own */
    VkDrawStorage next = read;
    next.n[ST_VS] = 1;
    next.b[ST_VS][0] = (VkDrawGbuf){.addr = gva + 0x2000, .size = 64};
    CHECK(vk_buf_draw_prepare(&read) && vk_buf_draw_prepare(&next));
    CHECK(read.map_buf[ST_GS] && read.map_size[ST_GS] == 48 * 4 && !read.b[ST_GS][0].aliased);
    CHECK(next.map_buf[ST_GS] == read.map_buf[ST_GS] && next.map_off[ST_GS] == read.map_off[ST_GS]);
    CHECK(next.map_buf[ST_VS] == read.map_buf[ST_GS] && next.map_off[ST_VS] == read.map_off[ST_GS] &&
          next.map_size[ST_VS] == 48 * 4);
    CHECK(draw.map_buf[ST_GS] != read.map_buf[ST_GS] || draw.map_off[ST_GS] != read.map_off[ST_GS]);
    vk_unlock();
    printf("draw storage: a geometry stage's writable global memory aliases the vertex stage's, and is prepared; draws "
           "with nothing aliased share the frame's map\n");
    return 0;
}
