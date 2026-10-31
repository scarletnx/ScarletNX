/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   An encrypted application game made into the decrypted one the loader reads. An installable CIA's layers, as 3dbrew
   documents them and Azahar decrypts them (core/hw/aes/key.cpp, file_sys/ncch_container.cpp, file_sys/ticket.cpp,
   GPL-2.0-or-later, read, not ported):
   - the CIA's: each content AES-128-CBC with the ticket's title key, whose IV is the content's index (big-endian, in
     the first two bytes); the title key itself AES-128-CBC with the common key the ticket names (slot 0x3D), its IV
     the title ID (big-endian, in the first eight bytes)
   - the NCCH's: AES-128-CTR. The ExHeader, the ExeFS header and the ExeFS's icon and banner take the primary key
     (slot 0x2C), the other ExeFS files and the RomFS the secondary key the crypto method names (0x2C, 0x25, 0x18 or
     0x1B). Both keys' KeyY is the first 16 bytes of the NCCH's signature; each section's counter starts from the
     partition ID and the section's type (1 ExHeader, 2 ExeFS, 3 RomFS), or for NCCH version 1 its byte offset.
   A normal key is the console's key scrambler of KeyX and KeyY: ((KeyX <<< 2) ^ KeyY) + C <<< 87, C the generator
   constant. Every key and the constant come from the user's aes_keys.txt (Azahar's format), none is in this code.
   The decrypted CIA's NCCHs say NoCrypto, and its TMD the contents' new hashes, so the loader checks it as any other
   decrypted CIA. A cartridge image (a CCI or a .3ds) and a bare NCCH (.cxi, an .app, homebrew's .hbb) have no CIA
   layer: their NCCHs' own is the same layer a CIA's contents carry, and it is taken off where each NCCH lies, the
   whole image copied through, the copy checked as any image is before it is published. */
#include "3ds/loader/internal.h"
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#define CHUNK (4u * 1024u * 1024u)
typedef struct {
    unsigned char x[0x40][16], y[0x40][16], common[6][16], generator[16];
    uint64_t have_x, have_y;
    unsigned have_common, have_generator;
} Keys;
/* name=32 hex digits; anything else on the line (comments, :AES sections, RSA moduli) is not a key read here */
static void key_line(Keys* k, const char* line) {
    const char* equals = strchr(line, '=');
    if (!equals) return;
    size_t name = (size_t)(equals - line), digits = 0;
    unsigned char key[16] = {0};
    for (const char* p = equals + 1; *p && !snx_is_space(*p); ++p, ++digits) {
        if (!snx_is_hex(*p) || digits >= 32) return;
        key[digits / 2] |= (unsigned char)(snx_hex_value(*p) << (digits & 1 ? 0 : 4));
    }
    if (digits != 32) return;
    unsigned slot;
    char kind[5];
    if (name == 17 && !strncmp(line, "generatorConstant", 17)) {
        memcpy(k->generator, key, 16);
        k->have_generator = 1;
    } else if (name == 7 && !strncmp(line, "common", 6) && line[6] >= '0' && line[6] <= '5') {
        memcpy(k->common[line[6] - '0'], key, 16);
        k->have_common |= 1u << (line[6] - '0');
    } else if (name == 12 && sscanf(line, "slot0x%2x%4[KeyXY]", &slot, kind) == 2 && slot < 0x40 &&
               (!strcmp(kind, "KeyX") || !strcmp(kind, "KeyY"))) {
        bool x = kind[3] == 'X';
        memcpy(x ? k->x[slot] : k->y[slot], key, 16);
        *(x ? &k->have_x : &k->have_y) |= UINT64_C(1) << slot;
    }
}
static bool keys_load(CtrReader* r, const char* path, Keys* k) {
    FILE* f = path ? fopen(path, "rb") : NULL;
    if (!f)
        return ctr_fail(r, SNX_CTR_EXTRACT_UNSUPPORTED,
                        "the game is encrypted, and there are no 3DS keys to decrypt it with at %s (aes_keys.txt in "
                        "Azahar's format, from your own console; --keys names another file)",
                        path ? path : "~/.3ds/aes_keys.txt");
    char line[1024];
    while (fgets(line, sizeof line, f)) key_line(k, line);
    fclose(f);
    if (!k->have_generator) return ctr_fail(r, SNX_CTR_EXTRACT_UNSUPPORTED, "%s has no generatorConstant", path);
    return true;
}
/* 128-bit big-endian rotate left by n (0 < n < 128) */
static void rol128(unsigned char out[16], const unsigned char in[16], unsigned n) {
    unsigned bytes = n / 8, bits = n % 8;
    for (unsigned i = 0; i < 16; ++i) {
        unsigned a = in[(i + bytes) % 16], b = in[(i + bytes + 1) % 16];
        out[i] = (unsigned char)(bits ? (a << bits | b >> (8 - bits)) : a);
    }
}
static void scramble(const Keys* k, const unsigned char x[16], const unsigned char y[16], unsigned char normal[16]) {
    unsigned char t[16];
    rol128(t, x, 2);
    unsigned carry = 0;
    for (int i = 15; i >= 0; --i) {
        unsigned sum = (unsigned)(t[i] ^ y[i]) + k->generator[i] + carry;
        t[i] = (unsigned char)sum;
        carry = sum >> 8;
    }
    rol128(normal, t, 87);
}
static bool slot_key(CtrReader* r, const Keys* k, unsigned slot, const unsigned char y[16], unsigned char out[16]) {
    if (!(k->have_x >> slot & 1))
        return ctr_fail(r, SNX_CTR_EXTRACT_UNSUPPORTED, "the 3DS keys have no slot0x%02XKeyX", slot);
    scramble(k, k->x[slot], y, out);
    return true;
}
static bool aes(CtrReader* r, const EVP_CIPHER* cipher, const unsigned char key[16], const unsigned char iv[16],
                const unsigned char* in, unsigned char* out, size_t size) {
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    int n = 0, end = 0;
    bool ok = ctx && EVP_DecryptInit_ex(ctx, cipher, NULL, key, iv) && EVP_CIPHER_CTX_set_padding(ctx, 0) &&
              EVP_DecryptUpdate(ctx, out, &n, in, (int)size) && EVP_DecryptFinal_ex(ctx, out + n, &end);
    EVP_CIPHER_CTX_free(ctx);
    return ok || ctr_fail(r, SNX_CTR_EXTRACT_IO, "AES failed");
}
/* the CTR keystream at byte offset at of a section whose counter starts at base, XORed over size bytes */
static bool ctr_xor(CtrReader* r, const unsigned char key[16], const unsigned char base[16], uint64_t at,
                    unsigned char* data, size_t size) {
    unsigned char counter[16];
    uint64_t blocks = at / 16;
    unsigned carry = 0;
    for (int i = 15; i >= 0; --i) {
        unsigned sum = base[i] + (unsigned)(blocks & 0xff) + carry;
        counter[i] = (unsigned char)sum;
        carry = sum >> 8;
        blocks >>= 8;
    }
    unsigned skip = (unsigned)(at % 16);
    unsigned char head[16] = {0};
    if (skip) { /* the partial first block: its keystream from a block of zeros */
        size_t n = size < 16u - skip ? size : 16u - skip;
        if (!aes(r, EVP_aes_128_ctr(), key, counter, head, head, 16)) return false;
        for (size_t i = 0; i < n; ++i) data[i] ^= head[skip + i];
        data += n, size -= n;
        for (int i = 15, c = 1; i >= 0 && c; --i) c = !++counter[i];
    }
    return !size || aes(r, EVP_aes_128_ctr(), key, counter, data, data, size);
}

typedef struct {
    uint64_t offset, size, origin; /* within the NCCH; the counter counts from origin, the section's start */
    unsigned char counter[16];
    unsigned key; /* 0 primary, 1 secondary */
} Range;
/* One content's NCCH layer, met in file order as the content streams through: its header first, then the ExeFS header
   before the ExeFS files it lays out */
typedef struct {
    bool active;
    unsigned char key[2][16];
    Range range[16];
    unsigned ranges;
    uint64_t exefs, exefs_size;
    unsigned char exefs_counter[16];
    bool exefs_laid_out;
} Ncch;
static void counter(const unsigned char* h, unsigned type, uint64_t offset, unsigned char out[16]) {
    memset(out, 0, 16);
    if (snx_ld_le16(h + 0x112) == 1) {
        memcpy(out, h + 0x108, 8);
        for (unsigned i = 0; i < 4; ++i) out[12 + i] = (unsigned char)(offset >> (24 - 8 * i));
    } else {
        for (unsigned i = 0; i < 8; ++i) out[i] = h[0x108 + 7 - i];
        out[8] = (unsigned char)type;
    }
}
static void add_range(Ncch* n, uint64_t offset, uint64_t size, uint64_t origin, const unsigned char base[16],
                      unsigned key) {
    if (!size || n->ranges == sizeof n->range / sizeof *n->range) return;
    Range* g = &n->range[n->ranges++];
    *g = (Range){.offset = offset, .size = size, .origin = origin, .key = key};
    memcpy(g->counter, base, 16);
}
/* the NCCH header of a content, decrypted by the CIA's layer: its sections' ranges and keys, and the header marked
   NoCrypto. A header that is not NCCH, or says NoCrypto already, leaves the content as it is */
static bool ncch_begin(CtrReader* r, const Keys* k, unsigned char* h, uint64_t content_size, Ncch* n) {
    *n = (Ncch){0};
    if (memcmp(h + 0x100, "NCCH", 4) || (h[0x18f] & 4)) return true;
    if (h[0x18f] & 0x20)
        return ctr_fail(r, SNX_CTR_EXTRACT_UNSUPPORTED, "NCCH seed crypto (a title's seed from seeddb) is not supported");
    if (h[0x18f] & 1) { /* FixedCryptoKey: zeros for an application */
        if ((snx_ld_le64(h + 0x118) >> 32) & 0x10)
            return ctr_fail(r, SNX_CTR_EXTRACT_UNSUPPORTED, "a system title's fixed NCCH key is not supported");
    } else {
        static const unsigned secondary[] = {[0x00] = 0x2c, [0x01] = 0x25, [0x0a] = 0x18, [0x0b] = 0x1b};
        unsigned method = h[0x18b];
        if (method >= sizeof secondary / sizeof *secondary || (method && !secondary[method]))
            return ctr_fail(r, SNX_CTR_EXTRACT_UNSUPPORTED, "unknown NCCH crypto method 0x%02x", method);
        if (!slot_key(r, k, 0x2c, h, n->key[0]) || !slot_key(r, k, secondary[method], h, n->key[1])) return false;
    }
    unsigned char base[16];
    uint64_t exheader = snx_ld_le32(h + 0x180);
    if (exheader) {
        counter(h, 1, 0x200, base);
        add_range(n, 0x200, 0x800, 0x200, base, 0);
    }
    n->exefs = (uint64_t)snx_ld_le32(h + 0x1a0) * 512, n->exefs_size = (uint64_t)snx_ld_le32(h + 0x1a4) * 512;
    if (n->exefs_size) {
        counter(h, 2, n->exefs, n->exefs_counter);
        add_range(n, n->exefs, 0x200, n->exefs, n->exefs_counter, 0); /* the ExeFS header: the files follow from it */
    }
    uint64_t romfs = (uint64_t)snx_ld_le32(h + 0x1b0) * 512, romfs_size = (uint64_t)snx_ld_le32(h + 0x1b4) * 512;
    if (romfs_size) {
        counter(h, 3, romfs, base);
        add_range(n, romfs, romfs_size, romfs, base, 1);
    }
    for (unsigned i = 0; i < n->ranges; ++i)
        if (!ctr_fits(content_size, n->range[i].offset, n->range[i].size))
            return ctr_fail(r, SNX_CTR_EXTRACT_FORMAT, "NCCH section exceeds its content");
    if (n->exefs_size && !ctr_fits(content_size, n->exefs, n->exefs_size))
        return ctr_fail(r, SNX_CTR_EXTRACT_FORMAT, "NCCH ExeFS exceeds its content");
    h[0x18b] = 0;
    h[0x18f] = (unsigned char)((h[0x18f] & ~0x21) | 4);
    n->active = true;
    return true;
}
/* the ExeFS's files, from its header once decrypted: the icon and banner under the primary key, the rest the
   secondary, each counted from the ExeFS's start */
static void exefs_files(Ncch* n, const unsigned char* header) {
    for (unsigned i = 0; i < 10; ++i) {
        const unsigned char* f = header + i * 16;
        uint64_t offset = snx_ld_le32(f + 8), size = snx_ld_le32(f + 12);
        if (!f[0] || !size || !ctr_fits(n->exefs_size - 0x200, offset, size)) continue;
        bool primary = !strncmp((const char*)f, "icon", 8) || !strncmp((const char*)f, "banner", 8);
        add_range(n, n->exefs + 0x200 + offset, size, n->exefs, n->exefs_counter, primary ? 0 : 1);
    }
    n->exefs_laid_out = true;
}
/* the ranges from first on, over NCCH bytes [at, at + size) */
static bool apply(CtrReader* r, const Ncch* n, unsigned first, uint64_t at, unsigned char* data, size_t size) {
    for (unsigned i = first; i < n->ranges; ++i) {
        const Range* g = &n->range[i];
        uint64_t from = g->offset > at ? g->offset : at;
        uint64_t to = g->offset + g->size < at + size ? g->offset + g->size : at + size;
        if (from < to &&
            !ctr_xor(r, n->key[g->key], g->counter, from - g->origin, data + (from - at), (size_t)(to - from)))
            return false;
    }
    return true;
}
/* NCCH bytes [at, at + size) of a content decrypted in place. The ExeFS header is a 0x200-byte block at a multiple of
   0x200, never split between chunks: once it is decrypted its files are laid out, and those in this chunk decrypted */
static bool ncch_chunk(CtrReader* r, Ncch* n, uint64_t at, unsigned char* data, size_t size) {
    if (!apply(r, n, 0, at, data, size)) return false;
    if (!n->exefs_size || n->exefs_laid_out || n->exefs < at || n->exefs + 0x200 > at + size) return true;
    unsigned first = n->ranges;
    exefs_files(n, data + (n->exefs - at));
    return apply(r, n, first, at, data, size);
}

static bool copy_span(CtrReader* r, FILE* out, uint64_t at, uint64_t size, unsigned char* buffer) {
    for (uint64_t done = 0; done < size;) {
        size_t n = size - done < CHUNK ? (size_t)(size - done) : CHUNK;
        if (!ctr_read(r, (CtrSpan){0, r->size}, at + done, buffer, n)) return false;
        if (fwrite(buffer, 1, n, out) != n) return ctr_fail(r, SNX_CTR_EXTRACT_IO, "cannot write the decrypted game");
        done += n;
    }
    return true;
}
static bool pad64(CtrReader* r, FILE* out) {
    static const unsigned char zero[64];
    int64_t at = snx_tell(out);
    size_t n = at < 0 ? 0 : (size_t)(snx_align_up((uint64_t)at, 64) - (uint64_t)at);
    return (at >= 0 && fwrite(zero, 1, n, out) == n) ||
           ctr_fail(r, SNX_CTR_EXTRACT_IO, "cannot write the decrypted game");
}
/* whether the CIA has a layer to decrypt: a content its TMD says is encrypted, or a present content's NCCH header
   without NoCrypto */
static bool cia_layers(CtrReader* r, const unsigned char* header, const unsigned char* tmd_body, size_t chunks,
                       uint64_t contents, uint64_t contents_size, bool* encrypted) {
    *encrypted = false;
    uint64_t at = contents;
    for (size_t i = 0; i < chunks && !*encrypted; ++i) {
        const unsigned char* chunk = tmd_body + 0x9c4 + i * 0x30;
        unsigned index = snx_ld_be16(chunk + 4);
        if (!(header[0x20 + (index >> 3)] & (0x80u >> (index & 7)))) continue;
        uint64_t size = snx_ld_be64(chunk + 8);
        unsigned char h[0x200];
        if (snx_ld_be16(chunk + 6) & 1)
            *encrypted = true;
        else if (size >= sizeof h && ctr_fits(contents + contents_size, at, size)) {
            if (!ctr_read(r, (CtrSpan){0, r->size}, at, h, sizeof h)) return false;
            *encrypted = !memcmp(h + 0x100, "NCCH", 4) && !(h[0x18f] & 4);
        }
        at += size;
    }
    return true;
}
/* the decrypted game checked as the loader checks any (hashes, bounds, the boot NCCH): keys that are not the
   console's decrypt to nonsense, which is never published */
static bool verified(CtrReader* r, const char* path) {
    /* the headers' and superblocks' hashes and every bound: wrong keys fail those. The contents' hashes are the ones the
       decryption itself just wrote */
    CtrReader check = {.error = r->error, .error_size = r->error_size, .quick = true};
    CtrSource s = {0};
    if (!(check.input = fopen(path, "rb"))) return ctr_fail(r, SNX_CTR_EXTRACT_IO, "cannot read %s", path);
    check.size = snx_file_size(path);
    bool ok = ctr_source_load(&check, &s) && (s.kind != CTR_KIND_CIA || ctr_cia_contents(&check, &s, NULL));
    ctr_source_free(&s);
    fclose(check.input);
    if (!ok) {
        char detail[256];
        snprintf(detail, sizeof detail, "%s", r->error_size ? r->error : "");
        ctr_fail(r, SNX_CTR_EXTRACT_FORMAT, "the decrypted game fails its checks (%s): are the keys your console's?",
                 detail);
    }
    return ok;
}

/* One NCCH of an image to decrypt: where it lies (a cartridge's partition, or a bare NCCH's whole file), its
   0x200 header with the NoCrypto the decryption writes into it, and the ranges ncch_begin laid out over it */
typedef struct {
    CtrSpan span;
    unsigned char header[0x200];
    Ncch ncch;
} Layer;
/* A cartridge image's or a bare NCCH's own layer, taken off where each NCCH lies: every partition that holds an
   NCCH is decrypted as a CIA's content is (ncch_begin lays its ranges out, ncch_chunk decrypts them), and the
   whole file copied through, so the copy reads as any image of its kind does, its NCCHs saying NoCrypto. An image
   with nothing encrypted is left alone, *encrypted false and nothing written; the keys are read only then. The
   copy is published only after it passes the checks the loader makes of any image, so keys that are not the
   console's never leave one behind. */
static bool ncch_image(CtrReader* r, const char* keys_path, const char* output, bool* encrypted) {
    unsigned char header[0x200];
    Layer* layers = calloc(8, sizeof *layers); /* a cartridge image's eight partitions, or a bare NCCH's one */
    FILE* out = NULL;
    char* stage = NULL;
    unsigned char* buffer = NULL;
    Keys* k = NULL;
    size_t count = 0, encrypted_count = 0;
    bool ok = false;
    if (!layers) return ctr_fail(r, SNX_CTR_EXTRACT_LIMIT, "out of memory");
    if (!ctr_read(r, (CtrSpan){0, r->size}, 0, header, sizeof header)) goto done;
    if (!memcmp(header + 0x100, "NCCH", 4)) {
        layers[count].span = (CtrSpan){0, r->size};
        memcpy(layers[count++].header, header, sizeof header);
    } else {
        for (unsigned index = 0; index < 8; ++index) {
            const unsigned char* p = header + 0x120 + index * 8;
            uint64_t at = (uint64_t)snx_ld_le32(p) * 0x200, size = (uint64_t)snx_ld_le32(p + 4) * 0x200;
            if (!size || !at || !ctr_fits(r->size, at, size)) continue;
            if (!ctr_read(r, (CtrSpan){0, r->size}, at, layers[count].header, sizeof layers[count].header))
                goto done;
            layers[count].span = (CtrSpan){at, size};
            ++count;
        }
    }
    for (size_t i = 0; i < count; ++i)
        if (!memcmp(layers[i].header + 0x100, "NCCH", 4) && !(layers[i].header[0x18f] & 4)) ++encrypted_count;
    if (!encrypted_count) {
        *encrypted = false; /* nothing to write: the image is read as it is */
        ok = true;
        goto done;
    }
    k = calloc(1, sizeof *k);
    if (!k) {
        ctr_fail(r, SNX_CTR_EXTRACT_LIMIT, "out of memory");
        goto done;
    }
    if (!keys_load(r, keys_path, k)) goto done;
    for (size_t i = 0; i < count; ++i) {
        if (memcmp(layers[i].header + 0x100, "NCCH", 4) || (layers[i].header[0x18f] & 4)) continue;
        /* the partition is the NCCH's container: its sections must fit in it, as a CIA content's do in it */
        if (!ncch_begin(r, k, layers[i].header, layers[i].span.size, &layers[i].ncch) || !layers[i].ncch.active)
            goto done;
    }
    stage = snx_format("%s.partial", output);
    if (!stage || !(out = fopen(stage, "wb"))) {
        ctr_fail(r, SNX_CTR_EXTRACT_IO, "cannot create %s", stage ? stage : output);
        goto done;
    }
    buffer = malloc(CHUNK);
    if (!buffer) {
        ctr_fail(r, SNX_CTR_EXTRACT_LIMIT, "out of memory");
        goto done;
    }
    for (uint64_t at = 0; at < r->size;) {
        size_t n = r->size - at < CHUNK ? (size_t)(r->size - at) : CHUNK;
        if (!ctr_read(r, (CtrSpan){0, r->size}, at, buffer, n)) goto done;
        for (size_t i = 0; i < count; ++i) {
            const Layer* l = &layers[i];
            if (!l->ncch.active) continue;
            uint64_t from = l->span.offset > at ? l->span.offset : at;
            uint64_t to = l->span.offset + l->span.size < at + n ? l->span.offset + l->span.size : at + n;
            if (from < to &&
                !ncch_chunk(r, &layers[i].ncch, from - l->span.offset, buffer + (from - at), (size_t)(to - from)))
                goto done;
            /* the header itself is plain, and carries the NoCrypto the decryption gave it */
            if (l->span.offset >= at && l->span.offset < at + n)
                memcpy(buffer + (l->span.offset - at), l->header, sizeof l->header);
        }
        if (fwrite(buffer, 1, n, out) != n) {
            ctr_fail(r, SNX_CTR_EXTRACT_IO, "cannot write the decrypted game");
            goto done;
        }
        at += n;
    }
    if (fclose(out)) {
        out = NULL;
        ctr_fail(r, SNX_CTR_EXTRACT_IO, "cannot write the decrypted game");
        goto done;
    }
    out = NULL;
    if (!verified(r, stage)) goto done;
    if (snx_replace(stage, output)) {
        ctr_fail(r, SNX_CTR_EXTRACT_IO, "cannot move %s to %s", stage, output);
        goto done;
    }
    *encrypted = true;
    ok = true;
done:
    if (out) fclose(out);
    if (!ok && stage) remove(stage);
    if (k) OPENSSL_cleanse(k, sizeof *k);
    free(k);
    free(buffer);
    free(stage);
    free(layers);
    return ok;
}
static bool decrypt(CtrReader* r, const char* keys_path, const char* output, bool* encrypted) {
    unsigned char header[0x2020], *ticket = NULL, *tmd = NULL, *buffer = NULL, title_key[16];
    FILE* out = NULL;
    char* stage = NULL;
    bool ok = false;
    Keys* k = calloc(1, sizeof *k);
    if (!k) return ctr_fail(r, SNX_CTR_EXTRACT_LIMIT, "out of memory");
    /* a file too short for any container's header is malformed data, as the loader's own read of it says */
    if (r->size < 0x200) {
        ctr_fail(r, SNX_CTR_EXTRACT_FORMAT, "truncated or out-of-range archive section");
        goto done;
    }
    /* a bundle first: the game is one of its entries, and the layers come off that entry */
    bool tar = false;
    if (!ctr_tar_game(r, &tar)) goto done;
    if (!ctr_read(r, (CtrSpan){0, r->size}, 0, header, sizeof header)) goto done;
    /* A cartridge image and a bare NCCH have no CIA layer: their NCCHs' own is the same layer the CIA's contents
       carry, taken off where each NCCH lies, and the whole image copied through with it */
    if (!memcmp(header + 0x100, "NCSD", 4) || !memcmp(header + 0x100, "NCCH", 4)) {
        ok = ncch_image(r, keys_path, output, encrypted);
        goto done;
    }
    if (snx_ld_le32(header) != 0x2020) {
        ctr_fail(r, SNX_CTR_EXTRACT_FORMAT,
                 "the game is not a CIA, a cartridge image (a CCI or a .3ds) or an NCCH");
        goto done;
    }
    uint64_t cert = 0x2040, cert_size = snx_ld_le32(header + 8);
    uint64_t tik = snx_align_up(cert + cert_size, 64), tik_size = snx_ld_le32(header + 12);
    uint64_t tmd_at = snx_align_up(tik + tik_size, 64), tmd_size = snx_ld_le32(header + 16);
    uint64_t contents = snx_align_up(tmd_at + tmd_size, 64), contents_size = snx_ld_le64(header + 24);
    uint64_t meta = snx_align_up(contents + contents_size, 64), meta_size = snx_ld_le32(header + 20);
    if (tik_size > CTR_METADATA_LIMIT || tmd_size > CTR_METADATA_LIMIT || !ctr_fits(r->size, tik, tik_size) ||
        !ctr_fits(r->size, tmd_at, tmd_size) || !ctr_fits(r->size, contents, contents_size) ||
        (meta_size && !ctr_fits(r->size, meta, meta_size))) {
        ctr_fail(r, SNX_CTR_EXTRACT_FORMAT, "CIA section exceeds input size");
        goto done;
    }
    ticket = malloc((size_t)tik_size + 1), tmd = malloc((size_t)tmd_size + 1), buffer = malloc(CHUNK);
    if (!ticket || !tmd || !buffer) {
        ctr_fail(r, SNX_CTR_EXTRACT_LIMIT, "out of memory");
        goto done;
    }
    if (!ctr_read(r, (CtrSpan){0, r->size}, tik, ticket, (size_t)tik_size) ||
        !ctr_read(r, (CtrSpan){0, r->size}, tmd_at, tmd, (size_t)tmd_size))
        goto done;
    size_t tb = tik_size >= 4 ? ctr_signed_body(snx_ld_be32(ticket)) : 0,
        mb = tmd_size >= 4 ? ctr_signed_body(snx_ld_be32(tmd)) : 0;
    if (!tb || !mb || !ctr_fits(tik_size, tb, 0xb2) || !ctr_fits(tmd_size, mb, 0x9c4)) {
        ctr_fail(r, SNX_CTR_EXTRACT_FORMAT, "invalid ticket/TMD signature");
        goto done;
    }
    unsigned char* body = tmd + mb;
    size_t chunks = snx_ld_be16(body + 0x9e);
    if (!chunks || !ctr_fits(tmd_size, mb + 0x9c4, chunks * 0x30)) {
        ctr_fail(r, SNX_CTR_EXTRACT_FORMAT, "invalid TMD content count");
        goto done;
    }
    if (!cia_layers(r, header, body, chunks, contents, contents_size, encrypted)) goto done;
    if (!*encrypted) {
        ok = true;
        goto done;
    }
    if (!keys_load(r, keys_path, k)) goto done;
    /* the title key, under the common key the ticket names */
    unsigned common = ticket[tb + 0xb1];
    if (common > 5 || !(k->have_common >> common & 1)) {
        ctr_fail(r, SNX_CTR_EXTRACT_UNSUPPORTED, "the 3DS keys have no common%u, which the ticket names", common);
        goto done;
    }
    unsigned char common_key[16], iv[16] = {0};
    if (!slot_key(r, k, 0x3d, k->common[common], common_key)) goto done;
    memcpy(iv, ticket + tb + 0x9c, 8);
    if (!aes(r, EVP_aes_128_cbc(), common_key, iv, ticket + tb + 0x7f, title_key, 16)) goto done;
    stage = snx_format("%s.partial", output);
    if (!stage || !(out = fopen(stage, "wb"))) {
        ctr_fail(r, SNX_CTR_EXTRACT_IO, "cannot create %s", stage ? stage : output);
        goto done;
    }
    /* everything before the contents as it is, the TMD written again at the end with the new hashes */
    if (!copy_span(r, out, 0, contents, buffer)) goto done;
    uint64_t at = contents;
    for (size_t i = 0; i < chunks; ++i) {
        unsigned char* chunk = body + 0x9c4 + i * 0x30;
        unsigned index = snx_ld_be16(chunk + 4);
        if (!(header[0x20 + (index >> 3)] & (0x80u >> (index & 7)))) continue;
        uint64_t size = snx_ld_be64(chunk + 8);
        bool cbc = snx_ld_be16(chunk + 6) & 1;
        if (!ctr_fits(contents + contents_size, at, size) || (cbc && size % 16)) {
            ctr_fail(r, SNX_CTR_EXTRACT_FORMAT, "invalid CIA content length");
            goto done;
        }
        unsigned char content_iv[16] = {(unsigned char)(index >> 8), (unsigned char)index};
        EVP_CIPHER_CTX* ctx = cbc ? EVP_CIPHER_CTX_new() : NULL;
        EVP_MD_CTX* md = EVP_MD_CTX_new();
        Ncch ncch = {0};
        bool good = md && EVP_DigestInit_ex(md, EVP_sha256(), NULL) &&
                    (!cbc || (ctx && EVP_DecryptInit_ex(ctx, EVP_aes_128_cbc(), NULL, title_key, content_iv) &&
                              EVP_CIPHER_CTX_set_padding(ctx, 0)));
        for (uint64_t done = 0; good && done < size;) {
            size_t n = size - done < CHUNK ? (size_t)(size - done) : CHUNK;
            int produced = 0;
            good = ctr_read(r, (CtrSpan){0, r->size}, at + done, buffer, n);
            if (good && cbc)
                good = EVP_DecryptUpdate(ctx, buffer, &produced, buffer, (int)n) && (size_t)produced == n;
            if (good && !done) good = n >= 0x200 && ncch_begin(r, k, buffer, size, &ncch);
            if (good && ncch.active) good = ncch_chunk(r, &ncch, done, buffer, n);
            good = good && EVP_DigestUpdate(md, buffer, n) && fwrite(buffer, 1, n, out) == n;
            done += n;
        }
        unsigned length = 0;
        good = good && EVP_DigestFinal_ex(md, chunk + 16, &length) && length == 32;
        EVP_CIPHER_CTX_free(ctx);
        EVP_MD_CTX_free(md);
        if (!good) {
            if (r->result == SNX_CTR_EXTRACT_OK) ctr_fail(r, SNX_CTR_EXTRACT_IO, "cannot decrypt content %zu", i);
            goto done;
        }
        chunk[7] &= (unsigned char)~1; /* no longer encrypted */
        at += size;
    }
    /* the content-info records hash the chunk records, the TMD's header the records */
    for (size_t i = 0; i < 64; ++i) {
        unsigned char* info = body + 0xc4 + i * 0x24;
        size_t first = snx_ld_be16(info), count = snx_ld_be16(info + 2);
        if (count && first < chunks && count <= chunks - first &&
            !ctr_sha256(r, body + 0x9c4 + first * 0x30, count * 0x30, info + 4))
            goto done;
    }
    if (!ctr_sha256(r, body + 0xc4, 64 * 0x24, body + 0xa4)) goto done;
    if (at != contents + contents_size || !pad64(r, out) ||
        (meta_size && !copy_span(r, out, meta, meta_size, buffer))) {
        if (r->result == SNX_CTR_EXTRACT_OK) ctr_fail(r, SNX_CTR_EXTRACT_FORMAT, "CIA content sizes do not add up");
        goto done;
    }
    if (snx_seek(out, tmd_at) || fwrite(tmd, 1, (size_t)tmd_size, out) != tmd_size || fclose(out)) {
        out = NULL;
        ctr_fail(r, SNX_CTR_EXTRACT_IO, "cannot write the decrypted game");
        goto done;
    }
    out = NULL;
    if (!verified(r, stage)) goto done;
    if (snx_replace(stage, output)) {
        ctr_fail(r, SNX_CTR_EXTRACT_IO, "cannot move %s to %s", stage, output);
        goto done;
    }
    ok = true;
done:
    if (out) fclose(out);
    if (!ok && stage) remove(stage);
    OPENSSL_cleanse(title_key, sizeof title_key);
    if (k) OPENSSL_cleanse(k, sizeof *k);
    free(k);
    free(stage);
    free(ticket);
    free(tmd);
    free(buffer);
    return ok;
}
SnxCtrExtractResult snx_ctr_decrypt(const char* input, const char* keys, const char* output, bool* encrypted,
                                    char* error, size_t error_size) {
    CtrReader r = {.error = error, .error_size = error_size};
    if (error_size) error[0] = 0;
    *encrypted = false;
    if (!input || !(r.input = fopen(input, "rb"))) {
        ctr_fail(&r, SNX_CTR_EXTRACT_IO, "cannot open the game");
        return r.result;
    }
    r.size = snx_file_size(input);
    decrypt(&r, keys, output, encrypted);
    fclose(r.input);
    return r.result;
}
