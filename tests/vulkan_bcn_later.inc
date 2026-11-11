/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* BC7 held as ETC2 RGBA8 and made ASTC 4x4 between frames (texture.c astc_later_*), on the device: a texture whose
   blocks all have one alpha loads as ETC2, is queued once sampled in VK_ASTC_LATER_READS presents, is encoded
   VK_ASTC_LATER_BLOCKS blocks a frame from guest memory, and takes the ETC2 image's place once whole, its blocks the
   CPU's run of the encoder (tests/glsl/bcn_astc_cpu.cpp) over the guest's, level by level; written with a block whose
   alpha moves, it is ETC2 again and stays so. Only where the device samples ETC2 and ASTC and BCn is encoded again
   (a GPU without BCn, or native_bc=0 on one with all three, as popola's Intel one): skipped elsewhere, llvmpipe among
   them. Uses vulkan_astc_later.inc's AL_CPU, AL_GVA and al_frame */
#include "bcn_astc_cpu.h"

#define BL_INDEX 12 /* its place among vulkan_astc_later.inc's cases' memory */

/* every 16 bytes a BC7 block of one alpha: random bytes, the mode one of 0 to 3, which hold no alpha */
static void bl_fill(NvnTexture* tex, const NvnTexLayout* lay, u64 cpu, u32 seed) {
    (void)tex;
    guest_done(cpu, lay->size);
    for (u64 i = 0; i < lay->size; i++) {
        seed ^= seed << 13, seed ^= seed >> 17, seed ^= seed << 5;
        GP(cpu)[i] = (u8)seed;
    }
    for (u64 i = 0; i < lay->size; i += 16) GP(cpu)[i] = (u8)((GP(cpu)[i] & 0xF0) | 1u << (i / 16 % 4));
}
/* level l's blocks as they lie in guest memory, in rows: malloc'd, bx by by of them */
static u8* bl_level_blocks(const NvnTexture* tex, u64 cpu, u32 l, u32* bx, u32* by) {
    u32 w = tex->d.width >> l ? tex->d.width >> l : 1, h = tex->d.height >> l ? tex->d.height >> l : 1;
    unsigned tile_h, tile_d;
    *bx = (w + 3) / 4, *by = (h + 3) / 4;
    nvn_tex_level_tiles(&tex->d, l, &tile_h, &tile_d);
    u8* out = malloc((size_t)*bx * *by * 16);
    const u8* level = GP(cpu) + nvn_tex_level_offset(&tex->d, l);
    for (u32 y = 0; out && y < *by; y++)
        for (u32 x = 0; x < *bx; x++)
            memcpy(out + ((size_t)y * *bx + x) * 16, level + bl_offset(x * 16, y, 0, *bx * 16, *by, tile_h, 0), 16);
    return out;
}
/* 1 where t's image, ASTC 4x4, isn't the CPU's run of the encoder over the guest's blocks, said with the first level and
   block that differs */
static int bl_differ(const NvnTexture* tex, u64 cpu, Surf* t) {
    int bad = 0;
    for (u32 l = 0; l < tex->d.levels && !bad; l++) {
        u32 bx, by, w = tex->d.width >> l ? tex->d.width >> l : 1, h = tex->d.height >> l ? tex->d.height >> l : 1;
        u8* blocks = bl_level_blocks(tex, cpu, l, &bx, &by);
        u8* want = malloc((size_t)bx * by * 16);
        VkBuffer buffer;
        VkBufferImageCopy c = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l, 0, 1}, .imageExtent = {w, h, 1}};
        if (!blocks || !want) return 1;
        bcn_astc_cpu(9 /* tests/support/bcn.h's BCN_BC7 */, blocks, bx, by, 1, 0, want);
        vk_lock();
        u8* got = vk_readback_buffer((u64)bx * by * 16, &buffer);
        if (got) {
            vk_surf_use(vk_cmd(), t, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                        VK_ACCESS_2_TRANSFER_READ_BIT);
            rec_CmdCopyImageToBuffer(vk_cmd(), t->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &c);
            vk_finish();
            vk_readback_ready();
        }
        u32 differ = 0, first = 0;
        for (u32 i = 0; got && i < bx * by; i++)
            if (memcmp(got + (size_t)i * 16, want + (size_t)i * 16, 16) && !differ++) first = i;
        vk_unlock();
        if (!got || differ) {
            fprintf(
                stderr,
                "BC7 as ASTC between frames: level %u, %u of %u blocks differ from the CPU's run of the encoder over "
                "the guest's, the first block %u\n",
                l, differ, bx * by, first);
            bad = 1;
        }
        free(blocks);
        free(want);
    }
    return bad;
}
static int check_bcn_astc_later(void) {
    NvnTexture tex = {0};
    NvnTexLayout lay;
    u32 tic[8];
    const u64 cpu = AL_CPU + BL_INDEX * AL_SPAN;
    VkAstcLaterStats a, b;
    tex.d.target = NVN_TEXTURE_TARGET_2D;
    tex.d.format = NVN_FORMAT_BPTC_UNORM;
    tex.d.levels = 10;
    tex.d.width = 512;
    tex.d.height = 320;
    tex.d.depth = 1;
    tex.gpu = AL_GVA + BL_INDEX * AL_GVA_STEP;
    nvn_tex_layout(&tex.d, &lay);
    CHECK(lay.size <= AL_SPAN && nvn_tic_encode(&tex, NULL, 0, 0, tic));
    VkFmt held = vk_tic_format(tic[0], 0);
    if (vk_bcn_astc_later(&held) == VK_FORMAT_UNDEFINED) {
        printf("BC7 as ASTC between frames: skipped, BC7 is held as Vulkan format %d here (it takes a GPU that samples "
               "ETC2 and ASTC, and BCn encoded again: none, or native_bc=0)\n",
               (int)held.vk);
        return 0;
    }
    CHECK(mem_map(cpu, AL_SPAN, MS_NORMAL, PERM_RW) == 0);
    bl_fill(&tex, &lay, cpu, 0x9e3779b9u);
    gmmu_map(tex.gpu, cpu, AL_SPAN);
    /* loaded as ETC2 RGBA8, which the game samples: not queued before its presents */
    Surf* t = al_frame(tic);
    CHECK(t && t->upload_valid && t->f.vk == VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK && t->astc_later && !t->astc.queued);
    const VkImage etc2 = t->img;
    /* sampled a present at a time: queued at VK_ASTC_LATER_READS of them and no sooner (the present that loaded it
       may count as one) */
    u32 presents = 0;
    while (!t->astc.queued && presents < 2 * VK_ASTC_LATER_READS) {
        CHECK(al_frame(tic) == t);
        presents++;
    }
    CHECK(t->astc.queued && t->img == etc2 && t->astc_reads == VK_ASTC_LATER_READS &&
          presents + 1 >= VK_ASTC_LATER_READS && presents <= VK_ASTC_LATER_READS);
    /* the encoder's frames: its blocks a frame at most, the ETC2 image the game's until the last */
    vk_tex_astc_later_stats(&a);
    /* the load's own work (the ETC2 encode among it) done first, so a frame's share below is the encoder's */
    vk_lock();
    vk_finish();
    vk_unlock();
    u32 frames = 0;
    u64 most = 0, total = 0, took[4] = {0, 0, 0, 0};
    while (t->astc.queued && frames < 64) {
        VkAstcLaterStats before, after;
        vk_tex_astc_later_stats(&before);
        /* a frame's share timed to the GPU's end of it, for the line below: what the encoder costs a frame here */
        const u64 started = plat_ns();
        vk_lock();
        vk_tex_later_frame();
        vk_finish();
        vk_unlock();
        if (frames < 4) took[frames] = plat_ns() - started;
        vk_tex_astc_later_stats(&after);
        const u64 blocks = after.blocks - before.blocks;
        CHECK(blocks && (t->astc.queued ? t->img == etc2 : t->img != etc2));
        if (blocks > most) most = blocks;
        total += blocks;
        frames++;
    }
    vk_tex_astc_later_stats(&b);
    u64 blocks = 0;
    for (u32 l = 0; l < tex.d.levels; l++) {
        u32 w = 512u >> l ? 512u >> l : 1, h = 320u >> l ? 320u >> l : 1;
        blocks += (u64)((w + 3) / 4) * ((h + 3) / 4);
    }
    /* a row of level 0 is 128 blocks: whole rows, 4096 a frame, and the small levels together in the last */
    if (t->astc.queued || t->f.vk != VK_FORMAT_ASTC_4x4_UNORM_BLOCK || total != blocks || most > VK_ASTC_LATER_BLOCKS ||
        b.made != a.made + 1) {
        fprintf(
            stderr,
            "BC7 as ASTC between frames: %u frames, %llu blocks (%llu in the texture), %llu in one frame (%u at most), "
            "format %d, %s, %llu made\n",
            frames, (unsigned long long)total, (unsigned long long)blocks, (unsigned long long)most,
            VK_ASTC_LATER_BLOCKS, (int)t->f.vk, t->astc.queued ? "queued still" : "whole",
            (unsigned long long)(b.made - a.made));
        return 1;
    }
    int failed = bl_differ(&tex, cpu, t);
    /* sampled on as ASTC, nothing more is made */
    CHECK(al_frame(tic) == t && !t->astc.queued && t->f.vk == VK_FORMAT_ASTC_4x4_UNORM_BLOCK);
    /* written, one block's alpha moving now (mode 6, its two alpha ends apart): ETC2 RGBA8 again as it loads, and left
       so after its presents */
    const VkImage astc = t->img;
    bl_fill(&tex, &lay, cpu, 0x2545f491u);
    u8* block = GP(cpu) + nvn_tex_level_offset(&tex.d, 1) + 16 * 5;
    memset(block, 0, 16);
    block[0] = 0x40;
    block[7] = 0x7F;
    CHECK(al_frame(tic) == t && t->img != astc && t->f.vk == VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK);
    for (u32 i = 0; i < VK_ASTC_LATER_READS + 2; i++) CHECK(al_frame(tic) == t);
    vk_tex_astc_later_stats(&a);
    CHECK(!t->astc.queued && t->astc_settled && a.kept == b.kept + 1 && a.undone == b.undone + 1);
    if (!failed)
        printf(
            "BC7 as ASTC between frames, 512x320 in 10 levels: ETC2 RGBA8 at once, queued at %u presents, ASTC 4x4 in "
            "%u frames of %llu blocks at most (%llu in all; the first four frames' shares %.2f, %.2f, %.2f and %.2f ms "
            "with the GPU's work, the first with the image made), the CPU's blocks; written with an "
            "alpha that moves, ETC2 again and left so\n",
            VK_ASTC_LATER_READS, frames, (unsigned long long)most, (unsigned long long)total, (double)took[0] / 1e6,
            (double)took[1] / 1e6, (double)took[2] / 1e6, (double)took[3] / 1e6);
    gmmu_space_unmap(tex.gpu, AL_SPAN);
    mem_unmap(cpu, AL_SPAN);
    return failed;
}
