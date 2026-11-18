/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What the console's driver takes in a game's SPIR-V and Vulkan doesn't (gvk.h gvk_spirv_mend) */
#include "nx/gpu/gvk/gvk.h"
#include <string.h>

enum {
    SPV_TYPE_MATRIX = 24,
    SPV_TYPE_ARRAY = 28,
    SPV_TYPE_RUNTIME_ARRAY = 29,
    SPV_TYPE_STRUCT = 30,
    SPV_DECORATE = 71,
    SPV_MEMBER_DECORATE = 72,
    SPV_ROW_MAJOR = 4,
    SPV_COL_MAJOR = 5,
    SPV_ARRAY_STRIDE = 6,
    SPV_MATRIX_STRIDE = 7,
};
#define SPV_IDS (1u << 22) /* a module's id bound gvk mends, at most */
enum { SPV_MATRIX = 1, SPV_STRIDED = 2 };

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
    u32 n = (u32)(bytes / 4);
    if (bytes % 4 || bytes / 4 > UINT32_MAX || n < 5 || w[0] != 0x07230203u || !w[3] || w[3] > SPV_IDS) return NULL;
    u32 bound = w[3];
    /* what each id is: a matrix, an array (its element type), a structure (where it is declared) */
    u8* flag = gvk_scratch(bound);
    u32* elem = gvk_scratch(sizeof *elem * bound);
    u32* struct_at = gvk_scratch(sizeof *struct_at * bound);
    memset(flag, 0, bound);
    memset(elem, 0, sizeof *elem * bound);
    memset(struct_at, 0, sizeof *struct_at * bound);
    u32 extra = 0, fixes = 0;
    for (u32 i = 5; i < n;) {
        u32 len = w[i] >> 16, op = w[i] & 0xffff;
        if (!len || len > n - i) return NULL;
        if ((op == SPV_TYPE_MATRIX || op == SPV_TYPE_STRUCT) && len >= 2 && w[i + 1] < bound) {
            if (op == SPV_TYPE_MATRIX)
                flag[w[i + 1]] |= SPV_MATRIX;
            else
                struct_at[w[i + 1]] = i;
        } else if ((op == SPV_TYPE_ARRAY || op == SPV_TYPE_RUNTIME_ARRAY) && len >= 3 && w[i + 1] < bound) {
            elem[w[i + 1]] = w[i + 2];
        } else if (op == SPV_DECORATE && len >= 3 && w[i + 2] == SPV_ARRAY_STRIDE && w[i + 1] < bound) {
            flag[w[i + 1]] |= SPV_STRIDED;
        } else if (op == SPV_MEMBER_DECORATE && len >= 4 &&
                   (w[i + 3] == SPV_ARRAY_STRIDE || w[i + 3] == SPV_MATRIX_STRIDE)) {
            fixes++;
            extra += w[i + 3] == SPV_MATRIX_STRIDE ? 4 : 0;
        }
        i += len;
    }
    if (!fixes) return NULL;
    u32* out = gvk_scratch(sizeof *out * (n + extra));
    memcpy(out, w, 5 * sizeof *out);
    u32 o = 5, mended = 0;
    for (u32 i = 5; i < n; i += w[i] >> 16) {
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
    if (!mended) return NULL;
    *out_bytes = (size_t)o * 4;
    return out;
}
