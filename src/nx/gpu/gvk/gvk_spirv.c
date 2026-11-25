/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What the console's driver takes in a game's SPIR-V and Vulkan doesn't (gvk.h gvk_spirv_mend) */
#include "nx/gpu/gvk/gvk.h"
#include <string.h>

enum {
    SPV_UNDEF = 1,
    SPV_CAPABILITY = 17,
    SPV_TYPE_VOID = 19,
    SPV_TYPE_INT = 21,
    SPV_TYPE_FLOAT = 22,
    SPV_TYPE_VECTOR = 23,
    SPV_TYPE_IMAGE = 25,
    SPV_TYPE_SAMPLER = 26,
    SPV_TYPE_SAMPLED_IMAGE = 27,
    SPV_TYPE_FORWARD_POINTER = 39,
    SPV_CONSTANT_TRUE = 41,
    SPV_FUNCTION = 54,
    SPV_FUNCTION_PARAMETER = 55,
    SPV_FUNCTION_CALL = 57,
    SPV_LOAD = 61,
    SPV_COMPOSITE_CONSTRUCT = 80,
    SPV_COMPOSITE_EXTRACT = 81,
    SPV_COPY_OBJECT = 83,
    SPV_SAMPLED_IMAGE = 86,
    SPV_IMAGE = 100,
    SPV_QUERY_SIZE_LOD = 103,
    SPV_QUERY_SIZE = 104,
    SPV_BITCAST = 124,
    SPV_UCONVERT = 113,
    SPV_SCONVERT = 114,
    SPV_IADD = 128,
    SPV_ISUB = 130,
    SPV_IMUL = 132,
    SPV_UDIV = 134,
    SPV_SDIV = 135,
    SPV_SHIFT_RIGHT_LOGICAL = 194,
    SPV_BITWISE_AND = 199,
    SPV_BLOCK = 2,
    SPV_BINDING = 33,
    SPV_DESCRIPTOR_SET = 34,
    SPV_OFFSET = 35,
    SPV_PUSH_CONSTANT = 9,
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
    SPV_UNIFORM_CONSTANT = 0,
    SPV_UNIFORM = 2,
    SPV_STORAGE_BUFFER = 12,
    SPV_OUTPUT = 3,
    /* what an image's Dim is, and its Sampled */
    SPV_DIM_BUFFER = 5,
    SPV_DIM_SUBPASS = 6,
    SPV_SAMPLED_NO = 2,
    /* capabilities */
    SPV_CAP_NON_UNIFORM = 5301,
    /* and its arrays', each the kind's of the same index (kind_cap) */
    SPV_CAP_UBO_NON_UNIFORM = 5306,
    SPV_CAP_SSBO_NON_UNIFORM = 5308,
};
/* the kinds of descriptor whose arrays a module indexes, in the order of the GVK_SPIRV_* bits. 0: no such thing */
enum {
    KIND_NONE,
    KIND_UBO,
    KIND_SSBO,
    KIND_SAMPLED,
    KIND_STORAGE_IMAGE,
    KIND_INPUT,
    KIND_UNIFORM_TEXEL,
    KIND_STORAGE_TEXEL,
    KINDS
};
/* the capability of each kind's arrays indexed by what can differ */
static u32 kind_cap(u32 kind) {
    static const u32 cap[KINDS] = {0,    SPV_CAP_UBO_NON_UNIFORM, SPV_CAP_SSBO_NON_UNIFORM,
                                   5307, /* SampledImageArrayNonUniformIndexing */
                                   5309, /* StorageImageArrayNonUniformIndexing */
                                   5310, /* InputAttachmentArrayNonUniformIndexing */
                                   5311, /* UniformTexelBufferArrayNonUniformIndexing */
                                   5312 /* StorageTexelBufferArrayNonUniformIndexing */};
    return cap[kind];
}
#define SPV_IDS (1u << 22) /* a module's id bound gvk mends, at most */
/* what an id is */
enum {
    SPV_MATRIX = 1,       /* a matrix type */
    SPV_STRIDED = 2,      /* an array type with its own ArrayStride */
    SPV_BUFFER_TYPE = 4,  /* a structure decorated BufferBlock: a storage buffer in the Uniform class */
    SPV_UNIFORM_ID = 8,   /* a value that is the same in every invocation of a draw or a dispatch */
    SPV_UNIFORM_PTR = 16, /* a pointer to one: to push constants, or into a uniform buffer, by such indices */
    SPV_ARRAY = 32,       /* a variable that is an array of descriptors of its kind */
    SPV_INDEXED = 64, /* a pointer into such an array by an index that can differ between invocations, and an image or
                         a sampler loaded by one */
    SPV_HAS_NU = 128, /* decorated NonUniform by the module itself */
};
/* The kind of each id: of an image or sampler type, the descriptors of it; of a variable, the array's; of a pointer or
   a value, the array it is of. allow names the kinds the host's device can index by such an index (GVK_SPIRV_*). One
   of an image's kinds it can't keeps the module's own code, as a host without the feature did before */
typedef struct {
    const u32* w;
    u32 n, bound, allow;
    u8 *flag, *kind;
    const u32* func_at; /* where each function is declared, 0 for none */
} Marks;
static int kind_allowed(const Marks* m, u32 kind) { return (m->allow >> (kind - 1)) & 1; }
/* flags and a kind given to an id: 1 when it has more of them than before */
static int mark(Marks* m, u32 id, u8 flag, u8 kind) {
    if (!id || id >= m->bound) return 0;
    int more = (flag & ~m->flag[id]) || (kind && !m->kind[id]);
    m->flag[id] |= flag;
    if (!m->kind[id]) m->kind[id] = kind; /* one kind, whatever a module gives it later: the marks only grow */
    return more;
}
/* whether every index of an access chain (n words at o, from the first index) is the same in every invocation */
static int uniform_indices(const Marks* m, const u32* o, u32 from, u32 n) {
    for (u32 j = from; j < n; j++)
        if (o[j] >= m->bound || !(m->flag[o[j]] & SPV_UNIFORM_ID)) return 0;
    return 1;
}
/* a pointer into an array of descriptors is indexed by what an invocation can hold of its own, and so are the pointers
   made from it, the images and samplers it loads, what is made of those, and the parameters of the functions they are
   passed to. Until nothing more is: a function's body can come before the call that passes it one. A buffer's load is
   its bytes, and not the buffer */
static void mark_indexed(Marks* m) {
    for (int more = 1; more;) {
        more = 0;
        for (u32 i = 5; i < m->n; i += m->w[i] >> 16) {
            u32 len = m->w[i] >> 16, op = m->w[i] & 0xffff;
            const u32* o = m->w + i + 1;
            if ((op == SPV_ACCESS_CHAIN || op == SPV_IN_BOUNDS_ACCESS_CHAIN) && len >= 5 && o[2] < m->bound &&
                o[3] < m->bound) {
                u8 base = m->flag[o[2]], k = m->kind[o[2]];
                u32 last =
                    k >= KIND_SAMPLED ? len - 1 : 4; /* an array of buffers is one of structures past its first */
                int pick = (base & SPV_ARRAY) && !uniform_indices(m, o, 3, last);
                if ((pick || (base & SPV_INDEXED)) && (k < KIND_SAMPLED || kind_allowed(m, k)))
                    more |= mark(m, o[1], SPV_INDEXED, k);
                if ((base & SPV_UNIFORM_PTR) && uniform_indices(m, o, 3, len - 1))
                    more |= mark(m, o[1], SPV_UNIFORM_PTR, 0);
            } else if (op == SPV_LOAD && len >= 4 && o[2] < m->bound) {
                if ((m->flag[o[2]] & SPV_INDEXED) && m->kind[o[2]] >= KIND_SAMPLED)
                    more |= mark(m, o[1], SPV_INDEXED, m->kind[o[2]]);
                if (m->flag[o[2]] & SPV_UNIFORM_PTR) more |= mark(m, o[1], SPV_UNIFORM_ID, 0);
            } else if (op == SPV_SAMPLED_IMAGE && len >= 5 && o[2] < m->bound && o[3] < m->bound) {
                if (m->flag[o[2]] & SPV_INDEXED)
                    more |= mark(m, o[1], SPV_INDEXED, m->kind[o[2]]);
                else if (m->flag[o[3]] & SPV_INDEXED)
                    more |= mark(m, o[1], SPV_INDEXED, m->kind[o[3]]);
            } else if ((op == SPV_IMAGE || op == SPV_COPY_OBJECT) && len >= 4 && o[2] < m->bound) {
                if (m->flag[o[2]] & SPV_INDEXED) more |= mark(m, o[1], SPV_INDEXED, m->kind[o[2]]);
                if (op == SPV_COPY_OBJECT && (m->flag[o[2]] & SPV_UNIFORM_ID)) more |= mark(m, o[1], SPV_UNIFORM_ID, 0);
            } else if ((op == SPV_BITCAST || op == SPV_UCONVERT || op == SPV_SCONVERT) && len >= 4 && o[2] < m->bound) {
                if (m->flag[o[2]] & SPV_UNIFORM_ID) more |= mark(m, o[1], SPV_UNIFORM_ID, 0);
            } else if (op == SPV_IADD || op == SPV_ISUB || op == SPV_IMUL) {
                if (len >= 5 && o[2] < m->bound && o[3] < m->bound && (m->flag[o[2]] & m->flag[o[3]] & SPV_UNIFORM_ID))
                    more |= mark(m, o[1], SPV_UNIFORM_ID, 0);
            } else if (op == SPV_FUNCTION_CALL && len >= 4 && o[2] < m->bound && m->func_at[o[2]]) {
                u32 p = m->func_at[o[2]];
                p += m->w[p] >> 16;
                for (u32 j = 3;
                     j < len - 1 && p < m->n && (m->w[p] & 0xffff) == SPV_FUNCTION_PARAMETER && m->w[p] >> 16 >= 3;
                     j++, p += m->w[p] >> 16)
                    if (o[j] < m->bound && m->kind[o[j]] >= KIND_SAMPLED)
                        more |= mark(m, m->w[p + 2], m->flag[o[j]] & (SPV_INDEXED | SPV_ARRAY), m->kind[o[j]]);
            }
        }
    }
}
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
    u8* kind = gvk_scratch(bound);
    u32* elem = gvk_scratch(sizeof *elem * bound);
    u32* struct_at = gvk_scratch(sizeof *struct_at * bound);
    u32* pointee = gvk_scratch(sizeof *pointee * bound);
    u32* stride = gvk_scratch(sizeof *stride * bound);
    u32* func_at = gvk_scratch(sizeof *func_at * bound);
    memset(flag, 0, bound);
    memset(kind, 0, bound);
    memset(elem, 0, sizeof *elem * bound);
    memset(struct_at, 0, sizeof *struct_at * bound);
    memset(pointee, 0, sizeof *pointee * bound);
    memset(stride, 0, sizeof *stride * bound);
    memset(func_at, 0, sizeof *func_at * bound);
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
    int has_cap[KINDS] = {0}, has_ext = 0, need[KINDS] = {0}; /* has_cap[0]: ShaderNonUniform */
    for (u32 i = 5; i < n;) {
        u32 len = w[i] >> 16, op = w[i] & 0xffff;
        const u32* o = w + i + 1;
        if (op == SPV_CAPABILITY && len >= 2) {
            if ((o[0] == SPV_CAP_UBO_NON_UNIFORM && !(allow & GVK_SPIRV_UBO_NONUNIFORM)) ||
                (o[0] == SPV_CAP_SSBO_NON_UNIFORM && !(allow & GVK_SPIRV_SSBO_NONUNIFORM)))
                return NULL;
            caps_end = i + len;
            has_cap[0] |= o[0] == SPV_CAP_NON_UNIFORM;
            for (u32 k = KIND_UBO; k < KINDS; k++) has_cap[k] |= o[0] == kind_cap(k);
        } else if (op == SPV_EXTENSION && len - 1 >= SPV_EXT_WORDS) {
            has_ext |= !memcmp(o, g_ext_name, sizeof g_ext_name);
        } else if ((op == SPV_TYPE_MATRIX || op == SPV_TYPE_STRUCT) && len >= 2 && o[0] < bound) {
            if (op == SPV_TYPE_MATRIX)
                flag[o[0]] |= SPV_MATRIX;
            else
                struct_at[o[0]] = i;
        } else if (op == SPV_TYPE_IMAGE && len >= 9 && o[0] < bound) {
            int loads = o[6] != SPV_SAMPLED_NO; /* sampled, or not known until it is made */
            kind[o[0]] = o[2] == SPV_DIM_SUBPASS  ? KIND_INPUT
                         : o[2] == SPV_DIM_BUFFER ? (loads ? KIND_UNIFORM_TEXEL : KIND_STORAGE_TEXEL)
                         : loads                  ? KIND_SAMPLED
                                                  : KIND_STORAGE_IMAGE;
        } else if (op == SPV_TYPE_SAMPLER && len >= 2 && o[0] < bound) {
            kind[o[0]] = KIND_SAMPLED;
        } else if (op == SPV_TYPE_SAMPLED_IMAGE && len >= 3 && o[0] < bound && o[1] < bound) {
            kind[o[0]] = kind[o[1]];
        } else if ((op == SPV_TYPE_ARRAY || op == SPV_TYPE_RUNTIME_ARRAY) && len >= 3 && o[0] < bound) {
            elem[o[0]] = o[1];
        } else if (op == SPV_TYPE_POINTER && len >= 4 && o[0] < bound) {
            pointee[o[0]] = o[2];
        } else if ((op == SPV_CONSTANT || op == SPV_SPEC_CONSTANT || op == SPV_CONSTANT_NULL ||
                    op == SPV_SPEC_CONSTANT_OP) &&
                   len >= 3 && o[1] < bound) {
            flag[o[1]] |= SPV_UNIFORM_ID;
        } else if (op == SPV_FUNCTION && len >= 5 && o[1] < bound) {
            func_at[o[1]] = i;
        } else if (op == SPV_VARIABLE && len >= 4 && o[1] < bound && o[0] < bound) {
            /* an array of descriptors, whose types come before variables, or what push constants and a uniform buffer
               hold, which no invocation changes */
            u32 t = pointee[o[0]], e = t && t < bound ? elem[t] : 0;
            if (e && e < bound && o[2] == SPV_UNIFORM_CONSTANT && kind[e]) {
                flag[o[1]] |= SPV_ARRAY;
                kind[o[1]] = kind[e];
            } else if (e && (o[2] == SPV_UNIFORM || o[2] == SPV_STORAGE_BUFFER)) {
                flag[o[1]] |= SPV_ARRAY;
                kind[o[1]] =
                    o[2] == SPV_STORAGE_BUFFER || (e < bound && (flag[e] & SPV_BUFFER_TYPE)) ? KIND_SSBO : KIND_UBO;
            } else if (o[2] == SPV_PUSH_CONSTANT ||
                       (o[2] == SPV_UNIFORM && !e && t && t < bound && !(flag[t] & SPV_BUFFER_TYPE))) {
                flag[o[1]] |= SPV_UNIFORM_PTR;
            }
        } else if (op == SPV_DECORATE && len >= 3 && o[0] < bound) {
            if (o[1] == SPV_ARRAY_STRIDE) flag[o[0]] |= SPV_STRIDED;
        } else if (op == SPV_MEMBER_DECORATE && len >= 4 && (o[2] == SPV_ARRAY_STRIDE || o[2] == SPV_MATRIX_STRIDE)) {
            fixes++;
            extra += o[2] == SPV_MATRIX_STRIDE ? 4 : 0;
        }
        i += len;
    }
    Marks marks = {w, n, bound, allow, flag, kind, func_at};
    mark_indexed(&marks);
    for (u32 id = 1; id < bound; id++) {
        if (!(flag[id] & SPV_INDEXED)) continue;
        need[kind[id]] = 1;
        if (!(flag[id] & SPV_HAS_NU)) indexed++;
    }
    if ((need[KIND_UBO] && !(allow & GVK_SPIRV_UBO_NONUNIFORM)) ||
        (need[KIND_SSBO] && !(allow & GVK_SPIRV_SSBO_NONUNIFORM)))
        return NULL;
    *out_bytes = bytes;
    /* the capabilities and the extension NonUniform needs, where the module has them not */
    int add_cap[KINDS] = {indexed && !has_cap[0]}, add_ext = indexed && !has_ext, added = add_cap[0];
    for (u32 k = KIND_UBO; k < KINDS; k++) added += add_cap[k] = indexed && need[k] && !has_cap[k];
    if (!fixes && !indexed) return w;
    extra += 3 * (size_t)indexed + 2 * (size_t)added + (add_ext ? 1 + SPV_EXT_WORDS : 0);
    if (extra > UINT32_MAX - n || extra > SIZE_MAX / sizeof(u32) - n) return NULL;
    u32* out = gvk_scratch(sizeof *out * (n + extra));
    memcpy(out, w, 5 * sizeof *out);
    u32 o = 5, mended = 0;
    int decorated = !indexed;
    for (u32 i = 5; i <= n; i += i < n ? w[i] >> 16 : 1) {
        if (i == caps_end) {
            for (u32 k = 0; k < KINDS; k++)
                if (add_cap[k]) {
                    out[o++] = 2u << 16 | SPV_CAPABILITY;
                    out[o++] = k ? kind_cap(k) : SPV_CAP_NON_UNIFORM;
                }
            if (add_ext) {
                out[o++] = (1 + SPV_EXT_WORDS) << 16 | SPV_EXTENSION;
                memset(out + o, 0, SPV_EXT_WORDS * 4);
                memcpy(out + o, g_ext_name, sizeof g_ext_name);
                o += SPV_EXT_WORDS;
            }
            mended += (u32)(added + add_ext);
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

/* the descriptor a size query's image operand id was loaded from, as GvkSizeSlots names it: through OpImage,
   OpSampledImage and OpCopyObject to the OpLoad of a variable, or of its array's element by a constant. gvk follows
   the images of bindings and elements below 8 alone (gvk_api.c's set_bit) */
static int image_slot(const u32* w, const u32* def, const u32* dset, const u32* dbind, u32 bound, u32 id, u16* slot) {
    for (u32 hops = 0; hops < 16; hops++) {
        if (id >= bound || !def[id]) return 0;
        const u32* d = w + def[id];
        u32 op = d[0] & 0xffff;
        if (op == SPV_IMAGE || op == SPV_SAMPLED_IMAGE || op == SPV_COPY_OBJECT) {
            id = d[3];
            continue;
        }
        if (op != SPV_LOAD) return 0;
        u32 ptr = d[3], elem = 0;
        if (ptr >= bound || !def[ptr]) return 0;
        const u32* p = w + def[ptr];
        u32 pop = p[0] & 0xffff;
        if (pop == SPV_ACCESS_CHAIN || pop == SPV_IN_BOUNDS_ACCESS_CHAIN) {
            if (p[0] >> 16 != 5) return 0;
            u32 idx = p[4];
            if (idx >= bound || !def[idx] || w[def[idx]] != (4u << 16 | SPV_CONSTANT)) return 0;
            elem = w[def[idx] + 3];
            ptr = p[3];
            if (ptr >= bound || !def[ptr]) return 0;
            pop = w[def[ptr]] & 0xffff;
        }
        if (pop != SPV_VARIABLE || dset[ptr] > 3 || dbind[ptr] > 7 || elem > 7) return 0;
        *slot = (u16)(dset[ptr] << 14 | dbind[ptr] << 8 | elem);
        return 1;
    }
    return 0;
}

/* whether the OpEntryPoint at e lists the global id among its interface, after its name */
static int entry_lists(const u32* e, u32 id) {
    u32 len = e[0] >> 16, at = 3;
    while (at < len && (e[at] & 0xff000000u) && (e[at] & 0xff0000u) && (e[at] & 0xff00u) && (e[at] & 0xffu)) at++;
    for (at++; at < len; at++)
        if (e[at] == id) return 1;
    return 0;
}
/* Byte extent of a push-constant type, or UINT32_MAX if unknown or reaching past GVK_SIZE_PUSH */
static u32 spv_type_size(const u32* w, u32 types_at, const u32* def, const u32* astride, u32 bound, u32 tid,
                         u32 mstride, int row_major, int depth) {
    if (depth > 8 || tid >= bound || !def[tid]) return UINT32_MAX;
    const u32* d = w + def[tid];
    u32 len = d[0] >> 16, op = d[0] & 0xffff;
    if (op == SPV_TYPE_INT || op == SPV_TYPE_FLOAT) {
        if (len < 3 || !d[2] || d[2] > 64) return UINT32_MAX;
        return (d[2] + 7) / 8;
    }
    if (op == SPV_TYPE_VECTOR) {
        if (len < 4) return UINT32_MAX;
        u32 elem = spv_type_size(w, types_at, def, astride, bound, d[2], 0, 0, depth + 1);
        if (elem == UINT32_MAX || d[3] > GVK_SIZE_PUSH / elem) return UINT32_MAX;
        return elem * d[3];
    }
    if (op == SPV_TYPE_MATRIX) {
        if (len < 4) return UINT32_MAX;
        u32 col = d[2], cols = d[3];
        if (col >= bound || !def[col] || (w[def[col]] & 0xffff) != SPV_TYPE_VECTOR || (w[def[col]] >> 16) < 4)
            return UINT32_MAX;
        u32 rows = w[def[col] + 3];
        u32 col_sz = spv_type_size(w, types_at, def, astride, bound, col, 0, 0, depth + 1);
        if (col_sz == UINT32_MAX) return UINT32_MAX;
        u32 nvecs = row_major ? rows : cols;
        u32 stride = mstride ? mstride : col_sz;
        if (stride < col_sz && !row_major) stride = col_sz;
        if (!stride || nvecs > GVK_SIZE_PUSH / stride) return UINT32_MAX;
        return nvecs * stride;
    }
    if (op == SPV_TYPE_ARRAY) {
        if (len < 4) return UINT32_MAX;
        u32 cid = d[3];
        if (cid >= bound || !def[cid] || w[def[cid]] != (4u << 16 | SPV_CONSTANT)) return UINT32_MAX;
        u32 count = w[def[cid] + 3];
        if (!count) return 0;
        u32 elem = spv_type_size(w, types_at, def, astride, bound, d[2], mstride, row_major, depth + 1);
        if (elem == UINT32_MAX) return UINT32_MAX;
        u32 stride = astride[tid] ? astride[tid] : elem;
        if (stride < elem) stride = elem;
        if (count > GVK_SIZE_PUSH / stride) return UINT32_MAX;
        return count * stride;
    }
    if (op == SPV_TYPE_STRUCT) {
        u32 members = len >= 2 ? len - 2 : 0;
        u32 end = 0;
        for (u32 m = 0; m < members; m++) {
            u32 off = UINT32_MAX, ms = 0;
            int rm = 0;
            for (u32 i = 5; i < types_at; i += w[i] >> 16) {
                if ((w[i] & 0xffff) != SPV_MEMBER_DECORATE || w[i + 1] != tid || w[i + 2] != m) continue;
                u32 dl = w[i] >> 16, dec = w[i + 3];
                if (dec == SPV_OFFSET && dl >= 5)
                    off = w[i + 4];
                else if (dec == SPV_MATRIX_STRIDE && dl >= 5)
                    ms = w[i + 4];
                else if (dec == SPV_ROW_MAJOR)
                    rm = 1;
            }
            if (off >= GVK_SIZE_PUSH) return UINT32_MAX;
            u32 sz = spv_type_size(w, types_at, def, astride, bound, d[2 + m], ms, rm, depth + 1);
            if (sz == UINT32_MAX || sz > GVK_SIZE_PUSH - off) return UINT32_MAX;
            if (off + sz > end) end = off + sz;
        }
        return end;
    }
    return UINT32_MAX;
}
/* Each query's result id becomes a new one, and its old id the result divided:
     the push uvec4 loaded, d = 1 + ((x >> k) & 1) * y, and the width and height divided by d (a third component, an
     array's layers, kept). A scalar query (a 1D image) is never of an image drawn at the scale, and stays.
   The uvec4 is a member at GVK_SIZE_PUSH of the module's push constant block, or of a block of its own where it has
   none. uint and uvec4 are made where the module has none, before the types (a block's members come first), the
   pointers and constants before the functions */
const u32* gvk_spirv_scaled_size(const u32* w, size_t bytes, size_t* out_bytes, GvkSizeSlots* slots) {
    *slots = (GvkSizeSlots){0};
    *out_bytes = bytes;
    if (bytes % 4 || bytes / 4 > UINT32_MAX || bytes / 4 < 5 || w[0] != 0x07230203u || !w[3] || w[3] > SPV_IDS)
        return NULL;
    u32 n = (u32)(bytes / 4), bound = w[3];
    u32* def = gvk_scratch(sizeof *def * bound); /* the instruction that makes each id gvk follows, 0 for none */
    u32* dset = gvk_scratch(sizeof *dset * bound);
    u32* dbind = gvk_scratch(sizeof *dbind * bound);
    u32* astride = gvk_scratch(sizeof *astride * bound);
    memset(def, 0, sizeof *def * bound);
    memset(dset, 0xff, sizeof *dset * bound);
    memset(dbind, 0xff, sizeof *dbind * bound);
    memset(astride, 0, sizeof *astride * bound);
    u32 types_at = 0, funcs_at = 0, queries = 0, uint_t = 0, v4u_t = 0, pc_var = 0, pc_ptr = 0, entries = 0;
    int fragment = 0, other = 0;
    for (u32 i = 5; i < n;) {
        u32 len = w[i] >> 16, op = w[i] & 0xffff;
        if (!len || len > n - i) return NULL;
        const u32* o = w + i + 1;
        int global = (op >= SPV_TYPE_VOID && op <= SPV_TYPE_FORWARD_POINTER) ||
                     (op >= SPV_CONSTANT_TRUE && op <= SPV_SPEC_CONSTANT_OP) || op == SPV_UNDEF ||
                     (op == SPV_VARIABLE && !funcs_at);
        if (global && !types_at) types_at = i;
        if (op == SPV_FUNCTION && !funcs_at) funcs_at = i;
        if (op == SPV_ENTRY_POINT && len >= 3) {
            fragment |= o[0] == SPV_MODEL_FRAGMENT;
            other |= o[0] != SPV_MODEL_FRAGMENT;
            entries++;
        } else if (op == SPV_DECORATE && len >= 4 && o[0] < bound) {
            if (o[1] == SPV_DESCRIPTOR_SET) dset[o[0]] = o[2];
            if (o[1] == SPV_BINDING) dbind[o[0]] = o[2];
            if (o[1] == SPV_ARRAY_STRIDE) astride[o[0]] = o[2];
        } else if (op == SPV_TYPE_INT && len >= 4 && o[0] < bound) {
            def[o[0]] = i;
            if (o[1] == 32 && o[2] == 0 && !uint_t) uint_t = o[0];
        } else if (op == SPV_TYPE_FLOAT && len >= 3 && o[0] < bound) {
            def[o[0]] = i;
        } else if (op == SPV_TYPE_VECTOR && len >= 4 && o[0] < bound) {
            def[o[0]] = i;
            if (uint_t && o[1] == uint_t && o[2] == 4 && !v4u_t) v4u_t = o[0];
        } else if ((op == SPV_TYPE_MATRIX || op == SPV_TYPE_ARRAY || op == SPV_TYPE_POINTER) && len >= 4 &&
                   o[0] < bound) {
            def[o[0]] = i;
        } else if (op == SPV_TYPE_STRUCT && len >= 2 && o[0] < bound) {
            def[o[0]] = i;
        } else if (op == SPV_VARIABLE && len >= 4 && o[1] < bound) {
            def[o[1]] = i;
            if (o[2] == SPV_PUSH_CONSTANT && !funcs_at) {
                if (pc_var) return w; /* two blocks: which takes the member is the entry point's */
                pc_var = o[1];
                pc_ptr = o[0];
            }
        } else if ((op == SPV_CONSTANT || op == SPV_LOAD || op == SPV_ACCESS_CHAIN ||
                    op == SPV_IN_BOUNDS_ACCESS_CHAIN || op == SPV_IMAGE || op == SPV_SAMPLED_IMAGE ||
                    op == SPV_COPY_OBJECT) &&
                   len >= 4 && o[1] < bound) {
            def[o[1]] = i;
        } else if ((op == SPV_QUERY_SIZE_LOD || op == SPV_QUERY_SIZE) && len >= 4 && funcs_at) {
            queries++;
        }
        i += len;
    }
    if (!queries || !fragment || other || !types_at || !funcs_at) return w;
    /* the push block's structure, whose members end before GVK_SIZE_PUSH, and declared after uvec4 */
    u32 pc_struct = 0, members = 0;
    if (pc_var) {
        if (pc_ptr >= bound || !def[pc_ptr] || (w[def[pc_ptr]] & 0xffff) != SPV_TYPE_POINTER) return NULL;
        pc_struct = w[def[pc_ptr] + 3];
        if (pc_struct >= bound || !def[pc_struct] || (w[def[pc_struct]] & 0xffff) != SPV_TYPE_STRUCT) return w;
        members = (w[def[pc_struct]] >> 16) - 2;
        if (v4u_t && def[v4u_t] > def[pc_struct]) return w;
        if (spv_type_size(w, types_at, def, astride, bound, pc_struct, 0, 0, 0) > GVK_SIZE_PUSH) return w;
    }
    /* each query's bit, or none */
    u32* q_at = gvk_scratch(sizeof *q_at * queries);
    u8* q_bit = gvk_scratch(queries);
    u32 nq = 0;
    for (u32 i = funcs_at; i < n; i += w[i] >> 16) {
        u32 op = w[i] & 0xffff;
        if ((op != SPV_QUERY_SIZE_LOD && op != SPV_QUERY_SIZE) || (w[i] >> 16) < 4) continue;
        u32 rt = w[i + 1];
        if (rt >= bound || !def[rt] || (w[def[rt]] & 0xffff) != SPV_TYPE_VECTOR) continue; /* a 1D image's */
        u32 count = w[def[rt] + 3];
        u16 slot;
        u32 comp = w[def[rt] + 2]; /* a 32-bit integer's, the divisor's type */
        int bits32 = comp < bound && def[comp] && (w[def[comp]] & 0xffff) == SPV_TYPE_INT && w[def[comp] + 2] == 32;
        if (count < 2 || count > 3 || !bits32 || !image_slot(w, def, dset, dbind, bound, w[i + 3], &slot)) {
            slots->skipped++;
            continue;
        }
        u32 k = 0;
        while (k < slots->n && slots->slot[k] != slot) k++;
        if (k == GVK_SIZE_SLOTS) {
            slots->skipped++;
            continue;
        }
        if (k == slots->n) slots->slot[slots->n++] = slot;
        q_at[nq] = i;
        q_bit[nq++] = (u8)k;
    }
    if (!nq) return w;
    int add_uint = !uint_t, add_v4u = !v4u_t, own_block = !pc_var;
    /* SPIR-V 1.4 lists every global an entry point reads: the block, where an entry point doesn't */
    int list_var = w[1] >= 0x00010400u;
    size_t extra = 13 + 4 * (size_t)(add_uint + add_v4u) + 4 * (size_t)(2 + slots->n) + (own_block ? 14 : 1) +
                   (list_var ? entries : 0) + 75 * (size_t)nq;
    if (extra > UINT32_MAX - n || extra > SIZE_MAX / sizeof(u32) - n) return NULL;
    u32 next = bound;
    if (add_uint) uint_t = next++;
    if (add_v4u) v4u_t = next++;
    u32 ptr_t = next++, c1 = next++, cmember = next++, ck[GVK_SIZE_SLOTS];
    for (u32 k = 0; k < slots->n; k++) ck[k] = next++;
    if (own_block) {
        pc_struct = next++;
        pc_ptr = next++;
        pc_var = next++;
    }
    if ((u64)next + 15ull * nq > SPV_IDS) return NULL;
    /* uvec4 after the module's uint, or after gvk's ahead of every type */
    u32 v4u_at = add_uint ? types_at : def[uint_t] + (w[def[uint_t]] >> 16);
    if (add_v4u && !add_uint && !own_block && def[uint_t] > def[pc_struct]) {
        slots->n = 0;
        return w;
    }
    u32* out = gvk_scratch(sizeof *out * (n + extra));
    memcpy(out, w, 5 * sizeof *out);
    u32 o = 5, q = 0;
    for (u32 i = 5; i < n;) {
        u32 len = w[i] >> 16, op = w[i] & 0xffff;
        if (i == types_at) { /* the member's offset, then uint ahead of every type */
            if (own_block) {
                out[o++] = 3u << 16 | SPV_DECORATE;
                out[o++] = pc_struct;
                out[o++] = SPV_BLOCK;
            }
            out[o++] = 5u << 16 | SPV_MEMBER_DECORATE;
            out[o++] = pc_struct;
            out[o++] = own_block ? 0 : members;
            out[o++] = SPV_OFFSET;
            out[o++] = GVK_SIZE_PUSH;
            if (add_uint) {
                out[o++] = 4u << 16 | SPV_TYPE_INT;
                out[o++] = uint_t;
                out[o++] = 32;
                out[o++] = 0;
            }
        }
        if (add_v4u && i == v4u_at) {
            out[o++] = 4u << 16 | SPV_TYPE_VECTOR;
            out[o++] = v4u_t;
            out[o++] = uint_t;
            out[o++] = 4;
        }
        if (i == funcs_at) { /* the pointer, the constants and a block of gvk's own, after every global */
            if (own_block) {
                out[o++] = 3u << 16 | SPV_TYPE_STRUCT;
                out[o++] = pc_struct;
                out[o++] = v4u_t;
                out[o++] = 4u << 16 | SPV_TYPE_POINTER;
                out[o++] = pc_ptr;
                out[o++] = SPV_PUSH_CONSTANT;
                out[o++] = pc_struct;
                out[o++] = 4u << 16 | SPV_VARIABLE;
                out[o++] = pc_ptr;
                out[o++] = pc_var;
                out[o++] = SPV_PUSH_CONSTANT;
            }
            out[o++] = 4u << 16 | SPV_TYPE_POINTER;
            out[o++] = ptr_t;
            out[o++] = SPV_PUSH_CONSTANT;
            out[o++] = v4u_t;
            const u32 consts[2][2] = {{c1, 1}, {cmember, own_block ? 0 : members}};
            for (u32 c = 0; c < 2; c++) {
                out[o++] = 4u << 16 | SPV_CONSTANT;
                out[o++] = uint_t;
                out[o++] = consts[c][0];
                out[o++] = consts[c][1];
            }
            for (u32 k = 0; k < slots->n; k++) {
                out[o++] = 4u << 16 | SPV_CONSTANT;
                out[o++] = uint_t;
                out[o++] = ck[k];
                out[o++] = k;
            }
        }
        if (op == SPV_TYPE_STRUCT && !own_block && w[i + 1] == pc_struct) {
            memcpy(out + o, w + i, sizeof *out * len);
            out[o] += 1u << 16;
            o += len;
            out[o++] = v4u_t;
        } else if (op == SPV_ENTRY_POINT && list_var && !entry_lists(w + i, pc_var)) {
            memcpy(out + o, w + i, sizeof *out * len);
            out[o] += 1u << 16;
            o += len;
            out[o++] = pc_var;
        } else if (q < nq && i == q_at[q]) {
            u32 rt = w[i + 1], res = w[i + 1 + 1], comp = w[def[rt] + 2], count = w[def[rt] + 3];
            int sign = comp < bound && def[comp] && w[def[comp] + 3];
            int cast = comp != uint_t;
            u32 nq_id = next++, pp = next++, pv = next++, mask = next++, sm1 = next++, sh = next++, b = next++,
                m = next++, du = next++, d = cast ? next++ : du;
            memcpy(out + o, w + i, sizeof *out * len);
            out[o + 2] = nq_id;
            o += len;
            const u32 seq[][6] = {
                {5u << 16 | SPV_ACCESS_CHAIN, ptr_t, pp, pc_var, cmember, 0},
                {4u << 16 | SPV_LOAD, v4u_t, pv, pp, 0, 0},
                {5u << 16 | SPV_COMPOSITE_EXTRACT, uint_t, mask, pv, 0, 0},
                {5u << 16 | SPV_COMPOSITE_EXTRACT, uint_t, sm1, pv, 1, 0},
                {5u << 16 | SPV_SHIFT_RIGHT_LOGICAL, uint_t, sh, mask, ck[q_bit[q]], 0},
                {5u << 16 | SPV_BITWISE_AND, uint_t, b, sh, c1, 0},
                {5u << 16 | SPV_IMUL, uint_t, m, b, sm1, 0},
                {5u << 16 | SPV_IADD, uint_t, du, m, c1, 0},
            };
            for (u32 s = 0; s < sizeof seq / sizeof *seq; s++) {
                u32 l = seq[s][0] >> 16;
                memcpy(out + o, seq[s], sizeof *out * l);
                o += l;
            }
            if (cast) {
                out[o++] = 4u << 16 | SPV_BITCAST;
                out[o++] = comp;
                out[o++] = d;
                out[o++] = du;
            }
            u32 part[3];
            for (u32 c = 0; c < count; c++) {
                u32 x = next++;
                out[o++] = 5u << 16 | SPV_COMPOSITE_EXTRACT;
                out[o++] = comp;
                out[o++] = x;
                out[o++] = nq_id;
                out[o++] = c;
                part[c] = x;
                if (c < 2) {
                    part[c] = next++;
                    out[o++] = 5u << 16 | (sign ? SPV_SDIV : SPV_UDIV);
                    out[o++] = comp;
                    out[o++] = part[c];
                    out[o++] = x;
                    out[o++] = d;
                }
            }
            out[o++] = (3 + count) << 16 | SPV_COMPOSITE_CONSTRUCT;
            out[o++] = rt;
            out[o++] = res;
            for (u32 c = 0; c < count; c++) out[o++] = part[c];
            q++;
        } else {
            memcpy(out + o, w + i, sizeof *out * len);
            o += len;
        }
        i += len;
    }
    out[3] = next;
    *out_bytes = (size_t)o * 4;
    return out;
}
