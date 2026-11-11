/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* SPIR-V reflection for snx-shaderperf (reflect.h). The enumerants are the SPIR-V specification's (unified1), written
   out here so that the tool needs no SPIR-V header */
#include "reflect.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    OP_NAME = 5, OP_MEMBER_NAME = 6, OP_EXTENSION = 10, OP_ENTRY_POINT = 15, OP_EXECUTION_MODE = 16,
    OP_CAPABILITY = 17, OP_TYPE_VOID = 19, OP_TYPE_BOOL = 20, OP_TYPE_INT = 21, OP_TYPE_FLOAT = 22,
    OP_TYPE_VECTOR = 23, OP_TYPE_MATRIX = 24, OP_TYPE_IMAGE = 25, OP_TYPE_SAMPLER = 26, OP_TYPE_SAMPLED_IMAGE = 27,
    OP_TYPE_ARRAY = 28, OP_TYPE_RUNTIME_ARRAY = 29, OP_TYPE_STRUCT = 30, OP_TYPE_POINTER = 32,
    OP_CONSTANT_TRUE = 41, OP_CONSTANT_FALSE = 42, OP_CONSTANT = 43, OP_CONSTANT_COMPOSITE = 44,
    OP_SPEC_CONSTANT_TRUE = 48, OP_SPEC_CONSTANT_FALSE = 49, OP_SPEC_CONSTANT = 50,
    OP_SPEC_CONSTANT_COMPOSITE = 51, OP_VARIABLE = 59, OP_DECORATE = 71, OP_MEMBER_DECORATE = 72,
    OP_EXECUTION_MODE_ID = 331, OP_TYPE_ACCELERATION_STRUCTURE = 5341,
};
enum {
    DEC_SPEC_ID = 1, DEC_BLOCK = 2, DEC_BUFFER_BLOCK = 3, DEC_ARRAY_STRIDE = 6, DEC_MATRIX_STRIDE = 7,
    DEC_BUILTIN = 11, DEC_FLAT = 14, DEC_NON_WRITABLE = 24, DEC_LOCATION = 30, DEC_COMPONENT = 31,
    DEC_BINDING = 33, DEC_DESCRIPTOR_SET = 34, DEC_OFFSET = 35,
};
enum { SC_UNIFORM_CONSTANT = 0, SC_INPUT = 1, SC_UNIFORM = 2, SC_OUTPUT = 3, SC_PUSH_CONSTANT = 9, SC_STORAGE = 12 };
enum {
    EM_INVOCATIONS = 0, EM_ORIGIN_LOWER_LEFT = 8, EM_EARLY_TESTS = 9, EM_DEPTH_REPLACING = 12, EM_LOCAL_SIZE = 17,
    EM_INPUT_POINTS = 19, EM_INPUT_LINES = 20, EM_INPUT_LINES_ADJACENCY = 21, EM_TRIANGLES = 22,
    EM_INPUT_TRIANGLES_ADJACENCY = 23, EM_OUTPUT_VERTICES = 26, EM_OUTPUT_POINTS = 27, EM_OUTPUT_LINE_STRIP = 28,
    EM_OUTPUT_TRIANGLE_STRIP = 29, EM_LOCAL_SIZE_ID = 38,
};
#define BUILTIN_WORKGROUP_SIZE 25
#define NONE UINT32_MAX

/* what one id has been given: its defining instruction and its decorations */
typedef struct {
    uint32_t at; /* word index of the defining instruction, 0 for none */
    uint32_t set, binding, location, component, spec_id, builtin, array_stride;
    bool block, buffer_block, flat, non_writable;
    const char* name;
} Id;

typedef struct {
    uint32_t id, member, decoration, value;
} MemberDec;
typedef struct {
    uint32_t id, member;
    const char* name;
} MemberName;

typedef struct {
    const uint32_t* w;
    size_t n;
    Id* ids;
    uint32_t bound;
    MemberDec* mdec;
    size_t mdec_count, mdec_cap;
    MemberName* mname;
    size_t mname_count, mname_cap;
} Ctx;

static void fail(char* err, size_t size, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, size, fmt, ap);
    va_end(ap);
}

static void* grow(void* p, size_t* cap, size_t count, size_t elem) {
    if (count < *cap)
        return p;
    size_t ncap = *cap ? *cap * 2 : 16;
    void* q = realloc(p, ncap * elem);
    if (!q) {
        fprintf(stderr, "snx-shaderperf: out of memory\n");
        exit(2);
    }
    *cap = ncap;
    return q;
}

/* strnlen, which C99 lacks */
static size_t bounded_len(const char* s, size_t max) {
    size_t n = 0;
    while (n < max && s[n])
        n++;
    return n;
}

/* a literal string's characters in place: SPIR-V strings end in a NUL inside their words. The words a string spans */
static uint32_t str_words(const uint32_t* w, uint32_t avail) {
    const char* s = (const char*)w;
    size_t len = bounded_len(s, (size_t)avail * 4);
    return (uint32_t)(len / 4 + 1);
}

static void copy_name(char* dst, size_t size, const char* src) {
    snprintf(dst, size, "%s", src ? src : "");
}

static const uint32_t* def(const Ctx* c, uint32_t id) {
    if (id == 0 || id >= c->bound || !c->ids[id].at)
        return NULL;
    return c->w + c->ids[id].at;
}

static uint32_t def_op(const Ctx* c, uint32_t id) {
    const uint32_t* d = def(c, id);
    return d ? (d[0] & 0xffffu) : 0;
}

static uint32_t mdec(const Ctx* c, uint32_t id, uint32_t member, uint32_t decoration, uint32_t fallback) {
    for (size_t i = 0; i < c->mdec_count; i++)
        if (c->mdec[i].id == id && c->mdec[i].member == member && c->mdec[i].decoration == decoration)
            return c->mdec[i].value;
    return fallback;
}

static const char* mname(const Ctx* c, uint32_t id, uint32_t member) {
    for (size_t i = 0; i < c->mname_count; i++)
        if (c->mname[i].id == id && c->mname[i].member == member)
            return c->mname[i].name;
    return NULL;
}

/* an OpConstant's or OpSpecConstant's value (its default), its low 64 bits */
static bool const_value(const Ctx* c, uint32_t id, uint64_t* out) {
    const uint32_t* d = def(c, id);
    if (!d)
        return false;
    uint32_t op = d[0] & 0xffffu, wc = d[0] >> 16;
    if (op == OP_CONSTANT || op == OP_SPEC_CONSTANT) {
        *out = d[3];
        if (wc > 4)
            *out |= (uint64_t)d[4] << 32;
        return true;
    }
    if (op == OP_CONSTANT_TRUE || op == OP_SPEC_CONSTANT_TRUE || op == OP_CONSTANT_FALSE ||
        op == OP_SPEC_CONSTANT_FALSE) {
        *out = op == OP_CONSTANT_TRUE || op == OP_SPEC_CONSTANT_TRUE;
        return true;
    }
    return false;
}

/* a scalar or vector type's element kind and width */
static void scalar_of(const Ctx* c, uint32_t type, uint8_t* scalar, uint8_t* width) {
    const uint32_t* d = def(c, type);
    *scalar = SR_OTHER;
    *width = 0;
    while (d && ((d[0] & 0xffffu) == OP_TYPE_VECTOR || (d[0] & 0xffffu) == OP_TYPE_MATRIX ||
                 (d[0] & 0xffffu) == OP_TYPE_ARRAY || (d[0] & 0xffffu) == OP_TYPE_RUNTIME_ARRAY))
        d = def(c, d[2]);
    if (!d)
        return;
    switch (d[0] & 0xffffu) {
    case OP_TYPE_INT:
        *scalar = d[3] ? SR_SINT : SR_UINT;
        *width = (uint8_t)d[2];
        break;
    case OP_TYPE_FLOAT:
        *scalar = SR_FLOAT;
        *width = (uint8_t)d[2];
        break;
    case OP_TYPE_BOOL:
        *scalar = SR_BOOL;
        *width = 32;
        break;
    default:
        break;
    }
}

/* bytes of type laid out in a block; matrix_stride is the member's MatrixStride for a matrix (0 when unknown) */
static uint32_t type_size(const Ctx* c, uint32_t type, uint32_t matrix_stride, int depth) {
    const uint32_t* d = def(c, type);
    if (!d || depth > 32)
        return 0;
    switch (d[0] & 0xffffu) {
    case OP_TYPE_BOOL:
        return 4;
    case OP_TYPE_INT:
    case OP_TYPE_FLOAT:
        return d[2] / 8;
    case OP_TYPE_VECTOR:
        return d[3] * type_size(c, d[2], 0, depth + 1);
    case OP_TYPE_MATRIX: {
        uint32_t col = type_size(c, d[2], 0, depth + 1);
        uint32_t stride = matrix_stride ? matrix_stride : col;
        return (d[3] - 1) * stride + col;
    }
    case OP_TYPE_ARRAY: {
        uint64_t len = 0;
        if (!const_value(c, d[3], &len))
            return 0;
        uint32_t stride = c->ids[type].array_stride;
        if (!stride)
            stride = type_size(c, d[2], matrix_stride, depth + 1);
        return (uint32_t)len * stride;
    }
    case OP_TYPE_RUNTIME_ARRAY:
        return 0;
    case OP_TYPE_STRUCT: {
        uint32_t members = (d[0] >> 16) - 2, size = 0;
        for (uint32_t i = 0; i < members; i++) {
            uint32_t off = mdec(c, type, i, DEC_OFFSET, 0);
            uint32_t end = off + type_size(c, d[2 + i], mdec(c, type, i, DEC_MATRIX_STRIDE, 0), depth + 1);
            if (end > size)
                size = end;
        }
        return size;
    }
    default:
        return 0;
    }
}

/* a block's members, flattened one level */
static SrMember* block_members(const Ctx* c, uint32_t type, size_t* count) {
    const uint32_t* d = def(c, type);
    *count = 0;
    if (!d || (d[0] & 0xffffu) != OP_TYPE_STRUCT)
        return NULL;
    uint32_t members = (d[0] >> 16) - 2;
    SrMember* out = calloc(members ? members : 1, sizeof *out);
    if (!out)
        return NULL;
    for (uint32_t i = 0; i < members; i++) {
        SrMember* m = &out[i];
        uint32_t mt = d[2 + i];
        const char* name = mname(c, type, i);
        if (name && *name)
            copy_name(m->name, sizeof m->name, name);
        else
            snprintf(m->name, sizeof m->name, "_m%u", i);
        m->offset = mdec(c, type, i, DEC_OFFSET, 0);
        uint32_t mstride = mdec(c, type, i, DEC_MATRIX_STRIDE, 0);
        m->size = type_size(c, mt, mstride, 0);
        m->vec = 1;
        m->cols = 1;
        const uint32_t* e = def(c, mt);
        if (e && ((e[0] & 0xffffu) == OP_TYPE_ARRAY || (e[0] & 0xffffu) == OP_TYPE_RUNTIME_ARRAY)) {
            uint64_t len = 0;
            m->array = (e[0] & 0xffffu) == OP_TYPE_RUNTIME_ARRAY ? UINT32_MAX
                       : const_value(c, e[3], &len)              ? (uint32_t)len
                                                                 : 0;
            m->stride = c->ids[mt].array_stride;
            e = def(c, e[2]);
        }
        if (e && (e[0] & 0xffffu) == OP_TYPE_MATRIX) {
            m->cols = (uint8_t)e[3];
            if (!m->array)
                m->stride = mstride;
            e = def(c, e[2]);
        }
        if (e && (e[0] & 0xffffu) == OP_TYPE_VECTOR)
            m->vec = (uint8_t)e[3];
        if (e && (e[0] & 0xffffu) == OP_TYPE_STRUCT)
            m->scalar = SR_OTHER;
        else
            scalar_of(c, mt, &m->scalar, &m->width);
    }
    *count = members;
    return out;
}

static VkShaderStageFlagBits stage_of(uint32_t model) {
    switch (model) {
    case 0: return VK_SHADER_STAGE_VERTEX_BIT;
    case 1: return VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
    case 2: return VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
    case 3: return VK_SHADER_STAGE_GEOMETRY_BIT;
    case 4: return VK_SHADER_STAGE_FRAGMENT_BIT;
    case 5: return VK_SHADER_STAGE_COMPUTE_BIT;
    case 5267: case 5364: return VK_SHADER_STAGE_TASK_BIT_EXT;
    case 5268: case 5365: return VK_SHADER_STAGE_MESH_BIT_EXT;
    default: return (VkShaderStageFlagBits)0;
    }
}

static const struct {
    uint32_t value;
    const char* name;
} k_caps[] = {
    {0, "Matrix"}, {1, "Shader"}, {2, "Geometry"}, {3, "Tessellation"}, {9, "Float16"}, {10, "Float64"},
    {11, "Int64"}, {12, "Int64Atomics"}, {14, "Groups"}, {22, "Int16"}, {23, "TessellationPointSize"},
    {24, "GeometryPointSize"}, {25, "ImageGatherExtended"}, {27, "StorageImageMultisample"},
    {28, "UniformBufferArrayDynamicIndexing"}, {29, "SampledImageArrayDynamicIndexing"},
    {30, "StorageBufferArrayDynamicIndexing"}, {31, "StorageImageArrayDynamicIndexing"}, {32, "ClipDistance"},
    {33, "CullDistance"}, {34, "ImageCubeArray"}, {35, "SampleRateShading"}, {37, "SampledRect"},
    {39, "Int8"}, {40, "InputAttachment"}, {41, "SparseResidency"}, {42, "MinLod"}, {43, "Sampled1D"},
    {44, "Image1D"}, {45, "SampledCubeArray"}, {46, "SampledBuffer"}, {47, "ImageBuffer"}, {48, "ImageMSArray"},
    {49, "StorageImageExtendedFormats"}, {50, "ImageQuery"}, {51, "DerivativeControl"},
    {52, "InterpolationFunction"}, {53, "TransformFeedback"}, {54, "GeometryStreams"},
    {55, "StorageImageReadWithoutFormat"}, {56, "StorageImageWriteWithoutFormat"}, {57, "MultiViewport"},
    {61, "GroupNonUniform"}, {62, "GroupNonUniformVote"}, {63, "GroupNonUniformArithmetic"},
    {64, "GroupNonUniformBallot"}, {65, "GroupNonUniformShuffle"}, {66, "GroupNonUniformShuffleRelative"},
    {67, "GroupNonUniformClustered"}, {68, "GroupNonUniformQuad"}, {4423, "SubgroupBallotKHR"},
    {4427, "DrawParameters"}, {4433, "StorageBuffer16BitAccess"}, {4434, "UniformAndStorageBuffer16BitAccess"},
    {4435, "StoragePushConstant16"}, {4436, "StorageInputOutput16"}, {4437, "DeviceGroup"}, {4439, "MultiView"},
    {4441, "VariablePointersStorageBuffer"}, {4442, "VariablePointers"}, {4448, "StorageBuffer8BitAccess"},
    {4449, "UniformAndStorageBuffer8BitAccess"}, {4450, "StoragePushConstant8"}, {4464, "DenormPreserve"},
    {4465, "DenormFlushToZero"}, {4466, "SignedZeroInfNanPreserve"}, {4467, "RoundingModeRTE"},
    {4468, "RoundingModeRTZ"}, {5008, "Float16ImageAMD"}, {5009, "ImageGatherBiasLodAMD"},
    {5013, "StencilExportEXT"}, {5015, "ImageReadWriteLodAMD"}, {5016, "Int64ImageEXT"},
    {5055, "ShaderClockKHR"}, {5249, "SampleMaskOverrideCoverageNV"}, {5251, "GeometryShaderPassthroughNV"},
    {5254, "ShaderViewportIndexLayerEXT"}, {5255, "ShaderViewportMaskNV"}, {5259, "ShaderStereoViewNV"},
    {5260, "PerViewAttributesNV"}, {5265, "FragmentFullyCoveredEXT"}, {5266, "MeshShadingNV"},
    {5282, "ImageFootprintNV"}, {5283, "MeshShadingEXT"}, {5284, "FragmentBarycentricKHR"},
    {5288, "ComputeDerivativeGroupQuadsNV"}, {5297, "GroupNonUniformPartitionedNV"},
    {5301, "ShaderNonUniform"}, {5302, "RuntimeDescriptorArray"}, {5345, "VulkanMemoryModel"},
    {5346, "VulkanMemoryModelDeviceScope"}, {5347, "PhysicalStorageBufferAddresses"},
    {5350, "ComputeDerivativeGroupLinearNV"}, {5357, "CooperativeMatrixNV"}, {5363, "FragmentShaderPixelInterlockEXT"},
    {5373, "DemoteToHelperInvocation"}, {5379, "FragmentShaderSampleInterlockEXT"},
    {5390, "BindlessTextureNV"}, {6016, "AtomicFloat32AddEXT"}, {6017, "AtomicFloat64AddEXT"},
    {6033, "AtomicFloat32MinMaxEXT"}, {6022, "CooperativeMatrixKHR"}, {6024, "ShaderSMBuiltinsNV"},
    {4429, "SubgroupVoteKHR"},
};

static char* cap_name(uint32_t value) {
    char buf[48];
    const char* name = NULL;
    for (size_t i = 0; i < sizeof k_caps / sizeof k_caps[0]; i++)
        if (k_caps[i].value == value)
            name = k_caps[i].name;
    if (name)
        snprintf(buf, sizeof buf, "%s", name);
    else
        snprintf(buf, sizeof buf, "Capability%u", value);
    char* s = malloc(strlen(buf) + 1);
    if (s)
        strcpy(s, buf);
    return s;
}

/* strips arrays from a type: the element type, and the element count (0 for a runtime array) */
static uint32_t strip_arrays(const Ctx* c, uint32_t type, uint32_t* count) {
    *count = 1;
    for (;;) {
        const uint32_t* d = def(c, type);
        if (!d)
            return type;
        uint32_t op = d[0] & 0xffffu;
        if (op == OP_TYPE_ARRAY) {
            uint64_t len = 1;
            const_value(c, d[3], &len);
            *count *= (uint32_t)len;
            type = d[2];
        } else if (op == OP_TYPE_RUNTIME_ARRAY) {
            *count = 0;
            type = d[2];
        } else {
            return type;
        }
    }
}

static bool all_members_non_writable(const Ctx* c, uint32_t type) {
    const uint32_t* d = def(c, type);
    if (!d || (d[0] & 0xffffu) != OP_TYPE_STRUCT)
        return false;
    uint32_t members = (d[0] >> 16) - 2;
    for (uint32_t i = 0; i < members; i++)
        if (mdec(c, type, i, DEC_NON_WRITABLE, NONE) == NONE)
            return false;
    return members > 0;
}

static void varying(const Ctx* c, uint32_t var, uint32_t type, SrVarying* v) {
    const Id* id = &c->ids[var];
    memset(v, 0, sizeof *v);
    copy_name(v->name, sizeof v->name, id->name);
    v->location = id->location;
    v->component = id->component == NONE ? 0 : id->component;
    v->flat = id->flat;
    v->builtin = id->builtin != NONE;
    uint32_t count = 1;
    uint32_t elem = strip_arrays(c, type, &count);
    const uint32_t* d = def(c, elem);
    uint32_t cols = 1;
    v->vec = 1;
    if (d && (d[0] & 0xffffu) == OP_TYPE_STRUCT) {
        /* a block of builtins, gl_PerVertex */
        if (mdec(c, elem, 0, DEC_BUILTIN, NONE) != NONE)
            v->builtin = true;
        v->scalar = SR_OTHER;
        v->locations = count;
        return;
    }
    if (d && (d[0] & 0xffffu) == OP_TYPE_MATRIX) {
        cols = d[3];
        d = def(c, d[2]);
    }
    if (d && (d[0] & 0xffffu) == OP_TYPE_VECTOR)
        v->vec = (uint8_t)d[3];
    scalar_of(c, elem, &v->scalar, &v->width);
    uint32_t per = v->width == 64 && v->vec > 2 ? 2 : 1;
    v->locations = (count ? count : 1) * cols * per;
}

static int cmp_binding(const void* a, const void* b) {
    const SrBinding *x = a, *y = b;
    if (x->set != y->set)
        return x->set < y->set ? -1 : 1;
    return x->binding < y->binding ? -1 : x->binding > y->binding;
}

static int cmp_varying(const void* a, const void* b) {
    const SrVarying *x = a, *y = b;
    if (x->builtin != y->builtin)
        return x->builtin ? 1 : -1;
    if (x->location != y->location)
        return x->location < y->location ? -1 : 1;
    return x->component < y->component ? -1 : x->component > y->component;
}

/* the value of a workgroup size component: a constant, or a spec constant with its SpecId */
static void size_part(const Ctx* c, uint32_t id, uint32_t* value, uint32_t* spec) {
    uint64_t v = 1;
    const_value(c, id, &v);
    *value = (uint32_t)v;
    uint32_t op = def_op(c, id);
    *spec = (op == OP_SPEC_CONSTANT && c->ids[id].spec_id != NONE) ? c->ids[id].spec_id : NONE;
}

bool sr_reflect(const uint32_t* words, size_t size, const char* entry, SrModule* out, char* err, size_t err_size) {
    memset(out, 0, sizeof *out);
    if (size < 20 || size % 4) {
        fail(err, err_size, "not SPIR-V: %zu bytes", size);
        return false;
    }
    if (words[0] != 0x07230203u) {
        if (words[0] == 0x03022307u)
            fail(err, err_size, "SPIR-V of the other byte order is not supported");
        else
            fail(err, err_size, "not SPIR-V (magic %08x); assemble text with spirv-as first", words[0]);
        return false;
    }
    Ctx c = {0};
    c.w = words;
    c.n = size / 4;
    c.bound = words[3];
    out->version = words[1];
    out->id_bound = c.bound;
    if (c.bound == 0 || c.bound > (1u << 24)) {
        fail(err, err_size, "bad id bound %u", c.bound);
        return false;
    }
    c.ids = calloc(c.bound, sizeof *c.ids);
    if (!c.ids) {
        fail(err, err_size, "out of memory");
        return false;
    }
    for (uint32_t i = 0; i < c.bound; i++) {
        Id* id = &c.ids[i];
        id->set = id->binding = id->location = id->component = id->spec_id = id->builtin = NONE;
    }
    uint32_t chosen_at = 0;
    size_t entry_cap = 0, cap_cap = 0, ext_cap = 0;
    bool ok = true;
    /* pass 1: definitions, names, decorations, entry points */
    for (size_t at = 5; at < c.n;) {
        uint32_t wc = words[at] >> 16, op = words[at] & 0xffffu;
        if (wc == 0 || at + wc > c.n) {
            fail(err, err_size, "truncated instruction at word %zu", at);
            ok = false;
            break;
        }
        out->instructions++;
        const uint32_t* in = words + at;
        switch (op) {
        case OP_NAME:
            if (wc > 2 && in[1] < c.bound)
                c.ids[in[1]].name = (const char*)(in + 2);
            break;
        case OP_MEMBER_NAME:
            if (wc > 3) {
                c.mname = grow(c.mname, &c.mname_cap, c.mname_count, sizeof *c.mname);
                c.mname[c.mname_count++] = (MemberName){in[1], in[2], (const char*)(in + 3)};
            }
            break;
        case OP_CAPABILITY:
            out->capabilities = grow(out->capabilities, &cap_cap, out->capability_count, sizeof(char*));
            out->capabilities[out->capability_count++] = cap_name(in[1]);
            break;
        case OP_EXTENSION: {
            out->extensions = grow(out->extensions, &ext_cap, out->extension_count, sizeof(char*));
            size_t len = bounded_len((const char*)(in + 1), (size_t)(wc - 1) * 4);
            char* s = malloc(len + 1);
            if (s) {
                memcpy(s, in + 1, len);
                s[len] = 0;
            }
            out->extensions[out->extension_count++] = s;
            break;
        }
        case OP_ENTRY_POINT: {
            out->entries = grow(out->entries, &entry_cap, out->entry_count, sizeof *out->entries);
            SrEntry* e = &out->entries[out->entry_count++];
            copy_name(e->name, sizeof e->name, (const char*)(in + 3));
            e->stage = stage_of(in[1]);
            if (!chosen_at && (!entry || !*entry || strcmp(entry, e->name) == 0)) {
                chosen_at = (uint32_t)at;
                out->entry = *e;
            }
            break;
        }
        case OP_DECORATE:
            if (wc >= 3 && in[1] < c.bound) {
                Id* id = &c.ids[in[1]];
                uint32_t v = wc > 3 ? in[3] : 0;
                switch (in[2]) {
                case DEC_SPEC_ID: id->spec_id = v; break;
                case DEC_BLOCK: id->block = true; break;
                case DEC_BUFFER_BLOCK: id->buffer_block = true; break;
                case DEC_ARRAY_STRIDE: id->array_stride = v; break;
                case DEC_BUILTIN: id->builtin = v; break;
                case DEC_FLAT: id->flat = true; break;
                case DEC_NON_WRITABLE: id->non_writable = true; break;
                case DEC_LOCATION: id->location = v; break;
                case DEC_COMPONENT: id->component = v; break;
                case DEC_BINDING: id->binding = v; break;
                case DEC_DESCRIPTOR_SET: id->set = v; break;
                default: break;
                }
            }
            break;
        case OP_MEMBER_DECORATE:
            if (wc >= 4) {
                c.mdec = grow(c.mdec, &c.mdec_cap, c.mdec_count, sizeof *c.mdec);
                c.mdec[c.mdec_count++] = (MemberDec){in[1], in[2], in[3], wc > 4 ? in[4] : 0};
            }
            break;
        case OP_TYPE_VOID: case OP_TYPE_BOOL: case OP_TYPE_INT: case OP_TYPE_FLOAT: case OP_TYPE_VECTOR:
        case OP_TYPE_MATRIX: case OP_TYPE_IMAGE: case OP_TYPE_SAMPLER: case OP_TYPE_SAMPLED_IMAGE:
        case OP_TYPE_ARRAY: case OP_TYPE_RUNTIME_ARRAY: case OP_TYPE_STRUCT: case OP_TYPE_POINTER:
        case OP_TYPE_ACCELERATION_STRUCTURE:
            if (wc > 1 && in[1] < c.bound)
                c.ids[in[1]].at = (uint32_t)at;
            break;
        case OP_CONSTANT_TRUE: case OP_CONSTANT_FALSE: case OP_CONSTANT: case OP_CONSTANT_COMPOSITE:
        case OP_SPEC_CONSTANT_TRUE: case OP_SPEC_CONSTANT_FALSE: case OP_SPEC_CONSTANT:
        case OP_SPEC_CONSTANT_COMPOSITE: case OP_VARIABLE:
            if (wc > 2 && in[2] < c.bound)
                c.ids[in[2]].at = (uint32_t)at;
            break;
        default:
            break;
        }
        at += wc;
    }
    if (ok && !out->entry_count) {
        fail(err, err_size, "the module has no entry point");
        ok = false;
    }
    if (ok && !chosen_at) {
        fail(err, err_size, "no entry point named \"%s\"", entry);
        ok = false;
    }
    if (!ok) {
        free(c.ids);
        free(c.mdec);
        free(c.mname);
        sr_free(out);
        return false;
    }
    /* the chosen entry point's interface */
    const uint32_t* ep = words + chosen_at;
    uint32_t ep_wc = ep[0] >> 16;
    uint32_t name_words = str_words(ep + 3, ep_wc - 3);
    const uint32_t* iface = ep + 3 + name_words;
    uint32_t iface_count = ep_wc > 3 + name_words ? ep_wc - 3 - name_words : 0;
    uint32_t ep_id = ep[2];
    bool all_globals = out->version >= 0x00010400u;
    out->local[0] = out->local[1] = out->local[2] = 1;
    out->gs_invocations = 1;
    out->local_spec[0] = out->local_spec[1] = out->local_spec[2] = NONE;
    size_t bind_cap = 0, in_cap = 0, out_cap = 0, spec_cap = 0;
    /* pass 2: execution modes, variables, spec constants */
    for (size_t at = 5; at < c.n;) {
        uint32_t wc = words[at] >> 16, op = words[at] & 0xffffu;
        const uint32_t* in = words + at;
        at += wc;
        if ((op == OP_EXECUTION_MODE || op == OP_EXECUTION_MODE_ID) && in[1] == ep_id) {
            switch (in[2]) {
            case EM_ORIGIN_LOWER_LEFT: out->origin_lower_left = true; break;
            case EM_EARLY_TESTS: out->early_tests = true; break;
            case EM_DEPTH_REPLACING: out->depth_replacing = true; break;
            case EM_INVOCATIONS:
                if (wc >= 4)
                    out->gs_invocations = in[3];
                break;
            case EM_INPUT_POINTS: out->gs_input_vertices = 1; break;
            case EM_INPUT_LINES: out->gs_input_vertices = 2; break;
            case EM_INPUT_LINES_ADJACENCY: out->gs_input_vertices = 4; break;
            case EM_TRIANGLES: out->gs_input_vertices = 3; break;
            case EM_INPUT_TRIANGLES_ADJACENCY: out->gs_input_vertices = 6; break;
            case EM_OUTPUT_VERTICES:
                if (wc >= 4)
                    out->gs_max_vertices = in[3];
                break;
            case EM_OUTPUT_POINTS: out->gs_output = 0; break;
            case EM_OUTPUT_LINE_STRIP: out->gs_output = 1; break;
            case EM_OUTPUT_TRIANGLE_STRIP: out->gs_output = 3; break;
            case EM_LOCAL_SIZE:
                if (wc >= 6)
                    for (int i = 0; i < 3; i++)
                        out->local[i] = in[3 + i];
                break;
            case EM_LOCAL_SIZE_ID:
                if (wc >= 6)
                    for (int i = 0; i < 3; i++)
                        size_part(&c, in[3 + i], &out->local[i], &out->local_spec[i]);
                break;
            default: break;
            }
            continue;
        }
        if (op == OP_SPEC_CONSTANT || op == OP_SPEC_CONSTANT_TRUE || op == OP_SPEC_CONSTANT_FALSE) {
            uint32_t id = in[2];
            if (c.ids[id].spec_id == NONE)
                continue;
            out->specs = grow(out->specs, &spec_cap, out->spec_count, sizeof *out->specs);
            SrSpec* s = &out->specs[out->spec_count++];
            memset(s, 0, sizeof *s);
            s->spec_id = c.ids[id].spec_id;
            copy_name(s->name, sizeof s->name, c.ids[id].name);
            if (op == OP_SPEC_CONSTANT) {
                scalar_of(&c, in[1], &s->scalar, &s->width);
            } else {
                s->scalar = SR_BOOL;
                s->width = 32;
            }
            const_value(&c, id, &s->value);
            continue;
        }
        if ((op == OP_SPEC_CONSTANT_COMPOSITE || op == OP_CONSTANT_COMPOSITE) && wc >= 6 &&
            c.ids[in[2]].builtin == BUILTIN_WORKGROUP_SIZE && out->entry.stage == VK_SHADER_STAGE_COMPUTE_BIT) {
            for (int i = 0; i < 3; i++)
                size_part(&c, in[3 + i], &out->local[i], &out->local_spec[i]);
            continue;
        }
        if (op != OP_VARIABLE)
            continue;
        uint32_t var = in[2], sc = in[3];
        bool listed = false;
        for (uint32_t i = 0; i < iface_count; i++)
            listed |= iface[i] == var;
        const uint32_t* ptr = def(&c, in[1]);
        if (!ptr || (ptr[0] & 0xffffu) != OP_TYPE_POINTER)
            continue;
        uint32_t type = ptr[3];
        const Id* id = &c.ids[var];
        if (sc == SC_INPUT || sc == SC_OUTPUT) {
            if (!listed)
                continue;
            SrVarying v;
            /* a geometry shader's inputs are arrays of one element a vertex of the primitive: the element is what
               a location holds */
            if (sc == SC_INPUT && out->entry.stage == VK_SHADER_STAGE_GEOMETRY_BIT) {
                const uint32_t* d = def(&c, type);
                if (d && (d[0] & 0xffffu) == OP_TYPE_ARRAY)
                    type = d[2];
            }
            varying(&c, var, type, &v);
            if (sc == SC_INPUT) {
                out->inputs = grow(out->inputs, &in_cap, out->input_count, sizeof v);
                out->inputs[out->input_count++] = v;
            } else {
                out->outputs = grow(out->outputs, &out_cap, out->output_count, sizeof v);
                out->outputs[out->output_count++] = v;
            }
            continue;
        }
        if (all_globals && !listed)
            continue;
        if (sc == SC_PUSH_CONSTANT) {
            if (out->push)
                continue;
            out->push = block_members(&c, type, &out->push_count);
            out->push_size = type_size(&c, type, 0, 0);
            copy_name(out->push_name, sizeof out->push_name, id->name);
            continue;
        }
        if (sc != SC_UNIFORM_CONSTANT && sc != SC_UNIFORM && sc != SC_STORAGE)
            continue;
        if (id->set == NONE && id->binding == NONE)
            continue;
        SrBinding b;
        memset(&b, 0, sizeof b);
        b.set = id->set == NONE ? 0 : id->set;
        b.binding = id->binding == NONE ? 0 : id->binding;
        copy_name(b.name, sizeof b.name, id->name);
        uint32_t elem = strip_arrays(&c, type, &b.count);
        const uint32_t* e = def(&c, elem);
        uint32_t eop = e ? (e[0] & 0xffffu) : 0;
        const uint32_t* image = NULL;
        if (sc == SC_STORAGE || (sc == SC_UNIFORM && c.ids[elem].buffer_block)) {
            b.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            b.writable = !id->non_writable && !all_members_non_writable(&c, elem);
        } else if (sc == SC_UNIFORM) {
            b.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        } else if (eop == OP_TYPE_SAMPLER) {
            b.type = VK_DESCRIPTOR_TYPE_SAMPLER;
        } else if (eop == OP_TYPE_SAMPLED_IMAGE) {
            image = def(&c, e[2]);
            /* glslang's samplerBuffer: a sampled image of a Buffer image, which Vulkan binds as a texel buffer */
            b.type = image && image[3] == 5 ? VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER
                                             : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        } else if (eop == OP_TYPE_IMAGE) {
            image = e;
            uint32_t dim = e[3], sampled = e[7];
            if (dim == 6)
                b.type = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
            else if (dim == 5)
                b.type = sampled == 2 ? VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER : VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
            else
                b.type = sampled == 2 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            b.writable = sampled == 2 && !id->non_writable;
        } else if (eop == OP_TYPE_ACCELERATION_STRUCTURE) {
            b.type = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        } else {
            continue;
        }
        if (image) {
            uint8_t width;
            b.dim = image[3];
            b.depth = image[4] == 1;
            b.arrayed = image[5] != 0;
            b.ms = image[6] != 0;
            b.image_format = image[8];
            scalar_of(&c, image[2], &b.sampled_scalar, &width);
        }
        if (b.type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER || b.type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) {
            b.block_size = type_size(&c, elem, 0, 0);
            const uint32_t* st = def(&c, elem);
            if (st && (st[0] & 0xffffu) == OP_TYPE_STRUCT && (st[0] >> 16) > 2) {
                uint32_t last = (st[0] >> 16) - 3;
                uint32_t lt = st[2 + last];
                if (def_op(&c, lt) == OP_TYPE_RUNTIME_ARRAY) {
                    b.runtime_stride = c.ids[lt].array_stride;
                    b.block_size = mdec(&c, elem, last, DEC_OFFSET, b.block_size);
                }
            }
        }
        out->bindings = grow(out->bindings, &bind_cap, out->binding_count, sizeof b);
        out->bindings[out->binding_count++] = b;
    }
    qsort(out->bindings, out->binding_count, sizeof *out->bindings, cmp_binding);
    qsort(out->inputs, out->input_count, sizeof *out->inputs, cmp_varying);
    qsort(out->outputs, out->output_count, sizeof *out->outputs, cmp_varying);
    free(c.ids);
    free(c.mdec);
    free(c.mname);
    return true;
}

void sr_free(SrModule* m) {
    for (size_t i = 0; i < m->capability_count; i++)
        free(m->capabilities[i]);
    for (size_t i = 0; i < m->extension_count; i++)
        free(m->extensions[i]);
    free(m->capabilities);
    free(m->extensions);
    free(m->entries);
    free(m->bindings);
    free(m->push);
    free(m->specs);
    free(m->inputs);
    free(m->outputs);
    memset(m, 0, sizeof *m);
}

VkFormat sr_image_format_vk(uint32_t f) {
    static const VkFormat k[] = {
        VK_FORMAT_UNDEFINED, VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R32_SFLOAT,
        VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SNORM, VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R16G16_SFLOAT,
        VK_FORMAT_B10G11R11_UFLOAT_PACK32, VK_FORMAT_R16_SFLOAT, VK_FORMAT_R16G16B16A16_UNORM,
        VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_FORMAT_R16G16_UNORM, VK_FORMAT_R8G8_UNORM, VK_FORMAT_R16_UNORM,
        VK_FORMAT_R8_UNORM, VK_FORMAT_R16G16B16A16_SNORM, VK_FORMAT_R16G16_SNORM, VK_FORMAT_R8G8_SNORM,
        VK_FORMAT_R16_SNORM, VK_FORMAT_R8_SNORM, VK_FORMAT_R32G32B32A32_SINT, VK_FORMAT_R16G16B16A16_SINT,
        VK_FORMAT_R8G8B8A8_SINT, VK_FORMAT_R32_SINT, VK_FORMAT_R32G32_SINT, VK_FORMAT_R16G16_SINT,
        VK_FORMAT_R8G8_SINT, VK_FORMAT_R16_SINT, VK_FORMAT_R8_SINT, VK_FORMAT_R32G32B32A32_UINT,
        VK_FORMAT_R16G16B16A16_UINT, VK_FORMAT_R8G8B8A8_UINT, VK_FORMAT_R32_UINT, VK_FORMAT_A2B10G10R10_UINT_PACK32,
        VK_FORMAT_R32G32_UINT, VK_FORMAT_R16G16_UINT, VK_FORMAT_R8G8_UINT, VK_FORMAT_R16_UINT, VK_FORMAT_R8_UINT,
        VK_FORMAT_R64_UINT, VK_FORMAT_R64_SINT,
    };
    return f < sizeof k / sizeof k[0] ? k[f] : VK_FORMAT_UNDEFINED;
}

const char* sr_stage_name(VkShaderStageFlagBits stage) {
    switch (stage) {
    case VK_SHADER_STAGE_VERTEX_BIT: return "vertex";
    case VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT: return "tess_control";
    case VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT: return "tess_eval";
    case VK_SHADER_STAGE_GEOMETRY_BIT: return "geometry";
    case VK_SHADER_STAGE_FRAGMENT_BIT: return "fragment";
    case VK_SHADER_STAGE_COMPUTE_BIT: return "compute";
    case VK_SHADER_STAGE_TASK_BIT_EXT: return "task";
    case VK_SHADER_STAGE_MESH_BIT_EXT: return "mesh";
    default: return "unknown";
    }
}

const char* sr_descriptor_name(VkDescriptorType type) {
    switch (type) {
    case VK_DESCRIPTOR_TYPE_SAMPLER: return "sampler";
    case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER: return "combined_image_sampler";
    case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE: return "sampled_image";
    case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE: return "storage_image";
    case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER: return "uniform_texel_buffer";
    case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER: return "storage_texel_buffer";
    case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER: return "uniform_buffer";
    case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER: return "storage_buffer";
    case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT: return "input_attachment";
    case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR: return "acceleration_structure";
    default: return "unknown";
    }
}

const char* sr_scalar_name(uint8_t scalar) {
    switch (scalar) {
    case SR_FLOAT: return "float";
    case SR_SINT: return "int";
    case SR_UINT: return "uint";
    case SR_BOOL: return "bool";
    default: return "other";
    }
}
