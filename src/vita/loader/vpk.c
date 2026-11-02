/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "vita/loader/vpk.h"
#include "vita/loader/inflate.h"
#include "common/utilities/common.h"
#include "common/utilities/endian.h"
#include <libdeflate.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EOCD_SIGNATURE    0x06054b50u
#define CENTRAL_SIGNATURE 0x02014b50u
#define LOCAL_SIGNATURE   0x04034b50u
#define MEMBER_LIMIT      (4ull * 1024 * 1024 * 1024) /* a member is read whole into memory */

typedef struct {
    char name[512];
    uint32_t method, crc, packed, size, local;
} Member;
typedef struct {
    FILE* file;
    unsigned char* directory; /* the central directory */
    uint32_t directory_size, count;
} Zip;

static bool read_at(FILE* f, uint64_t offset, void* out, size_t size) {
    return !snx_seek(f, offset) && fread(out, 1, size, f) == size;
}
static void zip_close(Zip* z) {
    if (z->file) fclose(z->file);
    free(z->directory);
    memset(z, 0, sizeof *z);
}
static bool zip_open(Zip* z, const char* path, char* error, size_t error_size) {
    memset(z, 0, sizeof *z);
    z->file = snx_fopen(path, "rb");
    if (!z->file) {
        char why[256];
        snprintf(error, error_size, "cannot open %s: %s", path, snx_os_error(why, sizeof why));
        return false;
    }
    uint64_t size = snx_file_size(path);
    /* the end of central directory record is in the last 64 KiB and 22 bytes (its comment's room) */
    size_t tail = size < 65557 ? (size_t)size : 65557;
    unsigned char* end = malloc(tail ? tail : 1);
    if (!end || !read_at(z->file, size - tail, end, tail)) {
        free(end);
        snprintf(error, error_size, "%s is not a VPK (a zip)", path);
        zip_close(z);
        return false;
    }
    bool found = false;
    uint32_t offset = 0;
    for (size_t i = tail >= 22 ? tail - 22 : 0; tail >= 22; --i) {
        if (snx_ld_le32(end + i) == EOCD_SIGNATURE) {
            z->count = snx_ld_le16(end + i + 10);
            z->directory_size = snx_ld_le32(end + i + 12);
            offset = snx_ld_le32(end + i + 16);
            found = true;
            break;
        }
        if (!i) break;
    }
    free(end);
    if (!found || z->count == 0xffff || offset == 0xffffffffu || (uint64_t)offset + z->directory_size > size) {
        snprintf(error, error_size, "%s is not a VPK: no zip directory (or a zip64 one)", path);
        zip_close(z);
        return false;
    }
    z->directory = malloc(z->directory_size ? z->directory_size : 1);
    if (!z->directory || !read_at(z->file, offset, z->directory, z->directory_size)) {
        snprintf(error, error_size, "cannot read the zip directory of %s", path);
        zip_close(z);
        return false;
    }
    return true;
}
/* The member at *cursor in the central directory, the cursor moved past it. False at its end or for a bad entry */
static bool zip_next(Zip* z, uint32_t* cursor, Member* m) {
    const unsigned char* p = z->directory + *cursor;
    if (*cursor + 46 > z->directory_size || snx_ld_le32(p) != CENTRAL_SIGNATURE) return false;
    uint32_t name = snx_ld_le16(p + 28), extra = snx_ld_le16(p + 30), comment = snx_ld_le16(p + 32);
    if (*cursor + 46 + name + extra + comment > z->directory_size || name >= sizeof m->name) return false;
    m->method = snx_ld_le16(p + 10);
    m->crc = snx_ld_le32(p + 16);
    m->packed = snx_ld_le32(p + 20);
    m->size = snx_ld_le32(p + 24);
    m->local = snx_ld_le32(p + 42);
    memcpy(m->name, p + 46, name);
    m->name[name] = 0;
    if (snx_ld_le16(p + 8) & 1) m->method = 0xffff; /* encrypted */
    *cursor += 46 + name + extra + comment;
    return true;
}
static unsigned char* zip_read(Zip* z, const Member* m, char* error, size_t error_size) {
    unsigned char local[30];
    if (m->method != 0 && m->method != 8) {
        snprintf(error, error_size, "%s is compressed with method %u (or encrypted), which a VPK doesn't use", m->name,
                 (unsigned)m->method);
        return NULL;
    }
    if (m->packed == 0xffffffffu || m->size == 0xffffffffu || (uint64_t)m->size > MEMBER_LIMIT) {
        snprintf(error, error_size, "%s is a zip64 member", m->name);
        return NULL;
    }
    if (!read_at(z->file, m->local, local, sizeof local) || snx_ld_le32(local) != LOCAL_SIGNATURE) {
        snprintf(error, error_size, "%s: bad local header", m->name);
        return NULL;
    }
    uint64_t data = (uint64_t)m->local + 30 + snx_ld_le16(local + 26) + snx_ld_le16(local + 28);
    unsigned char *packed = malloc(m->packed ? m->packed : 1), *out = malloc(m->size ? m->size : 1);
    bool ok = packed && out && read_at(z->file, data, packed, m->packed);
    if (ok && m->method == 0)
        ok = m->packed == m->size && (memcpy(out, packed, m->size), true);
    else if (ok)
        ok = vita_inflate(packed, m->packed, out, m->size);
    free(packed);
    if (ok && libdeflate_crc32(0, out, m->size) != m->crc) ok = false;
    if (!ok) {
        snprintf(error, error_size, "%s: cannot read or decompress it, or its CRC doesn't match", m->name);
        free(out);
        return NULL;
    }
    return out;
}
unsigned char* vita_vpk_member(const char* vpk, const char* name, size_t* size, char* error, size_t error_size) {
    Zip z;
    if (!zip_open(&z, vpk, error, error_size)) return NULL;
    Member m;
    unsigned char* data = NULL;
    snprintf(error, error_size, "%s has no %s", vpk, name);
    for (uint32_t cursor = 0, i = 0; i < z.count && zip_next(&z, &cursor, &m); ++i)
        if (!strcmp(m.name, name)) {
            data = zip_read(&z, &m, error, error_size);
            *size = m.size;
            break;
        }
    zip_close(&z);
    return data;
}
/* a member's name as a path inside the folder: relative, '/' between parts, no "." or ".." part */
static bool safe_name(const char* name) {
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
bool vita_vpk_extract(const char* vpk, const char* folder, char* error, size_t error_size) {
    Zip z;
    if (error_size) error[0] = 0;
    if (!zip_open(&z, vpk, error, error_size)) return false;
    bool ok = !snx_mkdirs(folder);
    if (!ok) snprintf(error, error_size, "cannot create %s", folder);
    Member m;
    uint32_t cursor = 0, i = 0;
    for (; ok && i < z.count && zip_next(&z, &cursor, &m); ++i) {
        if (!safe_name(m.name)) {
            snprintf(error, error_size, "%s names a file outside the game's folder: %s", vpk, m.name);
            ok = false;
            break;
        }
        char* path = snx_path_join(folder, m.name);
        size_t n = strlen(m.name);
        if (m.name[n - 1] == '/') { /* a folder */
            ok = path && !snx_mkdirs(path);
            if (!ok) snprintf(error, error_size, "cannot create %s", path ? path : m.name);
            free(path);
            continue;
        }
        char* parent = path ? snx_dirname(path) : NULL;
        unsigned char* data = parent && !snx_mkdirs(parent) ? zip_read(&z, &m, error, error_size) : NULL;
        if (!data && parent && !error[0]) snprintf(error, error_size, "cannot create %s", parent);
        ok = data && !snx_write_file(path, data, m.size);
        if (data && !ok) {
            char why[256];
            snprintf(error, error_size, "cannot write %s: %s", path, snx_os_error(why, sizeof why));
        }
        free(data);
        free(parent);
        free(path);
    }
    if (ok && i != z.count) {
        snprintf(error, error_size, "%s: bad zip directory entry %u", vpk, (unsigned)i);
        ok = false;
    }
    zip_close(&z);
    return ok;
}
char* vita_sfo_string(const unsigned char* sfo, size_t size, const char* key) {
    /* "\0PSF", version, the key table's offset, the data table's, the entries (16 bytes each from 20) */
    if (size < 20 || snx_ld_le32(sfo) != 0x46535000u) return NULL;
    uint32_t keys = snx_ld_le32(sfo + 8), data = snx_ld_le32(sfo + 12), count = snx_ld_le32(sfo + 16);
    if (keys > size || data > size || count > (size - 20) / 16) return NULL;
    for (uint32_t i = 0; i < count; ++i) {
        const unsigned char* e = sfo + 20 + i * 16;
        uint32_t name = keys + snx_ld_le16(e), format = snx_ld_le16(e + 2), length = snx_ld_le32(e + 4),
                 at = data + snx_ld_le32(e + 12);
        if (name >= size || memchr(sfo + name, 0, size - name) == NULL || strcmp((const char*)sfo + name, key))
            continue;
        if (format != 0x0204 || at > size || length > size - at || !length) return NULL;
        char* value = malloc(length + 1);
        if (!value) return NULL;
        memcpy(value, sfo + at, length);
        value[length] = 0;
        return value;
    }
    return NULL;
}
bool vita_title_id(const char* text) {
    if (!text || strlen(text) != 9) return false;
    for (unsigned i = 0; i < 9; ++i)
        if (i < 4 ? !(text[i] >= 'A' && text[i] <= 'Z') : !(text[i] >= '0' && text[i] <= '9')) return false;
    return true;
}
