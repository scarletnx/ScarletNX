/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* astc_encode.glsl's table of partitions, written by tools/astc_tables.py from the decoder's own rule
   (astc_decode.glsl): do not edit. ASTC's partitions of a 4x4 block in two nearest BC7's 64 (30 of them the
   same), a word each: in the low 16 bits a bit a texel, its subset, texel 0 in subset 0 (AS_P2); above them the
   partition's seed, 10 bits, and in bit 10 of that whether subset 0 is ASTC's partition 1 (AS_SEED2). Only the
   passes that fit a block of two partitions read it */
#define AS_P2(i) (ASTC_PART[i] & 0xFFFFu)
#define AS_SEED2(i) (ASTC_PART[i] >> 16u)
const uint ASTC_PART[64] = {
    0x001CCCCCu, 0x00148888u, 0x0410EEEEu, 0x001DECC8u, 0x045BC880u, 0x0009FEECu, 0x046BFEC8u, 0x0448EC80u,
    0x0095C800u, 0x04CCFFECu, 0x0032FE80u, 0x0472E800u, 0x05F0FFE8u, 0x0411FF00u, 0x004EFFF0u, 0x0427F000u,
    0x07E1FF10u, 0x04FC008Eu, 0x073C7100u, 0x002B08CEu, 0x009C008Cu, 0x00747310u, 0x04D23100u, 0x05DC8CCEu,
    0x0111088Cu, 0x06AC3110u, 0x01676666u, 0x049F326Cu, 0x043117ECu, 0x04F60FF0u, 0x0125708Eu, 0x00634666u,
    0x04C3AAAAu, 0x06B6F0F0u, 0x07C6127Eu, 0x040E36CCu, 0x07AA3C0Eu, 0x06235DBAu, 0x0382E868u, 0x06C92448u,
    0x055577EEu, 0x00AE1348u, 0x049F326Cu, 0x02DA3FFCu, 0x032B4892u, 0x052EC3FCu, 0x06D79B76u, 0x00900240u,
    0x01460070u, 0x004C00ECu, 0x01A20EC0u, 0x068F2220u, 0x060CC936u, 0x03BE836Cu, 0x06D43B06u, 0x00739CEEu,
    0x05D71336u, 0x00739CEEu, 0x0417813Eu, 0x04CFFF08u, 0x0642CEF0u, 0x03C40FC8u, 0x04197774u, 0x04E7EE02u};
