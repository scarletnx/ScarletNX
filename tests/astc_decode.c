/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "support/astc.h"
#include <stdio.h>
#include <string.h>

static int check_mode(const u8* block, const u8 expected[4], int srgb) {
    u8 pixels[4 * 4 * 4];
    astc_decode_image(block, 4, 4, 4, 4, pixels, srgb);
    for (int i = 0; i < 16; i++)
        if (memcmp(pixels + i * 4, expected, 4)) return 0;
    return 1;
}

static int check(const u8* block, const u8 expected[4]) { return check_mode(block, expected, 0); }

int main(void) {
    /* LDR void extent: mode bits 0..8 = 0x1fc, bit 9 = 0, reserved bits 10..11 = 11, extent coordinates (bits
       12..63) all ones. RGBA16 endpoints occupy the last eight bytes. The zero alpha masks the solid color. */
    u8 block[16] = {0xfc, 0xfd, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x80, 0xff, 0xff, 0x00, 0x00};
    const u8 transparent[4] = {0, 128, 255, 0};
    const u8 invalid[4] = {255, 0, 255, 255}; /* the LDR profile's error color, opaque magenta */
    if (!check(block, transparent)) {
        fputs("ASTC LDR void extent did not preserve transparent color\n", stderr);
        return 1;
    }
    block[1] &= ~(1u << 2); /* reserved bit 10 must be one */
    if (!check(block, invalid)) {
        fputs("ASTC void extent with reserved bit 10 clear was accepted\n", stderr);
        return 1;
    }
    block[1] |= 1u << 2;
    block[1] &= ~(1u << 3); /* reserved bit 11 must be one */
    if (!check(block, invalid)) {
        fputs("ASTC void extent with reserved bit 11 clear was accepted\n", stderr);
        return 1;
    }
    block[1] |= 1u << 3;
    block[1] |= 1u << 1; /* bit 9 selects unsupported HDR endpoints */
    if (!check(block, invalid)) {
        fputs("ASTC HDR void extent was accepted as LDR\n", stderr);
        return 1;
    }
    block[1] &= ~(1u << 1);
    block[4] = 0x00; /* clears the S extent maximum's high bits: the minimum is no longer below it */
    if (!check(block, invalid)) {
        fputs("ASTC void extent with an empty extent was accepted\n", stderr);
        return 1;
    }
    /* the sRGB decode mode takes the top 8 bits of a void extent's R, G and B, where UNORM rounds to nearest.
       Alpha rounds in both: RGBA 0x12c0, 0x34f0, 0x56f0, 0x10f0 round to 19, 53, 87, 17 */
    {
        static const u8 solid[16] = {0xfc, 0xfd, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                                     0xc0, 0x12, 0xf0, 0x34, 0xf0, 0x56, 0xf0, 0x10};
        static const u8 unorm[4] = {19, 53, 87, 17}, srgb[4] = {18, 52, 86, 17};
        if (!check_mode(solid, unorm, 0) || !check_mode(solid, srgb, 1)) {
            fputs("ASTC void extent color is wrong in the UNORM or sRGB decode mode\n", stderr);
            return 1;
        }
    }
    /* Amberlith's astc cases "error" (an illegal block) and "hdrfirst" (two partitions, the first with an HDR
       endpoint mode, which is the error color on its own texels in the LDR profile). The texels are those of
       Amberlith's reference decoder (source/astcref.c), as R, G, B, A */
    {
        static const u8 illegal[16] = {0x5f, 0x0f, 0x35, 0xcb, 0xe9, 0x15, 0x32, 0xa7,
                                       0x3f, 0xdc, 0x63, 0xdb, 0x94, 0x74, 0x55, 0xe2};
        static const u8 hdrfirst[16] = {0x2e, 0x4f, 0x2d, 0xfd, 0x10, 0xda, 0x68, 0xca,
                                        0xb6, 0x76, 0x8c, 0x2b, 0xcc, 0xf2, 0x51, 0xeb};
        static const u32 hdrfirst_texels[16] = {
            0xff00ffffu, 0x74dbe5ffu, 0xff00ffffu, 0xff00ffffu, 0xff00ffffu, 0x73dbe0ffu, 0xff00ffffu, 0xff00ffffu,
            0x73dbdfffu, 0x74dbe3ffu, 0xff00ffffu, 0xff00ffffu, 0x76dcebffu, 0xff00ffffu, 0xff00ffffu, 0xff00ffffu};
        u8 pixels[4 * 4 * 4];
        int i;
        if (!check(illegal, invalid)) {
            fputs("ASTC illegal block is not the error color\n", stderr);
            return 1;
        }
        astc_decode_image(hdrfirst, 4, 4, 4, 4, pixels, 0);
        for (i = 0; i < 16; i++) {
            const u32 v = hdrfirst_texels[i];
            const u8 rgba[4] = {(u8)(v >> 24), (u8)(v >> 16), (u8)(v >> 8), (u8)v};
            if (memcmp(pixels + i * 4, rgba, 4)) {
                fprintf(stderr, "ASTC HDR partition block: texel %d is %02x%02x%02x%02x, expected %08x\n", i,
                        pixels[i * 4], pixels[i * 4 + 1], pixels[i * 4 + 2], pixels[i * 4 + 3], (unsigned)v);
                return 1;
            }
        }
    }
    puts("PASS ASTC void extent color, invalid modes and HDR partitions, in both decode modes");
    return 0;
}
