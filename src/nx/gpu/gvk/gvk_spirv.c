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
    SPV_CONSTANT_NULL = 46,
    SPV_SPEC_CONSTANT = 50,
    SPV_SPEC_CONSTANT_OP = 52,
    SPV_VARIABLE = 59,
    SPV_ACCESS_CHAIN = 65,
    SPV_IN_BOUNDS_ACCESS_CHAIN = 66,
    SPV_DECORATE = 71,
    SPV_MEMBER_DECORATE = 72,
    SPV_GROUP_DECORATE = 74,
    SPV_GROUP_MEMBER_DECORATE = 75,
    SPV_EXTENSION = 10,
    SPV_ENTRY_POINT = 15,
    /* execution models */
    SPV_MODEL_FRAGMENT = 4,
    /* decorations */
    SPV_BUFFER_BLOCK = 3,
    SPV_ROW_MAJOR = 4,
    SPV_COL_MAJOR = 5,
    SPV_ARRAY_STRIDE = 6,
    SPV_MATRIX_STRIDE = 7,
    SPV_NON_UNIFORM = 5300,
    SPV_LOCATION = 30,
    SPV_INDEX = 32,
    /* storage classes */
    SPV_UNIFORM = 2,
    SPV_STORAGE_BUFFER = 12,
    SPV_OUTPUT = 3,
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
    SPV_HAS_NU = 128,     /* decorated NonUniform by the module itself */
};
static const char g_ext_name[] = "SPV_EXT_descriptor_indexing";
#define SPV_EXT_WORDS ((u32)(sizeof g_ext_name + 3) / 4) /* its words, with its NUL */

/* the type of member m of the structure s, 0 for none */
static u32 member_type(const u32* w, const u32* struct_at, u32 s, u32 m) {
    u32 at = struct_at[s];
    return at && m < (w[at] >> 16) - 2 ? w[at + 2 + m] : 0;
}
static int group_major(const u32* w, u32 n, u32 group) {
    for (u32 i = 5; i < n; i += w[i] >> 16)
        if ((w[i] & 0xffff) == SPV_DECORATE && (w[i] >> 16) >= 3 && w[i + 1] == group &&
            (w[i + 2] == SPV_ROW_MAJOR || w[i + 2] == SPV_COL_MAJOR))
            return 1;
    return 0;
}
/* whether member m of structure s has RowMajor or ColMajor, or is in a group's decorations, which may give it one */
static int has_major(const u32* w, u32 n, u32 s, u32 m) {
    for (u32 i = 5; i < n; i += w[i] >> 16) {
        u32 len = w[i] >> 16, op = w[i] & 0xffff;
        if (op == SPV_MEMBER_DECORATE && len >= 4 && w[i + 1] == s && w[i + 2] == m &&
            (w[i + 3] == SPV_ROW_MAJOR || w[i + 3] == SPV_COL_MAJOR))
            return 1;
        for (u32 k = 2; op == SPV_GROUP_MEMBER_DECORATE && k + 1 < len; k += 2)
            if (w[i + k] == s && w[i + k + 1] == m && group_major(w, n, w[i + 1])) return 1;
    }
    return 0;
}

const u32* gvk_spirv_mend(const u32* w, size_t bytes, size_t* out_bytes, u32 allow) {
    if (bytes % 4 || bytes / 4 > UINT32_MAX || bytes / 4 < 5 || w[0] != 0x07230203u || !w[3] || w[3] > SPV_IDS)
        return NULL;
    u32 n = (u32)(bytes / 4), bound = w[3];
    /* what each id is: its flags, an array's element type, where a structure is declared, a pointer type's pointee,
       an array type's ArrayStride (0 none) */
    u8* flag = gvk_scratch(bound);
    u32* elem = gvk_scratch(sizeof *elem * bound);
    u32* struct_at = gvk_scratch(sizeof *struct_at * bound);
    u32* pointee = gvk_scratch(sizeof *pointee * bound);
    u32* stride = gvk_scratch(sizeof *stride * bound);
    memset(flag, 0, bound);
    memset(elem, 0, sizeof *elem * bound);
    memset(struct_at, 0, sizeof *struct_at * bound);
    memset(pointee, 0, sizeof *pointee * bound);
    memset(stride, 0, sizeof *stride * bound);
    int groups = 0;
    for (u32 i = 5; i < n;) {
        u32 len = w[i] >> 16, op = w[i] & 0xffff;
        if (!len || len > n - i) return NULL;
        if (op == SPV_DECORATE && len >= 3 && w[i + 1] < bound) {
            if (w[i + 2] == SPV_NON_UNIFORM) flag[w[i + 1]] |= SPV_HAS_NU;
            if (w[i + 2] == SPV_BUFFER_BLOCK) flag[w[i + 1]] |= SPV_BUFFER_TYPE;
        }
        groups |= op == SPV_GROUP_DECORATE;
        i += len;
    }
    if (groups) {
        for (u32 i = 5; i < n; i += w[i] >> 16) {
            u32 len = w[i] >> 16;
            if ((w[i] & 0xffff) != SPV_GROUP_DECORATE || len < 3 || w[i + 1] >= bound) continue;
            u8 applied = flag[w[i + 1]] & (SPV_HAS_NU | SPV_BUFFER_TYPE);
            for (u32 j = 2; j < len; j++)
                if (w[i + j] < bound) flag[w[i + j]] |= applied;
        }
    }
    size_t extra = 0;
    u32 fixes = 0, indexed = 0, caps_end = 5;
    int has_cap[3] = {0, 0, 0}, need_ssbo = 0, need_ubo = 0, has_ext = 0;
    for (u32 i = 5; i < n;) {
        u32 len = w[i] >> 16, op = w[i] & 0xffff;
        const u32* o = w + i + 1;
        if (op == SPV_CAPABILITY && len >= 2) {
            if ((o[0] == SPV_CAP_UBO_NON_UNIFORM && !(allow & GVK_SPIRV_UBO_NONUNIFORM)) ||
                (o[0] == SPV_CAP_SSBO_NON_UNIFORM && !(allow & GVK_SPIRV_SSBO_NONUNIFORM)))
                return NULL;
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
        } else if ((op == SPV_CONSTANT || op == SPV_SPEC_CONSTANT || op == SPV_CONSTANT_NULL ||
                    op == SPV_SPEC_CONSTANT_OP) &&
                   len >= 3 && o[1] < bound) {
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
                if (!(flag[o[1]] & (SPV_INDEXED | SPV_HAS_NU))) indexed++;
                flag[o[1]] |= SPV_INDEXED | (flag[base] & SPV_SSBO_ARRAY);
                if (flag[base] & SPV_SSBO_ARRAY)
                    need_ssbo = 1;
                else
                    need_ubo = 1;
            }
        } else if (op == SPV_DECORATE && len >= 3 && o[0] < bound) {
            if (o[1] == SPV_ARRAY_STRIDE) flag[o[0]] |= SPV_STRIDED;
        } else if (op == SPV_MEMBER_DECORATE && len >= 4 && (o[2] == SPV_ARRAY_STRIDE || o[2] == SPV_MATRIX_STRIDE)) {
            fixes++;
            extra += o[2] == SPV_MATRIX_STRIDE ? 4 : 0;
        }
        i += len;
    }
    if ((need_ubo && !(allow & GVK_SPIRV_UBO_NONUNIFORM)) || (need_ssbo && !(allow & GVK_SPIRV_SSBO_NONUNIFORM)))
        return NULL;
    *out_bytes = bytes;
    /* the capabilities and the extension NonUniform needs, where the module has them not */
    int add_cap[3] = {indexed && !has_cap[0], indexed && need_ubo && !has_cap[1], indexed && need_ssbo && !has_cap[2]};
    int add_ext = indexed && !has_ext;
    if (!fixes && !indexed) return w;
    extra += 3 * (size_t)indexed + 2 * (size_t)(add_cap[0] + add_cap[1] + add_cap[2]) + (add_ext ? 1 + SPV_EXT_WORDS : 0);
    if (extra > UINT32_MAX - n || extra > SIZE_MAX / sizeof(u32) - n) return NULL;
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
            if (t && t < bound && elem[t]) {
                /* two members of one array type with two strides: one type can't hold both */
                if (stride[t] && stride[t] != w[i + 4]) return NULL;
                stride[t] = w[i + 4];
                if (!(flag[t] & SPV_STRIDED)) {
                    flag[t] |= SPV_STRIDED;
                    out[o++] = 4u << 16 | SPV_DECORATE;
                    out[o++] = t;
                    out[o++] = SPV_ARRAY_STRIDE;
                    out[o++] = w[i + 4];
                }
            }
            mended++;
            continue; /* the member's own: dropped */
        }
        memcpy(out + o, w + i, sizeof *out * len);
        o += len;
        if (!decorated && op == SPV_DECORATE) { /* NonUniform on each indexed pointer, among the decorations */
            for (u32 id = 1; indexed && id < bound; id++)
                if ((flag[id] & (SPV_INDEXED | SPV_HAS_NU)) == SPV_INDEXED) {
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
            u32 hops = 0;
            while (t && t < bound && elem[t] && hops++ < bound) t = elem[t];
            if (hops > bound) return NULL;
            if (t && t < bound && (flag[t] & SPV_MATRIX)) {
                out[o++] = 4u << 16 | SPV_MEMBER_DECORATE;
                out[o++] = w[i + 1];
                out[o++] = w[i + 2];
                out[o++] = SPV_COL_MAJOR;
                mended++;
            }
        }
    }
    if (!decorated) return NULL;
    if (!mended) return w;
    *out_bytes = (size_t)o * 4;
    return out;
}

/* the outputs at Location 1 of a module of fragment entry points alone, which no Index decorates, where no output
   has Index 1 already (the module's own second color) */
const u32* gvk_spirv_dual_source(const u32* w, size_t bytes, size_t* out_bytes) {
    if (bytes % 4 || bytes / 4 > UINT32_MAX || bytes / 4 < 5 || w[0] != 0x07230203u) return NULL;
    u32 n = (u32)(bytes / 4), at[8], nat = 0, fragment = 0, keep[8] = {0}, indexed[8], nindexed = 0;
    for (u32 i = 5; i < n;) {
        u32 len = w[i] >> 16, op = w[i] & 0xffff;
        if (!len || len > n - i) return NULL;
        /* The module-wide twin is restricted to directly decorated fragment interfaces. */
        if (op == SPV_GROUP_DECORATE || op == SPV_GROUP_MEMBER_DECORATE ||
            (op == SPV_MEMBER_DECORATE && len == 5 && w[i + 3] == SPV_INDEX))
            return NULL;
        if (op == SPV_ENTRY_POINT && len >= 2) {
            if (w[i + 1] != SPV_MODEL_FRAGMENT) return NULL;
            fragment = 1;
        } else if (op == SPV_DECORATE && len >= 3 && w[i + 2] == SPV_INDEX) {
            if (nindexed == 8 || (len >= 4 && w[i + 3] == 1)) return NULL;
            indexed[nindexed++] = w[i + 1];
        } else if (op == SPV_DECORATE && len == 4 && w[i + 2] == SPV_LOCATION && w[i + 3] == 1) {
            if (nat == 8) return NULL;
            at[nat++] = i;
        } else if (op == SPV_VARIABLE && len >= 4 && w[i + 3] == SPV_OUTPUT) {
            for (u32 k = 0; k < nat; k++) keep[k] |= w[at[k] + 1] == w[i + 2];
        }
        i += len;
    }
    u32 m = 0; /* the outputs among them that the game didn't give an Index, in order */
    for (u32 k = 0; k < nat; k++) {
        for (u32 j = 0; j < nindexed; j++) keep[k] &= indexed[j] != w[at[k] + 1];
        if (keep[k]) at[m++] = at[k];
    }
    nat = m;
    if (!fragment || !nat) return NULL;
    size_t extra = 4u * nat;
    if (extra > UINT32_MAX - n || extra > SIZE_MAX / sizeof(u32) - n) return NULL;
    u32* out = gvk_scratch(((size_t)n + extra) * sizeof *out);
    u32 o = 0, next = 0;
    for (u32 i = 0; i < n;) {
        u32 len = i < 5 ? 1 : w[i] >> 16;
        memcpy(out + o, w + i, (size_t)len * 4);
        o += len;
        if (next < nat && i == at[next]) {
            out[o - 1] = 0;
            out[o++] = 4u << 16 | SPV_DECORATE;
            out[o++] = w[i + 1];
            out[o++] = SPV_INDEX;
            out[o++] = 1;
            next++;
        }
        i += len;
    }
    *out_bytes = (size_t)o * 4;
    return out;
}
