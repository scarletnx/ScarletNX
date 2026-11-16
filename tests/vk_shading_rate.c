/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* An Ounce's shading rate image (gpu/vk/shading_rate_table.c, shading_rate.glsl, nvn_obj.h): which extension each
   stand-in device takes and its texel size, the value each palette entry gets, the conversion's push constants,
   the palette as the NVN record packs it, and the conversion's body run on the CPU (tests/glsl/shading_rate_glsl.cpp)
   against a reference made by pixels (tests/support/shading_rate_ref.c). No GPU needed */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nx/gpu/nvn/nvn_obj.h"
#include "nx/gpu/vk/shading_rate.h"
#include "support/shading_rate_ref.h"

void shading_rate_glsl_run(const uint32_t push[12], const uint8_t* game, uint8_t* out);

static unsigned g_checks, g_failures;
#define CHECK(c, what)                                                                                                 \
    do {                                                                                                               \
        g_checks++;                                                                                                    \
        if (!(c)) {                                                                                                    \
            g_failures++;                                                                                              \
            printf("FAIL line %d: %s\n", __LINE__, what);                                                              \
        }                                                                                                              \
    } while (0)

/* an RTX 4090 on Windows: both extensions, NVIDIA's with 16x16 texels and palettes of 16 entries */
static VkVrsDevice nvidia(void) {
    return (VkVrsDevice){.ounce = 1,
                         .nv = 1,
                         .nv_texel = {16, 16},
                         .nv_palette = 16,
                         .khr = 1,
                         .khr_min = {16, 16},
                         .khr_max = {16, 16},
                         .khr_aspect = 1};
}
/* an AMD integrated GPU: the KHR attachment only, at 8x8 */
static VkVrsDevice amd(void) {
    return (VkVrsDevice){.ounce = 1, .khr = 1, .khr_min = {8, 8}, .khr_max = {8, 8}, .khr_aspect = 1};
}

static void check_choose(void) {
    char line[300];
    VkExtent2D t;
    VkVrsDevice d = nvidia();
    CHECK(vk_vrs_choose(&d, &t, line, sizeof line) == VK_VRS_NV && t.width == 16 && t.height == 16,
          "a device with both extensions takes NVIDIA's");
    d.ounce = 0;
    CHECK(vk_vrs_choose(&d, &t, line, sizeof line) == VK_VRS_NONE && !t.width,
          "a game that is no Ounce program keeps its pipelines without either extension");
    d.ounce = 1;
    d.nv_palette = 12;
    CHECK(vk_vrs_choose(&d, &t, line, sizeof line) == VK_VRS_KHR && t.width == 16,
          "palettes shorter than the game's 16 entries take the KHR attachment");
    d.khr = 0;
    CHECK(vk_vrs_choose(&d, &t, line, sizeof line) == VK_VRS_NONE && !t.width,
          "palettes shorter than 16 entries and no KHR attachment: the full rate");
    d = amd();
    CHECK(vk_vrs_choose(&d, &t, line, sizeof line) == VK_VRS_KHR && t.width == 8 && t.height == 8,
          "an 8x8 attachment where the device takes only that");
    d.khr_min = (VkExtent2D){32, 32}, d.khr_max = (VkExtent2D){32, 32};
    CHECK(vk_vrs_choose(&d, &t, line, sizeof line) == VK_VRS_KHR && t.width == 32,
          "a 32x32 attachment where the device takes only that");
    d.khr_min = (VkExtent2D){8, 8}, d.khr_max = (VkExtent2D){64, 64};
    CHECK(vk_vrs_choose(&d, &t, line, sizeof line) == VK_VRS_KHR && t.width == 16 && t.height == 16,
          "the console's 16x16 where the range has it");
    d.khr_min = (VkExtent2D){12, 12}, d.khr_max = (VkExtent2D){12, 12};
    CHECK(vk_vrs_choose(&d, &t, line, sizeof line) == VK_VRS_NONE, "a texel size that is no power of two is refused");
    d = (VkVrsDevice){.ounce = 1};
    CHECK(vk_vrs_choose(&d, &t, line, sizeof line) == VK_VRS_NONE && strstr(line, "full rate"),
          "no extension: the full rate, and the line says so");
}

static void check_table(void) {
    /* VkShadingRatePaletteEntryNV's numbers, and the KHR encoding of each (log2 width << 2 | log2 height) */
    static const u32 khr[16] = {10, 0, 0, 0, 0, 0, 4, 1, 5, 9, 6, 10, 0, 0, 0, 0};
    char what[80];
    for (u32 e = 0; e < 16; e++) {
        snprintf(what, sizeof what, "palette entry %u as the KHR attachment encodes it", e);
        CHECK(vk_vrs_khr_rate(e) == khr[e], what);
    }
    VkVrsPush p;
    vk_vrs_push(VK_VRS_NV, (VkExtent2D){16, 16}, 160, 90, 16, 0xb985555555555555ull, &p);
    CHECK(p.size_x == 160 && p.size_y == 90, "16x16 texels over the game's 160x90 of 16x16 pixels");
    CHECK(p.table[0] == 0x03020100u && p.table[3] == 0x0f0e0d0cu, "VK_NV_shading_rate_image keeps each index");
    /* the race's palette of viewport 0: indices 13 to 15 are 2x2, 4x2 and 4x4, the others 1x1 */
    vk_vrs_push(VK_VRS_KHR, (VkExtent2D){8, 8}, 160, 90, 16, 0xb985555555555555ull, &p);
    CHECK(p.size_x == 320 && p.size_y == 180, "8x8 texels over the same pixels");
    CHECK(p.table[0] == 0 && p.table[1] == 0 && p.table[2] == 0, "indices 0 to 11 of the race's palette are 1x1");
    CHECK(p.table[3] == (0u | 5u << 8 | 9u << 16 | 10u << 24), "indices 12 to 15: 1x1, 2x2, 4x2 and 4x4");
    vk_vrs_push(VK_VRS_KHR, (VkExtent2D){32, 32}, 161, 91, 16, 0, &p);
    CHECK(p.size_x == 81 && p.size_y == 46, "32x32 texels round up over the game's last half texel");
    vk_vrs_push(VK_VRS_KHR, (VkExtent2D){16, 16}, 0, 0, 16, 0, &p);
    CHECK(p.size_x == 1 && p.size_y == 1, "an empty image still takes one texel");
}

static void check_palette(void) {
    const u32 rates[20] = {8, 8, 0x1b, 6, 7, 9, 10, 11, 0, 1, 2, 3, 4, 5, 8, 11, 8, 8, 8, 8};
    CHECK(nvn_rate_palette(NULL, 16) == NVN_RATE_PALETTE_FULL, "no rates: every entry 1x1, as the driver writes");
    CHECK(nvn_rate_palette(rates, 0) == NVN_RATE_PALETTE_FULL, "no entries: every entry 1x1");
    CHECK(nvn_rate_palette(rates, 1) == 0x5555555555555558ull, "one entry, the others 1x1");
    CHECK(nvn_rate_palette(rates, 3) == 0x5555555555555b88ull, "an entry's low 4 bits only");
    CHECK(nvn_rate_palette(rates, 16) == 0xb8543210ba976b88ull, "16 entries, entry i in bits 4i to 4i+3");
    CHECK(nvn_rate_palette(rates, 20) == nvn_rate_palette(rates, 16), "past 16 entries, the first 16");
}

static u32 g_rng = 0x2545f491u;
static u32 next(void) {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}
/* the body on the CPU against the reference, for game images of each size and device texels of 8, 16 and 32, some
   of the device's images larger than the game's (an image grown by an earlier, larger one) */
static void check_glsl(void) {
    static const u32 texels[3] = {8, 16, 32};
    unsigned cases = 0, wrong = 0;
    for (u32 round = 0; round < 60; round++) {
        u32 gw = 1 + next() % 40, gh = 1 + next() % 30, t = texels[round % 3];
        u64 palette = (u64)next() << 32 | next();
        int vrs = round & 1 ? VK_VRS_KHR : VK_VRS_NV;
        VkVrsPush p;
        vk_vrs_push(vrs, (VkExtent2D){t, t}, gw, gh, 16, palette, &p);
        if (round % 4 == 3) p.size_x += 3, p.size_y += 2; /* the renderer's image past the game's */
        u8* game = malloc((size_t)gw * gh);
        u8* gpu = malloc((size_t)p.size_x * p.size_y);
        u8* ref = malloc((size_t)p.size_x * p.size_y);
        if (!game || !gpu || !ref) abort();
        for (u32 i = 0; i < gw * gh; i++) game[i] = (u8)next(); /* the high bits too, which the body drops */
        u8 value[16];
        for (u32 i = 0; i < 16; i++) value[i] = (u8)(p.table[i / 4] >> (8 * (i % 4)));
        u32 push[12];
        memcpy(push, &p, sizeof push);
        shading_rate_glsl_run(push, game, gpu);
        shading_rate_ref(game, gw, gh, 16, t, t, p.size_x, p.size_y, value, (int)p.finest, ref);
        cases++;
        if (memcmp(gpu, ref, (size_t)p.size_x * p.size_y)) {
            wrong++;
            printf("FAIL: the body's %ux%u image for a %ux%u game image at %ux%u texels differs from the reference\n",
                   p.size_x, p.size_y, gw, gh, t, t);
        }
        free(game);
        free(gpu);
        free(ref);
    }
    g_checks += cases;
    g_failures += wrong;
    /* a KHR texel of 32x32 over 2x2 of the game's texels, with the race's palette: indices 15 (4x4) and 13 (2x2)
       give 2x2, the finer, and one index 0 (1x1) among them gives 1x1. NVIDIA's takes the center's index */
    VkVrsPush p;
    u8 out[1];
    u32 push[12];
    const u8 coarse[4] = {15, 15, 13, 15}, one[4] = {15, 0, 15, 15};
    vk_vrs_push(VK_VRS_KHR, (VkExtent2D){32, 32}, 2, 2, 16, 0xb985555555555555ull, &p);
    memcpy(push, &p, sizeof push);
    shading_rate_glsl_run(push, coarse, out);
    CHECK(p.size_x == 1 && out[0] == 5, "the finest of 4x4 and 2x2 under a texel is 2x2");
    shading_rate_glsl_run(push, one, out);
    CHECK(out[0] == 0, "a 1x1 texel under a coarse one makes it 1x1");
    vk_vrs_push(VK_VRS_NV, (VkExtent2D){32, 32}, 2, 2, 16, 0, &p);
    memcpy(push, &p, sizeof push);
    shading_rate_glsl_run(push, one, out);
    CHECK(out[0] == 15, "VK_NV_shading_rate_image's index under the center pixel, (16, 16): texel (1, 1)");
}

/* the texel's pixels at each resolution scale, and over a scaled image with no copy of the guest's size; and what a
   palette or image change needs on each path */
static void check_scale_and_reuse(void) {
    CHECK(vk_vrs_game_texel(16, 1.0f, 160, 160) == 16, "16 pixels at scale 1");
    CHECK(vk_vrs_game_texel(16, 2.0f, 160, 160) == 32, "32 pixels at scale 2, the guest's size");
    CHECK(vk_vrs_game_texel(16, 1.5f, 160, 160) == 24, "24 pixels at scale 1.5");
    CHECK(vk_vrs_game_texel(16, 2.0f, 160, 320) == 16, "16 pixels over a scaled image's texels at scale 2");
    CHECK(vk_vrs_game_texel(16, 0.5f, 160, 160) == 8, "8 pixels at scale 0.5");
    CHECK(vk_vrs_game_texel(16, 1.0f, 160, 0) == 1 && vk_vrs_game_texel(0, 1.0f, 160, 160) == 1, "1 at least");
    const u64 a = 0xb985555555555555ull, b = 0xbbbbbbbbbbbb8885ull;
    for (int vrs = VK_VRS_NV; vrs <= VK_VRS_KHR; vrs++) {
        CHECK(vk_vrs_reuse(vrs, 1, 1, a, a) == VK_VRS_CONVERT, "a new image is converted");
        CHECK(vk_vrs_reuse(vrs, 0, 0, a, a) == VK_VRS_CONVERT, "a palette after a failed conversion converts again");
    }
    CHECK(vk_vrs_reuse(VK_VRS_NV, 0, 1, a, b) == VK_VRS_NEW_ID, "NVIDIA's: a new palette is a new id, no conversion");
    CHECK(vk_vrs_reuse(VK_VRS_NV, 0, 1, a, a) == VK_VRS_NEW_ID, "NVIDIA's: another viewport's palette, a new id");
    CHECK(vk_vrs_reuse(VK_VRS_KHR, 0, 1, a, a) == VK_VRS_SAME_ID, "KHR: viewport 0's palette unchanged, nothing");
    CHECK(vk_vrs_reuse(VK_VRS_KHR, 0, 1, a, b) == VK_VRS_CONVERT, "KHR: viewport 0's palette changed, converted");
}

int main(void) {
    _Static_assert(sizeof(VkVrsPush) == 12 * sizeof(u32), "VkVrsPush is shading_rate.comp's 12 words");
    check_choose();
    check_table();
    check_palette();
    check_scale_and_reuse();
    check_glsl();
    printf("vk_shading_rate: %u checks, %u failures\n", g_checks, g_failures);
    return g_failures != 0;
}
