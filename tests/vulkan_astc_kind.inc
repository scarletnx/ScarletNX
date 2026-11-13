/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* astc_mode's Auto by what an ASTC texture's blocks hold (texture.c's astc_by_kind, gpu/texture/astc_kind.h): a texture
   whose blocks are luminance alone is made BC4, luminance and alpha BC5, opaque colour on one line a 4x4 block BC1 (sRGB
   luminance too, as BC4 has no sRGB), straight as it loads, its RGBA8 never made; its texels, the image decoded as the
   view gives them back (BC4 as R, R, R, 1, BC5 as R, R, R, G, whatever the header's swizzle picks of them), are its
   Exact twin's within what the encoder loses on such blocks (tests/glsl/astc_bcn_glsl.cpp's floors). Loaded again with
   blocks that hold more, it takes their format; a single block written loads it whole and reads it again; blocks of
   every kind take it to what Auto holds the rest as, RGBA8 with room, its twin's texels exactly. Needs vulkan_astc_later.inc
   (al_*) */
#include "astc_bcn_cpu.h"
#include "nx/gpu/texture/astc_kind.h"
#include "support/bcn.h"

/* the memory of x's texture: blocks of kind `want` alone (snx_astc_block_kind at its footprint), from random ones, one in
   two of one partition and an LDR endpoint mode without HDR (as tests/astc_kind.c), the layout's padding too */
static void ak_fill(AlTex* x, u32 want, int srgb, u32 bw, u32 bh) {
    static const u8 cems[7] = {0, 1, 4, 5, 6, 8, 9};
    guest_done(x->cpu, x->lay.size);
    for (u64 i = 0; i < x->lay.size; i += 16) {
        u8 b[16];
        do {
            for (int k = 0; k < 16; k += 4) {
                x->rng ^= x->rng << 13, x->rng ^= x->rng >> 17, x->rng ^= x->rng << 5;
                snx_st_le32(b + k, x->rng);
            }
            if (b[15] & 1) {
                const u32 cem = cems[b[14] % 7];
                b[1] = (u8)((b[1] & 0x80) | (cem << 5) | (b[1] & 7));
                b[2] = (u8)((b[2] & 0xFE) | (cem >> 3));
            }
        } while (snx_astc_block_kind(b, bw, bh, srgb) != want);
        memcpy(GP(x->cpu) + i, b, 16);
    }
}

/* t's image sampled as its view gives it back: each level of its one layer decoded (tests/support/bcn.c, the
   specification's interpolants), against its Exact twin's texels (astc_mode 1 over the same memory). The largest
   error of R, G, B and A in *most; 1 when the images could not be read */
static int ak_against_exact(AlTex* x, Surf* t, u32 most[4]) {
    u64 size = 0, exact_size = 0;
    const int had = g_settings.astc_mode;
    g_settings.astc_mode = 1;
    Surf* twin = al_frame(x->twin);
    g_settings.astc_mode = had;
    u8* got = al_image(x, t, &size);
    u8* exact = twin && twin->f.vk == (t->f.vk == VK_FORMAT_BC1_RGBA_SRGB_BLOCK ? VK_FORMAT_R8G8B8A8_SRGB
                                                                                : VK_FORMAT_R8G8B8A8_UNORM)
                    ? al_image(x, twin, &exact_size)
                    : NULL;
    const u32 bcn = vk_astc_bcn(t->f.vk);
    const int kind = bcn == 1 ? BCN_BC1 : bcn == 4 ? BCN_BC4 : BCN_BC5;
    int bad = !got || !exact || !bcn;
    u64 at = 0, texel_at = 0;
    most[0] = most[1] = most[2] = most[3] = 0;
    for (u32 l = 0; !bad && l < x->tex.d.levels; l++) {
        u32 w, h, d;
        al_level(x, l, &w, &h, &d);
        const u32 pw = (w + 3) / 4 * 4, ph = (h + 3) / 4 * 4;
        u8* px = malloc((size_t)pw * ph * bcn_pixel_bytes(kind));
        if (!px) {
            bad = 1;
            break;
        }
        bcn_decode_image_spec(got + at, pw, ph, kind, px);
        for (u32 y = 0; y < h; y++)
            for (u32 xx = 0; xx < w; xx++) {
                const u8* p = px + ((size_t)y * pw + xx) * bcn_pixel_bytes(kind);
                const u8* e = exact + texel_at + ((size_t)y * w + xx) * 4;
                u8 s[4];
                if (kind == BCN_BC1) {
                    memcpy(s, p, 4);
                } else { /* 16 bit channels to 8, nearest; R in R, G and B, and G (BC5) or 1 in alpha */
                    const u32 r = (u32)(p[0] | p[1] << 8), g = kind == BCN_BC5 ? (u32)(p[2] | p[3] << 8) : 65535u;
                    s[0] = s[1] = s[2] = (u8)((r * 255 + 32767) / 65535);
                    s[3] = (u8)((g * 255 + 32767) / 65535);
                }
                for (int c = 0; c < 4; c++) {
                    const u32 diff = (u32)(s[c] > e[c] ? s[c] - e[c] : e[c] - s[c]);
                    if (diff > most[c]) most[c] = diff;
                }
            }
        free(px);
        at += al_level_bytes(x, l, kind == BCN_BC5 ? 16 : 8);
        texel_at += al_level_bytes(x, l, 0);
    }
    bad |= at != size || texel_at != exact_size;
    free(got);
    free(exact);
    return bad;
}

/* the components of t's view for a header of swizzle sw (four of TIC's 3-bit sources: 0 zero, 2 R, 3 G, 4 B, 5 A,
   7 one), as vk_tex_bind_view makes it: VkComponentSwizzle each, from the view's key (texture.c's tex_view) */
static u32 ak_view(Surf* t, const AlTex* x, const u8 sw[4]) {
    u32 tic[8], tsc[8] = {0};
    memcpy(tic, x->tic, sizeof tic);
    tic[0] = (tic[0] & ~(0xFFFu << 19)) | (u32)sw[0] << 19 | (u32)sw[1] << 22 | (u32)sw[2] << 25 | (u32)sw[3] << 28;
    vk_lock();
    const VkImageView v = vk_tex_bind_view(t, VK_IMAGE_VIEW_TYPE_2D, tic, tsc, 0);
    u32 got = ~0u;
    for (int i = 0; v && i < t->nsv; i++)
        if (t->sv[i].v == v) got = (u32)(t->sv[i].key >> 4 & 0xFFFF);
    vk_unlock();
    return got;
}
#define AK_MAP(r, g, b, a) ((u32)(r) | (u32)(g) << 4 | (u32)(b) << 8 | (u32)(a) << 12)

typedef struct {
    const char* name;
    u16 format; /* NVN's, 8x8 */
    u32 kind;   /* the blocks' (snx_astc_block_kind) */
    int srgb;
    VkFormat held;
    u32 most[4];         /* the largest error of each channel the encoder may make on them */
    u32 identity, other; /* the view's components for the header's swizzle R, G, B, A and A, 1, R, 0 */
} AkCase;

static int ak_case(const AkCase* k, u32 index) {
    const AlCase c = {k->name, k->format, 128, 96, 1, 0};
    static const u8 identity[4] = {2, 3, 4, 5}, other[4] = {5, 7, 2, 0};
    AlTex x;
    u32 most[4];
    if (al_place(&x, &c, index, 14)) return 1;
    ak_fill(&x, k->kind, k->srgb, 8, 8);
    vk_lock();
    Surf* t = vk_tex_from_tic(x.tic);
    vk_unlock();
    int failed = !t || t->f.vk != k->held || t->bc7_later || !t->upload_valid;
    if (failed)
        fprintf(stderr, "ASTC by its blocks' kind, %s: format %d, held as BC7 later %d, loaded %d, not format %d\n",
                k->name, t ? (int)t->f.vk : -1, t ? t->bc7_later : -1, t ? t->upload_valid : -1, (int)k->held);
    if (!failed && (ak_against_exact(&x, t, most) || most[0] > k->most[0] || most[1] > k->most[1] ||
                    most[2] > k->most[2] || most[3] > k->most[3])) {
        fprintf(
            stderr,
            "ASTC by its blocks' kind, %s: its texels as sampled differ from its Exact twin's by up to %u %u %u %u, "
            "past %u %u %u %u\n",
            k->name, most[0], most[1], most[2], most[3], k->most[0], k->most[1], k->most[2], k->most[3]);
        failed = 1;
    }
    const u32 view_id = failed ? 0 : ak_view(t, &x, identity), view_other = failed ? 0 : ak_view(t, &x, other);
    if (!failed && (view_id != k->identity || view_other != k->other)) {
        fprintf(stderr,
                "ASTC by its blocks' kind, %s: the view's components %04x for the header's R, G, B, A and %04x for "
                "its A, 1, R, 0, not %04x and %04x\n",
                k->name, view_id, view_other, k->identity, k->other);
        failed = 1;
    }
    if (!failed)
        printf("ASTC by its blocks' kind, %s: held as format %d, sampled within %u %u %u %u of its Exact twin\n",
               k->name, (int)t->f.vk, most[0], most[1], most[2], most[3]);
    al_free(&x);
    return failed;
}

/* A BC4 texture loaded again: blocks of luminance and alpha take it to BC5; one block of colour written over them
   loads it whole (a piece of another kind would show in a format that does not hold it) and takes it to RGBA8, as
   colour with alpha is held by Auto with room; its texels then its Exact twin's, exactly */
static int ak_again(u32 index) {
    static const AlCase c = {"ASTC 8x8 128x96, loaded again", NVN_FORMAT_RGBA_ASTC_8x8, 128, 96, 1, 0};
    AlTex x;
    u32 most[4];
    u64 size = 0, exact_size = 0;
    if (al_place(&x, &c, index, 14)) return 1;
    ak_fill(&x, 0, 0, 8, 8);
    vk_lock();
    Surf* t = vk_tex_from_tic(x.tic);
    vk_unlock();
    CHECK(t && t->f.vk == VK_FORMAT_BC4_UNORM_BLOCK);
    al_idle(x.tic, 13);
    ak_fill(&x, SNX_ASTC_ALPHA, 0, 8, 8);
    const int to_bc5 = al_frame(x.tic) == t && t->f.vk == VK_FORMAT_BC5_UNORM_BLOCK && t->upload_valid &&
                       !ak_against_exact(&x, t, most) && most[0] <= 17 && most[3] <= 19;
    al_idle(x.tic, 13);
    vk_lock();
    const int tracked = vk_written_since(GP(x.cpu), x.lay.size, 0) >= 0;
    vk_unlock();
    if (!tracked) {
        al_free(&x);
        printf("ASTC by its blocks' kind, loaded again: BC4 to BC5 %d; no write tracking, a block written skipped\n",
               to_bc5);
        return !to_bc5;
    }
    /* one block's bytes, at the start of level 0, now a block of colour and alpha */
    u8 b[16];
    do {
        for (int k = 0; k < 16; k += 4) {
            x.rng ^= x.rng << 13, x.rng ^= x.rng >> 17, x.rng ^= x.rng << 5;
            snx_st_le32(b + k, x.rng);
        }
    } while (snx_astc_block_kind(b, 8, 8, 0) != (SNX_ASTC_CHROMA | SNX_ASTC_ALPHA));
    guest_done(x.cpu, 16);
    memcpy(GP(x.cpu), b, 16);
    const int to_rgba8 =
        al_frame(x.tic) == t && t->f.vk == VK_FORMAT_R8G8B8A8_UNORM && !t->bc7_later && t->upload_valid;
    g_settings.astc_mode = 1;
    Surf* twin = al_frame(x.twin);
    g_settings.astc_mode = 0;
    u8* got = to_rgba8 ? al_image(&x, t, &size) : NULL;
    u8* exact = twin ? al_image(&x, twin, &exact_size) : NULL;
    const int same = got && exact && size == exact_size && !memcmp(got, exact, (size_t)size);
    free(got);
    free(exact);
    al_free(&x);
    if (!to_bc5 || !to_rgba8 || !same) {
        fprintf(
            stderr,
            "ASTC by its blocks' kind, loaded again: BC4 to BC5 with blocks of luminance and alpha %d (within "
            "%u %u %u %u), to RGBA8 for one block of colour and alpha %d (format %d), the same texels as Exact %d\n",
            to_bc5, most[0], most[1], most[2], most[3], to_rgba8, (int)t->f.vk, same);
        return 1;
    }
    printf("ASTC by its blocks' kind, loaded again: BC4 to BC5 for luminance and alpha, then one block of colour and "
           "alpha written loaded it whole as RGBA8, its Exact twin's texels\n");
    return 0;
}

static int check_astc_kind(void) {
    enum {
        R = VK_COMPONENT_SWIZZLE_R,
        G = VK_COMPONENT_SWIZZLE_G,
        B = VK_COMPONENT_SWIZZLE_B,
        A = VK_COMPONENT_SWIZZLE_A,
        ONE = VK_COMPONENT_SWIZZLE_ONE,
        ZERO = VK_COMPONENT_SWIZZLE_ZERO
    };
    /* the floors tests/glsl/astc_bcn_glsl.cpp measured on random blocks of each kind, every footprint */
    const AkCase cases[4] = {{"luminance, ASTC 8x8",
                              NVN_FORMAT_RGBA_ASTC_8x8,
                              0,
                              0,
                              VK_FORMAT_BC4_UNORM_BLOCK,
                              {17, 17, 17, 0},
                              AK_MAP(R, R, R, ONE),
                              AK_MAP(ONE, ONE, R, ZERO)},
                             {"luminance and alpha, ASTC 8x8",
                              NVN_FORMAT_RGBA_ASTC_8x8,
                              SNX_ASTC_ALPHA,
                              0,
                              VK_FORMAT_BC5_UNORM_BLOCK,
                              {17, 17, 17, 19},
                              AK_MAP(R, R, R, G),
                              AK_MAP(G, ONE, R, ZERO)},
                             {"colour on one line, ASTC 8x8",
                              NVN_FORMAT_RGBA_ASTC_8x8,
                              SNX_ASTC_CHROMA,
                              0,
                              VK_FORMAT_BC1_RGBA_UNORM_BLOCK,
                              {39, 32, 33, 0},
                              AK_MAP(R, G, B, A),
                              AK_MAP(A, ONE, R, ZERO)},
                             {"luminance, ASTC 8x8 sRGB",
                              NVN_FORMAT_RGBA_ASTC_8x8_SRGB,
                              0,
                              1,
                              VK_FORMAT_BC1_RGBA_SRGB_BLOCK,
                              {40, 43, 40, 0},
                              AK_MAP(R, G, B, A),
                              AK_MAP(A, ONE, R, ZERO)}};
    const int had = g_settings.astc_mode;
    const SnxAstcAuto auto_had = g_vk.astc_auto;
    u32 tic[8];
    NvnTexture probe = {0};
    probe.d.target = NVN_TEXTURE_TARGET_2D;
    probe.d.format = NVN_FORMAT_RGBA_ASTC_8x8;
    probe.d.levels = 1;
    probe.d.width = probe.d.height = 64;
    probe.d.depth = 1;
    probe.gpu = AL_GVA;
    g_settings.astc_mode = 0;
    g_vk.astc_auto = (SnxAstcAuto){0, 0, 0}; /* Exact with room: the rest RGBA8 */
    CHECK(nvn_tic_encode(&probe, NULL, 0, 0, tic));
    const VkFmt f = vk_tic_format(tic[0], 0);
    if (f.conv != VKC_ASTC || !g_vk.feat.textureCompressionBC) {
        printf("ASTC by its blocks' kind: the GPU %s, skipped\n",
               f.conv != VKC_ASTC ? "samples ASTC as it is" : "samples no BCn");
        g_settings.astc_mode = had;
        g_vk.astc_auto = auto_had;
        return 0;
    }
    int failed = 0;
    for (u32 i = 0; i < 4; i++) failed |= ak_case(&cases[i], 20 + i);
    failed |= ak_again(24);
    g_settings.astc_mode = had;
    g_vk.astc_auto = auto_had;
    return failed;
}
