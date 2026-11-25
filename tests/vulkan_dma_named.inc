/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */

/* A buffer-to-texture copy that names its texture, level and layer (GpuDmaOp.tex, nvn_copy.c): the texture's image takes
   the rows there as well as guest memory (vk_tex_dma_rows). Every level of both layers of textures of several formats,
   whole and in a part, then one that names a layer it does not go into, whose image takes nothing of it there. After
   each the image is memory's, and after the named ones nothing was loaded again. No extracted shaders or game data are
   used */
#define DN_CPU  (HEAP_BASE + 0x3A000000)
#define DN_GVA  0x6c0000000ull
#define DN_SPAN 0x200000ull /* each case's memory: the texture, then its copies' source */
static const TpCase g_dn_cases[] = {
    {"RGBA8", NVN_FORMAT_RGBA8, 4, 1, VK_FORMAT_R8G8B8A8_UNORM, 100, 60, 0},
    {"RGBA8 of 256", NVN_FORMAT_RGBA8, 4, 1, VK_FORMAT_R8G8B8A8_UNORM, 256, 200, 0},
    {"R8", NVN_FORMAT_R8, 1, 1, VK_FORMAT_R8_UNORM, 130, 70, 0},
    {"BC1", NVN_FORMAT_RGBA_DXT1, 8, 4, VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 100, 60, 0},
    {"BC3", NVN_FORMAT_RGBA_DXT5, 16, 4, VK_FORMAT_BC3_UNORM_BLOCK, 132, 68, 0},
};
/* the source's bytes of a copy into level l of a layer, and the copy: of the level's blocks (x0, y0) to (+nx, +ny), that
   names layer `named` */
static int dn_copy(const TpTex* x, u32 layer, u32 l, u32 named, const u32 part[4]) {
    const TpCase* c = x->c;
    const u32 w = c->w >> l ? c->w >> l : 1, h = c->h >> l ? c->h >> l : 1, row = part[2] * c->eb;
    const u64 src = x->lay.size + 0x1000;
    unsigned th, td;
    nvn_tex_level_tiles(&x->tex.d, l, &th, &td);
    for (u32 i = 0; i < row * part[3]; i++)
        GP(x->cpu + src)[i] = (u8)(i * 53 + l * 91 + layer * 17 + part[0] + (i >> 8) + named * 5);
    g_renderer->mem_may_have_changed();
    GpuDmaOp o = {.multi = 1, .width = row, .line_in = row, .line_out = row, .lines = part[3]};
    o.src = pitch_side(x->gva + src, row);
    o.dst = (GpuDmaSide){.addr = x->gva + tp_level(x, layer, l),
                         .bl = 1,
                         .wb = (w + c->bw - 1) / c->bw * c->eb,
                         .h = (h + c->bw - 1) / c->bw,
                         .bh = th,
                         .bd = td,
                         .ox = part[0] * c->eb,
                         .oy = part[1]};
    o.tex = x->gva;
    o.tex_level = l;
    o.tex_layer = named;
    return g_renderer->dma_copy(&o);
}
static int dn_case(const TpCase* c, u64 at) {
    TpTex x = {.c = c, .layers = 2, .cpu = DN_CPU + at, .gva = DN_GVA + at};
    const u32 big = c->w > c->h ? c->w : c->h;
    int failed = 0;
    x.tex.d.target = NVN_TEXTURE_TARGET_2D_ARRAY;
    x.tex.d.format = (u16)c->format;
    x.tex.d.levels = (u8)(32u - snx_clz32(big));
    x.tex.d.width = c->w;
    x.tex.d.height = c->h;
    x.tex.d.depth = x.layers;
    x.tex.gpu = x.gva;
    nvn_tex_layout(&x.tex.d, &x.lay);
    CHECK(x.lay.size + 0x1000 + (u64)c->w * c->h * c->eb <= DN_SPAN && nvn_tic_encode(&x.tex, NULL, 0, 0, x.tic));
    if (vk_tic_format(x.tic[0], 0).vk != c->vk) {
        printf("copies that name a %s texture: the GPU has no such images, skipped\n", c->name);
        return 0;
    }
    for (u64 i = 0; i < x.lay.size; i++) GP(x.cpu)[i] = (u8)(i * 37 + (i >> 9) * 3 + 11 + at / DN_SPAN);
    g_renderer->mem_may_have_changed();
    g_vk_present++;
    vk_lock();
    x.t = vk_tex_from_tic(x.tic);
    vk_unlock();
    CHECK(x.t && x.t->upload_valid && x.t->bl && x.t->levels == x.tex.d.levels && x.t->layers == 2);
    u32 first[4] = {0}, image_has = 0, memory_has = 0;
    CHECK(tp_differ(&x, x.t, first, &image_has, &memory_has) == 0);
    for (int way = 0; way < 3; way++) { /* whole levels, parts, a wrong name */
        u64 loads, bytes, loads2, bytes2;
        vk_tex_stats(&loads, &bytes);
        for (u32 layer = 0; layer < 2; layer++)
            for (u32 l = 0; l < (way == 2 ? 1u : x.tex.d.levels); l++) {
                const u32 w = c->w >> l ? c->w >> l : 1, h = c->h >> l ? c->h >> l : 1;
                const u32 across = (w + c->bw - 1) / c->bw, rows = (h + c->bw - 1) / c->bw;
                u32 part[4] = {0, 0, across, rows};
                if (way == 1) {
                    part[0] = across / 3, part[1] = rows / 4;
                    part[2] = across - part[0] > 2 ? across - part[0] - 1 : across - part[0];
                    part[3] = rows - part[1] > 2 ? rows - part[1] - 1 : rows - part[1];
                }
                CHECK(dn_copy(&x, layer, l, way == 2 ? 1 - layer : layer, part));
            }
        vk_lock();
        Surf* t = vk_tex_from_tic(x.tic);
        vk_tex_stats(&loads2, &bytes2);
        vk_unlock();
        static const char* const how[3] = {"whole levels", "parts", "a layer it does not go into named"};
        const int differ = t ? tp_differ(&x, t, first, &image_has, &memory_has) : -1;
        /* a part of a texture of bytes starts at any byte of the staging, which a copy to an image can't: loaded */
        const int loaded = way < 2 && (way == 0 || c->eb >= 4) && loads2 != loads;
        if (t != x.t || differ || loaded) {
            fprintf(
                stderr,
                "copies that name a %s texture of %ux%u, %u levels, 2 layers (%s): the same texture %d, %d texels or "
                "blocks not memory's (the first of layer %u level %u (%u, %u): image %08x, memory %08x), %llu loads "
                "again\n",
                c->name, c->w, c->h, (u32)x.tex.d.levels, how[way], t == x.t, differ, first[0], first[1], first[2],
                first[3], image_has, memory_has, (unsigned long long)(loads2 - loads));
            failed = 1;
        }
    }
    if (!failed)
        printf(
            "copies that name a %s texture of %ux%u, %u levels, 2 layers: its image is memory's, with nothing loaded "
            "again\n",
            c->name, c->w, c->h, (u32)x.tex.d.levels);
    return failed;
}
static int check_dma_named(void) {
    const u32 n = sizeof g_dn_cases / sizeof *g_dn_cases;
    int failed = 0;
    CHECK(mem_map(DN_CPU, n * DN_SPAN, MS_NORMAL, PERM_RW) == 0);
    gmmu_map(DN_GVA, DN_CPU, n * DN_SPAN);
    for (u32 i = 0; i < n; i++) failed |= dn_case(&g_dn_cases[i], i * DN_SPAN);
    return failed;
}
