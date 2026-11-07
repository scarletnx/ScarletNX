/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* ZBIC (src/common/utilities/zbic.c), an Ounce game's NSO segment compression: a frame that kinnay's zbic compressor
   (github.com/kinnay/zbic, its zstd.c with ZSTD_ZBIC_SUPPORT, level 19) made from 20000 bytes of synthetic text
   decodes to that text. Its four FSE table headers, the literals' Huffman weights and the three sequence tables, are
   ZBIC's, three of them with counts of -1. The same frame under zstd's own magic does not decode to the text with
   zstd (snx_zstd), and the frame cut short or a buffer a byte short are refused. No byte here comes from a game */
#include "common/utilities/zbic.h"
#include <zstd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEXT_BYTES 20000

static int failures;
#define CHECK(c)                                                                                                       \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c);                                                    \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

/* the frame, in base64, a line a string (ISO C99 caps a literal's length) */
static const char* const frame_b64[] = {
    "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",
};

/* the text: words of a small vocabulary, numbers and punctuation, from a linear congruential generator */
static void make_text(unsigned char* p, size_t n) {
    static const char* const words[] = {"vertex", "fragment", "shader", "texture", "sampler", "buffer",  "queue",
                                        "fence",  "render",   "target", "depth",   "stencil", "blend",   "raster",
                                        "warp",   "lane",     "matrix", "tensor",  "upscale", "network", "frame"};
    unsigned seed = 0x5eed1234u;
    size_t i = 0;
    while (i < n) {
        const char* w;
        seed = seed * 1103515245u + 12345u;
        if ((seed >> 16) % 7 == 0) {
            int len = snprintf((char*)p + i, n - i, "%u", (seed >> 8) % 100000u);
            i += len > 0 ? (size_t)len : 0;
        } else {
            w = words[(seed >> 16) % (sizeof words / sizeof *words)];
            while (*w && i < n) p[i++] = (unsigned char)*w++;
        }
        if (i < n) p[i++] = (unsigned char)" ,.;:\n"[(seed >> 24) % 6];
    }
}

static int b64_value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    return c == '+' ? 62 : c == '/' ? 63 : -1;
}
static size_t b64_decode(const char* const* lines, size_t nlines, unsigned char* out) {
    size_t n = 0, i;
    unsigned bits = 0, have = 0;
    for (i = 0; i < nlines; i++) {
        const char* s;
        for (s = lines[i]; *s; s++) {
            int v = b64_value(*s);
            if (v < 0) continue;
            bits = (bits << 6 | (unsigned)v) & 0xFFFFu;
            have += 6;
            if (have >= 8) {
                have -= 8;
                out[n++] = (unsigned char)(bits >> have);
            }
        }
    }
    return n;
}

int main(void) {
    static unsigned char text[TEXT_BYTES], frame[8192], out[TEXT_BYTES + 16];
    size_t n = b64_decode(frame_b64, sizeof frame_b64 / sizeof *frame_b64, frame), cut;
    make_text(text, TEXT_BYTES);
    CHECK(n == 5541);
    CHECK(snx_zbic_is(frame, n));
    CHECK(!snx_zbic_is(frame, 3));
    CHECK(snx_zbic_decompress(out, sizeof out, frame, n) == TEXT_BYTES);
    CHECK(!memcmp(out, text, TEXT_BYTES));
    CHECK(snx_zbic_decompress(out, TEXT_BYTES - 1, frame, n) == -1);
    for (cut = 1; cut < n; cut += 997) CHECK(snx_zbic_decompress(out, sizeof out, frame, n - cut) == -1);
    /* zstd's magic: zstd reads the frame header, but not ZBIC's tables */
    {
        size_t got;
        frame[0] = 0x28, frame[1] = 0xB5, frame[2] = 0x2F, frame[3] = 0xFD;
        CHECK(!snx_zbic_is(frame, n));
        memset(out, 0, sizeof out);
        got = ZSTD_decompress(out, sizeof out, frame, n);
        CHECK(ZSTD_isError(got) || got != TEXT_BYTES || memcmp(out, text, TEXT_BYTES));
    }
    if (failures) {
        fprintf(stderr, "%d checks failed\n", failures);
        return 1;
    }
    printf("zbic: all checks pass\n");
    return 0;
}
