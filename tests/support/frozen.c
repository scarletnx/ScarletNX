/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A frozen reference's file read and written (frozen.h): strictly, so that a file cut short, a key spelled wrong or a
   hash of another width is an error that names its line, never a reference that checks less than it says */
#include "frozen.h"
#include "common/utilities/endian.h"
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SNX_FROZEN_MOST (1u << 24) /* items a file may hold */

uint64_t snx_frozen_bytes(uint64_t h, const void* bytes, size_t size) {
    const unsigned char* p = (const unsigned char*)bytes;
    size_t at = 0;
    for (; at + 8 <= size; at += 8) h = snx_frozen_mix(h, snx_ld_le64(p + at));
    if (at < size) {
        uint64_t tail = 0;
        for (size_t k = 0; at + k < size; ++k) tail |= (uint64_t)p[at + k] << (8 * k);
        h = snx_frozen_mix(h, tail);
    }
    return snx_frozen_mix(h, size);
}

/* the line's end taken off, a CR before it too (a checkout that writes CRLF): its length, or SIZE_MAX for a line longer
   than the buffer */
static size_t chomp(char* line) {
    size_t n = strlen(line);
    if (!n || line[n - 1] != '\n') return SIZE_MAX;
    line[--n] = 0;
    if (n && line[n - 1] == '\r') line[--n] = 0;
    return n;
}
static int hex64(const char* text, uint64_t* value) {
    uint64_t v = 0;
    for (int k = 0; k < 16; ++k) {
        char c = text[k];
        unsigned d = c >= '0' && c <= '9' ? (unsigned)(c - '0') : c >= 'a' && c <= 'f' ? (unsigned)(c - 'a' + 10) : 16;
        if (d == 16) return -1;
        v = v << 4 | d;
    }
    if (text[16]) return -1;
    *value = v;
    return 0;
}
/* the next word of *at, ended in place where a space follows it; NULL past the last */
static char* word(char** at) {
    char* w = *at;
    while (*w == ' ') ++w;
    if (!*w) return NULL;
    char* end = strchr(w, ' ');
    if (end) *end++ = 0;
    *at = end ? end : w + strlen(w);
    return w;
}
/* a whole decimal number of text, at most most */
static int decimal(const char* text, size_t most, size_t* value) {
    size_t v = 0;
    if (!*text) return -1;
    for (; *text; ++text) {
        if (*text < '0' || *text > '9' || v > (most - (size_t)(*text - '0')) / 10) return -1;
        v = v * 10 + (size_t)(*text - '0');
    }
    *value = v;
    return 0;
}

int snx_frozen_read(SnxFrozen* f, const char* path, unsigned generator, char* error, size_t size) {
    static const char* const keys[] = {"generator", "args", "items", "inputs", "recorded"};
    enum { GENERATOR, ARGS, ITEMS, INPUTS, RECORDED, KEYS };
    char line[SNX_FROZEN_LINE];
    int seen[KEYS] = {0};
    size_t number = 0, total = 0, count = 0;
    *f = (SnxFrozen){0};
    if (strlen(path) >= sizeof f->path)
        return snprintf(error, size, "%s: a path of more than %zu bytes", path, sizeof f->path - 1), -1;
    strcpy(f->path, path);
    FILE* in = fopen(path, "rb");
    if (!in) return snprintf(error, size, "%s: cannot read it: %s", path, strerror(errno)), -1;
#define FAIL(...)                                                                                                      \
    do {                                                                                                               \
        int at_ = snprintf(error, size, "%s:%zu: ", path, number);                                                     \
        if (at_ >= 0 && (size_t)at_ < size) snprintf(error + at_, size - (size_t)at_, __VA_ARGS__);                    \
        fclose(in);                                                                                                    \
        snx_frozen_free(f);                                                                                            \
        return -1;                                                                                                     \
    } while (0)
    while (fgets(line, sizeof line, in)) {
        ++number;
        size_t n = chomp(line);
        if (n == SIZE_MAX) FAIL("a line of more than %d bytes, or none at its end", SNX_FROZEN_LINE - 2);
        if (number == 1) {
            if (strncmp(line, "# ", 2) || n - 2 >= sizeof f->what)
                FAIL("\"%.60s\" is no title: the first line is \"# \" and what the file holds, under %zu bytes", line,
                     sizeof f->what);
            strcpy(f->what, line + 2);
            continue;
        }
        if (line[0] == '#') {
            if (f->hashes) FAIL("\"%.60s\" after the hashes: the header comes first", line);
            char* value = strncmp(line, "# ", 2) ? NULL : strchr(line + 2, ' ');
            size_t key = 0;
            if (!value) FAIL("\"%.60s\" is not \"# key value\"", line);
            *value++ = 0;
            while (key < KEYS && strcmp(line + 2, keys[key])) ++key;
            if (key == KEYS) FAIL("%s is no key: generator, args, items, inputs or recorded", line + 2);
            if (seen[key]++) FAIL("%s a second time", keys[key]);
            size_t v = 0;
            switch (key) {
                case GENERATOR:
                    if (decimal(value, 1000000, &v)) FAIL("generator %s is not a whole number", value);
                    f->generator = (unsigned)v;
                    break;
                case ARGS: strcpy(f->args, value); break;
                case ITEMS:
                    for (char* item = word(&value); item; item = word(&value)) {
                        if (f->parts == SNX_FROZEN_PARTS) FAIL("items of more than %d parts", SNX_FROZEN_PARTS);
                        if (decimal(item, SNX_FROZEN_MOST, &f->items[f->parts]) ||
                            (total += f->items[f->parts]) > SNX_FROZEN_MOST)
                            FAIL("items %s: a count of at most %u items in all, each a whole number", item,
                                 SNX_FROZEN_MOST);
                        ++f->parts;
                    }
                    if (!f->parts) FAIL("items with no count");
                    break;
                case INPUTS:
                    if (hex64(value, &f->inputs)) FAIL("inputs %s is not 16 lowercase hexadecimal digits", value);
                    break;
                default:
                    if (strlen(value) >= sizeof f->recorded)
                        FAIL("recorded: more than %zu bytes", sizeof f->recorded - 1);
                    strcpy(f->recorded, value);
                    break;
            }
            continue;
        }
        if (!f->hashes) {
            for (size_t key = 0; key < RECORDED; ++key)
                if (!seen[key]) FAIL("a hash before the header's %s line", keys[key]);
            if (f->generator != generator)
                FAIL(
                    "generator %u, the test's is %u: its inputs are drawn otherwise now, so it is recorded again where "
                    "its reference is built",
                    f->generator, generator);
            if (!(f->hashes = (uint64_t*)malloc((total ? total : 1) * sizeof *f->hashes)))
                FAIL("out of memory for %zu hashes", total);
            f->first_line = number;
        }
        if (count == total) FAIL("a hash past the %zu the items line counts", total);
        if (hex64(line, &f->hashes[count])) FAIL("\"%.60s\" is not a hash of 16 lowercase hexadecimal digits", line);
        ++count;
    }
    if (ferror(in)) FAIL("cannot read on: %s", strerror(errno));
    if (!f->hashes) FAIL("no hashes");
    if (count != total) FAIL("%zu hashes, where the items line counts %zu", count, total);
#undef FAIL
    fclose(in);
    return 0;
}

void snx_frozen_free(SnxFrozen* f) {
    free(f->hashes);
    f->hashes = NULL;
}

static size_t first_of(const SnxFrozen* f, size_t part) {
    size_t at = 0;
    for (size_t k = 0; k < part; ++k) at += f->items[k];
    return at;
}
uint64_t snx_frozen_hash(const SnxFrozen* f, size_t part, size_t index) { return f->hashes[first_of(f, part) + index]; }
size_t snx_frozen_line(const SnxFrozen* f, size_t part, size_t index) {
    return f->first_line + first_of(f, part) + index;
}

int snx_frozen_split(char* args, char** argv, int max) {
    int n = 0;
    for (char* w = word(&args); w && n < max; w = word(&args)) argv[n++] = w;
    return n;
}

int snx_frozen_write(const SnxFrozen* f, const char* path, char* error, size_t size) {
    char tmp[sizeof f->path + 8];
    size_t total = first_of(f, f->parts);
    if (snprintf(tmp, sizeof tmp, "%s.tmp", path) >= (int)sizeof tmp)
        return snprintf(error, size, "%s: a path of more than %zu bytes", path, sizeof f->path - 1), -1;
    FILE* out = fopen(tmp, "wb");
    if (!out) return snprintf(error, size, "%s: cannot write it: %s", tmp, strerror(errno)), -1;
    fprintf(out, "# %s\n# generator %u\n# args %s\n# items", f->what, f->generator, f->args);
    for (size_t k = 0; k < f->parts; ++k) fprintf(out, " %zu", f->items[k]);
    fprintf(out, "\n# inputs %016" PRIx64 "\n", f->inputs);
    if (*f->recorded) fprintf(out, "# recorded %s\n", f->recorded);
    for (size_t k = 0; k < total; ++k) fprintf(out, "%016" PRIx64 "\n", f->hashes[k]);
    int bad = ferror(out);
    if (fclose(out) || bad) return snprintf(error, size, "%s: cannot write it: %s", tmp, strerror(errno)), -1;
    /* Windows' rename keeps a file that is there */
    if ((remove(path) && errno != ENOENT) || rename(tmp, path))
        return snprintf(error, size, "%s: cannot put %s in its place: %s", path, tmp, strerror(errno)), -1;
    return 0;
}
