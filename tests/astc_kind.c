/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What an ASTC block can hold, read from its header and endpoint modes (gpu/texture/astc_kind.h), held to what the CPU
   decoder (tests/support/astc.c, which the GPU's decoder matches byte for byte) makes of it: random blocks of every
   block mode, partition count and endpoint mode, every footprint, both decode modes. A block said to have no colour
   decodes grey in every texel, one said to have no alpha opaque, one said to be colour on one line near one line (and
   only at a footprint whose 4x4 blocks lie in one ASTC block each), and none but those the decoder makes the error
   colour of is OTHER. Then a level's kind read through the guest's block
   linear layout (and pitch rows) from its own blocks alone, the padding left out, and the format each kind takes */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "nx/gpu/texture/astc_kind.h"
#include "support/astc.h"

/* the test's state: its random words, the failures, the blocks counted by the format their kind takes (by decode
   mode), the farthest a block of colour on one line lay off it, and a level's layout */
static struct {
    u32 rng;
    int failures;
    u32 as[2][4];
    double off_most[2];
    u8 level[1 << 20];
} g_astc_kind = {.rng = 0x13579bdfu};

static u32 rnd(void) {
    g_astc_kind.rng ^= g_astc_kind.rng << 13;
    g_astc_kind.rng ^= g_astc_kind.rng >> 17;
    g_astc_kind.rng ^= g_astc_kind.rng << 5;
    return g_astc_kind.rng;
}

/* a random block: every block mode; one in two of them one partition, its endpoint mode one of the LDR ones that
   hold no alpha or luminance, so the kinds BCn takes come often enough to be tested */
static void block_new(u8 b[16]) {
    for (int k = 0; k < 16; k++) b[k] = (u8)rnd();
    const u32 pick = rnd() % 8;
    if (pick < 4) { /* one partition, endpoint mode 0, 1, 4, 5, 6, 8 or 9 */
        static const u8 cems[7] = {0, 1, 4, 5, 6, 8, 9};
        const u32 cem = cems[rnd() % 7];
        b[1] = (u8)((b[1] & 0x80) | (cem << 5) | (0u << 3) | (b[1] & 7));
        b[2] = (u8)((b[2] & 0xFE) | (cem >> 3));
    } else if (pick == 4) { /* a void extent, all ones coordinates, R = G = B one time in two */
        u32 w[4] = {0xFFFFFDFCu, 0xFFFFFFFFu, rnd(), rnd()};
        if (rnd() & 1) w[2] = (w[2] & 0xFFFFu) | (w[2] << 16), w[3] = (w[3] & 0xFFFF0000u) | (w[2] & 0xFFFFu);
        if (rnd() & 1) w[3] |= 0xFFFF0000u;
        for (int k = 0; k < 16; k++) b[k] = (u8)(w[k / 4] >> (k % 4 * 8));
    }
}

/* the squared distance of the texels of a block from the line through its two texels farthest apart, the most of
   them (RGB, 8 bit each) */
static double off_line(const u8* px, u32 n) {
    u32 p = 0, q = 0;
    double far = -1, most = 0;
    for (u32 i = 0; i < n; i++)
        for (u32 j = i + 1; j < n; j++) {
            double d = 0;
            for (int c = 0; c < 3; c++) d += (double)(px[i * 4 + c] - px[j * 4 + c]) * (px[i * 4 + c] - px[j * 4 + c]);
            if (d > far) far = d, p = i, q = j;
        }
    if (far <= 0) return 0;
    for (u32 i = 0; i < n; i++) {
        double u[3], v[3], x[3];
        for (int c = 0; c < 3; c++) {
            u[c] = (double)px[q * 4 + c] - px[p * 4 + c];
            v[c] = (double)px[i * 4 + c] - px[p * 4 + c];
        }
        x[0] = u[1] * v[2] - u[2] * v[1];
        x[1] = u[2] * v[0] - u[0] * v[2];
        x[2] = u[0] * v[1] - u[1] * v[0];
        const double d = (x[0] * x[0] + x[1] * x[1] + x[2] * x[2]) / far;
        if (d > most) most = d;
    }
    return most;
}

/* the decoded texels of a block of each kind, against its kind: counted by the format it takes */
static void check_block(const u8 b[16], u32 bw, u32 bh, int srgb) {
    u8 px[144 * 4];
    astc_decode_image(b, bw, bh, bw, bh, px, srgb);
    const u32 kind = snx_astc_block_kind(b, bw, bh, srgb), n = bw * bh;
    u32 grey = 1, opaque = 1, error = 1;
    for (u32 i = 0; i < n; i++) {
        grey &= px[i * 4] == px[i * 4 + 1] && px[i * 4 + 1] == px[i * 4 + 2];
        opaque &= px[i * 4 + 3] == 255;
        error &= px[i * 4] == 255 && px[i * 4 + 1] == 0 && px[i * 4 + 2] == 255 && px[i * 4 + 3] == 255;
    }
    const char* wrong = NULL;
    /* OTHER promises nothing: the kinds below it hold only for blocks BCn is chosen for */
    if (!(kind & (SNX_ASTC_CHROMA | SNX_ASTC_OTHER)) && !grey) wrong = "no colour, and a texel is not grey";
    if (!(kind & (SNX_ASTC_ALPHA | SNX_ASTC_OTHER)) && !opaque) wrong = "no alpha, and a texel is not opaque";
    if (!(kind & SNX_ASTC_OTHER) && error) wrong = "not the error colour, and every texel is it";
    /* a 4x4 block that takes two ASTC blocks or more takes two lines or more of colour */
    if ((bw % 4 || bh % 4) && (kind & SNX_ASTC_CHROMA) && !(kind & SNX_ASTC_LINES))
        wrong = "colour at a footprint whose 4x4 blocks take several ASTC blocks, on one line";
    if (kind == SNX_ASTC_CHROMA) {
        const double off = off_line(px, n);
        if (off > g_astc_kind.off_most[srgb]) g_astc_kind.off_most[srgb] = off;
        /* each channel within half a level of the line in UNORM, under one level in sRGB (its top 8 bits), and the
           line through the farthest two texels moves as much again: these blocks come to 1.84 and 2.00 */
        if (off > 3.0) wrong = "colour on one line, and a texel lies off it";
    }
    if (wrong) {
        fprintf(stderr, "FAIL %ux%u srgb=%d block", bw, bh, srgb);
        for (int k = 0; k < 16; k++) fprintf(stderr, " %02x", b[k]);
        fprintf(stderr, ": kind %u, %s\n", kind, wrong);
        g_astc_kind.failures++;
    }
    g_astc_kind.as[srgb][snx_astc_kind_as(kind, srgb)]++;
}

/* the ISE's bits against the specification's search for the encoding (trits, quints or bits) of a range */
static void ise_bits(void) {
    static const u32 ranges[12] = {1, 2, 3, 4, 5, 7, 9, 11, 15, 19, 23, 31};
    for (u32 r = 0; r < 12; r++) {
        u32 m = ranges[r], kind = 0, bits = 0;
        while (m > 0) {
            const u32 check = m + 1;
            if (!(check & (check - 1))) {
                for (u32 v = m; v; v >>= 1) bits++;
                break;
            }
            if (check % 3 == 0 && !((check / 3) & (check / 3 - 1))) {
                kind = 3;
                for (u32 v = check / 3 - 1; v; v >>= 1) bits++;
                break;
            }
            if (check % 5 == 0 && !((check / 5) & (check / 5 - 1))) {
                kind = 5;
                for (u32 v = check / 5 - 1; v; v >>= 1) bits++;
                break;
            }
            m--;
        }
        for (u32 n = 1; n <= 64; n++) {
            const u32 want = bits * n + (kind == 3 ? (n * 8 + 4) / 5 : kind == 5 ? (n * 7 + 2) / 3 : 0);
            if (snx_astc_ise_bits(ranges[r], n) != want) {
                fprintf(stderr, "FAIL %u values up to %u: %u bits, not %u\n", n, ranges[r],
                        snx_astc_ise_bits(ranges[r], n), want);
                g_astc_kind.failures++;
                return;
            }
        }
    }
    puts("PASS the bits of a bounded integer sequence, every weight range, 1 to 64 values");
}

/* where block (x, y, z) of a block linear level lies, from the layout's own words (docs/layers/gpu/blocklinear.md):
   in a GOB, byte x's bits 0-3 stay, bit 4 goes to 5 and bit 5 to 8; row y's bit 0 goes to 4, bits 1-2 to 6-7 */
static u64 bl_at(u32 x, u32 y, u32 z, u32 ny, u32 gobs_x, u32 lbh, u32 lbd) {
    const u32 xb = x * 16,
              in = (xb & 15) | ((xb >> 4 & 1) << 5) | ((xb >> 5 & 1) << 8) | ((y & 1) << 4) | ((y >> 1 & 3) << 6);
    const u64 rows = (((ny + 7) / 8) + (1u << lbh) - 1) >> lbh, gy = y / 8;
    const u64 block = ((u64)(z >> lbd) * rows + (gy >> lbh)) * gobs_x + xb / 64;
    return block * (512ull << (lbh + lbd)) + (((u64)(z & ((1u << lbd) - 1)) << lbh) + (gy & ((1u << lbh) - 1))) * 512 +
           in;
}

/* a level of grey blocks (one of them coloured where colour_at is in it) in a block linear layout whose padding,
   past the level's rows, columns and slices and the extra GOBs a row, is blocks of the error colour (zero bytes) */
static void level_kind(const u8 grey[16], const u8 colour[16], u32 nx, u32 ny, u32 nz, u32 extra_gobs, u32 lbh, u32 lbd,
                       u32 colour_at) {
    const u32 gobs_x = (nx + 3) / 4 + extra_gobs;
    /* the layout's bytes, and the end of the last GOB that holds a block of the level, which the walk needs */
    const u64 all = bl_at(0, 0, (nz + (1u << lbd) - 1) >> lbd << lbd, ny, gobs_x, lbh, lbd);
    u64 size = 0;
    assert(all <= sizeof g_astc_kind.level);
    memset(g_astc_kind.level, 0, all);
    u32 i = 0;
    for (u32 z = 0; z < nz; z++)
        for (u32 y = 0; y < ny; y++)
            for (u32 x = 0; x < nx; x++, i++) {
                const u64 at = bl_at(x, y, z, ny, gobs_x, lbh, lbd);
                memcpy(g_astc_kind.level + at, i == colour_at ? colour : grey, 16);
                if ((at | 511) + 1 > size) size = (at | 511) + 1;
            }
    const u32 want = colour_at < i ? SNX_ASTC_CHROMA : 0;
    const u32 got = snx_astc_level_kind(g_astc_kind.level, size, nx, ny, nz, 0, gobs_x, lbh, lbd, 4, 4, 0);
    const u32 short_of = snx_astc_level_kind(g_astc_kind.level, size - 1, nx, ny, nz, 0, gobs_x, lbh, lbd, 4, 4, 0);
    if (got != want || short_of != SNX_ASTC_OTHER) {
        fprintf(stderr,
                "FAIL a %ux%ux%u level, %u GOBs past a row, GOB blocks 2^%u x 2^%u, colour at %u: kind %u, not %u "
                "(a byte short: %u)\n",
                nx, ny, nz, extra_gobs, lbh, lbd, colour_at, got, want, short_of);
        g_astc_kind.failures++;
    }
}

int main(void) {
    static const u32 footprints[14][2] = {{4, 4}, {5, 4},  {5, 5},  {6, 5},  {6, 6},   {8, 5},   {8, 6},
                                          {8, 8}, {10, 5}, {10, 6}, {10, 8}, {10, 10}, {12, 10}, {12, 12}};
    ise_bits();
    u8 grey[16] = {0}, colour[16] = {0};
    int have_grey = 0, have_colour = 0;
    for (int srgb = 0; srgb < 2; srgb++)
        for (u32 f = 0; f < 14; f++)
            for (u32 n = 0; n < 6000; n++) {
                u8 b[16];
                block_new(b);
                check_block(b, footprints[f][0], footprints[f][1], srgb);
                const u32 kind = snx_astc_block_kind(b, 4, 4, 0);
                if (f == 0 && !srgb && !kind && !have_grey) memcpy(grey, b, 16), have_grey = 1;
                if (f == 0 && !srgb && kind == SNX_ASTC_CHROMA && !have_colour) memcpy(colour, b, 16), have_colour = 1;
            }
    for (int srgb = 0; srgb < 2; srgb++) {
        printf("srgb=%d: %u usual, %u BC4, %u BC5, %u BC1; colour on one line at most %.2f off it (squared)\n", srgb,
               g_astc_kind.as[srgb][0], g_astc_kind.as[srgb][1], g_astc_kind.as[srgb][2], g_astc_kind.as[srgb][3],
               g_astc_kind.off_most[srgb]);
        /* each kind came often enough for the checks above to mean something */
        for (int as = 0; as < 4; as++)
            if (g_astc_kind.as[srgb][as] < (srgb && as == SNX_ASTC_AS_BC4   ? 0u
                                            : srgb && as == SNX_ASTC_AS_BC5 ? 0u
                                                                            : 500u)) {
                fprintf(stderr, "FAIL srgb=%d: only %u blocks take format %d\n", srgb, g_astc_kind.as[srgb][as], as);
                g_astc_kind.failures++;
            }
    }
    if (!g_astc_kind.failures)
        puts("PASS every block's kind holds for its decoded texels, both decode modes, every footprint");

    /* a level read through its layout: padding of error blocks left out, the coloured block found wherever it is */
    assert(have_grey && have_colour);
    static const u32 shapes[][6] = {{1, 1, 1, 0, 0, 0},  {5, 9, 1, 0, 0, 0},   {16, 16, 1, 1, 1, 0},
                                    {7, 40, 3, 2, 2, 1}, {33, 17, 2, 0, 3, 1}, {64, 64, 1, 3, 4, 0},
                                    {3, 3, 5, 0, 0, 2}};
    for (u32 s = 0; s < sizeof shapes / sizeof shapes[0]; s++) {
        const u32* v = shapes[s];
        const u32 blocks = v[0] * v[1] * v[2];
        level_kind(grey, colour, v[0], v[1], v[2], v[3], v[4], v[5], ~0u);
        level_kind(grey, colour, v[0], v[1], v[2], v[3], v[4], v[5], 0);
        level_kind(grey, colour, v[0], v[1], v[2], v[3], v[4], v[5], blocks - 1);
        level_kind(grey, colour, v[0], v[1], v[2], v[3], v[4], v[5], rnd() % blocks);
    }
    /* pitch rows: the bytes past a row's blocks left out */
    memset(g_astc_kind.level, 0, 4096);
    for (u32 y = 0; y < 5; y++)
        for (u32 x = 0; x < 3; x++) memcpy(g_astc_kind.level + y * 64 + x * 16, grey, 16);
    assert(snx_astc_level_kind(g_astc_kind.level, 4 * 64 + 48, 3, 5, 1, 64, 0, 0, 0, 4, 4, 0) == 0);
    assert(snx_astc_level_kind(g_astc_kind.level, 4 * 64 + 47, 3, 5, 1, 64, 0, 0, 0, 4, 4, 0) == SNX_ASTC_OTHER);
    memcpy(g_astc_kind.level + 4 * 64 + 2 * 16, colour, 16);
    assert(snx_astc_level_kind(g_astc_kind.level, 4 * 64 + 48, 3, 5, 1, 64, 0, 0, 0, 4, 4, 0) == SNX_ASTC_CHROMA);
    if (!g_astc_kind.failures) puts("PASS a level's kind from its own blocks through block linear and pitch layouts");

    /* the format each kind takes: BC4 grey, BC5 grey with alpha, BC1 one line of opaque colour; sRGB has no BC4 or
       BC5, so grey goes to BC1 and grey with alpha stays */
    for (u32 kind = 0; kind < 16; kind++)
        for (int srgb = 0; srgb < 2; srgb++) {
            const int other = kind & SNX_ASTC_OTHER, chroma = kind & SNX_ASTC_CHROMA, alpha = kind & SNX_ASTC_ALPHA;
            const int want = other                            ? SNX_ASTC_AS_USUAL
                             : !chroma && !alpha              ? (srgb ? SNX_ASTC_AS_BC1 : SNX_ASTC_AS_BC4)
                             : !chroma                        ? (srgb ? SNX_ASTC_AS_USUAL : SNX_ASTC_AS_BC5)
                             : alpha || kind & SNX_ASTC_LINES ? SNX_ASTC_AS_USUAL
                                                              : SNX_ASTC_AS_BC1;
            if (snx_astc_kind_as(kind, srgb) != want) {
                fprintf(stderr, "FAIL kind %u srgb=%d takes %d, not %d\n", kind, srgb, snx_astc_kind_as(kind, srgb),
                        want);
                g_astc_kind.failures++;
            }
        }
    if (g_astc_kind.failures) {
        fprintf(stderr, "%d failures\n", g_astc_kind.failures);
        return 1;
    }
    puts("PASS the format each kind takes");
    return 0;
}
