/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "common/cpu/recompiler/object/objects_pack.h"
#include "common/utilities/common.h"
#include "common/utilities/endian.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

unsigned char* snx_objects_pack(const SnxLinkInput* inputs, size_t count, size_t* size) {
    size_t total = 16;
    for (size_t i = 0; i < count; ++i) total += 8 + inputs[i].size;
    unsigned char* data = (unsigned char*)malloc(total);
    if (!data) return NULL;
    memcpy(data, "snx-objs", 8);
    snx_st_le64(data + 8, count);
    for (size_t i = 0, at = 16; i < count; ++i) {
        snx_st_le64(data + at, inputs[i].size);
        memcpy(data + at + 8, inputs[i].data, inputs[i].size);
        at += 8 + inputs[i].size;
    }
    *size = total;
    return data;
}

int snx_objects_pack_write(const SnxLinkInput* inputs, size_t count, const char* path, size_t* size,
                           unsigned char* (*load)(void* arg, size_t index, size_t* size, char* why, size_t why_size),
                           void (*written)(void* arg, size_t index), void* arg, char* error, size_t error_size) {
    unsigned char head[16];
    char reason[256], why[768];
    size_t total = sizeof head;
    int rc = -1;
    FILE* f = snx_fopen(path, "wb");
    if (!f) {
        snprintf(error, error_size, "cannot write %s: %s", path, snx_os_error(reason, sizeof reason));
        return -1;
    }
    memcpy(head, "snx-objs", 8);
    snx_st_le64(head + 8, count);
    if (fwrite(head, 1, sizeof head, f) != sizeof head) {
        snprintf(error, error_size, "cannot write %s: %s", path, snx_os_error(reason, sizeof reason));
        goto done;
    }
    for (size_t i = 0; i < count; ++i) {
        unsigned char length[8], *loaded = NULL;
        const unsigned char* data = inputs[i].data;
        size_t loaded_size = 0;
        int ok = 1;
        if (!data) {
            loaded = load ? load(arg, i, &loaded_size, why, sizeof why) : NULL;
            if (!load) snprintf(why, sizeof why, "no object in memory, and nothing to read it from");
            ok = loaded && loaded_size == inputs[i].size;
            if (!loaded)
                snprintf(error, error_size, "cannot read back input %zu of %zu (%s): %s", i, count, inputs[i].name,
                         why);
            else if (!ok)
                snprintf(error, error_size, "input %zu of %zu (%s) read back is %zu bytes, %zu when it was made", i,
                         count, inputs[i].name, loaded_size, inputs[i].size);
            data = loaded;
        }
        if (ok) {
            snx_st_le64(length, inputs[i].size);
            ok = fwrite(length, 1, sizeof length, f) == sizeof length &&
                 fwrite(data, 1, inputs[i].size, f) == inputs[i].size;
            if (!ok) snprintf(error, error_size, "cannot write %s: %s", path, snx_os_error(reason, sizeof reason));
        }
        free(loaded);
        if (written) written(arg, i);
        if (!ok) goto done;
        total += sizeof length + inputs[i].size;
    }
    rc = 0;
done:
    if (fclose(f) && !rc) {
        snprintf(error, error_size, "cannot write %s: %s", path, snx_os_error(reason, sizeof reason));
        rc = -1;
    }
    if (!rc) *size = total;
    return rc;
}

SnxLinkInput* snx_objects_unpack(const unsigned char* data, size_t size, const char* name, size_t* count) {
    size_t at = 16;
    if (size < 16 || memcmp(data, "snx-objs", 8)) return NULL;
    size_t n = (size_t)snx_ld_le64(data + 8);
    if (!n || n > size / 8) return NULL;
    SnxLinkInput* inputs = (SnxLinkInput*)calloc(n, sizeof(*inputs));
    if (!inputs) return NULL;
    for (size_t i = 0; i < n; ++i) {
        uint64_t length;
        if (size - at < 8 || (length = snx_ld_le64(data + at)) > size - at - 8) {
            free(inputs);
            return NULL;
        }
        inputs[i].data = data + at + 8;
        inputs[i].size = (size_t)length;
        inputs[i].name = name;
        at += 8 + (size_t)length;
    }
    *count = n;
    return inputs;
}
