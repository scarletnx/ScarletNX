/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A save state's depth targets (surface.c's vk_zt_save, vk_zt_load, zt_restore): a 3x3 target of two layers, each
   texel's depth and stencil its own, is saved; its record, loaded at another address, gives the target made there
   its pixels back. A layer's stencil is 9 bytes: each layer of each aspect is copied at an offset that is a multiple
   of 4, as a depth or stencil copy must be (the validation layer says so where it runs), and the state holds the
   layers' bytes alone, one after another */
#include "nx/runtime/state_io.h"
typedef struct {
    u8* p;
    u64 n, cap, at;
} DsBytes;
static int ds_put(void* ctx, const void* p, u64 n) {
    DsBytes* b = ctx;
    if (b->n + n > b->cap) {
        u64 cap = b->cap ? b->cap : 4096;
        while (cap < b->n + n) cap *= 2;
        u8* grown = realloc(b->p, cap);
        if (!grown) return -1;
        b->p = grown;
        b->cap = cap;
    }
    memcpy(b->p + b->n, p, n);
    b->n += n;
    return 0;
}
static int ds_get(void* ctx, void* p, u64 n) {
    DsBytes* b = ctx;
    if (b->at + n > b->n) return -1;
    memcpy(p, b->p + b->at, n);
    b->at += n;
    return 0;
}
/* texel k of a layer: its depth's word as the copy gives it (D24's top byte not compared), and its stencil */
static u32 ds_depth(VkFormat f, u32 layer, u32 k) {
    const u32 v = layer * 16 + k + 1;
    if (f == VK_FORMAT_D24_UNORM_S8_UINT) return v * 0x10101u;
    float x = (float)v / 64.0f;
    u32 w;
    memcpy(&w, &x, 4);
    return w;
}
static u8 ds_stencil(u32 layer, u32 k) { return (u8)(0x40 + layer * 16 + k); }
/* the target's layers, its depth's at 0 and 64 and its stencil's at 128 and 192 of buf at at, between it and the
   buffer (out) */
static void ds_copy(Surf* s, VkBuffer buf, u64 at, int out) {
    VkBufferImageCopy c[4];
    for (u32 a = 0; a < 2; a++)
        for (u32 layer = 0; layer < 2; layer++)
            c[a * 2 + layer] = (VkBufferImageCopy){
                .bufferOffset = at + a * 128 + layer * 64,
                .imageSubresource = {a ? VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_DEPTH_BIT, 0, layer, 1},
                .imageExtent = {3, 3, 1}};
    VkCommandBuffer cb = vk_cmd();
    vk_render_end();
    if (out) {
        vk_surf_use(cb, s, VK_LAYOUT(TRANSFER_SRC_OPTIMAL), VK_PIPELINE_STAGE_2_COPY_BIT,
                    VK_ACCESS_2_TRANSFER_READ_BIT);
        rec_CmdCopyImageToBuffer(cb, s->img, VK_LAYOUT(TRANSFER_SRC_OPTIMAL), buf, 4, c);
    } else {
        vk_surf_use(cb, s, VK_LAYOUT(TRANSFER_DST_OPTIMAL), VK_PIPELINE_STAGE_2_COPY_BIT,
                    VK_ACCESS_2_TRANSFER_WRITE_BIT);
        rec_CmdCopyBufferToImage(cb, buf, s->img, VK_LAYOUT(TRANSFER_DST_OPTIMAL), 4, c);
    }
}
/* whether the 3x3x2 texels differ: the depth words at d and the stencil bytes at st, each layer step bytes after the
   one before (the first that differs said) */
static int ds_differs(const char* what, VkFormat f, const u8* d, const u8* st, u32 dstep, u32 sstep) {
    const u32 mask = f == VK_FORMAT_D24_UNORM_S8_UINT ? 0xffffffu : ~0u;
    for (u32 layer = 0; layer < 2; layer++)
        for (u32 k = 0; k < 9; k++) {
            u32 w;
            memcpy(&w, d + layer * dstep + k * 4, 4);
            const u8 sv = st[layer * sstep + k];
            if ((w & mask) != (ds_depth(f, layer, k) & mask) || sv != ds_stencil(layer, k)) {
                fprintf(stderr, "depth save state: %s: layer %u texel %u is depth %#x stencil %#x, not %#x %#x\n", what,
                        layer, k, w & mask, sv, ds_depth(f, layer, k) & mask, ds_stencil(layer, k));
                return 1;
            }
        }
    return 0;
}
static int check_depth_save_state(void) {
    const u64 gva = 0x9d0000000ull, again = gva + 0x8000, cpu = HEAP_BASE + 0x3c400000;
    const u32 zeta = 0x14; /* Z24_UNORM_S8, D24S8 or D32S8 on the host */
    const VkFormat f = vk_zt_format(zeta).vk;
    if (f != VK_FORMAT_D24_UNORM_S8_UINT && f != VK_FORMAT_D32_SFLOAT_S8_UINT) {
        printf("depth save state: Z24S8 is format %d on this device, with no stencil of a byte: not checked\n", (int)f);
        return 0;
    }
    CHECK(mem_map(cpu, 0x10000, MS_NORMAL, PERM_RW) == 0);
    gmmu_map(gva, cpu, 0x10000);
    const u32 validation = g_vk_validation_messages;
    DsBytes saved = {0}, record = {0};
    StateIo out = {&saved, ds_put, ds_get}, in = {&record, ds_put, ds_get};
    int failed = 0;
    vk_lock();
    Surf* s = vk_zt_array(gva, 3, 3, zeta, 0x1000, 2, 0x1000, DS_MSAA_1X1);
    CHECK(s && s->layers == 2 && s->sw == 3 && s->sh == 3);
    {
        VkBuffer buf;
        u64 at;
        u8* p = vk_stage(256, 16, &buf, &at);
        CHECK(p);
        for (u32 layer = 0; layer < 2; layer++)
            for (u32 k = 0; k < 9; k++) {
                const u32 w = ds_depth(f, layer, k);
                memcpy(p + layer * 64 + k * 4, &w, 4);
                p[128 + layer * 64 + k] = ds_stencil(layer, k);
            }
        ds_copy(s, buf, at, 0);
    }
    CHECK(!vk_zt_save(&out));
    /* the target's record: n, then each one's address, sizes, keys, the depth's and stencil's byte counts and bytes */
    u32 n = 0;
    CHECK(!ds_get(&saved, &n, 4));
    for (u32 i = 0; i < n && !record.n; i++) {
        u64 at = saved.at, rgva, dsize, ssize;
        u32 w[7];
        CHECK(!ds_get(&saved, &rgva, 8) && !ds_get(&saved, w, sizeof w) && !ds_get(&saved, &dsize, 8) &&
              !ds_get(&saved, &ssize, 8));
        if (rgva == gva) {
            CHECK(w[6] == 2 && dsize == 2 * 9 * 4 && ssize == 2 * 9 && saved.at + dsize + ssize <= saved.n);
            failed |= ds_differs("saved", f, saved.p + saved.at, saved.p + saved.at + dsize, 36, 9);
            const u32 one = 1;
            CHECK(!ds_put(&record, &one, 4) && !ds_put(&record, &again, 8));
            CHECK(!ds_put(&record, saved.p + at + 8, saved.at + dsize + ssize - (at + 8)));
        }
        saved.at += dsize + ssize;
    }
    if (!record.n)
        fprintf(stderr, "depth save state: the target at %#llx is not among the %u saved\n", (unsigned long long)gva,
                n);
    CHECK(record.n);
    /* loaded, then the target made at the record's address takes its pixels */
    CHECK(!vk_zt_load(&in));
    Surf* back = vk_zt_array(again, 3, 3, zeta, 0x1000, 2, 0x1000, DS_MSAA_1X1);
    CHECK(back && back != s);
    {
        VkBuffer buf;
        u8* p = vk_readback_buffer(256, &buf);
        CHECK(p);
        ds_copy(back, buf, 0, 1);
        vk_finish();
        vk_readback_ready();
        failed |= ds_differs("restored", f, p, p + 128, 64, 64);
    }
    vk_zt_forget();
    vk_unlock();
    CHECK(mem_unmap(cpu, 0x10000) == 0);
    free(saved.p);
    free(record.p);
    if (g_vk_validation_messages != validation)
        fprintf(stderr, "depth save state: %u validation messages\n", g_vk_validation_messages - validation);
    CHECK(!failed && g_vk_validation_messages == validation);
    printf("depth save state: a 3x3 target of 2 layers of %s saved and restored, each layer of each aspect copied at a "
           "multiple of 4\n",
           f == VK_FORMAT_D24_UNORM_S8_UINT ? "D24S8" : "D32S8");
    return 0;
}
