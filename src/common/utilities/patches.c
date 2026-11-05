/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* IPS, IPS32 and IPSwitch patches (patches.h), read as Eden's ips_layer.cpp reads them. Where Eden logs a line or a
   record it cannot use and goes on, this refuses the patch and says which */
#include "common/utilities/patches.h"
#include "common/utilities/mods.h"
#include "common/utilities/ascii.h"
#include "common/utilities/bits.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fail(char* error, size_t size, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (size) vsnprintf(error, size, fmt, ap);
    va_end(ap);
    return 1;
}
static const char* leaf(const char* path) {
    const char* at = path + strlen(path);
    while (at > path && !snx_is_any_separator(at[-1])) --at;
    return at;
}

size_t snx_mods_patches(const char* folder, const char* off, char*** paths) {
    return snx_mods_patches_of(folder, NULL, off, paths);
}
size_t snx_mods_patches_of(const char* folder, const char* own, const char* off, char*** paths) {
    char** exefs;
    size_t count = snx_mods_played_of(folder, own, off, "exefs", &exefs), n = 0;
    *paths = NULL;
    for (size_t i = 0; i < count; ++i) {
        char** files = NULL;
        size_t found = 0;
        if (snx_list_dir(exefs[i], &files, &found)) continue;
        if (found) qsort(files, found, sizeof *files, snx_compare_strings);
        for (size_t j = 0; j < found; ++j) {
            char* path = snx_path_join(exefs[i], files[j]);
            if (files[j][0] == '.' ||
                !(snx_ends_with_nocase(files[j], ".ips") || snx_ends_with_nocase(files[j], ".pchtxt")) ||
                !snx_is_file(path)) {
                free(path);
                continue;
            }
            *paths = (char**)snx_realloc(*paths, (n + 1) * sizeof **paths);
            (*paths)[n++] = path;
        }
        snx_free_strings(files, found);
    }
    snx_free_strings(exefs, count);
    return n;
}

/* n bytes at at, as patches address the module (its header, then image), written from data or, without it, all
   fill: those past the header alone. 1 when they end past the image, nothing written */
static int place(unsigned char* image, size_t size, unsigned long long at, const unsigned char* data, int fill,
                 size_t n) {
    if (at + n > SNX_PATCH_HEADER + (unsigned long long)size) return 1;
    if (at + n <= SNX_PATCH_HEADER) return 0;
    if (at < SNX_PATCH_HEADER) {
        size_t skip = (size_t)(SNX_PATCH_HEADER - at);
        if (data) data += skip;
        n -= skip;
        at = SNX_PATCH_HEADER;
    }
    if (data)
        memcpy(image + (at - SNX_PATCH_HEADER), data, n);
    else
        memset(image + (at - SNX_PATCH_HEADER), fill, n);
    return 0;
}
static unsigned big_endian(const unsigned char* p, unsigned n) {
    unsigned v = 0;
    for (unsigned i = 0; i < n; ++i) v = v << 8 | p[i];
    return v;
}

int snx_ips_apply(const unsigned char* ips, size_t ips_size, unsigned char* image, size_t size, char* error,
                  size_t error_size) {
    unsigned wide;
    size_t at = 5, record;
    if (ips_size >= 5 && !memcmp(ips, "PATCH", 5))
        wide = 0;
    else if (ips_size >= 5 && !memcmp(ips, "IPS32", 5))
        wide = 1;
    else
        return fail(error, error_size, "no IPS patch: it starts with neither PATCH nor IPS32");
    unsigned w = wide ? 4 : 3;
    const char* kind = wide ? "IPS32" : "IPS";
    const char* close = wide ? "EEOF" : "EOF";
    for (record = 1;; ++record) {
        if (at + w > ips_size)
            return fail(error, error_size, "the %s patch ends at its byte %zu, before the %s that closes it", kind,
                        ips_size, close);
        if (!memcmp(ips + at, close, w)) return 0;
        unsigned long long offset = big_endian(ips + at, w);
        at += w;
        if (at + 2 > ips_size) break;
        unsigned n = big_endian(ips + at, 2);
        at += 2;
        if (!n) { /* a run of one byte */
            if (at + 3 > ips_size) break;
            n = big_endian(ips + at, 2);
            if (place(image, size, offset, NULL, ips[at + 2], n))
                return fail(error, error_size,
                            "record %zu writes 0x%X bytes at 0x%llX, past the module's end at 0x%llX", record, n,
                            offset, SNX_PATCH_HEADER + (unsigned long long)size);
            at += 3;
        } else {
            if (at + n > ips_size) break;
            if (place(image, size, offset, ips + at, 0, n))
                return fail(error, error_size,
                            "record %zu writes 0x%X bytes at 0x%llX, past the module's end at 0x%llX", record, n,
                            offset, SNX_PATCH_HEADER + (unsigned long long)size);
            at += n;
        }
    }
    return fail(error, error_size, "the %s patch ends at its byte %zu, inside its record %zu", kind, ips_size, record);
}

/* ---- IPSwitch text: a line at a time, as Eden's IPSwitchCompiler reads it ---- */

typedef struct {
    unsigned long long at;
    unsigned char* data;
    size_t n;
    int line;
} Record;
typedef struct {
    Record* records;
    size_t count;
    int enabled;
} Patch;
typedef struct {
    Patch* patches;
    size_t count;
    unsigned char build_id[32];
    int has_id;
    long long shift; /* @flag offset_shift */
} Text;

static void text_free(Text* t) {
    for (size_t i = 0; i < t->count; ++i) {
        for (size_t j = 0; j < t->patches[i].count; ++j) free(t->patches[i].records[j].data);
        free(t->patches[i].records);
    }
    free(t->patches);
}
/* the next line of text from *at, with its spaces around it and its comment (from a / or a # outside quotes) taken
   out, into out (a buffer of the line's size and one); its number from the line feeds before it. 0 at the end */
static int next_line(const char* text, size_t size, size_t* at, int* number, char** out) {
    size_t end, p, last;
    char quote = 0;
    if (*at >= size) return 0;
    size_t start = *at;
    for (end = start; end < size && text[end] != '\n' && text[end] != '\r'; ++end) continue;
    *at = end + 1;
    if (end < size && text[end] == '\r' && end + 1 < size && text[end + 1] == '\n') ++*at;
    ++*number;
    for (p = start; p < end && snx_is_space(text[p]); ++p) continue;
    start = last = p;
    while (p < end) {
        if (snx_is_space(text[p]))
            ++p;
        else if (!quote && (text[p] == '/' || text[p] == '#'))
            break;
        else if (text[p] == '"' || text[p] == '\'') {
            quote = text[p] == quote ? 0 : text[p];
            last = ++p;
        } else if (text[p] == '\\' && p + 1 < end)
            last = p += 2;
        else
            last = ++p;
    }
    *out = (char*)snx_alloc(last - start + 1);
    memcpy(*out, text + start, last - start);
    (*out)[last - start] = 0;
    return 1;
}
/* @nsobid-'s hex, up to 64 digits, zeros after it: 0, else nonzero */
static int build_id_of(const char* hex, unsigned char id[32]) {
    size_t n = strlen(hex);
    if (!n || n > 64) return 1;
    memset(id, 0, 32);
    for (size_t i = 0; i < n; ++i) {
        int v = snx_hex_value(hex[i]);
        if (v < 0) return 1;
        id[i / 2] |= (unsigned char)(i % 2 ? v : v << 4);
    }
    return 0;
}
/* a quoted value's bytes from s, its escapes as C's, into *data: 0, else nonzero for a value with no closing quote */
static int string_value(const char* s, unsigned char** data, size_t* n) {
    char quote = *s++;
    const char* end = s;
    size_t k = 0;
    while (*end && *end != quote) end += end[0] == '\\' && end[1] ? 2 : 1;
    if (*end != quote) return 1;
    *data = (unsigned char*)snx_alloc((size_t)(end - s) + 1);
    while (s < end) {
        char c = *s++;
        if (c == '\\') switch (c = *s++) {
                case 'a': c = '\a'; break;
                case 'b': c = '\b'; break;
                case 'e': c = 27; break;
                case 'f': c = '\f'; break;
                case 'n': c = '\n'; break;
                case 'r': c = '\r'; break;
                case 't': c = '\t'; break;
                case 'v': c = '\v'; break;
                default: break; /* \? \\ \" and any other: the character itself */
            }
        (*data)[k++] = (unsigned char)c;
    }
    *n = k;
    return 0;
}
/* hex digits' bytes, in the order written (as Eden writes them, @little or @big): 0, else nonzero */
static int hex_value(const char* s, unsigned char** data, size_t* n) {
    size_t len = strlen(s);
    if (!len || len % 2) return 1;
    *data = (unsigned char*)snx_alloc(len / 2);
    for (size_t i = 0; i < len; i += 2) {
        int hi = snx_hex_value(s[i]), lo = snx_hex_value(s[i + 1]);
        if (hi < 0 || lo < 0) {
            free(*data);
            return 1;
        }
        (*data)[i / 2] = (unsigned char)(hi << 4 | lo);
    }
    *n = len / 2;
    return 0;
}
/* one line of the text into t: 0, 2 at @stop, else 1 with error */
static int take_line(Text* t, const char* line, int number, char* error, size_t error_size) {
    size_t len = strlen(line);
    unsigned long long offset;
    char* end;
    unsigned char* data = NULL;
    size_t n = 0, i;
    Patch* patch;
    if (line[0] == '@' && len > 2) {
        static const char directives[] =
            "the directives are @nsobid-, @enabled, @disabled, @flag, @little, @big and @stop";
        switch (snx_to_lower(line[1])) {
            case 'n':
                if (len <= 8 || build_id_of(line + 8, t->build_id))
                    return fail(error, error_size, "line %d: %s: the build ID is up to 64 hex digits after @nsobid-",
                                number, line);
                t->has_id = 1;
                return 0;
            case 's': return 2;
            case 'e':
            case 'd':
                t->patches = (Patch*)snx_realloc(t->patches, (t->count + 1) * sizeof *t->patches);
                t->patches[t->count] = (Patch){0};
                t->patches[t->count++].enabled = snx_to_lower(line[1]) == 'e';
                return 0;
            case 'f':
                if (strncmp(line, "@flag ", 6))
                    return fail(error, error_size, "line %d: %s: %s", number, line, directives);
                if (!strncmp(line, "@flag offset_shift", 18)) {
                    t->shift = strtoll(line + 18, &end, 0);
                    if (end == line + 18 || *end)
                        return fail(error, error_size, "line %d: %s: offset_shift takes a number", number, line);
                    return 0;
                }
                if (!strcmp(line, "@flag print_values")) return 0; /* a log of the values, which this has none of */
                return fail(error, error_size, "line %d: %s: the flags are offset_shift and print_values", number,
                            line);
            case 'l':
            case 'b': return 0; /* the bytes go as written either way, as Eden writes them */
            default: return fail(error, error_size, "line %d: %s: %s", number, line, directives);
        }
    }
    if (!t->count)
        return fail(error, error_size, "line %d: %s: a value outside any patch, with no @enabled or @disabled above it",
                    number, line);
    if (snx_hex_value(line[0]) < 0)
        return fail(error, error_size, "line %d: %s: a value line starts with its offset in hex", number, line);
    offset = strtoull(line, &end, 16);
    long long at = (long long)offset + t->shift;
    if (offset > 0x7fffffffffffffffULL || at < 0)
        return fail(error, error_size, "line %d: %s: offset 0x%llX with offset_shift %lld is below 0", number, line,
                    offset, t->shift);
    const char* quote = strpbrk(end, "\"'");
    if (quote) {
        if (string_value(quote, &data, &n))
            return fail(error, error_size, "line %d: %s: a string with no closing %c", number, line, *quote);
    } else {
        const char* value = NULL;
        for (i = 0; line[i]; ++i)
            if (line[i] == ' ' || line[i] == '\t' || line[i] == '/') value = line + i + 1;
        if (!value) return fail(error, error_size, "line %d: %s: an offset with no value after it", number, line);
        if (hex_value(value, &data, &n))
            return fail(error, error_size, "line %d: %s: the value %s is not an even number of hex digits", number,
                        line, value);
    }
    patch = &t->patches[t->count - 1];
    for (i = 0; i < patch->count && patch->records[i].at != (unsigned long long)at; ++i) continue;
    if (i == patch->count) { /* a value at an offset the patch has already takes its place */
        patch->records = (Record*)snx_realloc(patch->records, (patch->count + 1) * sizeof *patch->records);
        patch->count++;
    } else
        free(patch->records[i].data);
    patch->records[i].at = (unsigned long long)at;
    patch->records[i].data = data;
    patch->records[i].n = n;
    patch->records[i].line = number;
    return 0;
}
static int read_text(const char* text, size_t size, Text* t, char* error, size_t error_size) {
    size_t at = 0;
    int number = 0, rc = 0;
    char* line;
    *t = (Text){0};
    while (!rc && next_line(text, size, &at, &number, &line)) {
        if (line[0]) rc = take_line(t, line, number, error, error_size);
        free(line);
    }
    return rc == 2 ? 0 : rc;
}

int snx_pchtxt_build_id(const char* text, size_t text_size, unsigned char build_id[32]) {
    size_t at = 0;
    int number = 0, found = 0;
    char* line;
    while (!found && next_line(text, text_size, &at, &number, &line)) {
        if (line[0] == '@' && (line[1] == 's' || line[1] == 'S')) {
            free(line);
            break;
        }
        found = line[0] == '@' && (line[1] == 'n' || line[1] == 'N') && strlen(line) > 8 &&
                !build_id_of(line + 8, build_id);
        free(line);
    }
    return found;
}

int snx_pchtxt_apply(const char* text, size_t text_size, unsigned char* image, size_t size, char* error,
                     size_t error_size) {
    Text t;
    int rc = read_text(text, text_size, &t, error, error_size);
    for (size_t i = 0; !rc && i < t.count; ++i)
        for (size_t j = 0; !rc && t.patches[i].enabled && j < t.patches[i].count; ++j) {
            const Record* r = &t.patches[i].records[j];
            if (place(image, size, r->at, r->data, 0, r->n))
                rc = fail(error, error_size, "line %d: 0x%zX bytes at 0x%llX end past the module's end at 0x%llX",
                          r->line, r->n, r->at, SNX_PATCH_HEADER + (unsigned long long)size);
        }
    text_free(&t);
    return rc;
}

/* 1 when name (a .ips) is named after build_id: its stem the hex of the ID's first bytes, the rest zeros */
static int ips_named(const char* name, const unsigned char build_id[32]) {
    size_t stem = strcspn(name, ".");
    if (!stem || stem > 64) return 0;
    for (size_t i = 0; i < 64; ++i) {
        int want = i % 2 ? build_id[i / 2] & 15 : build_id[i / 2] >> 4;
        int got = i < stem ? snx_hex_value(name[i]) : 0;
        if (got != want) return 0;
    }
    return 1;
}

int snx_patch_for(const char* path, const unsigned char build_id[32], char* error, size_t error_size) {
    const char* name = leaf(path);
    unsigned char id[32];
    size_t size = 0;
    if (snx_ends_with_nocase(name, ".ips")) return ips_named(name, build_id);
    if (!snx_ends_with_nocase(name, ".pchtxt")) return 0;
    unsigned char* data = snx_read_file(path, &size);
    if (!data) {
        char why[256];
        fail(error, error_size, "cannot read %s: %s", path, snx_os_error(why, sizeof why));
        return -1;
    }
    int is = snx_pchtxt_build_id((const char*)data, size, id) && !memcmp(id, build_id, 32);
    free(data);
    return is;
}

int snx_patch_module(unsigned char* image, size_t size, const unsigned char build_id[32], char* const* paths,
                     size_t count, unsigned char* applied, char* error, size_t error_size) {
    for (size_t i = 0; i < count; ++i) {
        int is = snx_patch_for(paths[i], build_id, error, error_size);
        if (is < 0 || (is && snx_patch_apply(paths[i], image, size, error, error_size))) return 1;
        if (applied) applied[i] = (unsigned char)is;
    }
    return 0;
}
int snx_patch_apply(const char* path, unsigned char* image, size_t size, char* error, size_t error_size) {
    char why[512];
    size_t n = 0;
    unsigned char* data = snx_read_file(path, &n);
    int rc;
    if (!data) return fail(error, error_size, "cannot read %s: %s", path, snx_os_error(why, sizeof why));
    rc = snx_ends_with_nocase(path, ".pchtxt") ? snx_pchtxt_apply((const char*)data, n, image, size, why, sizeof why)
                                               : snx_ips_apply(data, n, image, size, why, sizeof why);
    free(data);
    if (rc) fail(error, error_size, "%s: %s", path, why);
    return rc;
}
