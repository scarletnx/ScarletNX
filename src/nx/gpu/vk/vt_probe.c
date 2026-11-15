/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* aogo/vt-working-set: vt_probe=1's sampling probe, a local measurement for the virtual texture study (never pushed) */
#include <stdlib.h>
#include <string.h>
#include "nx/gpu/vk/vk.h"
#include "nx/gpu/shader/shader.h"
#include "nx/runtime/options.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"

#define VT_PROBE_WINDOW 60u
/* what the probe keeps of the texture a header index named when a draw last bound it: a copy, as the texture may be
   freed before its marks land (a pointer kept here read freed textures in the run's union) */
typedef struct {
    u32 w, h, layers, levels, depth3d, type;
    u8 bw, bh, valid;
    /* cls: 0 ASTC, 1 BCn as the guest has it, 2 any other format (render targets are left out). gbpp: the guest's
       bytes a block of bw x bh texels. hbits: the bits a texel takes as held now. slot, gen: its vt_sparse record (slot
       plus one, 0 none) and the record's generation when the copy was made */
    u8 cls, gbpp;
    u16 hbits;
    u32 slot, gen;
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
/* vt_sparse (aogo/vt-sparse): the textures whose memory follows the probe's marks, a record each. seen[b]: the
   present (g_vts.now) the probe last saw sparse block b sampled in, 0 never; bound: the blocks its last bind gave
   memory; fresh: of those, the ones with no texels yet. used: presents a draw bound it for sampling in, marked: frames
   of marks that landed for it; with 8 presents used and no mark it is sampled in a way the probe doesn't see (a vertex
   or compute program, a fetch), and is bound whole from then on */
typedef struct {
    Surf* t; /* NULL: a free record */
    u32 gen;
    u32 nblocks;
    u32* seen;
    u8 *bound, *fresh;
    u32 used, used_at, marked;
    u8 changed, whole;
} VtLive;
static struct {
    VtLive* v;
    u32 n, cap, now;
    /* since the last window: binds that went through and that found no memory, loads, strips decoded, blocks let
       go, textures turned whole, and the host's time in vk_vt_frame */
    u64 binds, fails, loads, strips, evicted, wholes, ns;
} g_vts;
static VtLive* vt_rec(const Surf* t) { return t && t->vt_slot ? &g_vts.v[t->vt_slot - 1] : NULL; }
static int vt_bit(const u8* b, u64 i) { return b[i >> 3] >> (i & 7) & 1; }
int vk_vt_register(Surf* t) {
    const VkSparseShape* sh = vk_image_sparse_shape(t->img);
    if (!sh) return 0;
    u32 i = 0;
    while (i < g_vts.n && g_vts.v[i].t) i++;
    if (i == g_vts.n) {
        if (g_vts.n == g_vts.cap) {
            const u32 cap = g_vts.cap ? g_vts.cap * 2 : 256;
            VtLive* grown = realloc(g_vts.v, cap * sizeof *grown);
            if (!grown) return 0;
            g_vts.v = grown;
            g_vts.cap = cap;
        }
        g_vts.v[g_vts.n++] = (VtLive){0};
    }
    VtLive* L = &g_vts.v[i];
    const u32 n = sh->layers * sh->per_layer, bytes = n / 8 + 1;
    u32* seen = calloc(1, (size_t)n * sizeof *seen + 2u * bytes); /* the three arrays in one allocation */
    if (!seen) return 0;
    *L = (VtLive){.t = t, .gen = L->gen + 1, .nblocks = n, .seen = seen};
    L->bound = (u8*)(seen + n);
    L->fresh = L->bound + bytes;
    t->vt_slot = i + 1;
    return 1;
}
void vk_vt_forget(Surf* t) {
    VtLive* L = vt_rec(t);
    if (!L) return;
    free(L->seen);
    const u32 gen = L->gen;
    *L = (VtLive){.gen = gen};
    t->vt_slot = 0;
}
void vk_vt_want(const Surf* t, u8* bits, u64 nbits) {
    const VtLive* L = vt_rec(t);
    if (!L || L->whole) return;
    const u32 window = (u32)g_settings.vt_frames;
    for (u64 b = 0; b < nbits; b++) {
        const int keep = b < L->nblocks && L->seen[b] && g_vts.now - L->seen[b] < window;
        if (!keep) bits[b >> 3] &= (u8) ~(1u << (b & 7));
    }
}
void vk_vt_bound(const Surf* t, const u8* bits, u64 nbits) {
    VtLive* L = vt_rec(t);
    if (!L) return;
    const u64 n = (nbits < L->nblocks ? nbits : L->nblocks) / 8 + 1;
    for (u64 i = 0; i < n; i++) {
        L->fresh[i] |= (u8)(bits[i] & ~L->bound[i]);
        L->fresh[i] &= bits[i]; /* a block let go has nothing left to decode */
        L->bound[i] = bits[i];
    }
    g_vts.binds++;
}
/* rows y0 to y1 of a level of a layer have a block bound with no texels yet (a mip tail's level: no, it loads whole) */
int vk_vt_strip_fresh(const Surf* t, u32 layer, u32 level, u32 y0, u32 y1) {
    const VtLive* L = vt_rec(t);
    const VkSparseShape* sh = L ? vk_image_sparse_shape(t->img) : NULL;
    if (!sh || level >= sh->tail_first || layer >= sh->layers || y1 <= y0) return 0;
    const u32 gh = sh->gran.height, last = sh->by[level] ? sh->by[level] - 1 : 0;
    u32 by1 = (y1 - 1) / gh;
    if (by1 > last) by1 = last;
    const u64 base = (u64)layer * sh->per_layer + sh->first[level];
    for (u32 by = y0 / gh; by <= by1; by++)
        for (u32 bx = 0; bx < sh->bx[level]; bx++)
            if (vt_bit(L->fresh, base + (u64)by * sh->bx[level] + bx)) {
                g_vts.strips++;
                return 1;
            }
    return 0;
}
int vk_vt_level_fresh(const Surf* t, u32 layer, u32 level) {
    const VtLive* L = vt_rec(t);
    const VkSparseShape* sh = L ? vk_image_sparse_shape(t->img) : NULL;
    if (!sh || level >= sh->tail_first || layer >= sh->layers) return 0;
    const u64 base = (u64)layer * sh->per_layer + sh->first[level], n = (u64)sh->bx[level] * sh->by[level];
    for (u64 b = base; b < base + n; b++)
        if (vt_bit(L->fresh, b)) return 1;
    return 0;
}
void vk_vt_loaded(const Surf* t) {
    VtLive* L = vt_rec(t);
    if (L) memset(L->fresh, 0, L->nblocks / 8 + 1);
}
/* a header index's marks of a frame, words its region, on the sparse blocks of the VT texture x names: each block a
   marked tile covers is seen now, and one without memory makes the texture bind again */
static void vt_mark_blocks(VtLive* L, const VtTex* x, const u32* words) {
    const VkSparseShape* sh = vk_image_sparse_shape(L->t->img);
    if (!sh) return;
    const u32 layers = x->layers ? x->layers : 1, levels = x->levels ? x->levels : 1;
    u32 across, down;
    const u32 side = SH_VT_PAGE << shader_vt_tile(x->w, x->h, layers, levels, &across, &down);
    const u32 per_layer = across * down, per_level = per_layer * layers;
    const u32 n = levels * per_level < SH_VT_REGION ? levels * per_level : SH_VT_REGION;
    const u32 gw = sh->gran.width, gh = sh->gran.height;
    for (u32 i = 0; i < (n + 31) / 32; i++) {
        u32 word = words[i] & (i == n / 32 ? (1u << (n % 32)) - 1 : ~0u);
        while (word) {
            const u32 b = i * 32 + (u32)__builtin_ctz(word);
            word &= word - 1;
            const u32 m = b / per_level, r = b % per_level, layer = r / per_layer, tile = r % per_layer;
            if (m >= sh->tail_first || layer >= sh->layers) continue;
            const u32 wm = x->w >> m ? x->w >> m : 1, hm = x->h >> m ? x->h >> m : 1;
            const u32 x0 = tile % across * side, y0 = tile / across * side;
            if (x0 >= wm || y0 >= hm) continue;
            const u32 x1 = x0 + side < wm ? x0 + side : wm, y1 = y0 + side < hm ? y0 + side : hm;
            for (u32 by = y0 / gh; by <= (y1 - 1) / gh && by < sh->by[m]; by++)
                for (u32 bx = x0 / gw; bx <= (x1 - 1) / gw && bx < sh->bx[m]; bx++) {
                    const u64 k = (u64)layer * sh->per_layer + sh->first[m] + (u64)by * sh->bx[m] + bx;
                    if (k >= L->nblocks) continue;
                    L->seen[k] = g_vts.now;
                    if (!vt_bit(L->bound, k)) L->changed = 1;
                }
        }
    }
    L->marked++;
}
void vk_vt_frame(void) {
    if (!g_settings.vt_sparse || !g_vt_probe_address) return;
    const u64 t0 = plat_ns();
    g_vts.now++;
    const u32 window = (u32)g_settings.vt_frames;
    const int sweep = g_vts.now % 30 == 0;
    for (u32 i = 0; i < g_vts.n; i++) {
        VtLive* L = &g_vts.v[i];
        if (!L->t) continue;
        if (!L->whole && L->used >= 8 && !L->marked) {
            L->whole = 1;
            L->changed = 1;
            g_vts.wholes++;
        }
        if (sweep && !L->whole)
            for (u32 b = 0; b < L->nblocks; b++)
                if (vt_bit(L->bound, b) && (!L->seen[b] || g_vts.now - L->seen[b] >= window)) {
                    L->changed = 1;
                    g_vts.evicted++;
                }
        if (!L->changed) continue;
        L->changed = 0;
        g_vts.loads++;
        if (!vk_tex_vt_load(L->t)) {
            g_vts.fails++;
            if (L->t) L->changed = 1; /* tried again at the next present */
        }
    }
    g_vts.ns += plat_ns() - t0;
}
/* the VT textures now: how many, their memory, blocks bound, whole ones; and the window's counts, cleared */
static void vt_sparse_say(void) {
    if (!g_settings.vt_sparse) return;
    u64 n = 0, bytes = 0, blocks = 0, all = 0, whole = 0, whole_bytes = 0;
    for (u32 i = 0; i < g_vts.n; i++) {
        const VtLive* L = &g_vts.v[i];
        if (!L->t) continue;
        n++;
        bytes += L->t->bytes;
        all += L->nblocks;
        for (u32 b = 0; b < L->nblocks; b++) blocks += vt_bit(L->bound, b);
        if (L->whole) whole++, whole_bytes += L->t->bytes;
    }
    LOG("vt sparse: %llu textures, %llu MB bound, %llu of their %llu blocks; %llu whole (%llu MB), %llu turned whole; "
        "the window: %llu binds, %llu without memory, %llu loads, %llu strips decoded, %llu blocks let go, %.2f ms of "
        "the host a present\n",
        (unsigned long long)n, (unsigned long long)(bytes >> 20), (unsigned long long)blocks, (unsigned long long)all,
        (unsigned long long)whole, (unsigned long long)(whole_bytes >> 20), (unsigned long long)g_vts.wholes,
        (unsigned long long)g_vts.binds, (unsigned long long)g_vts.fails, (unsigned long long)g_vts.loads,
        (unsigned long long)g_vts.strips, (unsigned long long)g_vts.evicted, (double)g_vts.ns / 1e6 / VT_PROBE_WINDOW);
    g_vts.binds = g_vts.fails = g_vts.loads = g_vts.strips = g_vts.evicted = g_vts.wholes = g_vts.ns = 0;
}
/* the bits a texel of t takes as it is held now: an ASTC one as BC7, BC1, BC4, BC5 or RGBA8; another as the guest
   has it */
static u32 vt_held_bits(const Surf* t) {
    const VkFormat v = t->f.vk;
    if (t->f.conv == VKC_ASTC || t->f.conv != VKC_NONE) {
        if (v >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && v <= VK_FORMAT_BC7_SRGB_BLOCK)
            return v <= VK_FORMAT_BC1_RGBA_SRGB_BLOCK || v == VK_FORMAT_BC4_UNORM_BLOCK ||
                           v == VK_FORMAT_BC4_SNORM_BLOCK
                       ? 4
                       : 8;
        return 32;
    }
    const u32 bits = t->f.bpp * 8u / (t->f.bw * t->f.bh ? t->f.bw * t->f.bh : 1);
    return bits ? bits : 32;
}

/* the header indices the draws bound ASTC textures by: the most, and how many were past SH_VT_TICS (logged with the
   window, then cleared), with a few handles as they were */
static struct {
    u32 most, past, seen, sample[8];
} g_vt_handles;
void vk_tex_vt_handle(u32 handle, Surf* t) {
    if (!g_vt_probe_address || !t || t->is_rt) return;
    VtLive* L = vt_rec(t);
    if (L && L->used_at != g_vts.now + 1) {
        L->used_at = g_vts.now + 1;
        L->used++;
    }
    const u32 tic = handle & 0xFFFFF;
    if (tic > g_vt_handles.most) g_vt_handles.most = tic;
    if (g_vt_handles.seen < 8) g_vt_handles.sample[g_vt_handles.seen] = handle;
    g_vt_handles.seen++;
    const u8 cls =
        t->f.conv == VKC_ASTC                                                                                      ? 0
        : t->f.conv == VKC_NONE && t->f.vk >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && t->f.vk <= VK_FORMAT_BC7_SRGB_BLOCK ? 1
                                                                                                                   : 2;
    if (tic < SH_VT_TICS - 1)
        g_vt_bits.tic_tex[tic] =
            (VtTex){t->w,       t->h,           t->layers, t->levels, t->depth3d,   (u32)t->type,
                    t->f.bw,    t->f.bh,        1,         cls,       (u8)t->f.bpp, (u16)vt_held_bits(t),
                    t->vt_slot, L ? L->gen : 0, t->bytes};
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
        const VtTex* x = &g_vt_bits.tic_tex[tic];
        if (x->valid && x->slot && x->slot <= g_vts.n) { /* vt_sparse: its blocks seen now */
            VtLive* L = &g_vts.v[x->slot - 1];
            if (L->t && L->gen == x->gen) vt_mark_blocks(L, x, words);
        }
        /* the counts of phase 1 are ASTC's; vt_classes counts every class */
        const VtTex* t = x->valid && !x->cls ? x : NULL;
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
/* What virtual texturing of each class (0 ASTC, 1 BCn, 2 the others) would hold and load, from the marks of a set of
   frames, all (the window's union, or the run's): textures marked, their bytes held now, the 64 KiB sparse blocks the
   marked tiles touch in the format VT would hold them in (RGBA8 for ASTC, decoded exact; the held one for the others)
   with each layer's mip tail whole, the guest bytes of the textures whole, and of the marked tiles */
typedef struct {
    u64 n, held, vt, guest, touched;
    u64 big_n,
        big_vt; /* of those, the textures too big for 128 texel tiles in the probe's region: their tiles count more */
} VtClass;
static void vt_granule(u32 bits, u32* gw, u32* gh) { /* Vulkan's standard 2D sparse block shapes */
    switch (bits) {
        case 4: *gw = 512, *gh = 256; return;
        case 8: *gw = 256, *gh = 256; return;
        case 16: *gw = 256, *gh = 128; return;
        case 64: *gw = 128, *gh = 64; return;
        case 128: *gw = 64, *gh = 64; return;
        default: *gw = 128, *gh = 128; return;
    }
}
static void vt_classes(const u32* all, VtClass out[3]) {
    static u8 seen[1u << 13]; /* a level's blocks of one layer: 256 x 256 at most (16384 texels over 64) */
    memset(out, 0, 3 * sizeof *out);
    for (u32 tic = 0; tic < SH_VT_TICS - 1; tic++) {
        const u32* words = all + (u64)tic * (SH_VT_REGION / 32);
        u32 any = 0;
        for (u32 i = 0; i < SH_VT_REGION / 32; i++) any |= words[i];
        const VtTex* x = &g_vt_bits.tic_tex[tic];
        if (!any || !x->valid || x->cls > 2) continue;
        VtClass* c = &out[x->cls];
        const u32 layers = x->layers ? x->layers : 1, levels = x->levels ? x->levels : 1;
        const u32 bw = x->bw ? x->bw : 1, bh = x->bh ? x->bh : 1, gb = x->gbpp ? x->gbpp : 4;
        const u32 hbits = x->cls ? x->hbits : 32;
        u32 across, down, gw, gh;
        const u32 side = SH_VT_PAGE << shader_vt_tile(x->w, x->h, layers, levels, &across, &down);
        const u32 per_layer = across * down, per_level = per_layer * layers;
        vt_granule(hbits, &gw, &gh);
        const u64 vt0 = c->vt;
        c->n++;
        c->held += x->bytes;
        u64 tail = 0;
        for (u32 m = 0; m < levels; m++) {
            const u32 wm = x->w >> m ? x->w >> m : 1, hm = x->h >> m ? x->h >> m : 1;
            c->guest += (u64)((wm + bw - 1) / bw) * ((hm + bh - 1) / bh) * gb * layers;
            if (wm < gw || hm < gh) { /* the mip tail: bound whole, as a sparse image's is */
                tail += (u64)wm * hm * hbits / 8;
                continue;
            }
            const u32 bx = (wm + gw - 1) / gw, by = (hm + gh - 1) / gh;
            if ((u64)bx * by > sizeof seen * 8) continue;
            for (u32 layer = 0; layer < layers; layer++) {
                const u32 at = m * per_level + layer * per_layer;
                if (at >= SH_VT_REGION) break;
                memset(seen, 0, ((u64)bx * by + 7) / 8);
                for (u32 tile = 0; tile < per_layer && at + tile < SH_VT_REGION; tile++) {
                    if (!(words[(at + tile) >> 5] >> ((at + tile) & 31) & 1)) continue;
                    const u32 x0 = tile % across * side, y0 = tile / across * side;
                    if (x0 >= wm || y0 >= hm) continue;
                    const u32 x1 = x0 + side < wm ? x0 + side : wm, y1 = y0 + side < hm ? y0 + side : hm;
                    c->touched += (u64)((x1 - x0 + bw - 1) / bw) * ((y1 - y0 + bh - 1) / bh) * gb;
                    for (u32 j = y0 / gh; j <= (y1 - 1) / gh; j++)
                        for (u32 i = x0 / gw; i <= (x1 - 1) / gw; i++) {
                            const u32 k = j * bx + i;
                            if (!(seen[k >> 3] >> (k & 7) & 1)) {
                                seen[k >> 3] |= (u8)(1u << (k & 7));
                                c->vt += 65536;
                            }
                        }
                }
            }
        }
        c->vt += (tail + 65535) / 65536 * 65536 * layers;
        if (side > SH_VT_PAGE) c->big_n++, c->big_vt += c->vt - vt0;
    }
}
static void vt_classes_say(const char* what, const u32* all) {
    VtClass c[3];
    vt_classes(all, c);
    static const char* names[3] = {"ASTC", "BCn", "other"};
    for (int k = 0; k < 3; k++)
        if (c[k].n)
            LOG("vt probe: %s, %s: %llu textures marked, %llu MB held whole now, %llu MB as virtual textures (%s 64 KiB "
                "blocks of the marked tiles and the mip tails; %llu textures of tiles over 128 texels, %llu MB of it); "
                "%llu MB in guest memory whole, %llu MB of it in the marked tiles\n",
                what, names[k], (unsigned long long)c[k].n, (unsigned long long)(c[k].held >> 20),
                (unsigned long long)(c[k].vt >> 20), k ? "held-format" : "RGBA8", (unsigned long long)c[k].big_n,
                (unsigned long long)(c[k].big_vt >> 20), (unsigned long long)(c[k].guest >> 20),
                (unsigned long long)(c[k].touched >> 20));
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
        const VtTex* t = g_vt_bits.tic_tex[tic].valid && !g_vt_bits.tic_tex[tic].cls ? &g_vt_bits.tic_tex[tic] : NULL;
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
            const VtTex* t =
                g_vt_bits.tic_tex[tic].valid && !g_vt_bits.tic_tex[tic].cls ? &g_vt_bits.tic_tex[tic] : NULL;
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
    vt_classes_say("the window's frames", g_vt_bits.union_bits);
    vt_classes_say("the run so far", g_vt_bits.run_bits);
    vt_sparse_say();
    memset(g_vt_bits.union_bits, 0, (size_t)SH_VT_TICS * SH_VT_REGION / 8);
    g_vt_bits.nframes = 0;
    g_vt_bits.overflow_bits = 0;
}
