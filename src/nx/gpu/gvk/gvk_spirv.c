/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What the console's driver takes in a game's SPIR-V and Vulkan doesn't (gvk.h gvk_spirv_mend) */
#include "nx/gpu/gvk/gvk.h"
#include <string.h>

enum {
    SPV_CAPABILITY = 17,
    SPV_TYPE_MATRIX = 24,
    SPV_TYPE_ARRAY = 28,
    SPV_TYPE_RUNTIME_ARRAY = 29,
    SPV_TYPE_STRUCT = 30,
    SPV_TYPE_POINTER = 32,
    SPV_CONSTANT = 43,
    SPV_SPEC_CONSTANT = 50,
    SPV_VARIABLE = 59,
    SPV_ACCESS_CHAIN = 65,
    SPV_IN_BOUNDS_ACCESS_CHAIN = 66,
    SPV_DECORATE = 71,
    SPV_MEMBER_DECORATE = 72,
    SPV_EXTENSION = 10,
    /* decorations */
    SPV_BUFFER_BLOCK = 3,
    SPV_ROW_MAJOR = 4,
    SPV_COL_MAJOR = 5,
    SPV_ARRAY_STRIDE = 6,
    SPV_MATRIX_STRIDE = 7,
    SPV_NON_UNIFORM = 5300,
    /* storage classes */
    SPV_UNIFORM = 2,
    SPV_STORAGE_BUFFER = 12,
    /* capabilities */
    SPV_CAP_NON_UNIFORM = 5301,
    SPV_CAP_UBO_NON_UNIFORM = 5306,
    SPV_CAP_SSBO_NON_UNIFORM = 5308,
};
#define SPV_IDS (1u << 22) /* a module's id bound gvk mends, at most */
/* what an id is */
enum {
    SPV_MATRIX = 1,       /* a matrix type */
    SPV_STRIDED = 2,      /* an array type with its own ArrayStride */
    SPV_BUFFER_TYPE = 4,  /* a structure decorated BufferBlock: a storage buffer in the Uniform class */
    SPV_UNIFORM_ID = 8,   /* a constant, the same in every invocation */
    SPV_BUFFER_ARRAY = 16, /* a variable that is an array of buffers (with 32 for storage buffers) */
    SPV_SSBO_ARRAY = 32,
    SPV_INDEXED = 64,     /* a pointer into such an array, by an index that can differ between invocations */
};
static const char g_ext_name[] = "SPV_EXT_descriptor_indexing";
#define SPV_EXT_WORDS ((u32)(sizeof g_ext_name + 3) / 4) /* its words, with its NUL */

/* the type of member m of the structure s, 0 for none */
static u32 member_type(const u32* w, const u32* struct_at, u32 s, u32 m) {
    u32 at = struct_at[s];
    return at && m + 2 < (w[at] >> 16) ? w[at + 2 + m] : 0;
}
static int has_major(const u32* w, u32 n, u32 s, u32 m) {
    for (u32 i = 5; i < n; i += w[i] >> 16)
        if ((w[i] & 0xffff) == SPV_MEMBER_DECORATE && (w[i] >> 16) >= 4 && w[i + 1] == s && w[i + 2] == m &&
            (w[i + 3] == SPV_ROW_MAJOR || w[i + 3] == SPV_COL_MAJOR))
            return 1;
    return 0;
}

const u32* gvk_spirv_mend(const u32* w, size_t bytes, size_t* out_bytes) {
    if (bytes % 4 || bytes / 4 > UINT32_MAX || bytes / 4 < 5 || w[0] != 0x07230203u || !w[3] || w[3] > SPV_IDS)
        return NULL;
    u32 n = (u32)(bytes / 4), bound = w[3];
    /* what each id is: its flags, an array's element type, where a structure is declared, a pointer type's pointee */
    u8* flag = gvk_scratch(bound);
    u32* elem = gvk_scratch(sizeof *elem * bound);
    u32* struct_at = gvk_scratch(sizeof *struct_at * bound);
    u32* pointee = gvk_scratch(sizeof *pointee * bound);
    memset(flag, 0, bound);
    memset(elem, 0, sizeof *elem * bound);
    memset(struct_at, 0, sizeof *struct_at * bound);
    memset(pointee, 0, sizeof *pointee * bound);
    u32 extra = 0, fixes = 0, indexed = 0, caps_end = 5;
    int has_cap[3] = {0, 0, 0}, need_ssbo = 0, need_ubo = 0, has_ext = 0;
    for (u32 i = 5; i < n;) {
        u32 len = w[i] >> 16, op = w[i] & 0xffff;
        if (!len || len > n - i) return NULL;
        const u32* o = w + i + 1;
        if (op == SPV_CAPABILITY && len >= 2) {
            caps_end = i + len;
            has_cap[0] |= o[0] == SPV_CAP_NON_UNIFORM;
            has_cap[1] |= o[0] == SPV_CAP_UBO_NON_UNIFORM;
            has_cap[2] |= o[0] == SPV_CAP_SSBO_NON_UNIFORM;
        } else if (op == SPV_EXTENSION && len - 1 >= SPV_EXT_WORDS) {
            has_ext |= !memcmp(o, g_ext_name, sizeof g_ext_name);
        } else if ((op == SPV_TYPE_MATRIX || op == SPV_TYPE_STRUCT) && len >= 2 && o[0] < bound) {
            if (op == SPV_TYPE_MATRIX)
                flag[o[0]] |= SPV_MATRIX;
            else
                struct_at[o[0]] = i;
        } else if ((op == SPV_TYPE_ARRAY || op == SPV_TYPE_RUNTIME_ARRAY) && len >= 3 && o[0] < bound) {
            elem[o[0]] = o[1];
        } else if (op == SPV_TYPE_POINTER && len >= 4 && o[0] < bound) {
            pointee[o[0]] = o[2];
        } else if ((op == SPV_CONSTANT || op == SPV_SPEC_CONSTANT) && len >= 3 && o[1] < bound) {
            flag[o[1]] |= SPV_UNIFORM_ID;
        } else if (op == SPV_VARIABLE && len >= 4 && o[1] < bound && o[0] < bound &&
                   (o[2] == SPV_UNIFORM || o[2] == SPV_STORAGE_BUFFER)) {
            /* an array of buffers (a descriptor array): types come before variables */
            u32 t = pointee[o[0]];
            if (t && t < bound && elem[t]) {
                u32 e = elem[t];
                flag[o[1]] |= SPV_BUFFER_ARRAY;
                if (o[2] == SPV_STORAGE_BUFFER || (e < bound && (flag[e] & SPV_BUFFER_TYPE))) flag[o[1]] |= SPV_SSBO_ARRAY;
            }
        } else if ((op == SPV_ACCESS_CHAIN || op == SPV_IN_BOUNDS_ACCESS_CHAIN) && len >= 5 && o[1] < bound &&
                   o[2] < bound && o[3] < bound) {
            /* a pointer into a buffer of the array by an index that isn't a constant, or into an indexed one: the
               console's GPU takes each invocation's own buffer, the host's driver the first's unless told */
            u32 base = o[2];
            int pick = (flag[base] & SPV_BUFFER_ARRAY) && !(flag[o[3]] & SPV_UNIFORM_ID);
            if (pick || (flag[base] & SPV_INDEXED)) {
                if (!(flag[o[1]] & SPV_INDEXED)) indexed++;
                flag[o[1]] |= SPV_INDEXED | (flag[base] & SPV_SSBO_ARRAY);
                if (flag[base] & SPV_SSBO_ARRAY)
                    need_ssbo = 1;
                else
                    need_ubo = 1;
            }
        } else if (op == SPV_DECORATE && len >= 3 && o[0] < bound) {
            if (o[1] == SPV_ARRAY_STRIDE) flag[o[0]] |= SPV_STRIDED;
            if (o[1] == SPV_BUFFER_BLOCK) flag[o[0]] |= SPV_BUFFER_TYPE;
        } else if (op == SPV_MEMBER_DECORATE && len >= 4 && (o[2] == SPV_ARRAY_STRIDE || o[2] == SPV_MATRIX_STRIDE)) {
            fixes++;
            extra += o[2] == SPV_MATRIX_STRIDE ? 4 : 0;
        }
        i += len;
    }
    /* the capabilities and the extension NonUniform needs, where the module has them not */
    int add_cap[3] = {indexed && !has_cap[0], need_ubo && !has_cap[1], need_ssbo && !has_cap[2]};
    int add_ext = indexed && !has_ext;
    if (!fixes && !indexed) return NULL;
    extra += 3 * indexed + 2 * (u32)(add_cap[0] + add_cap[1] + add_cap[2]) + (add_ext ? 1 + SPV_EXT_WORDS : 0);
    u32* out = gvk_scratch(sizeof *out * (n + extra));
    memcpy(out, w, 5 * sizeof *out);
    u32 o = 5, mended = 0;
    int decorated = !indexed;
    for (u32 i = 5; i <= n; i += i < n ? w[i] >> 16 : 1) {
        if (i == caps_end) {
            static const u32 cap_id[3] = {SPV_CAP_NON_UNIFORM, SPV_CAP_UBO_NON_UNIFORM, SPV_CAP_SSBO_NON_UNIFORM};
            for (int k = 0; k < 3; k++)
                if (add_cap[k]) {
                    out[o++] = 2u << 16 | SPV_CAPABILITY;
                    out[o++] = cap_id[k];
                }
            if (add_ext) {
                out[o++] = (1 + SPV_EXT_WORDS) << 16 | SPV_EXTENSION;
                memset(out + o, 0, SPV_EXT_WORDS * 4);
                memcpy(out + o, g_ext_name, sizeof g_ext_name);
                o += SPV_EXT_WORDS;
            }
            mended += add_cap[0] + add_cap[1] + add_cap[2] + add_ext;
        }
        if (i == n) break;
        u32 len = w[i] >> 16, op = w[i] & 0xffff;
        if (op == SPV_MEMBER_DECORATE && len == 5 && w[i + 3] == SPV_ARRAY_STRIDE && w[i + 1] < bound) {
            u32 t = member_type(w, struct_at, w[i + 1], w[i + 2]);
            if (t && t < bound && elem[t] && !(flag[t] & SPV_STRIDED)) {
                flag[t] |= SPV_STRIDED;
                out[o++] = 4u << 16 | SPV_DECORATE;
                out[o++] = t;
                out[o++] = SPV_ARRAY_STRIDE;
                out[o++] = w[i + 4];
            }
            mended++;
            continue; /* the member's own: dropped */
        }
        memcpy(out + o, w + i, sizeof *out * len);
        o += len;
        if (!decorated && op == SPV_DECORATE) { /* NonUniform on each indexed pointer, among the decorations */
            for (u32 id = 1; id < bound; id++)
                if (flag[id] & SPV_INDEXED) {
                    out[o++] = 3u << 16 | SPV_DECORATE;
                    out[o++] = id;
                    out[o++] = SPV_NON_UNIFORM;
                    mended++;
                }
            decorated = 1;
        }
        if (op == SPV_MEMBER_DECORATE && len >= 4 && w[i + 3] == SPV_MATRIX_STRIDE && w[i + 1] < bound &&
            !has_major(w, n, w[i + 1], w[i + 2])) {
            u32 t = member_type(w, struct_at, w[i + 1], w[i + 2]);
            while (t && t < bound && elem[t]) t = elem[t]; /* a decoration of an array of matrices is its matrices' */
            if (t && t < bound && (flag[t] & SPV_MATRIX)) {
                out[o++] = 4u << 16 | SPV_MEMBER_DECORATE;
                out[o++] = w[i + 1];
                out[o++] = w[i + 2];
                out[o++] = SPV_COL_MAJOR;
                mended++;
            }
        }
    }
    if (!mended || !decorated) return NULL;
    *out_bytes = (size_t)o * 4;
    return out;
}
