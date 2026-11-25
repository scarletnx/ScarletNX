/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* ncz.h's NCZ over a storage of its bytes: its header and tables read once, its blocks or its stream's pages decoded as
   reads reach them and kept a few at a time, the one read longest ago making room */
#include "ncz.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zstd.h>
#include "aes.h"

#define NCZ_SECTION 0x40          /* a section's entry */
#define NCZ_BLOCK_HEAD 0x18       /* NCZBLOCK's fixed part, before the blocks' sizes */
#define NCZ_SECTIONS_MAX 0x100000 /* nsz lists an update's AesCtrEx runs a section each: thousands of them */
#define NCZ_PAGE 0x100000         /* a solid stream is kept in pages of this many bytes */
#define NCZ_PAGES 32
#define NCZ_KEPT 0x800000         /* the bytes of blocks kept: 2 blocks at least, 16 at most */
#define NCZ_CHUNK 0x1000          /* the tables are read this many bytes at a time */

typedef struct {
    uint64_t offset, size, crypto; /* in the NCA; nsz's crypto type */
    unsigned char key[16], counter[16];
} ncz_section_t;

struct ncz {
    storage_t nca; /* first: what ncz_storage hands out */
    storage_t *packed;
    int kind;
    uint64_t body;     /* where the stream, or the first block, starts in packed */
    uint64_t stream;   /* the bytes it decompresses to: the NCA's past NCZ_HEADER_SIZE */
    uint64_t count;    /* sections, a gap before the first included */
    unsigned exponent; /* a block's size, 1 << exponent */
    uint64_t blocks;
    ncz_section_t *sections; /* in this allocation, after the struct */
    uint64_t *block_at;      /* there too, blocks + 1 offsets in packed: block b is [block_at[b], block_at[b + 1]) */
    /* What reads keep, allocated at the first read past the header, in one block: slots units (blocks, or pages of
       the stream) of slot bytes each, slot s holding unit tag[s] - 1 (0 for none), last read at tick used[s]; then
       the input buffer, in_size bytes */
    unsigned char *kept, *in;
    uint64_t *tag, *used, tick;
    size_t slot, in_size;
    unsigned slots;
    ZSTD_DCtx *zstd;
    /* a solid stream: packed read up to in_at, in[in_pos, in_len) not decoded yet, the stream decoded up to out_at */
    uint64_t in_at, out_at;
    size_t in_pos, in_len;
    aes_ctx_t *aes; /* keyed with aes_key, NULL until a section needs it */
    unsigned char aes_key[16];
};

static struct {
    void (*said)(const char *line, void *data);
    void *data;
} g_ncz;

static uint64_t le(const unsigned char *p, unsigned n) {
    uint64_t v = 0;
    while (n--) v = v << 8 | p[n];
    return v;
}

static uint64_t min64(uint64_t a, uint64_t b) { return a < b ? a : b; }

static void say(char *why, size_t size, const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    vsnprintf(why, size, format, ap);
    va_end(ap);
}

typedef unsigned long long ull; /* what %llx takes */

static uint64_t unit_of(const ncz_t *z) { return z->kind == NCZ_BLOCK ? (uint64_t)1 << z->exponent : NCZ_PAGE; }

/* the bytes of unit u (a block, or a page of the stream): the last one may be short */
static uint64_t unit_size(const ncz_t *z, uint64_t u) { return min64(unit_of(z), z->stream - u * unit_of(z)); }

static int ncz_read(storage_t *storage, uint64_t offset, void *buffer, uint64_t size);

/* The sections, read from the table at table into z->sections from index 1, then the gap before the first one at
   index 0 when there is one: 0, or nonzero with why */
static int read_sections(ncz_t *z, storage_t *packed, uint64_t table, uint64_t count, char *why, size_t size) {
    unsigned char chunk[NCZ_CHUNK];
    const uint64_t per = sizeof chunk / NCZ_SECTION;
    ncz_section_t *s = z->sections + 1;
    uint64_t i, end = 0;
    for (i = 0; i < count; i++) {
        const unsigned char *e = chunk + i % per * NCZ_SECTION;
        if (i % per == 0 && !storage_read(packed, table + i * NCZ_SECTION, chunk, min64(count - i, per) * NCZ_SECTION)) {
            say(why, size, "its section table at 0x%llx cannot be read", (ull)(table + i * NCZ_SECTION));
            return 1;
        }
        s[i].offset = le(e, 8);
        s[i].size = le(e + 8, 8);
        s[i].crypto = le(e + 0x10, 8);
        memcpy(s[i].key, e + 0x20, 16);
        memcpy(s[i].counter, e + 0x30, 16);
    }
    memset(chunk, 0, sizeof chunk); /* the keys */
    if (s[0].offset > NCZ_HEADER_SIZE) { /* the bytes up to the first section, as they are */
        z->sections[0].offset = NCZ_HEADER_SIZE;
        z->sections[0].size = s[0].offset - NCZ_HEADER_SIZE;
        z->sections[0].crypto = 1;
        s = z->sections;
        count++;
    }
    for (i = 0; i < count; i++) {
        uint64_t n = s == z->sections ? i - 1 : i; /* its number and entry in the table; the gap is no entry */
        if (s[i].size > UINT64_MAX - s[i].offset) {
            say(why, size, "section %llu (its entry at 0x%llx) has offset 0x%llx and size 0x%llx, past 2^64", (ull)n,
                (ull)(table + n * NCZ_SECTION), (ull)s[i].offset, (ull)s[i].size);
            return 1;
        }
        if (i && s[i].offset != end) {
            say(why, size,
                "section %llu (its entry at 0x%llx) starts at 0x%llx of the NCA, and the one before it ends at 0x%llx: "
                "nsz lays them one after another",
                (ull)n, (ull)(table + n * NCZ_SECTION), (ull)s[i].offset, (ull)end);
            return 1;
        }
        if (i && s[i].offset < NCZ_HEADER_SIZE) {
            say(why, size,
                "section %llu (its entry at 0x%llx) starts at 0x%llx of the NCA, inside the 0x%x bytes an NCZ keeps as "
                "they are, which only its first section may start in",
                (ull)n, (ull)(table + n * NCZ_SECTION), (ull)s[i].offset, NCZ_HEADER_SIZE);
            return 1;
        }
        end = s[i].offset + s[i].size;
    }
    if (end <= NCZ_HEADER_SIZE) {
        say(why, size, "its sections end at 0x%llx of the NCA, inside the 0x%x bytes it keeps as they are: nothing of it "
            "is compressed", (ull)end, NCZ_HEADER_SIZE);
        return 1;
    }
    if (s != z->sections) { /* no gap: down to index 0, the last one's place left with no key */
        memmove(z->sections, s, (size_t)count * sizeof *s);
        memset(&z->sections[count], 0, sizeof *s);
    }
    z->count = count;
    z->stream = end - NCZ_HEADER_SIZE;
    return 0;
}

/* NCZBLOCK's blocks at body (its fixed part in fixed), checked against the sections: 0, or nonzero with why */
static int read_blocks(ncz_t *z, storage_t *packed, const unsigned char *fixed, char *why, size_t size) {
    unsigned char chunk[NCZ_CHUNK];
    const uint64_t per = sizeof chunk / 4, sizes = z->body + NCZ_BLOCK_HEAD;
    uint64_t block = (uint64_t)1 << z->exponent, decompressed = le(fixed + 16, 8), b;
    uint64_t want = z->stream / block + (z->stream % block != 0);
    if (decompressed != z->stream) {
        say(why, size,
            "its NCZBLOCK at 0x%llx says 0x%llx bytes decompress, and its sections make 0x%llx past 0x%x",
            (ull)z->body, (ull)decompressed, (ull)z->stream, NCZ_HEADER_SIZE);
        return 1;
    }
    if (z->blocks != want) {
        say(why, size, "its NCZBLOCK at 0x%llx has %llu blocks, and 0x%llx bytes in blocks of 0x%llx make %llu",
            (ull)z->body, (ull)z->blocks, (ull)z->stream, (ull)block, (ull)want);
        return 1;
    }
    z->block_at[0] = sizes + z->blocks * 4;
    for (b = 0; b < z->blocks; b++) {
        uint64_t stored, holds = unit_size(z, b);
        if (b % per == 0 && !storage_read(packed, sizes + b * 4, chunk, min64(z->blocks - b, per) * 4)) {
            say(why, size, "its blocks' sizes at 0x%llx cannot be read", (ull)(sizes + b * 4));
            return 1;
        }
        stored = le(chunk + b % per * 4, 4);
        if (!stored || stored > holds) {
            say(why, size,
                "block %llu's stored size, at 0x%llx, is 0x%llx, and the block holds 0x%llx bytes: nsz stores a block "
                "in 1 to as many bytes as it holds",
                (ull)b, (ull)(sizes + b * 4), (ull)stored, (ull)holds);
            return 1;
        }
        z->block_at[b + 1] = z->block_at[b] + stored;
    }
    if (z->block_at[z->blocks] > packed->size) {
        say(why, size, "its blocks end at 0x%llx, past the NCZ's 0x%llx bytes: the file is cut short",
            (ull)z->block_at[z->blocks], (ull)packed->size);
        return 1;
    }
    return 0;
}

ncz_t *ncz_open(storage_t *packed, char *why, size_t size) {
    unsigned char head[0x10], fixed[NCZ_BLOCK_HEAD] = {0};
    const uint64_t table = NCZ_HEADER_SIZE + sizeof head;
    uint64_t count, body, blocks = 0, bytes;
    int kind = NCZ_SOLID;
    ncz_t *z = NULL;
    if (packed->size < table) {
        say(why, size, "the NCZ is 0x%llx bytes, short of the 0x%llx its NCA's header and NCZSECTN take",
            (ull)packed->size, (ull)table);
        goto fail;
    }
    if (!storage_read(packed, NCZ_HEADER_SIZE, head, sizeof head)) {
        say(why, size, "its NCZSECTN at 0x%x cannot be read", NCZ_HEADER_SIZE);
        goto fail;
    }
    if (memcmp(head, "NCZSECTN", 8)) {
        say(why, size, "0x%x holds %02x %02x %02x %02x %02x %02x %02x %02x, not NCZSECTN: no NCZ nsz wrote",
            NCZ_HEADER_SIZE, head[0], head[1], head[2], head[3], head[4], head[5], head[6], head[7]);
        goto fail;
    }
    count = le(head + 8, 8);
    if (!count || count > NCZ_SECTIONS_MAX) {
        say(why, size, "its section count, at 0x%x, is %llu: an NCZ lists 1 to %u", NCZ_HEADER_SIZE + 8, (ull)count,
            (unsigned)NCZ_SECTIONS_MAX);
        goto fail;
    }
    if (count > (packed->size - table) / NCZ_SECTION) {
        say(why, size, "its %llu sections take 0x%llx bytes from 0x%llx, past the NCZ's 0x%llx", (ull)count,
            (ull)(count * NCZ_SECTION), (ull)table, (ull)packed->size);
        goto fail;
    }
    body = table + count * NCZ_SECTION;
    if (packed->size - body >= 8 && !storage_read(packed, body, fixed, 8)) {
        say(why, size, "what follows its section table, at 0x%llx, cannot be read", (ull)body);
        goto fail;
    }
    if (packed->size - body >= 8 && !memcmp(fixed, "NCZBLOCK", 8)) {
        kind = NCZ_BLOCK;
        if (packed->size - body < sizeof fixed || !storage_read(packed, body, fixed, sizeof fixed)) {
            say(why, size, "its NCZBLOCK at 0x%llx is cut short: the NCZ ends at 0x%llx", (ull)body,
                (ull)packed->size);
            goto fail;
        }
        if (fixed[8] != 2 || fixed[9] != 1) {
            say(why, size, "its NCZBLOCK at 0x%llx is version %u type %u, and nsz writes version 2 type 1", (ull)body,
                fixed[8], fixed[9]);
            goto fail;
        }
        if (fixed[11] < 14 || fixed[11] > 32) {
            say(why, size, "its NCZBLOCK at 0x%llx gives blocks of 2^%u bytes, and nsz's are 2^14 to 2^32", (ull)body,
                fixed[11]);
            goto fail;
        }
        blocks = le(fixed + 12, 4);
        if (blocks > (packed->size - body - sizeof fixed) / 4) {
            say(why, size, "its %llu blocks' sizes take 0x%llx bytes from 0x%llx, past the NCZ's 0x%llx", (ull)blocks,
                (ull)(blocks * 4), (ull)(body + sizeof fixed), (ull)packed->size);
            goto fail;
        }
    } else if (body >= packed->size) {
        say(why, size, "its section table ends at 0x%llx, where the NCZ does: no blocks or zstd stream after it",
            (ull)body);
        goto fail;
    }
    /* the struct, the sections (one more for a gap before the first) and the blocks' offsets, in one allocation */
    bytes = sizeof *z + (count + 1) * sizeof *z->sections + (blocks + 1) * sizeof *z->block_at;
    if (bytes > SIZE_MAX || !(z = calloc(1, (size_t)bytes))) {
        say(why, size, "out of memory for its %llu sections and %llu blocks, 0x%llx bytes", (ull)count, (ull)blocks,
            (ull)bytes);
        goto fail;
    }
    z->sections = (ncz_section_t *)(z + 1);
    z->block_at = (uint64_t *)(z->sections + count + 1);
    z->kind = kind;
    z->body = body;
    z->blocks = blocks;
    z->exponent = kind == NCZ_BLOCK ? fixed[11] : 0;
    if (read_sections(z, packed, table, count, why, size) ||
        (kind == NCZ_BLOCK && read_blocks(z, packed, fixed, why, size)))
        goto fail;
    z->packed = packed;
    z->nca.read = ncz_read;
    z->nca.size = NCZ_HEADER_SIZE + z->stream;
    z->in_at = body;
    return z;
fail:
    if (z) {
        memset(z->sections, 0, (size_t)(count + 1) * sizeof *z->sections); /* the keys */
        free(z);
    }
    storage_free(packed);
    return NULL;
}

void ncz_free(ncz_t *z) {
    if (!z) return;
    free_aes_ctx(z->aes);
    ZSTD_freeDCtx(z->zstd);
    free(z->kept);
    storage_free(z->packed);
    memset(z->sections, 0, (size_t)z->count * sizeof *z->sections); /* the keys */
    memset(z->aes_key, 0, sizeof z->aes_key);
    free(z);
}

int ncz_kind(const ncz_t *z) { return z->kind; }

storage_t *ncz_storage(ncz_t *z) { return &z->nca; }

void ncz_set_packed(ncz_t *z, storage_t *packed) {
    storage_free(z->packed);
    z->packed = packed;
}

void ncz_on_restart(void (*said)(const char *line, void *data), void *data) {
    g_ncz.said = said;
    g_ncz.data = data;
}

/* ------------------------------------------------------------------ kept blocks and pages */

/* what reads keep, made at the first that needs it: 1, or 0 said why */
static int keep(ncz_t *z) {
    uint64_t unit = unit_of(z), slot = min64(unit, z->stream), in, bytes;
    unsigned slots = NCZ_PAGES;
    if (z->kept) return 1;
    if (z->kind == NCZ_BLOCK) {
        uint64_t fit = NCZ_KEPT / unit;
        slots = fit < 2 ? 2 : fit > 16 ? 16 : (unsigned)fit;
    }
    in = z->kind == NCZ_BLOCK ? slot : ZSTD_DStreamInSize(); /* a stored block is at most what it holds */
    bytes = 2 * slots * sizeof *z->tag;
    if (slot > (UINT64_MAX - bytes - in) / slots || (bytes += slots * slot + in) > SIZE_MAX ||
        !(z->kept = calloc(1, (size_t)bytes))) {
        fprintf(stderr, "Out of memory for the NCZ's %u %s of 0x%llx bytes and its input, 0x%llx bytes\n", slots,
                z->kind == NCZ_BLOCK ? "blocks" : "stream pages", (ull)slot, (ull)(slots * slot + in));
        return 0;
    }
    if (!(z->zstd = ZSTD_createDCtx())) {
        free(z->kept);
        z->kept = NULL;
        fprintf(stderr, "zstd cannot make a decompression context for the NCZ\n");
        return 0;
    }
    z->tag = (uint64_t *)z->kept;
    z->used = z->tag + slots;
    z->slots = slots;
    z->slot = (size_t)slot;
    z->in = (unsigned char *)(z->used + slots) + slots * z->slot;
    z->in_size = (size_t)in;
    return 1;
}

static unsigned char *slot_data(const ncz_t *z, unsigned s) { return (unsigned char *)(z->used + z->slots) + s * z->slot; }

/* the slot unit u is kept in, else the one read longest ago, emptied, with *found 0 */
static unsigned slot_for(ncz_t *z, uint64_t u, int *found) {
    unsigned s, oldest = 0;
    for (s = 0; s < z->slots; s++) {
        if (z->tag[s] == u + 1) {
            *found = 1;
            return s;
        }
        if (z->used[s] < z->used[oldest]) oldest = s;
    }
    *found = 0;
    z->tag[oldest] = 0;
    return oldest;
}

static unsigned char *kept_as(ncz_t *z, unsigned s, uint64_t u) {
    z->tag[s] = u + 1;
    z->used[s] = ++z->tick;
    return slot_data(z, s);
}

/* block b, decompressed or as it is stored, or NULL said why */
static unsigned char *block_get(ncz_t *z, uint64_t b) {
    int found;
    unsigned s = slot_for(z, b, &found);
    uint64_t at = z->block_at[b], stored = z->block_at[b + 1] - at, holds = unit_size(z, b);
    unsigned char *data = slot_data(z, s);
    size_t made;
    if (found) return kept_as(z, s, b);
    if (stored == holds) { /* nsz keeps a block as it is when zstd makes it no smaller */
        if (!storage_read(z->packed, at, data, holds)) {
            fprintf(stderr, "NCZ block %llu, 0x%llx bytes stored as they are at 0x%llx, cannot be read\n", (ull)b,
                    (ull)stored, (ull)at);
            return NULL;
        }
        return kept_as(z, s, b);
    }
    if (!storage_read(z->packed, at, z->in, stored)) {
        fprintf(stderr, "NCZ block %llu, 0x%llx bytes at 0x%llx, cannot be read\n", (ull)b, (ull)stored, (ull)at);
        return NULL;
    }
    made = ZSTD_decompressDCtx(z->zstd, data, (size_t)holds, z->in, (size_t)stored);
    if (ZSTD_isError(made)) {
        fprintf(stderr, "NCZ block %llu, 0x%llx bytes at 0x%llx (the NCA's 0x%llx on), does not decompress: zstd says %s\n",
                (ull)b, (ull)stored, (ull)at, (ull)(NCZ_HEADER_SIZE + b * unit_of(z)), ZSTD_getErrorName(made));
        return NULL;
    }
    if (made != holds) {
        fprintf(stderr, "NCZ block %llu, 0x%llx bytes at 0x%llx (the NCA's 0x%llx on), decompresses to 0x%llx bytes, "
                "and the block holds 0x%llx\n", (ull)b, (ull)stored, (ull)at, (ull)(NCZ_HEADER_SIZE + b * unit_of(z)),
                (ull)made, (ull)holds);
        return NULL;
    }
    return kept_as(z, s, b);
}

/* the solid stream back at its start */
static void rewind_stream(ncz_t *z) {
    ZSTD_DCtx_reset(z->zstd, ZSTD_reset_session_only);
    z->in_at = z->body;
    z->in_pos = z->in_len = 0;
    z->out_at = 0;
}

/* the stream's next size bytes into data: 1, or 0 said why */
static int stream_next(ncz_t *z, unsigned char *data, size_t size) {
    ZSTD_outBuffer out = {data, size, 0};
    while (out.pos < out.size) {
        ZSTD_inBuffer in;
        size_t was = out.pos, left;
        if (z->in_pos == z->in_len) {
            uint64_t rest = z->packed->size - z->in_at;
            if (!rest) {
                fprintf(stderr, "The NCZ's zstd stream ends with the file, at 0x%llx, and its NCA's bytes from 0x%llx "
                        "are still to come: the NCZ is cut short\n", (ull)z->in_at,
                        (ull)(NCZ_HEADER_SIZE + z->out_at + out.pos));
                return 0;
            }
            z->in_len = (size_t)min64(rest, z->in_size);
            if (!storage_read(z->packed, z->in_at, z->in, z->in_len)) {
                fprintf(stderr, "The NCZ's zstd stream at 0x%llx cannot be read\n", (ull)z->in_at);
                return 0;
            }
            z->in_at += z->in_len;
            z->in_pos = 0;
        }
        in.src = z->in;
        in.size = z->in_len;
        in.pos = z->in_pos;
        left = ZSTD_decompressStream(z->zstd, &out, &in);
        if (ZSTD_isError(left) || (in.pos == z->in_pos && out.pos == was)) {
            fprintf(stderr, "The NCZ's zstd stream fails at 0x%llx, the NCA's 0x%llx: zstd says %s\n",
                    (ull)(z->in_at - z->in_len + in.pos), (ull)(NCZ_HEADER_SIZE + z->out_at + out.pos),
                    ZSTD_isError(left) ? ZSTD_getErrorName(left) : "nothing more, without an error");
            return 0;
        }
        z->in_pos = in.pos;
    }
    return 1;
}

/* page p of the solid stream, decoded as far as it, from its start again when it lies before where it got to, or NULL
   said why */
static unsigned char *page_get(ncz_t *z, uint64_t p) {
    int found;
    unsigned s = slot_for(z, p, &found);
    uint64_t start = p * NCZ_PAGE;
    if (found) return kept_as(z, s, p);
    if (start < z->out_at) {
        if (g_ncz.said) {
            char line[256];
            snprintf(line, sizeof line,
                     "a read at 0x%llx of a solid NCZ's NCA lies before the 0x%llx its stream reached: 0x%llx bytes "
                     "decoded again from its start",
                     (ull)(NCZ_HEADER_SIZE + start), (ull)(NCZ_HEADER_SIZE + z->out_at),
                     (ull)(start + unit_size(z, p)));
            g_ncz.said(line, g_ncz.data);
        }
        rewind_stream(z);
    }
    while (z->out_at <= start) { /* the pages up to it, each into its slot, which it ends holding */
        uint64_t n = unit_size(z, z->out_at / NCZ_PAGE);
        if (!stream_next(z, slot_data(z, s), (size_t)n)) {
            rewind_stream(z); /* its state is the failure's: the next read starts it over */
            return NULL;
        }
        z->out_at += n;
    }
    return kept_as(z, s, p);
}

/* [at, at + size) of the NCA, from NCZ_HEADER_SIZE on, encrypted again where its sections are AES-CTR */
static void encrypt_again(ncz_t *z, uint64_t at, unsigned char *data, uint64_t size) {
    uint64_t low = 0, high = z->count, end = at + size;
    while (high - low > 1) { /* the last section starting at or before at: the first starts at or before 0x4000 */
        uint64_t mid = low + (high - low) / 2;
        if (z->sections[mid].offset <= at)
            low = mid;
        else
            high = mid;
    }
    for (; low < z->count && at < end; low++) {
        const ncz_section_t *s = &z->sections[low];
        uint64_t stop = min64(end, s->offset + s->size), n;
        unsigned char iv[16];
        unsigned skip = (unsigned)(at % 16);
        if (stop <= at) continue; /* one of no bytes */
        n = stop - at;
        if (s->crypto == 3 || s->crypto == 4) {
            if (!z->aes || memcmp(z->aes_key, s->key, 16)) {
                free_aes_ctx(z->aes);
                z->aes = new_aes_ctx(s->key, 16, AES_MODE_CTR);
                memcpy(z->aes_key, s->key, 16);
            }
            memcpy(iv, s->counter, 8);
            if (skip) { /* a first block that starts before at: its keystream from its start */
                unsigned char block[16] = {0};
                uint64_t used = min64(16 - skip, n), counter = at / 16;
                for (unsigned i = 0; i < 8; i++) iv[15 - i] = (unsigned char)(counter >> 8 * i);
                memcpy(block + skip, data, (size_t)used);
                aes_setiv(z->aes, iv, sizeof iv);
                aes_encrypt(z->aes, block, block, sizeof block);
                memcpy(data, block + skip, (size_t)used);
                data += used;
                at += used;
                n -= used;
            }
            if (n) {
                uint64_t counter = at / 16;
                for (unsigned i = 0; i < 8; i++) iv[15 - i] = (unsigned char)(counter >> 8 * i);
                aes_setiv(z->aes, iv, sizeof iv);
                aes_encrypt(z->aes, data, data, (size_t)n);
            }
        }
        data += n;
        at += n;
    }
}

static int ncz_read(storage_t *storage, uint64_t offset, void *buffer, uint64_t size) {
    ncz_t *z = (ncz_t *)storage;
    unsigned char *out = buffer;
    uint64_t at;
    if (offset < NCZ_HEADER_SIZE) { /* the NCA's own bytes */
        uint64_t n = min64(size, NCZ_HEADER_SIZE - offset);
        if (!storage_read(z->packed, offset, out, n)) return 0;
        out += n;
        offset += n;
        size -= n;
    }
    if (!size) return 1;
    if (!keep(z)) return 0;
    for (at = offset; at < offset + size;) {
        uint64_t past = at - NCZ_HEADER_SIZE, u = past / unit_of(z), within = past - u * unit_of(z);
        uint64_t n = min64(offset + size - at, unit_size(z, u) - within);
        unsigned char *data = z->kind == NCZ_BLOCK ? block_get(z, u) : page_get(z, u);
        if (!data) return 0;
        memcpy(out + (at - offset), data + within, (size_t)n);
        at += n;
    }
    encrypt_again(z, offset, out, size);
    return 1;
}
