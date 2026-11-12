/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The block texture formats a device is given (gpu/vk/format.c: vk_blocks_want, vk_tic_format, vk_blocks_say), with no
   GPU, on stand-in devices: what device.c turns on of the features each announces, the Vulkan format every ASTC,
   ETC2, EAC and BCn texture header then takes, and the log's line for each family. The first is a player's Mali-G52
   MC2 (Arm's driver r49p1, Vulkan 1.3), from its vkjson: ASTC LDR and ETC2 sampled, BCn not. Its ASTC and ETC2
   textures are sampled as the games' blocks, its BCn ones encoded again as ETC2 and EAC (BC6H, which has none,
   decoded); then a desktop GPU (BCn alone), one without ETC2 either, a device of all three with native_bc=0 (as
   llvmpipe's tests run), and each setting at 0 on the Mali */
#include "nx/gpu/vk/vk.h"
#include "nx/runtime/platform/log.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

VkState g_vk;
Settings g_settings;

static struct {
    unsigned checks, failures;
    char lines[8][768]; /* the log's lines since the last reset, "W " before a warning's */
    int n;
    /* the stand-in device's optimal tiling features of each format it lists */
    const VkFormat* formats;
    const VkFormatFeatureFlags* features;
    int nformats;
} g_test;
#define CHECK(c, ...)                                                                                                  \
    do {                                                                                                               \
        g_test.checks++;                                                                                               \
        if (!(c)) {                                                                                                    \
            g_test.failures++;                                                                                         \
            printf("FAIL line %d: ", __LINE__);                                                                        \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
        }                                                                                                              \
    } while (0)

void logf_(int lvl, const char* fmt, ...) {
    if (g_test.n >= 8) return;
    char* l = g_test.lines[g_test.n++];
    int at = snprintf(l, sizeof g_test.lines[0], "%s", lvl == 0 ? "W " : "");
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(l + at, sizeof g_test.lines[0] - (size_t)at, fmt, ap);
    va_end(ap);
    size_t n = strlen(l);
    if (n && l[n - 1] == '\n') l[n - 1] = 0;
}
void fatal(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    exit(1);
}

/* the stand-in device's answers, as the loader's would be */
static VkFormatFeatureFlags features_of(VkFormat f) {
    for (int i = 0; i < g_test.nformats; i++)
        if (g_test.formats[i] == f) return g_test.features[i];
    return 0;
}
static VKAPI_ATTR void VKAPI_CALL fake_format_properties2(VkPhysicalDevice pd, VkFormat f, VkFormatProperties2* p) {
    (void)pd;
    p->formatProperties.optimalTilingFeatures = features_of(f);
    for (VkBaseOutStructure* s = (VkBaseOutStructure*)p->pNext; s; s = s->pNext)
        if (s->sType == VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3)
            ((VkFormatProperties3*)s)->optimalTilingFeatures = features_of(f);
}
/* 2D images of the formats it samples; no 3D image of a block format (no vkjson says either way) */
static VKAPI_ATTR VkResult VKAPI_CALL fake_image_format_properties(VkPhysicalDevice pd, VkFormat f, VkImageType type,
                                                                   VkImageTiling tiling, VkImageUsageFlags usage,
                                                                   VkImageCreateFlags flags,
                                                                   VkImageFormatProperties* p) {
    (void)pd, (void)tiling, (void)usage, (void)flags;
    memset(p, 0, sizeof *p);
    int block = (f >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && f <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK);
    if (!(features_of(f) & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) || (block && type == VK_IMAGE_TYPE_3D))
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    return VK_SUCCESS;
}

/* The Mali-G52 MC2's vkjson (r49p1): textureCompressionASTC_LDR 1, textureCompressionETC2 1, textureCompressionBC 0;
   every ETC2, EAC and ASTC format (147 to 184) 0xd401 in optimal tiling (sampled, filtered linearly, blit source,
   transfer source and destination), no BCn format listed at all; RGBA8 0x6dd83, the 16 bit ones it decodes into
   0xdd83 (R16), 0xcd83 (RG16) and 0x6dd83 (RGBA16F) */
static const VkFormat mali_formats[] = {VK_FORMAT_R8G8B8A8_UNORM,
                                        VK_FORMAT_R8G8B8A8_SRGB,
                                        VK_FORMAT_R16_UNORM,
                                        VK_FORMAT_R16_SNORM,
                                        VK_FORMAT_R16G16_UNORM,
                                        VK_FORMAT_R16G16_SNORM,
                                        VK_FORMAT_R16G16B16A16_SFLOAT,
                                        VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK,
                                        VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK,
                                        VK_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK,
                                        VK_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK,
                                        VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK,
                                        VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK,
                                        VK_FORMAT_EAC_R11_UNORM_BLOCK,
                                        VK_FORMAT_EAC_R11_SNORM_BLOCK,
                                        VK_FORMAT_EAC_R11G11_UNORM_BLOCK,
                                        VK_FORMAT_EAC_R11G11_SNORM_BLOCK,
                                        VK_FORMAT_ASTC_4x4_UNORM_BLOCK,
                                        VK_FORMAT_ASTC_8x8_UNORM_BLOCK,
                                        VK_FORMAT_ASTC_12x12_SRGB_BLOCK};
static const VkFormatFeatureFlags mali_features[] = {0x6dd83, 0x6dd83, 0xdd83, 0xdd83, 0xcd83, 0xcd83, 0x6dd83,
                                                     0xd401,  0xd401,  0xd401, 0xd401, 0xd401, 0xd401, 0xd401,
                                                     0xd401,  0xd401,  0xd401, 0xd401, 0xd401, 0xd401};

/* a stand-in device: its compression features and the settings' native_* as the run has them, then device.c's choice
   of features (vk_blocks_want) as g_vk.feat, and the log's lines from vk_blocks_say */
static void device(int astc, int etc2, int bc, int native_astc, int native_etc2, int native_bc) {
    VkPhysicalDeviceFeatures have = {.textureCompressionASTC_LDR = (VkBool32)astc,
                                     .textureCompressionETC2 = (VkBool32)etc2,
                                     .textureCompressionBC = (VkBool32)bc};
    memset(&g_vk.feat, 0, sizeof g_vk.feat);
    g_settings.native_astc = native_astc;
    g_settings.native_etc2 = native_etc2;
    g_settings.native_bc = native_bc;
    g_settings.astc_mode = 1; /* Exact: ASTC decoded stays RGBA8, which needs no memory budget here */
    g_settings.bcn_astc = 0;  /* BCn as ETC2: at its default of 1 the BCn checks below would take ASTC 4x4 */
    g_vk.astc_auto_bc7 = 0;
    vk_blocks_want(&have, &g_vk.feat);
    g_test.n = 0;
    vk_blocks_say();
}

/* the Vulkan format and conversion a texture header word 0 (unorm, or snorm with sn) takes */
static void takes(u32 fmt, int sn, int srgb, VkFormat want, int conv, const char* what) {
    u32 w0 = fmt | (sn ? 1u : 2u) << 7; /* the first component's type: 1 snorm, 2 unorm */
    VkFmt f = vk_tic_format(w0, srgb);
    CHECK(f.vk == want && f.conv == conv, "%s: Vulkan format %d conversion %d, not %d and %d", what, (int)f.vk, f.conv,
          (int)want, conv);
}
static void line(int i, const char* want) {
    CHECK(i < g_test.n && !strcmp(g_test.lines[i], want), "log line %d:\n  %s\nnot\n  %s", i,
          i < g_test.n ? g_test.lines[i] : "(none)", want);
}

int main(void) {
    vkGetPhysicalDeviceFormatProperties2 = fake_format_properties2;
    vkGetPhysicalDeviceImageFormatProperties = fake_image_format_properties;

    /* the Mali-G52 MC2: ASTC and ETC2 sampled as the games' blocks, BCn encoded again as ETC2 and EAC */
    g_test.formats = mali_formats;
    g_test.features = mali_features;
    g_test.nformats = (int)(sizeof mali_formats / sizeof mali_formats[0]);
    device(1, 1, 0, 1, 1, 1);
    CHECK(g_vk.feat.textureCompressionASTC_LDR && g_vk.feat.textureCompressionETC2 && !g_vk.feat.textureCompressionBC,
          "Mali: features turned on ASTC %u ETC2 %u BC %u, not 1 1 0", g_vk.feat.textureCompressionASTC_LDR,
          g_vk.feat.textureCompressionETC2, g_vk.feat.textureCompressionBC);
    takes(0x40, 0, 0, VK_FORMAT_ASTC_4x4_UNORM_BLOCK, VKC_NONE, "Mali ASTC 4x4");
    takes(0x44, 0, 0, VK_FORMAT_ASTC_8x8_UNORM_BLOCK, VKC_NONE, "Mali ASTC 8x8");
    takes(0x46, 0, 1, VK_FORMAT_ASTC_12x12_SRGB_BLOCK, VKC_NONE, "Mali ASTC 12x12 sRGB");
    takes(0x0B, 0, 0, VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK, VKC_NONE, "Mali ETC2 RGBA8");
    takes(0x1A, 1, 0, VK_FORMAT_EAC_R11G11_SNORM_BLOCK, VKC_NONE, "Mali EAC RG11 snorm");
    takes(0x24, 0, 0, VK_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK, VKC_BC1, "Mali BC1");
    takes(0x24, 0, 1, VK_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK, VKC_BC1, "Mali BC1 sRGB");
    takes(0x25, 0, 0, VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK, VKC_BC2, "Mali BC2");
    takes(0x26, 0, 1, VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK, VKC_BC3, "Mali BC3 sRGB");
    takes(0x27, 0, 0, VK_FORMAT_EAC_R11_UNORM_BLOCK, VKC_BC4, "Mali BC4");
    takes(0x27, 1, 0, VK_FORMAT_EAC_R11_SNORM_BLOCK, VKC_BC4_SNORM, "Mali BC4 snorm");
    takes(0x28, 0, 0, VK_FORMAT_EAC_R11G11_UNORM_BLOCK, VKC_BC5, "Mali BC5");
    takes(0x28, 1, 0, VK_FORMAT_EAC_R11G11_SNORM_BLOCK, VKC_BC5_SNORM, "Mali BC5 snorm");
    takes(0x17, 0, 1, VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK, VKC_BC7, "Mali BC7 sRGB");
    takes(0x11, 0, 0, VK_FORMAT_R16G16B16A16_SFLOAT, VKC_BC6H_UF, "Mali BC6H");
    /* a 3D BCn texture, which no device here takes as ETC2, is decoded */
    VkFmt v = vk_tic_format_3d(0x24 | 2u << 7, 0);
    CHECK(v.vk == VK_FORMAT_R8G8B8A8_UNORM && v.conv == VKC_BC1, "Mali 3D BC1: Vulkan format %d conversion %d",
          (int)v.vk, v.conv);
    line(0, "vulkan: ASTC textures (native_astc=1, the GPU samples ASTC, astc_mode=1): sampled as the game's blocks: "
            "ASTC");
    line(1, "vulkan: ETC2 and EAC textures (native_etc2=1, the GPU samples ETC2): sampled as the game's blocks: ETC2 "
            "RGB, ETC2 RGB8A1, ETC2 RGBA8, EAC R11, EAC RG11");
    line(2, "W vulkan: BCn textures (native_bc=1, the GPU samples no BCn, ETC2 sampled): encoded again on the GPU: BC1 "
            "as ETC2 RGB8A1, BC2 as ETC2 RGBA8, BC3 as ETC2 RGBA8, BC4 as EAC R11, BC5 as EAC RG11, BC7 as ETC2 RGBA8; "
            "decoded on the GPU: BC6H to RGBA16F");
    CHECK(g_test.n == 3, "Mali: %d log lines, not 3", g_test.n);

    /* native_etc2=0 on it: ETC2 isn't turned on, so ETC2 and BCn are decoded */
    device(1, 1, 0, 1, 0, 1);
    takes(0x24, 0, 0, VK_FORMAT_R8G8B8A8_UNORM, VKC_BC1, "Mali native_etc2=0 BC1");
    takes(0x27, 0, 0, VK_FORMAT_R16_UNORM, VKC_BC4, "Mali native_etc2=0 BC4");
    takes(0x0B, 0, 0, VK_FORMAT_R8G8B8A8_UNORM, VKC_ETC2_RGBA, "Mali native_etc2=0 ETC2 RGBA8");
    line(1, "vulkan: ETC2 and EAC textures (native_etc2=0, the GPU samples ETC2): decoded on the GPU: ETC2 RGB to "
            "RGBA8, ETC2 RGB8A1 to RGBA8, ETC2 RGBA8 to RGBA8, EAC R11 to R16, EAC RG11 to RG16");
    line(2, "W vulkan: BCn textures (native_bc=1, the GPU samples no BCn, ETC2 not sampled): decoded on the GPU: BC1 "
            "to RGBA8, BC2 to RGBA8, BC3 to RGBA8, BC4 to R16, BC5 to RG16, BC6H to RGBA16F, BC7 to RGBA8");
    /* native_astc=0: ASTC decoded to RGBA8, no warning (it is what was asked) */
    device(1, 1, 0, 0, 1, 1);
    takes(0x40, 0, 0, VK_FORMAT_R8G8B8A8_UNORM, VKC_ASTC, "Mali native_astc=0 ASTC 4x4");
    line(0, "vulkan: ASTC textures (native_astc=0, the GPU samples ASTC, astc_mode=1): decoded on the GPU: ASTC to "
            "RGBA8");

    /* a desktop GPU, BCn alone: BCn as it is, ASTC and ETC2 decoded, a warning each as the settings ask for them */
    device(0, 0, 1, 1, 1, 1);
    takes(0x24, 0, 0, VK_FORMAT_BC1_RGBA_UNORM_BLOCK, VKC_NONE, "desktop BC1");
    takes(0x40, 0, 0, VK_FORMAT_R8G8B8A8_UNORM, VKC_ASTC, "desktop ASTC 4x4, astc_mode=1");
    line(0,
         "W vulkan: ASTC textures (native_astc=1, the GPU samples no ASTC, astc_mode=1): decoded on the GPU: ASTC to "
         "RGBA8");
    line(1, "W vulkan: ETC2 and EAC textures (native_etc2=1, the GPU samples no ETC2): decoded on the GPU: ETC2 RGB to "
            "RGBA8, ETC2 RGB8A1 to RGBA8, ETC2 RGBA8 to RGBA8, EAC R11 to R16, EAC RG11 to RG16");
    line(2, "vulkan: BCn textures (native_bc=1, the GPU samples BCn, ETC2 not sampled): sampled as the game's blocks: "
            "BC1, BC2, BC3, BC4, BC5, BC6H, BC7");
    /* astc_mode=2 there: ASTC decoded, then encoded again as BC7 */
    g_settings.astc_mode = 2;
    g_test.n = 0;
    vk_blocks_say();
    line(0, "W vulkan: ASTC textures (native_astc=1, the GPU samples no ASTC, astc_mode=2): encoded again on the GPU: "
            "ASTC as BC7");

    /* a device of all three with native_bc=0 (llvmpipe's tests): BCn takes the ETC2 path a phone's GPU does */
    device(1, 1, 1, 1, 1, 0);
    takes(0x26, 0, 0, VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK, VKC_BC3, "native_bc=0 BC3");
    line(2, "vulkan: BCn textures (native_bc=0, the GPU samples BCn, ETC2 sampled): encoded again on the GPU: BC1 as "
            "ETC2 RGB8A1, BC2 as ETC2 RGBA8, BC3 as ETC2 RGBA8, BC4 as EAC R11, BC5 as EAC RG11, BC7 as ETC2 RGBA8; "
            "decoded on the GPU: BC6H to RGBA16F");

    /* bcn_astc=1 on a phone's GPU (ASTC and ETC2 sampled, BCn not): BC2, BC3 and BC7 encoded again as ASTC 4x4, which
       holds them closer than ETC2 RGBA8, and BC1, BC4 and BC5 as ETC2 and EAC as before */
    device(1, 1, 0, 1, 1, 1);
    g_settings.bcn_astc = 1;
    takes(0x17, 0, 0, VK_FORMAT_ASTC_4x4_UNORM_BLOCK, VKC_BC7, "bcn_astc=1 BC7");
    takes(0x17, 0, 1, VK_FORMAT_ASTC_4x4_SRGB_BLOCK, VKC_BC7, "bcn_astc=1 BC7 sRGB");
    takes(0x26, 0, 0, VK_FORMAT_ASTC_4x4_UNORM_BLOCK, VKC_BC3, "bcn_astc=1 BC3");
    takes(0x24, 0, 0, VK_FORMAT_ASTC_4x4_UNORM_BLOCK, VKC_BC1, "bcn_astc=1 BC1, memory to spare");
    takes(0x27, 0, 0, VK_FORMAT_EAC_R11_UNORM_BLOCK, VKC_BC4, "bcn_astc=1 BC4");
    takes(0x40, 0, 0, VK_FORMAT_ASTC_4x4_UNORM_BLOCK, VKC_NONE, "bcn_astc=1 ASTC 4x4, the game's own blocks");
    g_test.n = 0;
    vk_blocks_say();
    line(2, "W vulkan: BCn textures (native_bc=1, the GPU samples no BCn, ETC2 sampled): encoded again on the GPU: BC1 "
            "as ASTC 4x4, BC2 as ASTC 4x4, BC3 as ASTC 4x4, BC4 as EAC R11, BC5 as EAC RG11, BC7 as ASTC 4x4; "
            "decoded on the GPU: BC6H to RGBA16F");
    /* the images run short of memory: BC1, which ASTC 4x4 holds in twice its bytes, is ETC2 RGB8A1 from then on, said
       once; the others, the same bytes either way, stay ASTC. bcn_astc=2 keeps BC1 ASTC */
    g_test.n = 0;
    CHECK(vk_bc1_tight("an image found no memory") == 1 && vk_bc1_tight("again") == 0, "memory short: not said once");
    line(0, "vulkan: an image found no memory: BC1 textures loaded from now on are encoded again as ETC2 RGB8A1, half "
            "ASTC 4x4's bytes and slower to load (bcn_astc=1, Auto; 2 keeps them ASTC)");
    takes(0x24, 0, 0, VK_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK, VKC_BC1, "bcn_astc=1 BC1, memory short");
    takes(0x17, 0, 0, VK_FORMAT_ASTC_4x4_UNORM_BLOCK, VKC_BC7, "bcn_astc=1 BC7, memory short");
    g_settings.bcn_astc = 2;
    takes(0x24, 0, 0, VK_FORMAT_ASTC_4x4_UNORM_BLOCK, VKC_BC1, "bcn_astc=2 BC1, memory short");
    g_settings.bcn_astc = 0;
    g_vk.bc1_tight = 0;

    /* a GPU of none of them (a software rasterizer without them): everything decoded */
    device(0, 0, 0, 1, 1, 1);
    takes(0x17, 0, 0, VK_FORMAT_R8G8B8A8_UNORM, VKC_BC7, "none BC7");
    takes(0x0A, 0, 0, VK_FORMAT_R8G8B8A8_UNORM, VKC_ETC2_PTA, "none ETC2 RGB8A1");

    if (g_test.failures) {
        printf("%u of %u checks failed\n", g_test.failures, g_test.checks);
        return 1;
    }
    printf("block formats: %u checks: the Mali-G52 samples ASTC and ETC2 as the games' blocks and takes BCn as ETC2 "
           "and EAC, the desktop, native_bc=0 and settings at 0 as they should, a log line a family\n",
           g_test.checks);
    return 0;
}
