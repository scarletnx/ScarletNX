/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "common/cpu/recompiler/llvm/link.h"
#include "common/utilities/endian.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int snx_link(const SnxLinkInput* inputs, size_t count, const char* const* exports, size_t export_count,
             const char* soname, SnxLinkOutput* output, char* error, size_t error_size) {
    *output = (SnxLinkOutput){0};
    if (!count) {
        snprintf(error, error_size, "nothing to link");
        return -1;
    }
    if (inputs[0].size >= 4 && !memcmp(inputs[0].data, "\177ELF", 4)) {
#ifdef __APPLE__
        /* Darwin loads Mach-O: the native back ends' arm64 objects and the runtime library's link into one
           (link_macho.c). Another machine's are no module of this host's (e_machine 183 is AArch64) */
        if (inputs[0].size >= 20 && snx_ld_le16(inputs[0].data + 18) == 183)
            return snx_link_macho(inputs, count, exports, export_count, soname, output, error, error_size);
#endif
        return snx_link_elf(inputs, count, exports, export_count, soname, output, error, error_size);
    }
    /* COFF's machine: IMAGE_FILE_MACHINE_AMD64 or IMAGE_FILE_MACHINE_ARM64, little-endian */
    if (inputs[0].size >= 2 && inputs[0].data[0] == 0x64 && (inputs[0].data[1] == 0x86 || inputs[0].data[1] == 0xAA))
        return snx_link_coff(inputs, count, exports, export_count, soname, output, error, error_size);
    if (inputs[0].size >= 4 && !memcmp(inputs[0].data, "\xcf\xfa\xed\xfe", 4)) /* MH_MAGIC_64, little-endian */
        return snx_link_macho(inputs, count, exports, export_count, soname, output, error, error_size);
    snprintf(error, error_size, "%s: not an ELF, COFF or Mach-O object", inputs[0].name);
    return -1;
}

void snx_link_reserve(SnxLinkBuffer* buffer, size_t size) {
    if (size > buffer->capacity) {
        size_t capacity = buffer->capacity ? buffer->capacity : 4096;
        while (capacity < size) capacity *= 2;
        unsigned char* data = (unsigned char*)realloc(buffer->data, capacity);
        if (!data) {
            fputs("out of memory\n", stderr);
            abort();
        }
        memset(data + buffer->capacity, 0, capacity - buffer->capacity);
        buffer->data = data;
        buffer->capacity = capacity;
    }
    if (size > buffer->size) buffer->size = size;
}

size_t snx_link_append(SnxLinkBuffer* buffer, const void* data, size_t size) {
    size_t at = buffer->size;
    snx_link_reserve(buffer, at + size);
    if (size && data) memcpy(buffer->data + at, data, size);
    return at;
}

void snx_link_align(SnxLinkBuffer* buffer, size_t alignment) {
    if (alignment > 1) snx_link_reserve(buffer, (buffer->size + alignment - 1) / alignment * alignment);
}

static size_t name_hash(const char* key) {
    size_t h = 5381;
    while (*key) h = h * 33 + (unsigned char)*key++;
    return h;
}

int snx_link_find(const SnxLinkNames* names, const char* key, size_t* value) {
    if (!names->capacity) return 0;
    for (size_t i = name_hash(key) & (names->capacity - 1); names->keys[i]; i = (i + 1) & (names->capacity - 1))
        if (!strcmp(names->keys[i], key)) {
            *value = names->values[i];
            return 1;
        }
    return 0;
}

void snx_link_insert(SnxLinkNames* names, const char* key, size_t value) {
    if ((names->count + 1) * 2 > names->capacity) {
        SnxLinkNames grown;
        grown.capacity = names->capacity ? names->capacity * 2 : 1024;
        grown.count = 0;
        grown.keys = (char**)calloc(grown.capacity, sizeof(char*) + sizeof(size_t)); /* the values follow */
        if (!grown.keys) {
            fputs("out of memory\n", stderr);
            abort();
        }
        grown.values = (size_t*)(grown.keys + grown.capacity);
        for (size_t i = 0; i < names->capacity; ++i)
            if (names->keys[i]) {
                size_t j = name_hash(names->keys[i]) & (grown.capacity - 1);
                while (grown.keys[j]) j = (j + 1) & (grown.capacity - 1);
                grown.keys[j] = names->keys[i];
                grown.values[j] = names->values[i];
                ++grown.count;
            }
        free(names->keys);
        *names = grown;
    }
    size_t i;
    for (i = name_hash(key) & (names->capacity - 1); names->keys[i]; i = (i + 1) & (names->capacity - 1))
        if (!strcmp(names->keys[i], key)) {
            names->values[i] = value;
            return;
        }
    names->keys[i] = (char*)malloc(strlen(key) + 1);
    if (!names->keys[i]) abort();
    strcpy(names->keys[i], key);
    names->values[i] = value;
    ++names->count;
}

void snx_link_names_free(SnxLinkNames* names) {
    for (size_t i = 0; i < names->capacity; ++i) free(names->keys[i]);
    free(names->keys);
    *names = (SnxLinkNames){0};
}

void snx_link_put32(unsigned char* p, uint32_t value) { snx_st_le32(p, value); }

void snx_link_put64(unsigned char* p, uint64_t value) { snx_st_le64(p, value); }

int snx_link_rel32(unsigned char* p, uint64_t target, uint64_t place) {
    /* in unsigned arithmetic, then read as signed: the difference wraps where a signed one would overflow */
    uint64_t difference = target - place;
    if (difference + 0x80000000u > 0xFFFFFFFFu) return SNX_LINK_RANGE;
    snx_st_le32(p, (uint32_t)difference);
    return 0;
}
