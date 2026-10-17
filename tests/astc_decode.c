/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "nx/gpu/texture/astc.h"
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
    const u8 invalid[4] = {0, 0, 0, 0}; /* the error color, transparent black */
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
    puts("PASS ASTC void extent color and invalid modes, in both decode modes");
    return 0;
}
