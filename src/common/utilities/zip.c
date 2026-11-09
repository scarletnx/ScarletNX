/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "common/utilities/zip.h"
#include "common/utilities/common.h"
#include "common/utilities/endian.h"
#include "common/platform/file.h"
#include <libdeflate.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EOCD_SIGNATURE    0x06054b50u
#define CENTRAL_SIGNATURE 0x02014b50u
#define LOCAL_SIGNATURE   0x04034b50u
#define MEMBER_LIMIT      0xfffffffeu /* a member is read whole into memory, its size and a zero byte */

struct Sorted {
    const char* name;
    uint32_t index;
};
typedef struct {
    uint32_t name;   /* its name's offset in names */
    uint32_t method, crc, packed, size, local;
} Member;
struct SnxZip {
    FILE* file;
    uint32_t count;
    Member* members;  /* the directory's order */
    struct Sorted* sorted; /* the members by name */
    char* names;      /* each name, zero-terminated */
};

/* a read at an offset that leaves the stream's position alone (plat_pread): threads read members of one zip at once
   (an app's resources on one, its assets on another) */
static bool read_at(FILE* f, uint64_t offset, void* out, size_t size) {
    return plat_pread(f, out, size, offset) == (int64_t)size;
}
void snx_zip_close(SnxZip* zip) {
    if (!zip) return;
    if (zip->file) fclose(zip->file);
    free(zip->members);
    free(zip->sorted);
    free(zip->names);
    free(zip);
}
static int by_name(const void* a, const void* b) {
    return strcmp(((const struct Sorted*)a)->name, ((const struct Sorted*)b)->name);
}
/* the directory's entries into zip, false for a bad one */
static bool index_directory(SnxZip* zip, const unsigned char* directory, uint32_t directory_size) {
    zip->members = calloc(zip->count ? zip->count : 1, sizeof *zip->members);
    zip->sorted = calloc(zip->count ? zip->count : 1, sizeof *zip->sorted);
    zip->names = malloc((size_t)directory_size + 1); /* the names fit in the directory that holds them */
    if (!zip->members || !zip->sorted || !zip->names) return false;
    uint32_t cursor = 0, names = 0;
    for (uint32_t i = 0; i < zip->count; ++i) {
        const unsigned char* p = directory + cursor;
        if ((uint64_t)cursor + 46 > directory_size || snx_ld_le32(p) != CENTRAL_SIGNATURE) return false;
        uint32_t name = snx_ld_le16(p + 28), extra = snx_ld_le16(p + 30), comment = snx_ld_le16(p + 32);
        if ((uint64_t)cursor + 46 + name + extra + comment > directory_size) return false;
        Member* m = &zip->members[i];
        m->method = snx_ld_le16(p + 10);
        if (snx_ld_le16(p + 8) & 1) m->method = 0xffff; /* encrypted */
        m->crc = snx_ld_le32(p + 16);
        m->packed = snx_ld_le32(p + 20);
        m->size = snx_ld_le32(p + 24);
        m->local = snx_ld_le32(p + 42);
        m->name = names;
        memcpy(zip->names + names, p + 46, name);
        names += name;
        zip->names[names++] = 0;
        cursor += 46 + name + extra + comment;
    }
    for (uint32_t i = 0; i < zip->count; ++i) zip->sorted[i] = (struct Sorted){zip->names + zip->members[i].name, i};
    qsort(zip->sorted, zip->count, sizeof *zip->sorted, by_name);
    return true;
}
SnxZip* snx_zip_open(const char* path, char* error, size_t error_size) {
    SnxZip* zip = calloc(1, sizeof *zip);
    if (!zip) {
        snprintf(error, error_size, "out of memory");
        return NULL;
    }
    zip->file = snx_fopen(path, "rb");
    if (!zip->file) {
        char why[256];
        snprintf(error, error_size, "cannot open %s: %s", path, snx_os_error(why, sizeof why));
        snx_zip_close(zip);
        return NULL;
    }
    uint64_t size = snx_file_size(path);
    /* the end of central directory record is in the last 64 KiB and 22 bytes (its comment's room) */
    size_t tail = size < 65557 ? (size_t)size : 65557;
    unsigned char* end = malloc(tail ? tail : 1);
    if (!end || !read_at(zip->file, size - tail, end, tail)) {
        free(end);
        snprintf(error, error_size, "%s is not a zip", path);
        snx_zip_close(zip);
        return NULL;
    }
    bool found = false;
    uint32_t offset = 0, directory_size = 0;
    for (size_t i = tail >= 22 ? tail - 22 : 0; tail >= 22; --i) {
        if (snx_ld_le32(end + i) == EOCD_SIGNATURE) {
            zip->count = snx_ld_le16(end + i + 10);
            directory_size = snx_ld_le32(end + i + 12);
            offset = snx_ld_le32(end + i + 16);
            found = true;
            break;
        }
        if (!i) break;
    }
    free(end);
    if (!found || zip->count == 0xffff || offset == 0xffffffffu || (uint64_t)offset + directory_size > size) {
        snprintf(error, error_size, "%s has no zip directory (or a zip64 one)", path);
        snx_zip_close(zip);
        return NULL;
    }
    unsigned char* directory = malloc(directory_size ? directory_size : 1);
    bool ok = directory && read_at(zip->file, offset, directory, directory_size) &&
              index_directory(zip, directory, directory_size);
    free(directory);
    if (!ok) {
        snprintf(error, error_size, "cannot read the zip directory of %s", path);
        snx_zip_close(zip);
        return NULL;
    }
    return zip;
}
uint32_t snx_zip_count(const SnxZip* zip) {
    return zip->count;
}
const char* snx_zip_name(const SnxZip* zip, uint32_t index) {
    return zip->names + zip->members[index].name;
}
uint32_t snx_zip_size(const SnxZip* zip, uint32_t index) {
    return zip->members[index].size;
}
bool snx_zip_find(const SnxZip* zip, const char* name, uint32_t* index) {
    uint32_t low = 0, high = zip->count;
    while (low < high) {
        uint32_t mid = low + (high - low) / 2;
        int order = strcmp(zip->sorted[mid].name, name);
        if (!order) {
            *index = zip->sorted[mid].index;
            return true;
        }
        if (order < 0)
            low = mid + 1;
        else
            high = mid;
    }
    return false;
}
unsigned char* snx_zip_read(SnxZip* zip, uint32_t index, size_t* size, char* error, size_t error_size) {
    const Member* m = &zip->members[index];
    const char* name = zip->names + m->name;
    unsigned char local[30];
    if (m->method != 0 && m->method != 8) {
        snprintf(error, error_size, "%s is compressed with method %u (or encrypted)", name, (unsigned)m->method);
        return NULL;
    }
    if (m->packed == 0xffffffffu || m->size > MEMBER_LIMIT) {
        snprintf(error, error_size, "%s is a zip64 member", name);
        return NULL;
    }
    if (!read_at(zip->file, m->local, local, sizeof local) || snx_ld_le32(local) != LOCAL_SIGNATURE) {
        snprintf(error, error_size, "%s: bad local header", name);
        return NULL;
    }
    uint64_t data = (uint64_t)m->local + 30 + snx_ld_le16(local + 26) + snx_ld_le16(local + 28);
    unsigned char* out = malloc((size_t)m->size + 1);
    unsigned char* packed = m->method && out ? malloc(m->packed ? m->packed : 1) : NULL;
    bool ok = out && (m->method ? packed && read_at(zip->file, data, packed, m->packed)
                                : m->packed == m->size && read_at(zip->file, data, out, m->size));
    if (ok && m->method) {
        struct libdeflate_decompressor* d = libdeflate_alloc_decompressor();
        /* libdeflate refuses a stream that ends short of the size or needs more room */
        ok = d && libdeflate_deflate_decompress(d, packed, m->packed, out, m->size, NULL) == LIBDEFLATE_SUCCESS;
        libdeflate_free_decompressor(d);
    }
    free(packed);
    if (ok && libdeflate_crc32(0, out, m->size) != m->crc) ok = false;
    if (!ok) {
        snprintf(error, error_size, "%s: cannot read or decompress it, or its CRC doesn't match", name);
        free(out);
        return NULL;
    }
    out[m->size] = 0;
    *size = m->size;
    return out;
}
bool snx_zip_stored_at(SnxZip* zip, uint32_t index, uint64_t* offset) {
    const Member* m = &zip->members[index];
    unsigned char local[30];
    if (m->method != 0 || m->packed != m->size || !read_at(zip->file, m->local, local, sizeof local) ||
        snx_ld_le32(local) != LOCAL_SIGNATURE)
        return false;
    *offset = (uint64_t)m->local + 30 + snx_ld_le16(local + 26) + snx_ld_le16(local + 28);
    return true;
}
bool snx_zip_extract(SnxZip* zip, uint32_t index, const char* path, char* error, size_t error_size) {
    const Member* m = &zip->members[index];
    const char* name = zip->names + m->name;
    if (m->method != 0) {
        size_t size;
        unsigned char* data = snx_zip_read(zip, index, &size, error, error_size);
        if (!data) return false;
        bool ok = !snx_write_file(path, data, size);
        free(data);
        if (!ok) snprintf(error, error_size, "cannot write %s", path);
        return ok;
    }
    unsigned char local[30];
    if (m->packed != m->size || m->packed == 0xffffffffu || !read_at(zip->file, m->local, local, sizeof local) ||
        snx_ld_le32(local) != LOCAL_SIGNATURE) {
        snprintf(error, error_size, "%s: bad local header", name);
        return false;
    }
    uint64_t data = (uint64_t)m->local + 30 + snx_ld_le16(local + 26) + snx_ld_le16(local + 28);
    FILE* out = snx_fopen(path, "wb");
    enum { PIECE = 1 << 20 };
    unsigned char* buffer = malloc(PIECE);
    uint32_t crc = 0;
    bool ok = out && buffer;
    for (uint32_t left = m->size; ok && left;) {
        size_t n = left < PIECE ? left : PIECE;
        ok = read_at(zip->file, data + (m->size - left), buffer, n) && fwrite(buffer, 1, n, out) == n;
        crc = libdeflate_crc32(crc, buffer, n);
        left -= (uint32_t)n;
    }
    free(buffer);
    if (out && fclose(out)) ok = false;
    if (ok && crc != m->crc) ok = false;
    if (!ok) {
        snprintf(error, error_size, "%s: cannot copy it to %s, or its CRC doesn't match", name, path);
        remove(path);
    }
    return ok;
}
unsigned char* snx_zip_member(const char* path, const char* name, size_t* size, char* error, size_t error_size) {
    SnxZip* zip = snx_zip_open(path, error, error_size);
    if (!zip) return NULL;
    uint32_t index;
    unsigned char* data = NULL;
    if (snx_zip_find(zip, name, &index))
        data = snx_zip_read(zip, index, size, error, error_size);
    else
        snprintf(error, error_size, "%s has no %s", path, name);
    snx_zip_close(zip);
    return data;
}
bool snx_zip_safe_name(const char* name) {
    if (!name[0] || name[0] == '/' || strchr(name, '\\') || strchr(name, ':')) return false;
    for (const char* p = name; *p;) {
        const char* slash = strchr(p, '/');
        size_t n = slash ? (size_t)(slash - p) : strlen(p);
        if ((n == 1 && p[0] == '.') || (n == 2 && p[0] == '.' && p[1] == '.')) return false;
        for (size_t i = 0; i < n; ++i)
            if ((unsigned char)p[i] < 32) return false;
        if (!slash) break;
        p = slash + 1;
    }
    return true;
}
