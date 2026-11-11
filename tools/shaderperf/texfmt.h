/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* snx-shaderperf's texel formats (texfmt.c): a table of the Vulkan formats the tool can make an image or a buffer of,
   by name, the PPM/PGM/PFM images it reads and writes, and the conversions between such an image and the plain,
   packed and depth formats of the table. Block formats (BCn, ETC2 and EAC, ASTC LDR) are in the table for raw
   uploads only: the tool takes their bytes as they are, and has no encoder or decoder for them. texfmt.c uses only
   the C99 library and vulkan_core.h */
#ifndef SNX_SHADERPERF_TEXFMT_H
#define SNX_SHADERPERF_TEXFMT_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan_core.h>

typedef enum {
    SP_KIND_PLAIN,  /* one texel a "block", bytes bytes */
    SP_KIND_PACKED, /* one texel in a packed word (R5G6B5, A2B10G10R10, B10G11R11, E5B9G9R9...) */
    SP_KIND_DEPTH,  /* depth and stencil formats */
    SP_KIND_BC,     /* BC1 to BC7 */
    SP_KIND_ETC2,   /* ETC2 and EAC */
    SP_KIND_ASTC,   /* ASTC LDR, every 2D block size */
} SpKind;

typedef enum { SP_UNORM, SP_SNORM, SP_UINT, SP_SINT, SP_UFLOAT, SP_SFLOAT, SP_SRGB, SP_USCALED, SP_SSCALED } SpNumeric;

typedef struct {
    const char* name; /* short lower-case name: "rgba8_unorm", "bc7_srgb", "astc_6x6_unorm", "d32_sfloat" */
    VkFormat vk;
    uint8_t kind;      /* SpKind */
    uint8_t numeric;   /* SpNumeric of the color channels */
    uint8_t bw, bh;    /* block footprint, 1x1 for plain and packed formats */
    uint8_t bytes;     /* bytes a block (a texel for plain and packed formats) */
    uint8_t channels;  /* components a texel has, 1 to 4 */
} SpFormat;

/* the table, count entries, in a fixed order */
const SpFormat* sp_formats(size_t* count);
/* a format by name: the table's short name, or Vulkan's name with or without "VK_FORMAT_" (any case), or the aliases
   rgba8, bgra8, rgba16f, rgba32f, r8, rg8, r32f, rg32f, r16f, rg16f, r11g11b10f, rgb10a2, srgba8. NULL for none */
const SpFormat* sp_format_find(const char* name);
/* a format by VkFormat, NULL when the table lacks it */
const SpFormat* sp_format_vk(VkFormat vk);
/* the bytes of a w x h x d level of fmt, whole blocks */
size_t sp_format_level_bytes(const SpFormat* fmt, uint32_t w, uint32_t h, uint32_t d);

/* An image of linear float RGBA texels, w x h, rows top to bottom. A PPM's or PGM's values are scaled to 0..1 (a PGM
   gives r = g = b and a = 1). A PFM's are taken as they are. No color-space conversion happens on load */
typedef struct {
    uint32_t w, h;
    float* rgba; /* malloc'd, w * h * 4 */
} SpImage;
/* reads P2, P3, P5, P6 (maxval up to 65535) and PFM (PF, Pf): false with a message in err on an error */
bool sp_image_load(const char* path, SpImage* out, char* err, size_t err_size);
/* writes P6 (8 bits, values clamped to 0..1), or PFM (PF, color) when the name ends in ".pfm" */
bool sp_image_save(const char* path, const SpImage* img, char* err, size_t err_size);
void sp_image_free(SpImage* img);
/* a w x h copy of img resized with a box filter when smaller (each level of a mip chain) or bilinear when larger */
bool sp_image_resize(const SpImage* img, uint32_t w, uint32_t h, SpImage* out);

/* Encodes img into fmt, sp_format_level_bytes(fmt, img->w, img->h, 1) bytes at out, for the plain, packed and depth
   formats only. SRGB formats encode the linear values (r, g, b) to sRGB first. UNORM and SNORM scale 0..1 (-1..1),
   rounded to nearest. UINT, SINT and the SCALED formats take the value itself, rounded and clamped to their range,
   not scaled: integers come from a PFM or a pattern. Half and small floats round to nearest even; B10G11R11 and
   E5B9G9R9 follow the Vulkan specification. Depth formats take r, stencil takes g. D24_UNORM_S8_UINT is the packed
   word (depth in bits 0-23, stencil in 24-31), which is also what a copy of its depth aspect reads; D32_SFLOAT_S8_UINT
   is the 4-byte layout of a copy of its depth aspect, the depth float only. False for a block format */
bool sp_encode(const SpFormat* fmt, const SpImage* img, uint8_t* out);
/* Decodes w x h of fmt at src back to img (allocates it), the inverse of sp_encode: every plain, packed and depth
   format (depth to r, stencil to g, missing channels 0 and alpha 1). Used for reading back an output image. False for
   a block format: the tool can't read back those or fall back for a device that lacks them */
bool sp_decode(const SpFormat* fmt, const uint8_t* src, uint32_t w, uint32_t h, SpImage* img);

/* Fills a w x h image with a generated pattern, so that a run needs no file: "noise" (uniform 0..1, seed), "gradient"
   (r = x, g = y, b = 1 - x, a = 1), "checker" (8x8 cells), "solid:R,G,B,A", "zero". False on an unknown name */
bool sp_image_pattern(const char* pattern, uint32_t w, uint32_t h, uint64_t seed, SpImage* out);
#endif
