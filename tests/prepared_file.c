/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A console's prepared shaders' file (common/gpu/vk/prepared.c), without a GPU: records come back in their order as
   they were added, packed or not; a body that zstd cannot make smaller is kept as it is; a file of another key starts
   over and says what differs; a record cut short, changed on disk or refused by the renderer ends the reading, and
   the file goes on from the last good one; past its most size it takes no more and says so; and a file written over
   another replaces it whole, leaving no file beside it */
#include "common/gpu/vk/prepared.h"
#include "common/platform/file.h"
#include "common/utilities/common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned failures, checks;
#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        ++checks;                                                                                                      \
        if (!(x)) {                                                                                                    \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);                                                    \
            ++failures;                                                                                                \
        }                                                                                                              \
    } while (0)

#define MAGIC "SNXTESTP"
static char path[1024];

/* what each was handed: the types and the payloads' first word, the record it refuses */
static struct {
    unsigned n, refuse;
    uint32_t type[16], first[16], len[16];
} got;
static bool each(void* ctx, uint32_t type, const uint8_t* payload, uint32_t len) {
    (void)ctx;
    if (got.n + 1 == got.refuse) return false;
    if (got.n < 16) {
        got.type[got.n] = type;
        got.len[got.n] = len;
        memcpy(&got.first[got.n], payload, 4);
    }
    ++got.n;
    return true;
}
static uint32_t open_file(SnxPrepFile* p, const char* key, bool pack, unsigned refuse) {
    memset(&got, 0, sizeof got);
    got.refuse = refuse;
    *p = (SnxPrepFile){.who = "prepared test", .pack = pack};
    return snx_prep_open(p, path, MAGIC, 1, key, each, NULL);
}
static long file_size(void) {
    size_t n = 0;
    unsigned char* d = snx_read_file(path, &n);
    free(d);
    return d ? (long)n : -1;
}
/* a body of n words, the first first, the rest of a pattern zstd packs, or of noise it cannot */
static uint32_t* body(size_t n, uint32_t first, bool noise) {
    uint32_t* b = malloc(n * 4);
    uint32_t x = first * 2654435761u + 1;
    for (size_t i = 0; i < n; ++i) {
        x ^= x << 13, x ^= x >> 17, x ^= x << 5;
        b[i] = i ? (noise ? x : (uint32_t)(i % 7)) : first;
    }
    return b;
}
/* the file's byte at changed */
static void change(long at) {
    size_t n = 0;
    unsigned char* d = snx_read_file(path, &n);
    if (d && at < (long)n) d[at] ^= 0x5A;
    FILE* f = d ? fopen(path, "wb") : NULL;
    if (f) {
        fwrite(d, 1, n, f);
        fclose(f);
    }
    free(d);
}

int main(void) {
    char* base = snx_temp_dir(NULL);
    if (!base) return 1;
    snprintf(path, sizeof path, "%s/prepared.bin", base);
    remove(path);
    SnxPrepFile p;

    /* a first run: the header alone, then three records, the second's body packed, the third's not (noise) */
    CHECK(open_file(&p, "guest=1; build-a", true, 0) == 0 && p.f && got.n == 0);
    const long header = file_size();
    CHECK(header == 16 + 16 && header % 8 == 0); /* "guest=1; build-a" is 16 bytes */
    const uint32_t head[2] = {0x11, 0x22};
    uint32_t *packs = body(4096, 0xAA, false), *noise = body(64, 0xBB, true);
    CHECK(!snx_prep_add(&p, 1, head, sizeof head, NULL, 0));
    CHECK(!snx_prep_add(&p, 2, head, sizeof head, packs, 4096 * 4));
    CHECK(!snx_prep_add(&p, 3, head, 4, noise, 64 * 4));
    CHECK(p.records == 3 && (long)p.size == file_size());
    snx_prep_close(&p);
    /* the packed one is far smaller than its 16 KB */
    CHECK(file_size() < header + 3 * 16 + 2048 + 64 * 4 + 64);

    /* read back in order, the packed one unpacked whole: head, then body */
    CHECK(open_file(&p, "guest=1; build-a", true, 0) == 3 && got.n == 3);
    CHECK(got.type[0] == 1 && got.type[1] == 2 && got.type[2] == 3);
    CHECK(got.first[0] == 0x11 && got.first[1] == 0x11 && got.first[2] == 0x11);
    CHECK(got.len[0] == 8 && got.len[1] == 8 + 4096 * 4 && got.len[2] == 4 + 64 * 4 + 4); /* padded to 8 */
    /* appended after what was read */
    CHECK(!snx_prep_add(&p, 4, head, sizeof head, NULL, 0));
    snx_prep_close(&p);
    CHECK(open_file(&p, "guest=1; build-a", false, 0) == 4 && got.type[3] == 4);
    snx_prep_close(&p);

    /* a record the renderer refuses (the third) ends the file there: two kept, and the next run reads two */
    CHECK(open_file(&p, "guest=1; build-a", false, 3) == 2);
    snx_prep_close(&p);
    CHECK(open_file(&p, "guest=1; build-a", false, 0) == 2 && got.n == 2);
    snx_prep_close(&p);

    /* a byte of the second record changed: the first kept */
    change(header + 16 + 8 + 16 + 20);
    CHECK(open_file(&p, "guest=1; build-a", false, 0) == 1 && got.n == 1);
    const long one = file_size();
    snx_prep_close(&p);
    CHECK(one == header + 16 + 8);
    /* a record cut short by a crash: the ones before it kept */
    CHECK(open_file(&p, "guest=1; build-a", false, 0) == 1);
    CHECK(!snx_prep_add(&p, 2, head, sizeof head, packs, 64));
    snx_prep_close(&p);
    {
        size_t n = 0;
        unsigned char* d = snx_read_file(path, &n);
        FILE* f = d ? fopen(path, "wb") : NULL;
        if (f) {
            fwrite(d, 1, n - 5, f);
            fclose(f);
        }
        free(d);
    }
    CHECK(open_file(&p, "guest=1; build-a", false, 0) == 1 && file_size() == one);
    snx_prep_close(&p);

    /* another build's key: started over, the header alone */
    CHECK(open_file(&p, "guest=1; build-b", false, 0) == 0 && got.n == 0 && file_size() == header);
    snx_prep_close(&p);
    /* a file of no such header: started over too */
    {
        FILE* f = fopen(path, "wb");
        if (f) {
            fputs("not a prepared file", f);
            fclose(f);
        }
    }
    CHECK(open_file(&p, "guest=1; build-b", false, 0) == 0 && file_size() == header);
    /* past the most size: refused, and nothing more is added */
    p.size = SNX_PREP_MAX - 8;
    CHECK(snx_prep_add(&p, 1, head, sizeof head, NULL, 0) == -1 && !p.f);
    CHECK(snx_prep_add(&p, 1, head, sizeof head, NULL, 0) == -1);
    snx_prep_close(&p);
    CHECK(file_size() == header);

    /* the key's difference, as a file started over says it */
    char why[256];
    CHECK(snx_prep_key_diff(why, sizeof why, "guest=1; build-a", 16, "guest=1; build-b") == 1 &&
          !strcmp(why, "build build-a in the file, build-b now"));

    /* a file written over another: replaced whole, and nothing left beside it */
    {
        char tmp[1100];
        snprintf(tmp, sizeof tmp, "%s.%u.tmp", path, snx_prep_pid());
        uint8_t* h = NULL;
        size_t hn = 0;
        CHECK((h = snx_prep_header(MAGIC, 1, "guest=1; build-c", &hn)) != NULL && hn == 32);
        CHECK(open_file(&p, "guest=1; build-c", false, 0) == 0 && file_size() == (long)hn);
        snx_prep_close(&p);
        FILE* left = fopen(tmp, "rb");
        CHECK(!left);
        if (left) fclose(left);
        free(h);
    }

    free(packs);
    free(noise);
    remove(path);
    (void)snx_remove_tree(base);
    free(base);
    printf("prepared_file: %u checks, %u failed\n", checks, failures);
    return failures ? 1 : 0;
}
