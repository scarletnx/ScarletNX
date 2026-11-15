/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* aogo/vt-working-set: vt_probe=1's sampling probe, a local measurement for the virtual texture study (never pushed) */
#include <stdlib.h>
#include <string.h>
#include "nx/gpu/vk/vk.h"
#include "nx/gpu/shader/shader.h"
#include "nx/runtime/options.h"
#include "nx/runtime/platform/log.h"

#define VT_PROBE_WINDOW 60u
/* what the probe keeps of the texture a header index named when a draw last bound it: a copy, as the texture may be
   freed before its marks land (a pointer kept here read freed textures in the run's union) */
typedef struct {
    u32 w, h, layers, levels, depth3d, type;
    u8 bw, bh, valid;
    u64 bytes;
} VtTex;
static u64 vt_texels(const VtTex* t) {
    u64 n = 0;
    const u32 d = t->type == VK_IMAGE_VIEW_TYPE_3D ? t->depth3d : 1;
    for (u32 l = 0; l < t->levels; l++)
        n += (u64)(t->w >> l ? t->w >> l : 1) * (t->h >> l ? t->h >> l : 1) * (d >> l ? d >> l : 1);
    return n * (t->layers ? t->layers : 1);
}
static int vt_cmp(const void* a, const void* b) {
    const u64 x = *(const u64*)a, y = *(const u64*)b;
    return x < y ? -1 : x > y;
}
/* The sampling probe (the SM86 translator's vt_mark): its bitmap on the device, copied every present into one of
   VT_BACKS host buffers and cleared, each read once its submission is done: the bytes of the tiles a frame sampled,
   by texture header index (tic_tex: a copy of the texture each index named when a draw bound it last), and their union over
   the window. A tile counts its texels in RGBA8, min(T, level width) x min(T, level height) */
#define VT_BACKS 3
static struct {
    VkBuffer bits, back[VT_BACKS];
    DevMem bits_mem, back_mem[VT_BACKS];
    u32* back_ptr[VT_BACKS];
    u64 back_sub[VT_BACKS];
    u32 slot, made;
    u32* union_bits;
    u32* last_bits; /* the frame landed before: pages new in a frame are its churn */
    u32* run_bits;  /* every frame's since the start: the run's pages, which their ASTC blocks would back */
    u64 frames_pages[VT_PROBE_WINDOW], frames_new[VT_PROBE_WINDOW], frames_bigtile[VT_PROBE_WINDOW];
    VtTex tic_tex[SH_VT_TICS];
    u64 frames_bytes[VT_PROBE_WINDOW], frames_tex[VT_PROBE_WINDOW], frames_bound[VT_PROBE_WINDOW];
    u32 nframes;
    u64 overflow_bits;
} g_vt_bits;
/* the header indices the draws bound ASTC textures by: the most, and how many were past SH_VT_TICS (logged with the
   window, then cleared), with a few handles as they were */
static struct {
    u32 most, past, seen, sample[8];
} g_vt_handles;
void vk_tex_vt_handle(u32 handle, Surf* t) {
    if (!g_vt_probe_address || !t || t->f.conv != VKC_ASTC) return;
    const u32 tic = handle & 0xFFFFF;
    if (tic > g_vt_handles.most) g_vt_handles.most = tic;
    if (g_vt_handles.seen < 8) g_vt_handles.sample[g_vt_handles.seen] = handle;
    g_vt_handles.seen++;
    if (tic < SH_VT_TICS - 1)
        g_vt_bits.tic_tex[tic] =
            (VtTex){t->w, t->h, t->layers, t->levels, t->depth3d, (u32)t->type, t->f.bw, t->f.bh, 1, t->bytes};
    else
        g_vt_handles.past++;
}
static void vt_bits_buffer(VkBuffer* b, DevMem* m, u32 at, VkBufferUsageFlags usage, u32** ptr) {
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                             .size = (u64)SH_VT_TICS * SH_VT_REGION / 8,
                             .usage = usage,
                             .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    DevMemWant want = {
        .at = at, .mapped = at != DM_DEVICE, .need = at != DM_DEVICE ? VK_MEMORY_PROPERTY_HOST_COHERENT_BIT : 0};
    DevMemInfo info;
    if (!VK_OK(vk_devmem_buffer(&bi, &want, b, m, &info))) fatal("vt probe: no memory for its bitmap");
    if (ptr) *ptr = info.ptr;
}
void vk_tex_vt_init(void) {
    if (!g_settings.vt_probe) return;
    vt_bits_buffer(&g_vt_bits.bits, &g_vt_bits.bits_mem, DM_DEVICE,
                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                       VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                   NULL);
    for (u32 i = 0; i < VT_BACKS; i++)
        vt_bits_buffer(&g_vt_bits.back[i], &g_vt_bits.back_mem[i], DM_CACHED, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                       &g_vt_bits.back_ptr[i]);
    g_vt_bits.union_bits = calloc((size_t)SH_VT_TICS * SH_VT_REGION / 32, sizeof(u32));
    g_vt_bits.last_bits = calloc((size_t)SH_VT_TICS * SH_VT_REGION / 32, sizeof(u32));
    g_vt_bits.run_bits = calloc((size_t)SH_VT_TICS * SH_VT_REGION / 32, sizeof(u32));
    if (!g_vt_bits.run_bits) fatal("vt probe: no host memory for its bitmaps");
    if (!g_vt_bits.union_bits || !g_vt_bits.last_bits || !g_vt_bits.back_ptr[0])
        fatal("vt probe: no host memory for its bitmaps");
    VkBufferDeviceAddressInfo ai = {.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = g_vt_bits.bits};
    g_vt_probe_address = vkGetBufferDeviceAddress(g_vk.dev, &ai);
    LOG("vt probe: the samples' bitmap at device address %llx, %u MB, %u header indices of %u tiles\n",
        (unsigned long long)g_vt_probe_address, (u32)(((u64)SH_VT_TICS * SH_VT_REGION / 8) >> 20), SH_VT_TICS, SH_VT_REGION);
}
/* the tiles set in a header index's region, and of those the ones not set in prev (NULL: none), its tile side in
   *side */
static u64 vt_region_tiles(const VtTex* t, const u32* words, const u32* prev, u64* fresh, u32* side) {
    const u32 layers = t->layers ? t->layers : 1, levels = t->levels ? t->levels : 1;
    u32 across, down;
    *side = SH_VT_PAGE << shader_vt_tile(t->w, t->h, layers, levels, &across, &down);
    const u32 n = levels * across * down * layers < SH_VT_REGION ? levels * across * down * layers : SH_VT_REGION;
    u64 tiles = 0;
    for (u32 i = 0; i < (n + 31) / 32; i++) {
        const u32 mask = i == n / 32 ? (1u << (n % 32)) - 1 : ~0u;
        tiles += (u64)__builtin_popcount(words[i] & mask);
        if (prev && fresh) *fresh += (u64)__builtin_popcount(words[i] & ~prev[i] & mask);
    }
    return tiles;
}
/* the ASTC bytes of the tiles set in a header index's region: each tile's blocks, a level's edge tiles cut */
static u64 vt_region_astc(const VtTex* t, const u32* words) {
    const u32 layers = t->layers ? t->layers : 1, levels = t->levels ? t->levels : 1;
    u32 across, down;
    const u32 side = SH_VT_PAGE << shader_vt_tile(t->w, t->h, layers, levels, &across, &down);
    const u32 per_level = across * down * layers, bw = t->bw ? t->bw : 4, bh = t->bh ? t->bh : 4;
    u64 bytes = 0;
    for (u32 m = 0; m < levels; m++) {
        u64 tiles = 0;
        for (u32 b = m * per_level; b < (m + 1) * per_level && b < SH_VT_REGION; b++) tiles += words[b >> 5] >> (b & 31) & 1;
        const u64 wm = t->w >> m ? t->w >> m : 1, hm = t->h >> m ? t->h >> m : 1;
        const u64 tw = wm < side ? wm : side, th = hm < side ? hm : side;
        bytes += tiles * ((tw + bw - 1) / bw) * ((th + bh - 1) / bh) * 16;
    }
    return bytes;
}
/* the RGBA8 bytes of the tiles set in a header index's region, words its SH_VT_REGION / 32 */
static u64 vt_region_bytes(const VtTex* t, const u32* words) {
    const u32 layers = t->layers ? t->layers : 1, levels = t->levels ? t->levels : 1;
    u32 across, down;
    const u32 k = shader_vt_tile(t->w, t->h, layers, levels, &across, &down), side = 64u << k;
    const u32 per_level = across * down * layers;
    u64 bytes = 0;
    for (u32 m = 0; m < levels; m++) {
        u64 tiles = 0;
        for (u32 b = m * per_level; b < (m + 1) * per_level && b < SH_VT_REGION; b++)
            tiles += words[b >> 5] >> (b & 31) & 1;
        const u64 wm = t->w >> m ? t->w >> m : 1, hm = t->h >> m ? t->h >> m : 1;
        bytes += tiles * (wm < side ? wm : side) * (hm < side ? hm : side) * 4;
    }
    return bytes;
}
static void vt_bits_land(u32 slot) {
    const u32* bits = g_vt_bits.back_ptr[slot];
    u64 bytes = 0, tex = 0, bound = 0, pages = 0, fresh = 0, bigtile = 0;
    for (u32 tic = 0; tic < SH_VT_TICS; tic++) {
        const u32* words = bits + (u64)tic * (SH_VT_REGION / 32);
        u32 any = 0;
        for (u32 i = 0; i < SH_VT_REGION / 32; i++) any |= words[i];
        if (!any) continue;
        u32* u = g_vt_bits.union_bits + (u64)tic * (SH_VT_REGION / 32);
        u32* r = g_vt_bits.run_bits + (u64)tic * (SH_VT_REGION / 32);
        for (u32 i = 0; i < SH_VT_REGION / 32; i++) u[i] |= words[i], r[i] |= words[i];
        if (tic == SH_VT_TICS - 1) {
            for (u32 i = 0; i < SH_VT_REGION / 32; i++) g_vt_bits.overflow_bits += (u64)__builtin_popcount(words[i]);
            continue;
        }
        const VtTex* t = g_vt_bits.tic_tex[tic].valid ? &g_vt_bits.tic_tex[tic] : NULL;
        u32* last = g_vt_bits.last_bits + (u64)tic * (SH_VT_REGION / 32);
        if (t) {
            u32 side;
            const u64 tiles = vt_region_tiles(t, words, last, &fresh, &side);
            if (side == SH_VT_PAGE)
                pages += tiles;
            else
                bigtile += tiles; /* textures too big for the region at 128: their tiles are bigger */
            bytes += vt_region_bytes(t, words);
            bound += vt_texels(t) * 4;
            tex++;
        }
        memcpy(last, words, SH_VT_REGION / 8);
    }
    /* header indices this frame left: their pages are gone from the last frame */
    for (u32 tic = 0; tic < SH_VT_TICS; tic++) {
        const u32* words = bits + (u64)tic * (SH_VT_REGION / 32);
        u32 any = 0;
        for (u32 i = 0; i < SH_VT_REGION / 32; i++) any |= words[i];
        if (!any) memset(g_vt_bits.last_bits + (u64)tic * (SH_VT_REGION / 32), 0, SH_VT_REGION / 8);
    }
    if (g_vt_bits.nframes < VT_PROBE_WINDOW) {
        g_vt_bits.frames_bytes[g_vt_bits.nframes] = bytes;
        g_vt_bits.frames_tex[g_vt_bits.nframes] = tex;
        g_vt_bits.frames_pages[g_vt_bits.nframes] = pages;
        g_vt_bits.frames_new[g_vt_bits.nframes] = fresh;
        g_vt_bits.frames_bigtile[g_vt_bits.nframes] = bigtile;
        g_vt_bits.frames_bound[g_vt_bits.nframes++] = bound;
    }
}
void vk_vt_bits_present(void) {
    if (!g_vt_probe_address) return;
    /* the frames whose copies are done */
    u64 done = 0;
    if (!VK_OK(vkGetSemaphoreCounterValue(g_vk.dev, vk_gfx_timeline(), &done))) return;
    for (u32 i = 0; i < VT_BACKS; i++)
        if (g_vt_bits.back_sub[i] && g_vt_bits.back_sub[i] <= done) {
            vt_bits_land(i);
            g_vt_bits.back_sub[i] = 0;
        }
    /* this frame's marks into a free host buffer, the bitmap cleared for the next */
    u32 s = g_vt_bits.slot;
    if (g_vt_bits.back_sub[s]) return; /* none free: the frame's marks go into the next one's */
    VkCommandBuffer cb = vk_cmd();
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                           .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
                           .dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                           .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT};
    VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    rec_CmdPipelineBarrier2(cb, &d);
    VkBufferCopy copy = {0, 0, (u64)SH_VT_TICS * SH_VT_REGION / 8};
    rec_CmdCopyBuffer(cb, g_vt_bits.bits, g_vt_bits.back[s], 1, &copy);
    rec_CmdPipelineBarrier2(cb, &d);
    rec_CmdFillBuffer(cb, g_vt_bits.bits, 0, VK_WHOLE_SIZE, 0);
    mb.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
    mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_TRANSFER_READ_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    mb.dstAccessMask =
        VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_HOST_READ_BIT;
    rec_CmdPipelineBarrier2(cb, &d);
    g_vt_bits.back_sub[s] = vk_last_submit() + 1;
    g_vt_bits.slot = (s + 1) % VT_BACKS;
}
void vk_vt_bits_window(void) {
    if (!g_vt_probe_address || !g_vt_bits.nframes) return;
    u64 b[VT_PROBE_WINDOW], n[VT_PROBE_WINDOW], bound[VT_PROBE_WINDOW], pg[VT_PROBE_WINDOW], nw[VT_PROBE_WINDOW],
        bt[VT_PROBE_WINDOW];
    const u32 nf = g_vt_bits.nframes;
    memcpy(b, g_vt_bits.frames_bytes, nf * sizeof(u64));
    memcpy(n, g_vt_bits.frames_tex, nf * sizeof(u64));
    memcpy(bound, g_vt_bits.frames_bound, nf * sizeof(u64));
    memcpy(pg, g_vt_bits.frames_pages, nf * sizeof(u64));
    memcpy(nw, g_vt_bits.frames_new, nf * sizeof(u64));
    memcpy(bt, g_vt_bits.frames_bigtile, nf * sizeof(u64));
    qsort(b, nf, sizeof(u64), vt_cmp);
    qsort(n, nf, sizeof(u64), vt_cmp);
    qsort(bound, nf, sizeof(u64), vt_cmp);
    qsort(pg, nf, sizeof(u64), vt_cmp);
    qsort(nw, nf, sizeof(u64), vt_cmp);
    qsort(bt, nf, sizeof(u64), vt_cmp);
    u64 ubytes = 0, utex = 0, ubound = 0, upages = 0, ubig = 0;
    for (u32 tic = 0; tic < SH_VT_TICS - 1; tic++) {
        const u32* words = g_vt_bits.union_bits + (u64)tic * (SH_VT_REGION / 32);
        u32 any = 0;
        for (u32 i = 0; i < SH_VT_REGION / 32; i++) any |= words[i];
        const VtTex* t = g_vt_bits.tic_tex[tic].valid ? &g_vt_bits.tic_tex[tic] : NULL;
        if (!any || !t) continue;
        ubytes += vt_region_bytes(t, words);
        ubound += vt_texels(t) * 4;
        utex++;
        u32 side;
        const u64 tiles = vt_region_tiles(t, words, NULL, NULL, &side);
        if (side == SH_VT_PAGE)
            upages += tiles;
        else
            ubig += tiles;
    }
    LOG("vt probe: tiles over %u frames: a frame samples %llu pages of 128x128 (median; %llu at most), %llu of them "
        "not sampled the frame before (median; %llu at most), and %llu bigger tiles (median); %llu MB of tiles as RGBA8 "
        "(median; %llu at most) of %llu ASTC textures (median) whose whole size is %llu MB (median); the frames "
        "together %llu pages and %llu bigger tiles, %llu MB as RGBA8, of %llu textures whose whole size is %llu MB; "
        "%llu marks past the header indices\n",
        nf, (unsigned long long)pg[nf / 2], (unsigned long long)pg[nf - 1], (unsigned long long)nw[nf / 2],
        (unsigned long long)nw[nf - 1], (unsigned long long)bt[nf / 2], (unsigned long long)(b[nf / 2] >> 20),
        (unsigned long long)(b[nf - 1] >> 20), (unsigned long long)n[nf / 2], (unsigned long long)(bound[nf / 2] >> 20),
        (unsigned long long)upages, (unsigned long long)ubig, (unsigned long long)(ubytes >> 20),
        (unsigned long long)utex, (unsigned long long)(ubound >> 20), (unsigned long long)g_vt_bits.overflow_bits);
    LOG("vt probe: ASTC textures bound by %u handles, header index at most %u, %u past %u; handles %08x %08x %08x %08x "
        "%08x %08x %08x %08x\n",
        g_vt_handles.seen, g_vt_handles.most, g_vt_handles.past, SH_VT_TICS - 1, g_vt_handles.sample[0],
        g_vt_handles.sample[1], g_vt_handles.sample[2], g_vt_handles.sample[3], g_vt_handles.sample[4],
        g_vt_handles.sample[5], g_vt_handles.sample[6], g_vt_handles.sample[7]);
    g_vt_handles.most = g_vt_handles.past = g_vt_handles.seen = 0;
    {
        u64 rpages = 0, rbig = 0, rastc = 0, rrgba8 = 0, rtex = 0;
        /* by footprint (bw - 4, bh - 4): textures, pages and bigger tiles, the tiles' ASTC and RGBA8 bytes, and the
           whole textures' ASTC and held bytes */
        static u64 fp[9][9][7];
        memset(fp, 0, sizeof fp);
        for (u32 tic = 0; tic < SH_VT_TICS - 1; tic++) {
            const u32* words = g_vt_bits.run_bits + (u64)tic * (SH_VT_REGION / 32);
            u32 any = 0;
            for (u32 i = 0; i < SH_VT_REGION / 32; i++) any |= words[i];
            const VtTex* t = g_vt_bits.tic_tex[tic].valid ? &g_vt_bits.tic_tex[tic] : NULL;
            if (!any || !t) continue;
            u32 side;
            const u64 tiles = vt_region_tiles(t, words, NULL, NULL, &side);
            if (side == SH_VT_PAGE) rpages += tiles;
            else rbig += tiles;
            const u64 a = vt_region_astc(t, words), b = vt_region_bytes(t, words);
            rastc += a;
            rrgba8 += b;
            rtex++;
            if (t->bw >= 4 && t->bw <= 12 && t->bh >= 4 && t->bh <= 12) {
                u64* f = fp[t->bw - 4][t->bh - 4];
                f[0]++;
                f[side == SH_VT_PAGE ? 1 : 2] += tiles;
                f[3] += a;
                f[4] += b;
                f[5] += vt_texels(t) * 16 / ((u64)t->bw * t->bh);
                f[6] += t->bytes;
            }
        }
        if (g_settings.vt_probe)
            for (u32 x = 0; x < 9; x++)
                for (u32 y = 0; y < 9; y++)
                    if (fp[x][y][0])
                        LOG("vt probe: the run so far, ASTC %ux%u: %llu textures, %llu pages and %llu bigger tiles, %llu KB "
                            "as ASTC blocks, %llu MB as RGBA8; the textures whole %llu MB as ASTC, %llu MB held\n",
                            x + 4, y + 4, (unsigned long long)fp[x][y][0], (unsigned long long)fp[x][y][1],
                            (unsigned long long)fp[x][y][2], (unsigned long long)(fp[x][y][3] >> 10),
                            (unsigned long long)(fp[x][y][4] >> 20), (unsigned long long)(fp[x][y][5] >> 20),
                            (unsigned long long)(fp[x][y][6] >> 20));
        LOG("vt probe: the run so far: %llu pages of 128x128 and %llu bigger tiles of %llu textures, %llu MB as ASTC "
            "blocks, %llu MB as RGBA8\n",
            (unsigned long long)rpages, (unsigned long long)rbig, (unsigned long long)rtex,
            (unsigned long long)(rastc >> 20), (unsigned long long)(rrgba8 >> 20));
    }
    memset(g_vt_bits.union_bits, 0, (size_t)SH_VT_TICS * SH_VT_REGION / 8);
    g_vt_bits.nframes = 0;
    g_vt_bits.overflow_bits = 0;
}
