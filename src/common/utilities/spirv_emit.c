/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "common/utilities/bits.h"
#include "common/utilities/spirv_emit.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

/* the SPIR-V version new modules take: 1.6, or the most the device takes (spv_emit_set_version) */
static struct {
    uint32_t version;
} g_spirv_emit = {0x00010600};

void spv_emit_set_version(uint32_t version) { g_spirv_emit.version = version; }

/* s at a capacity of at least need words: doubled from 512 (a small shader's section, so most grow once or never)
   until it holds them. Out of line, so the test that calls it stays small enough to be inlined everywhere */
__attribute__((noinline, cold)) static void grow(SpvWords* s, size_t need) {
    size_t cap = s->capacity ? s->capacity : 512;
    while (cap < need) {
        assert(cap <= SIZE_MAX / 2);
        cap *= 2;
    }
    assert(cap <= SIZE_MAX / sizeof(uint32_t));
    uint32_t* p = realloc(s->words, cap * sizeof(uint32_t));
    if (!p) abort();
    s->words = p;
    s->capacity = cap;
}

/* room for more words after s's: the test is all most instructions take, the growing out of line */
static void reserve(SpvWords* s, size_t more) {
    assert(more <= SIZE_MAX - s->count);
    if (more > s->capacity - s->count) grow(s, s->count + more);
}

static void instruction(SpvWords* s, uint32_t op, uint32_t words) {
    assert(op <= UINT16_MAX && words > 0 && words <= UINT16_MAX);
    reserve(s, words);
    s->words[s->count++] = (words << 16) | op;
}

/* an instruction of op and its n operand words, in s: one test for room, then the words */
static void emit_words(SpvWords* s, uint32_t op, const uint32_t* operands, uint32_t n) {
    assert(op <= UINT16_MAX && n < UINT16_MAX);
    reserve(s, (size_t)n + 1);
    uint32_t* w = s->words + s->count;
    w[0] = (n + 1) << 16 | op;
    for (uint32_t i = 0; i < n; i++) w[1 + i] = operands[i];
    s->count += (size_t)n + 1;
}
/* EMIT(s, op, words...): emit_words of the words given, which call no function (an initializer's are evaluated in no
   set order) */
#define EMIT(s, op, ...)                                                                                               \
    do {                                                                                                               \
        const uint32_t emit_operands[] = {__VA_ARGS__};                                                                \
        emit_words((s), (op), emit_operands, (uint32_t)(sizeof emit_operands / sizeof emit_operands[0]));             \
    } while (0)

static void put(SpvWords* s, uint32_t word) {
    assert(s->count < s->capacity);
    s->words[s->count++] = word;
}
/* n words of an instruction instruction() made room for, at once */
static void put_n(SpvWords* s, const uint32_t* words, size_t n) {
    assert(n <= s->capacity - s->count);
    if (n) memcpy(s->words + s->count, words, n * sizeof *words);
    s->count += n;
}

static size_t string_words(const char* str) {
    size_t n = strlen(str);
    assert(n <= SIZE_MAX - 4);
    return (n + 4) / 4;
}

/* word i of a string's words (n its bytes with the terminator): little-endian, padded with zeros */
static uint32_t string_word(const char* str, size_t n, size_t i) {
    uint32_t word = 0;
    for (size_t j = 0; j < 4 && i * 4 + j < n; j++) word |= (uint32_t)(uint8_t)str[i * 4 + j] << (8 * j);
    return word;
}

static void put_str(SpvWords* s, const char* str) {
    size_t n = strlen(str) + 1;
    for (size_t i = 0; i * 4 < n; i++) put(s, string_word(str, n, i));
}

/* The hash tables below are kept at most half full on a power of two, and grow only as an entry joins one, so a
   table's size follows from its count: 0 for none, else the power of two at least twice count, and least. Each keeps
   its mask (the size less one, what a lookup takes) for a hit not to work the size out again; a miss does */
static size_t table_size(size_t count, size_t least) {
    if (!count) return 0;
    size_t size = (size_t)1 << (64u - snx_clz64((uint64_t)(2 * count - 1)));
    return size < least ? least : size;
}
#define KNOWN_SLOTS_LEAST   256u
#define KNOWN_LEAST         128u
#define CONSTANTS_LEAST     256u
#define NONUNIFORM_LEAST    64u

/* the slot of a known_slots of size slots to look for hash h from */
static size_t known_slot(uint64_t h, size_t size) { return (size_t)(h ^ h >> 29) & (size - 1); }

/* known_slots at the size of count entries: the entries of known placed again, as each one's slot depends on the
   size (a slot of the table's old size moves up by it, or not, and probing moved some past their own) */
static void known_slots_resize(SpvEmitter* e, size_t count) {
    size_t size = table_size(count, KNOWN_SLOTS_LEAST);
    e->known_mask = size - 1;
    uint32_t* slots = (uint32_t*)realloc(e->known_slots, size * sizeof *slots);
    if (!slots) abort();
    memset(slots, 0, size * sizeof *slots);
    for (size_t k = 0; k < e->known_count; k++) {
        size_t i = known_slot(e->known[k].hash, size);
        while (slots[i]) i = (i + 1) & (size - 1);
        slots[i] = (uint32_t)(k + 1);
    }
    e->known_slots = slots;
}

/* a type (the id first, at = 1) or constant (its type first, the id second, at = 2) of these words in types, made
   once */
static uint32_t known(SpvEmitter* e, uint32_t op, size_t at, const uint32_t* operands, size_t count) {
    uint64_t h = SNX_FNV_BASIS ^ op;
    for (size_t i = 0; i < count; i++) h = snx_fnv_step(h, operands[i]);
    size_t size = e->known_count ? e->known_mask + 1 : 0, slot = 0;
    for (slot = size ? known_slot(h, size) : 0; size && e->known_slots[slot]; slot = (slot + 1) & (size - 1)) {
        const SpvKnown* k = &e->known[e->known_slots[slot] - 1];
        if (k->hash != h) continue;
        const uint32_t* w = e->types.words + k->at;
        if ((w[0] & 0xffff) != op || w[0] >> 16 != count + 2) continue;
        int same = 1;
        for (size_t i = 0, j = 1; i < count && same; i++, j++) {
            if (j == at) j++;
            same = w[j] == operands[i];
        }
        if (same) return k->id;
    }
    /* a new one: the index at the size its count takes, and known doubled from KNOWN_LEAST as it fills */
    size_t grown = table_size(e->known_count + 1, KNOWN_SLOTS_LEAST);
    if (grown != size) {
        known_slots_resize(e, e->known_count + 1);
        for (slot = known_slot(h, grown); e->known_slots[slot]; slot = (slot + 1) & (grown - 1)) {}
    }
    size_t n = e->known_count;
    if (!n || (n >= KNOWN_LEAST && !(n & (n - 1)))) {
        SpvKnown* more = (SpvKnown*)realloc(e->known, (n ? 2 * n : KNOWN_LEAST) * sizeof *more);
        if (!more) abort();
        e->known = more;
    }
    uint32_t id = spv_emit_id(e);
    e->known_slots[slot] = (uint32_t)(e->known_count + 1); /* the free slot the search stopped at */
    e->known[e->known_count++] = (SpvKnown){h, e->types.count, id};
    instruction(&e->types, op, (uint32_t)(count + 2));
    for (size_t i = 0, j = 1; j < count + 2; j++) put(&e->types, j == at ? id : operands[i++]);
    return id;
}

void spv_emit_init(SpvEmitter* e, size_t estimated_function_words) {
    *e = (SpvEmitter){0};
    e->next_id = 1;
    e->version = g_spirv_emit.version;
    /* Most instructions are in function bodies. Reserve from the guest program size once. */
    if (estimated_function_words) reserve(&e->functions, estimated_function_words);
}

void spv_emit_destroy(SpvEmitter* e) {
    SpvWords* sections[] = {&e->capabilities, &e->extensions,  &e->imports, &e->entry_points, &e->execution_modes,
                            &e->debug,        &e->decorations, &e->types,   &e->functions,    &e->locals};
    for (size_t i = 0; i < sizeof sections / sizeof sections[0]; i++) free(sections[i]->words);
    free(e->known);
    free(e->known_slots);
    free(e->interface);
    free(e->nonuniform);
    free(e->constants);
    free(e->constant_ids);
    *e = (SpvEmitter){0};
}

uint32_t spv_emit_id(SpvEmitter* e) {
    assert(e->next_id && e->next_id < UINT32_MAX);
    return e->next_id++;
}

uint32_t spv_emit_glsl450_import(SpvEmitter* e) {
    const char* name = "GLSL.std.450";
    if (e->glsl) return e->glsl;
    uint32_t id = e->glsl = spv_emit_id(e);
    instruction(&e->imports, SPV_OP_EXT_INST_IMPORT, (uint32_t)(2 + string_words(name)));
    put(&e->imports, id);
    put_str(&e->imports, name);
    return id;
}

void spv_emit_shader_clock_extension(SpvEmitter* e) {
    spv_emit_extension(e, "SPV_KHR_shader_clock");
    spv_emit_capability(e, SPV_CAPABILITY_SHADER_CLOCK_KHR);
}





void spv_emit_subgroup_capabilities(SpvEmitter* e) {
    spv_emit_capability(e, SPV_CAPABILITY_GROUP_NON_UNIFORM);
    spv_emit_capability(e, SPV_CAPABILITY_GROUP_NON_UNIFORM_BALLOT);
    spv_emit_capability(e, SPV_CAPABILITY_GROUP_NON_UNIFORM_SHUFFLE);
}



void spv_emit_entry_str(SpvEmitter* e, SpvShaderStage stage, uint32_t function, const char* name,
                        const uint32_t* interfaces, size_t interface_count) {
    assert((stage >= SPV_STAGE_VERTEX && stage <= SPV_STAGE_COMPUTE) || stage == SPV_STAGE_TASK ||
           stage == SPV_STAGE_MESH);
    size_t n = string_words(name);
    assert(n <= UINT16_MAX - 3 && interface_count <= UINT16_MAX - 3 - n);
    instruction(&e->entry_points, SPV_OP_ENTRY_POINT, (uint32_t)(3 + n + interface_count));
    put(&e->entry_points, (uint32_t)stage);
    put(&e->entry_points, function);
    put_str(&e->entry_points, name);
    put_n(&e->entry_points, interfaces, interface_count);
}


void spv_emit_fragment_origin_upper_left(SpvEmitter* e, uint32_t function) {
    EMIT(&e->execution_modes, SPV_OP_EXECUTION_MODE, function, 7); /* OriginUpperLeft */
}

void spv_emit_fragment_depth_replacing(SpvEmitter* e, uint32_t function) {
    EMIT(&e->execution_modes, SPV_OP_EXECUTION_MODE, function, 12); /* DepthReplacing */
}

void spv_emit_float_controls(SpvEmitter* e, uint32_t function, int flush_denorms, int preserve_infnan,
                             int preserve_infnan64) {
    if (!flush_denorms && !preserve_infnan && !preserve_infnan64) return;
    spv_emit_extension(e, "SPV_KHR_float_controls");
    if (flush_denorms) {
        spv_emit_capability(e, SPV_CAPABILITY_DENORM_FLUSH_TO_ZERO);
        spv_emit_execution_mode(e, function, SPV_MODE_DENORM_FLUSH_TO_ZERO, 1, 32);
    }
    if (preserve_infnan) {
        spv_emit_capability(e, SPV_CAPABILITY_SIGNED_ZERO_INF_NAN_PRESERVE);
        spv_emit_execution_mode(e, function, SPV_MODE_SIGNED_ZERO_INF_NAN_PRESERVE, 1, 32);
    }
    if (preserve_infnan64) {
        spv_emit_capability(e, SPV_CAPABILITY_SIGNED_ZERO_INF_NAN_PRESERVE);
        spv_emit_execution_mode(e, function, SPV_MODE_SIGNED_ZERO_INF_NAN_PRESERVE, 1, 64);
    }
}

/* Widths16 and64 must share controls on devices whose independence is32_BIT_ONLY. A module that already
   preserves64-bit zeros/NaNs must not repeat that execution mode. */
void spv_emit_half_rounding(SpvEmitter* e, uint32_t function, int linked64) {
    static const uint32_t modes[] = {SPV_MODE_DENORM_PRESERVE, SPV_MODE_SIGNED_ZERO_INF_NAN_PRESERVE,
                                     SPV_MODE_ROUNDING_RTE};
    spv_emit_extension(e, "SPV_KHR_float_controls");
    spv_emit_capability(e, SPV_CAPABILITY_DENORM_PRESERVE);
    spv_emit_capability(e, SPV_CAPABILITY_SIGNED_ZERO_INF_NAN_PRESERVE);
    spv_emit_capability(e, SPV_CAPABILITY_ROUNDING_RTE);
    for (uint32_t width = 16; width <= (linked64 ? 64u : 16u); width += 48)
        for (uint32_t mode = 0; mode < sizeof modes / sizeof *modes; mode++) {
            int found = 0;
            for (size_t at = 0; at < e->execution_modes.count; at += e->execution_modes.words[at] >> 16) {
                const uint32_t* words = e->execution_modes.words + at;
                if ((words[0] >> 16) == 4 && words[1] == function && words[2] == modes[mode] && words[3] == width)
                    found = 1;
            }
            if (!found) spv_emit_execution_mode(e, function, modes[mode], 1, width);
        }
}

void spv_emit_local_size(SpvEmitter* e, uint32_t function, uint32_t x, uint32_t y, uint32_t z) {
    assert(x && y && z);
    EMIT(&e->execution_modes, SPV_OP_EXECUTION_MODE, function, 17, x, y, z); /* LocalSize */
}

void spv_emit_name_str(SpvEmitter* e, uint32_t id, const char* name) {
    size_t n = string_words(name);
    assert(n <= UINT16_MAX - 2);
    instruction(&e->debug, SPV_OP_NAME, (uint32_t)(2 + n));
    put(&e->debug, id);
    put_str(&e->debug, name);
}

uint32_t spv_emit_type_void(SpvEmitter* e) { return known(e, SPV_OP_TYPE_VOID, 1, NULL, 0); }

uint32_t spv_emit_type_bool(SpvEmitter* e) { return known(e, SPV_OP_TYPE_BOOL, 1, NULL, 0); }




uint32_t spv_emit_int_convert(SpvEmitter* e, uint32_t type, uint32_t value, int is_signed) {
    uint32_t id = spv_emit_id(e);
    EMIT(&e->functions, is_signed ? SPV_OP_S_CONVERT : SPV_OP_U_CONVERT, type, id, value);
    return id;
}

uint32_t spv_emit_int_to_f32_nearest(SpvEmitter* e, uint32_t value, int is_signed) {
    uint32_t u32 = spv_emit_type_u32(e), f32 = spv_emit_type_f32(e);
    uint32_t high = spv_emit_binary(e, SPV_OP_BITWISE_AND, u32, value, spv_emit_constant_u32(e, u32, 0xffff0000u));
    uint32_t low = spv_emit_binary(e, SPV_OP_BITWISE_AND, u32, value, spv_emit_constant_u32(e, u32, 0xffffu));
    high = is_signed ? spv_emit_unary(e, SPV_OP_CONVERT_S_TO_F, f32,
                                      spv_emit_unary(e, SPV_OP_BITCAST, spv_emit_type_i32(e), high))
                     : spv_emit_unary(e, SPV_OP_CONVERT_U_TO_F, f32, high);
    return spv_emit_binary(e, SPV_OP_F_ADD, f32, high, spv_emit_unary(e, SPV_OP_CONVERT_U_TO_F, f32, low));
}





uint32_t spv_emit_type_runtime_array(SpvEmitter* e, uint32_t element_type, uint32_t stride) {
    uint32_t id = spv_emit_id(e);
    EMIT(&e->decorations, SPV_OP_DECORATE, id, 6, stride); /* ArrayStride */
    EMIT(&e->types, SPV_OP_TYPE_RUNTIME_ARRAY, id, element_type);
    return id;
}

uint32_t spv_emit_type_struct_one(SpvEmitter* e, uint32_t member_type) {
    uint32_t id = spv_emit_id(e);
    EMIT(&e->decorations, SPV_OP_DECORATE, id, 2); /* Block */
    EMIT(&e->decorations, SPV_OP_MEMBER_DECORATE, id, 0, 35, 0); /* Offset */
    EMIT(&e->types, SPV_OP_TYPE_STRUCT, id, member_type);
    return id;
}




uint32_t spv_emit_type_array_u32(SpvEmitter* e, uint32_t u32_type, uint32_t length) {
    assert(length);
    uint32_t length_id = spv_emit_constant_u32(e, u32_type, length);
    uint32_t id = spv_emit_id(e);
    EMIT(&e->types, SPV_OP_TYPE_ARRAY, id, u32_type, length_id);
    return id;
}

uint32_t spv_emit_workgroup_variable(SpvEmitter* e, uint32_t pointer_type) {
    uint32_t id = spv_emit_id(e);
    EMIT(&e->types, SPV_OP_VARIABLE, pointer_type, id, 4); /* Workgroup */
    return id;
}

uint32_t spv_emit_function_element_pointer(SpvEmitter* e, uint32_t pointer_type, uint32_t array, uint32_t index) {
    uint32_t id = spv_emit_id(e);
    EMIT(&e->functions, SPV_OP_ACCESS_CHAIN, pointer_type, id, array, index);
    return id;
}


static size_t constant_slot(uint32_t a, uint32_t b, size_t capacity) {
    uint64_t h = ((uint64_t)a << 32 | b) * 0x9E3779B97F4A7C15ull;
    return (size_t)(h >> 40) & (capacity - 1);
}
static void constant_insert(SpvConstant* table, size_t capacity, const SpvConstant* c, int by_id) {
    size_t i = by_id ? constant_slot(c->id, 0, capacity) : constant_slot(c->type, c->value, capacity);
    while (table[i].id) i = (i + 1) & (capacity - 1);
    table[i] = *c;
}
/* both tables from size to grown slots, the constants made so far in them again */
static void constants_resize(SpvEmitter* e, size_t size, size_t grown) {
    SpvConstant* constants = (SpvConstant*)calloc(grown, sizeof *constants);
    SpvConstant* ids = (SpvConstant*)calloc(grown, sizeof *ids);
    if (!constants || !ids) abort();
    for (size_t i = 0; i < size; i++)
        if (e->constants[i].id) {
            constant_insert(constants, grown, &e->constants[i], 0);
            constant_insert(ids, grown, &e->constants[i], 1);
        }
    free(e->constants);
    free(e->constant_ids);
    e->constants = constants;
    e->constant_ids = ids;
    e->constant_mask = grown - 1;
}

uint32_t spv_emit_constant_u32(SpvEmitter* e, uint32_t type, uint32_t value) {
    size_t size = e->constant_count ? e->constant_mask + 1 : 0, i = 0;
    for (i = size ? constant_slot(type, value, size) : 0; size && e->constants[i].id; i = (i + 1) & (size - 1))
        if (e->constants[i].type == type && e->constants[i].value == value) return e->constants[i].id;
    size_t grown = table_size(e->constant_count + 1, CONSTANTS_LEAST);
    if (grown != size) {
        constants_resize(e, size, grown);
        for (i = constant_slot(type, value, grown); e->constants[i].id; i = (i + 1) & (grown - 1)) {}
    }
    SpvConstant c = {type, value, spv_emit_id(e)};
    EMIT(&e->types, SPV_OP_CONSTANT, type, c.id, value);
    e->constants[i] = c;
    constant_insert(e->constant_ids, grown, &c, 1);
    e->constant_count++;
    return c.id;
}

int spv_emit_constant_bits(const SpvEmitter* e, uint32_t id, uint32_t* type, uint32_t* value) {
    if (!e->constant_count || !id) return 0;
    size_t size = e->constant_mask + 1;
    for (size_t i = constant_slot(id, 0, size); e->constant_ids[i].id; i = (i + 1) & (size - 1))
        if (e->constant_ids[i].id == id) {
            *type = e->constant_ids[i].type;
            *value = e->constant_ids[i].value;
            return 1;
        }
    return 0;
}

uint32_t spv_emit_constant64(SpvEmitter* e, uint32_t type, uint64_t value) {
    uint32_t id = spv_emit_id(e);
    EMIT(&e->types, SPV_OP_CONSTANT, type, id, (uint32_t)value, (uint32_t)(value >> 32)); /* the low word first */
    return id;
}

uint32_t spv_emit_constant_bool(SpvEmitter* e, uint32_t type, int value) {
    uint32_t id = spv_emit_id(e);
    EMIT(&e->types, value ? SPV_OP_CONSTANT_TRUE : SPV_OP_CONSTANT_FALSE, type, id);
    return id;
}

uint32_t spv_emit_spec_constant_u32(SpvEmitter* e, uint32_t type, uint32_t spec_id, uint32_t value) {
    uint32_t id = spv_emit_id(e);
    EMIT(&e->types, SPV_OP_SPEC_CONSTANT, type, id, value);
    EMIT(&e->decorations, SPV_OP_DECORATE, id, SPV_DECORATION_SPEC_ID, spec_id);
    return id;
}

uint32_t spv_emit_constant_f32(SpvEmitter* e, uint32_t type, float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof bits);
    return spv_emit_constant_u32(e, type, bits);
}

uint32_t spv_emit_constant_vec4(SpvEmitter* e, uint32_t type, const uint32_t components[4]) {
    uint32_t id = spv_emit_id(e);
    instruction(&e->types, SPV_OP_CONSTANT_COMPOSITE, 7);
    put(&e->types, type);
    put(&e->types, id);
    put_n(&e->types, components, 4);
    return id;
}

uint32_t spv_emit_constant_vec2(SpvEmitter* e, uint32_t type, uint32_t x, uint32_t y) {
    uint32_t id = spv_emit_id(e);
    EMIT(&e->types, SPV_OP_CONSTANT_COMPOSITE, type, id, x, y);
    return id;
}

uint32_t spv_emit_constant_vec3(SpvEmitter* e, uint32_t type, uint32_t x, uint32_t y, uint32_t z) {
    uint32_t id = spv_emit_id(e);
    EMIT(&e->types, SPV_OP_CONSTANT_COMPOSITE, type, id, x, y, z);
    return id;
}

uint32_t spv_emit_storage_buffer(SpvEmitter* e, uint32_t struct_type, uint32_t set, uint32_t binding) {
    uint32_t pointer_type = spv_emit_type_storage_pointer(e, struct_type);
    uint32_t id = spv_emit_id(e);
    EMIT(&e->decorations, SPV_OP_DECORATE, id, 34, set); /* DescriptorSet */
    EMIT(&e->decorations, SPV_OP_DECORATE, id, 33, binding); /* Binding */
    EMIT(&e->types, SPV_OP_VARIABLE, pointer_type, id, 12); /* StorageBuffer */
    return id;
}

static uint32_t emit_vec4_array(SpvEmitter* e, uint32_t vec4_type, uint32_t component_type, uint32_t u32_type,
                                uint32_t set, uint32_t binding, uint32_t elements, uint32_t storage_class,
                                uint32_t* component_pointer_type) {
    assert(elements && component_pointer_type);
    uint32_t length = spv_emit_constant_u32(e, u32_type, elements);
    uint32_t array = spv_emit_id(e);
    EMIT(&e->decorations, SPV_OP_DECORATE, array, 6, 16); /* ArrayStride */
    EMIT(&e->types, SPV_OP_TYPE_ARRAY, array, vec4_type, length);
    uint32_t structure = spv_emit_type_struct_one(e, array);
    uint32_t struct_pointer = spv_emit_type_pointer(e, storage_class, structure);
    uint32_t variable = spv_emit_id(e);
    EMIT(&e->decorations, SPV_OP_DECORATE, variable, 34, set); /* DescriptorSet */
    EMIT(&e->decorations, SPV_OP_DECORATE, variable, 33, binding); /* Binding */
    if (storage_class == 12) {
        /* a device need not let the vertex stages write storage buffers, and these are never written */
        EMIT(&e->decorations, SPV_OP_DECORATE, variable, 24); /* NonWritable */
    }
    EMIT(&e->types, SPV_OP_VARIABLE, struct_pointer, variable, storage_class);
    *component_pointer_type = spv_emit_type_pointer(e, storage_class, component_type);
    return variable;
}
uint32_t spv_emit_uniform_vec4_array(SpvEmitter* e, uint32_t vec4_type, uint32_t component_type, uint32_t u32_type,
                                     uint32_t set, uint32_t binding, uint32_t elements,
                                     uint32_t* component_pointer_type) {
    return emit_vec4_array(e, vec4_type, component_type, u32_type, set, binding, elements, 2, /* Uniform */
                           component_pointer_type);
}
uint32_t spv_emit_readonly_vec4_array(SpvEmitter* e, uint32_t vec4_type, uint32_t component_type, uint32_t u32_type,
                                      uint32_t set, uint32_t binding, uint32_t elements,
                                      uint32_t* component_pointer_type) {
    return emit_vec4_array(e, vec4_type, component_type, u32_type, set, binding, elements, 12, /* StorageBuffer */
                           component_pointer_type);
}

uint32_t spv_emit_uniform_component(SpvEmitter* e, uint32_t pointer_type, uint32_t buffer, uint32_t member,
                                    uint32_t element, uint32_t component, uint32_t component_type) {
    uint32_t pointer = spv_emit_id(e);
    EMIT(&e->functions, SPV_OP_ACCESS_CHAIN, pointer_type, pointer, buffer, member, element, component);
    return spv_emit_load(e, component_type, pointer);
}

uint32_t spv_emit_push_vec4(SpvEmitter* e, uint32_t vec4_type, uint32_t f32_type, uint32_t offset, uint32_t second_type,
                            uint32_t* component_pointer_type) {
    assert(component_pointer_type && !(offset & 15));
    uint32_t members = second_type ? 2 : 1;
    uint32_t structure = spv_emit_id(e);
    EMIT(&e->decorations, SPV_OP_DECORATE, structure, 2); /* Block */
    for (uint32_t m = 0; m < members; m++) {
        EMIT(&e->decorations, SPV_OP_MEMBER_DECORATE, structure, m, 35, offset + 16 * m); /* Offset */
    }
    instruction(&e->types, SPV_OP_TYPE_STRUCT, 2 + members);
    put(&e->types, structure);
    put(&e->types, vec4_type);
    if (second_type) put(&e->types, second_type);
    uint32_t pointer = spv_emit_type_pointer(e, 9, structure); /* PushConstant */
    uint32_t variable = spv_emit_id(e);
    EMIT(&e->types, SPV_OP_VARIABLE, pointer, variable, 9);
    *component_pointer_type = spv_emit_type_pointer(e, 9, f32_type);
    return variable;
}

uint32_t spv_emit_push_component_f32(SpvEmitter* e, uint32_t pointer_type, uint32_t block, uint32_t member,
                                     uint32_t component, uint32_t f32_type) {
    uint32_t pointer = spv_emit_id(e);
    EMIT(&e->functions, SPV_OP_ACCESS_CHAIN, pointer_type, pointer, block, member, component);
    return spv_emit_load(e, f32_type, pointer);
}

static uint32_t output_vec4(SpvEmitter* e, uint32_t vec4_type, uint32_t decoration, uint32_t value) {
    uint32_t pointer_type = spv_emit_type_pointer(e, 3, vec4_type); /* Output */
    uint32_t id = spv_emit_id(e);
    EMIT(&e->decorations, SPV_OP_DECORATE, id, decoration, value);
    EMIT(&e->types, SPV_OP_VARIABLE, pointer_type, id, 3); /* Output */
    return id;
}

uint32_t spv_emit_output_vec4(SpvEmitter* e, uint32_t vec4_type, uint32_t location) {
    return output_vec4(e, vec4_type, 30, location); /* Location */
}

uint32_t spv_emit_private_variable(SpvEmitter* e, uint32_t type) {
    uint32_t pointer_type = spv_emit_type_pointer(e, 6, type); /* Private */
    uint32_t id = spv_emit_id(e);
    EMIT(&e->types, SPV_OP_VARIABLE, pointer_type, id, 6);
    return id;
}

uint32_t spv_emit_output_component(SpvEmitter* e, uint32_t type, uint32_t location, uint32_t component) {
    uint32_t id = output_vec4(e, type, 30, location); /* Location */
    EMIT(&e->decorations, SPV_OP_DECORATE, id, 31, component); /* Component */
    return id;
}

void spv_emit_xfb_capture(SpvEmitter* e, uint32_t variable, uint32_t buffer, uint32_t stride, uint32_t offset) {
    static const uint32_t decorations[3] = {36, 37, 35}; /* XfbBuffer, XfbStride, Offset */
    uint32_t values[3] = {buffer, stride, offset};
    for (uint32_t i = 0; i < 3; i++) {
        EMIT(&e->decorations, SPV_OP_DECORATE, variable, decorations[i], values[i]);
    }
}

uint32_t spv_emit_position_output(SpvEmitter* e, uint32_t vec4_type) {
    uint32_t id = output_vec4(e, vec4_type, 11, 0); /* BuiltIn Position */
    EMIT(&e->decorations, SPV_OP_DECORATE, id, 18); /* Invariant */
    return id;
}

uint32_t spv_emit_point_size_output(SpvEmitter* e, uint32_t f32_type) {
    return output_vec4(e, f32_type, 11, 1); /* BuiltIn PointSize */
}

uint32_t spv_emit_layer_output(SpvEmitter* e, uint32_t i32_type) {
    return output_vec4(e, i32_type, 11, 9); /* BuiltIn Layer */
}

uint32_t spv_emit_clip_distance_output(SpvEmitter* e, uint32_t f32_type, uint32_t u32_type, uint32_t length) {
    uint32_t length_id = spv_emit_constant_u32(e, u32_type, length);
    uint32_t array_type = spv_emit_id(e);
    EMIT(&e->types, SPV_OP_TYPE_ARRAY, array_type, f32_type, length_id);
    uint32_t pointer_type = spv_emit_type_pointer(e, 3, array_type);
    uint32_t id = spv_emit_id(e);
    EMIT(&e->decorations, SPV_OP_DECORATE, id, 11, 3); /* BuiltIn */ /* ClipDistance */
    EMIT(&e->types, SPV_OP_VARIABLE, pointer_type, id, 3); /* Output */
    return id;
}


uint32_t spv_emit_frag_depth_output(SpvEmitter* e, uint32_t f32_type) {
    return output_vec4(e, f32_type, 11, 22); /* BuiltIn FragDepth */
}

static uint32_t input_vec4(SpvEmitter* e, uint32_t vec4_type, uint32_t decoration, uint32_t value) {
    uint32_t pointer_type = spv_emit_type_pointer(e, 1, vec4_type); /* Input */
    uint32_t id = spv_emit_id(e);
    EMIT(&e->decorations, SPV_OP_DECORATE, id, decoration, value);
    EMIT(&e->types, SPV_OP_VARIABLE, pointer_type, id, 1); /* Input */
    return id;
}

uint32_t spv_emit_input_vec4(SpvEmitter* e, uint32_t vec4_type, uint32_t location, int interpolation) {
    assert(interpolation >= 0 && interpolation <= 2);
    uint32_t id = input_vec4(e, vec4_type, 30, location); /* Location */
    if (interpolation) {
        EMIT(&e->decorations, SPV_OP_DECORATE, id, interpolation == 1 ? 14 : 13); /* Flat / NoPerspective */
    }
    return id;
}

uint32_t spv_emit_fragcoord_input(SpvEmitter* e, uint32_t vec4_type) {
    return input_vec4(e, vec4_type, 11, 15); /* BuiltIn FragCoord */
}

uint32_t spv_emit_builtin_input_vec3(SpvEmitter* e, uint32_t vec3_type, uint32_t builtin) {
    return input_vec4(e, vec3_type, 11, builtin);
}

uint32_t spv_emit_builtin_input_u32(SpvEmitter* e, uint32_t u32_type, uint32_t builtin) {
    uint32_t id = input_vec4(e, u32_type, 11, builtin);
    if (builtin == 9 || builtin == 18) spv_emit_flat(e, id); /* Layer and SampleId are read in fragment shaders only */
    return id;
}

/* Integer inputs of a fragment shader need Flat interpolation; Vulkan rejects Flat on vertex inputs. */
void spv_emit_flat(SpvEmitter* e, uint32_t variable) {
    EMIT(&e->decorations, SPV_OP_DECORATE, variable, 14);
}

void spv_emit_capability(SpvEmitter* e, uint32_t capability) {
    for (size_t i = 0; i + 1 < e->capabilities.count; i += 2)
        if (e->capabilities.words[i + 1] == capability) return;
    EMIT(&e->capabilities, SPV_OP_CAPABILITY, capability);
}

uint32_t spv_emit_interface_variable(SpvEmitter* e, uint32_t storage, uint32_t type, uint32_t u32_type, uint32_t count,
                                     uint32_t builtin, uint32_t location, int patch) {
    uint32_t id = spv_emit_id(e);
    assert(storage == 1 || storage == 3);
    if (count) {
        uint32_t length = spv_emit_constant_u32(e, u32_type, count);
        uint32_t array_type = spv_emit_id(e);
        EMIT(&e->types, SPV_OP_TYPE_ARRAY, array_type, type, length);
        type = array_type;
    }
    uint32_t pointer_type = spv_emit_type_pointer(e, storage, type);
    /* Location, or BuiltIn */
    EMIT(&e->decorations, SPV_OP_DECORATE, id, builtin == SPV_NOT_BUILTIN ? 30 : 11,
         builtin == SPV_NOT_BUILTIN ? location : builtin);
    if (patch) {
        EMIT(&e->decorations, SPV_OP_DECORATE, id, 15); /* Patch */
    }
    EMIT(&e->types, SPV_OP_VARIABLE, pointer_type, id, storage);
    return id;
}

uint32_t spv_emit_interface_scalar_pointer(SpvEmitter* e, uint32_t storage, uint32_t scalar_type, uint32_t variable,
                                           uint32_t element, uint32_t component) {
    uint32_t id = spv_emit_id(e);
    instruction(&e->functions, SPV_OP_ACCESS_CHAIN, element ? 6 : 5);
    put(&e->functions, spv_emit_type_pointer(e, storage, scalar_type));
    put(&e->functions, id);
    put(&e->functions, variable);
    if (element) put(&e->functions, element);
    put(&e->functions, component);
    return id;
}

void spv_emit_execution_mode(SpvEmitter* e, uint32_t function, uint32_t mode, int has_operand, uint32_t operand) {
    instruction(&e->execution_modes, SPV_OP_EXECUTION_MODE, has_operand ? 4 : 3);
    put(&e->execution_modes, function);
    put(&e->execution_modes, mode);
    if (has_operand) put(&e->execution_modes, operand);
}

void spv_emit_emit_vertex(SpvEmitter* e) { instruction(&e->functions, 218, 1); /* OpEmitVertex */ }

void spv_emit_end_primitive(SpvEmitter* e) { instruction(&e->functions, 219, 1); /* OpEndPrimitive */ }

void spv_emit_patch_barrier(SpvEmitter* e, uint32_t u32_type) {
    /* as GLSL's barrier() in a tessellation control shader: Workgroup execution, Invocation memory scope and no
       semantics, which still orders the outputs written before it for the patch's other invocations */
    instruction(&e->functions, 224, 4); /* OpControlBarrier */
    put(&e->functions, spv_emit_constant_u32(e, u32_type, 2));
    put(&e->functions, spv_emit_constant_u32(e, u32_type, 4));
    put(&e->functions, spv_emit_constant_u32(e, u32_type, 0));
}

/* Ids that are not dynamically uniform: an element of a texture array, and what is loaded or made from it.
   Vulkan wants each decorated NonUniform. */
/* the slot of the set where id is, or the free one where it would go */
static size_t nonuniform_slot(const uint32_t* set, size_t capacity, uint32_t id) {
    size_t i = (size_t)((id * 0x9e3779b1u) >> 8) & (capacity - 1);
    while (set[i] && set[i] != id) i = (i + 1) & (capacity - 1);
    return i;
}
static int is_nonuniform(const SpvEmitter* e, uint32_t id) {
    size_t size = e->nonuniform_count ? e->nonuniform_mask + 1 : 0;
    return size && e->nonuniform[nonuniform_slot(e->nonuniform, size, id)] == id;
}
static void nonuniform(SpvEmitter* e, uint32_t id) {
    if (is_nonuniform(e, id)) return;
    size_t size = e->nonuniform_count ? e->nonuniform_mask + 1 : 0;
    size_t grown = table_size(e->nonuniform_count + 1, NONUNIFORM_LEAST);
    if (grown != size) { /* the ids placed again in a new set: the set is all that holds them */
        uint32_t* set = (uint32_t*)calloc(grown, sizeof *set);
        if (!set) abort();
        for (size_t i = 0; i < size; i++)
            if (e->nonuniform[i]) set[nonuniform_slot(set, grown, e->nonuniform[i])] = e->nonuniform[i];
        free(e->nonuniform);
        e->nonuniform = set;
        e->nonuniform_mask = grown - 1;
    }
    e->nonuniform[nonuniform_slot(e->nonuniform, grown, id)] = id;
    e->nonuniform_count++;
    EMIT(&e->decorations, SPV_OP_DECORATE, id, 5300); /* NonUniform */
}

/* the sampled image type of a texture, and its image type, declared once each */
static uint32_t sampled_image_type(SpvEmitter* e, uint32_t scalar_type, uint32_t type, int array, int shadow,
                                   int multisample) {
    assert(type <= 3 && (array == 0 || array == 1) && (shadow == 0 || shadow == 1));
    /* 0 1D, 1 2D, 2 3D, 3 cube; Sampled, of an unknown format */
    return spv_emit_type_sampled_image(e, spv_emit_type_image(e, scalar_type, type, shadow, array, multisample, 1, 0));
}
/* the image type of a sampled image type */
static uint32_t image_of(const SpvEmitter* e, uint32_t sampled_type) {
    for (size_t k = 0; k < e->known_count; k++) {
        const uint32_t* w = e->types.words + e->known[k].at;
        if (e->known[k].id == sampled_type && (w[0] & 0xffff) == SPV_OP_TYPE_SAMPLED_IMAGE) return w[2];
    }
    assert(!"not a sampled image type");
    return 0;
}

uint32_t spv_emit_sampler_image(SpvEmitter* e, uint32_t scalar_type, uint32_t set, uint32_t binding, uint32_t type,
                                int array, int shadow, int multisample, uint32_t* sampled_type) {
    assert(sampled_type);
    *sampled_type = sampled_image_type(e, scalar_type, type, array, shadow, multisample);
    uint32_t pointer = spv_emit_type_pointer(e, 0, *sampled_type); /* UniformConstant */
    uint32_t sampler = spv_emit_id(e);
    EMIT(&e->decorations, SPV_OP_DECORATE, sampler, 34, set); /* DescriptorSet */
    EMIT(&e->decorations, SPV_OP_DECORATE, sampler, 33, binding); /* Binding */
    EMIT(&e->types, SPV_OP_VARIABLE, pointer, sampler, 0); /* UniformConstant */
    return sampler;
}

uint32_t spv_emit_storage_image(SpvEmitter* e, uint32_t scalar_type, uint32_t set, uint32_t binding, uint32_t type,
                                int array, uint32_t format, uint32_t* image_type) {
    assert(image_type && (type <= 2 || type == 5) && (array == 0 || array == 1));
    if (!format) spv_emit_capability(e, SPV_CAPABILITY_STORAGE_IMAGE_WRITE_WITHOUT_FORMAT);
    if (type == 5) spv_emit_capability(e, SPV_CAPABILITY_IMAGE_BUFFER); /* a storage texel buffer */
    *image_type = spv_emit_type_image(e, scalar_type, type, 0, array, 0, 2, format);
    uint32_t pointer = spv_emit_type_pointer(e, 0, *image_type);
    uint32_t variable = spv_emit_id(e);
    EMIT(&e->decorations, SPV_OP_DECORATE, variable, 34, set); /* DescriptorSet */
    EMIT(&e->decorations, SPV_OP_DECORATE, variable, 33, binding); /* Binding */
    EMIT(&e->types, SPV_OP_VARIABLE, pointer, variable, 0); /* UniformConstant */
    return variable;
}

void spv_emit_image_write(SpvEmitter* e, uint32_t image_type, uint32_t image_variable, uint32_t coordinates,
                          uint32_t texel) {
    uint32_t image = spv_emit_load(e, image_type, image_variable);
    EMIT(&e->functions, 99, image, coordinates, texel); /* OpImageWrite */
}

uint32_t spv_emit_image_read(SpvEmitter* e, uint32_t result_type, uint32_t image_type, uint32_t image_variable,
                             uint32_t coordinates) {
    uint32_t image = spv_emit_load(e, image_type, image_variable);
    uint32_t id = spv_emit_id(e);
    EMIT(&e->functions, 98, result_type, id, image, coordinates); /* OpImageRead */
    return id;
}

uint32_t spv_emit_image_texel_pointer(SpvEmitter* e, uint32_t scalar_type, uint32_t image_variable,
                                      uint32_t coordinates, uint32_t sample) {
    uint32_t pointer_type = spv_emit_type_pointer(e, 11, scalar_type); /* Image storage class */
    uint32_t id = spv_emit_id(e);
    EMIT(&e->functions, 60, pointer_type, id, image_variable, coordinates, sample); /* OpImageTexelPointer */
    return id;
}


uint32_t spv_emit_sampler_image_array_ex(SpvEmitter* e, uint32_t scalar_type, uint32_t u32_type, uint32_t set,
                                         uint32_t binding, uint32_t type, int array, int shadow, int multisample,
                                         uint32_t count, uint32_t* sampled_type) {
    *sampled_type = sampled_image_type(e, scalar_type, type, array, shadow, multisample);
    spv_emit_capability(e, SPV_CAPABILITY_SHADER_NON_UNIFORM);
    spv_emit_capability(e, SPV_CAPABILITY_SAMPLED_IMAGE_ARRAY_NON_UNIFORM_INDEXING);
    uint32_t length = spv_emit_constant_u32(e, u32_type, count);
    uint32_t array_type = spv_emit_id(e);
    EMIT(&e->types, SPV_OP_TYPE_ARRAY, array_type, *sampled_type, length);
    uint32_t pointer = spv_emit_type_pointer(e, 0, array_type); /* UniformConstant */
    uint32_t variable = spv_emit_id(e);
    EMIT(&e->decorations, SPV_OP_DECORATE, variable, 34, set); /* DescriptorSet */
    EMIT(&e->decorations, SPV_OP_DECORATE, variable, 33, binding); /* Binding */
    EMIT(&e->types, SPV_OP_VARIABLE, pointer, variable, 0); /* UniformConstant */
    return variable;
}

/* the element index of a texture array, a pointer the sampling functions take in place of a texture variable */
uint32_t spv_emit_sampler_array_element(SpvEmitter* e, uint32_t sampled_type, uint32_t array_variable, uint32_t index) {
    uint32_t id = spv_emit_id(e);
    instruction(&e->functions, SPV_OP_ACCESS_CHAIN, 5);
    put(&e->functions, spv_emit_type_pointer(e, 0, sampled_type));
    put(&e->functions, id);
    put(&e->functions, array_variable);
    put(&e->functions, index);
    nonuniform(e, index);
    nonuniform(e, id);
    return id;
}


void spv_emit_function_void(SpvEmitter* e, uint32_t void_type, uint32_t function, uint32_t function_type) {
    EMIT(&e->functions, SPV_OP_FUNCTION, void_type, function, 0, function_type); /* FunctionControlMaskNone */
}

void spv_emit_label(SpvEmitter* e, uint32_t label) {
    e->block++;
    EMIT(&e->functions, SPV_OP_LABEL, label);
}

uint32_t spv_emit_function_variable(SpvEmitter* e, uint32_t pointer_type) {
    uint32_t id = spv_emit_id(e);
    EMIT(&e->functions, SPV_OP_VARIABLE, pointer_type, id, 7); /* Function */
    return id;
}

uint32_t spv_emit_function_variable_init(SpvEmitter* e, uint32_t pointer_type, uint32_t initial_value) {
    uint32_t id = spv_emit_id(e);
    EMIT(&e->functions, SPV_OP_VARIABLE, pointer_type, id, 7, initial_value); /* Function */
    return id;
}

uint32_t spv_emit_load(SpvEmitter* e, uint32_t result_type, uint32_t pointer) {
    uint32_t id = spv_emit_id(e);
    EMIT(&e->functions, SPV_OP_LOAD, result_type, id, pointer);
    if (e->nonuniform_count && is_nonuniform(e, pointer)) nonuniform(e, id);
    return id;
}

static uint32_t binary(SpvEmitter* e, uint32_t opcode, uint32_t result_type, uint32_t left, uint32_t right) {
    uint32_t id = spv_emit_id(e);
    EMIT(&e->functions, opcode, result_type, id, left, right);
    return id;
}

uint32_t spv_emit_atomic_u32(SpvEmitter* e, uint32_t u32_type, uint32_t opcode, uint32_t pointer, uint32_t value) {
    assert(opcode == SPV_OP_ATOMIC_EXCHANGE || opcode == SPV_OP_ATOMIC_I_ADD || opcode == 236 ||
           opcode == SPV_OP_ATOMIC_U_MIN || opcode == 238 || opcode == SPV_OP_ATOMIC_U_MAX ||
           opcode == SPV_OP_ATOMIC_AND || opcode == SPV_OP_ATOMIC_OR || opcode == SPV_OP_ATOMIC_XOR);
    uint32_t id = spv_emit_id(e);
    instruction(&e->functions, opcode, 7);
    put(&e->functions, u32_type);
    put(&e->functions, id);
    put(&e->functions, pointer);
    put(&e->functions, spv_emit_constant_u32(e, u32_type, 1)); /* Device scope */
    put(&e->functions, spv_emit_constant_u32(e, u32_type, 0)); /* Relaxed */
    put(&e->functions, value);
    return id;
}

uint32_t spv_emit_memory_semantics(const SpvEmitter* e, uint32_t semantics) {
    if (!e->vulkan_memory_model) return semantics;
    if (semantics & 0xa) semantics |= 0x4000; /* Acquire, AcquireRelease: MakeVisible */
    if (semantics & 0xc) semantics |= 0x2000; /* Release, AcquireRelease: MakeAvailable */
    return semantics;
}
uint32_t spv_emit_atomic_shared_u32(SpvEmitter* e, uint32_t u32_type, uint32_t opcode, uint32_t pointer,
                                    uint32_t value) {
    assert(opcode == SPV_OP_ATOMIC_EXCHANGE || opcode == SPV_OP_ATOMIC_I_ADD || opcode == 236 ||
           opcode == SPV_OP_ATOMIC_U_MIN || opcode == 238 || opcode == SPV_OP_ATOMIC_U_MAX ||
           opcode == SPV_OP_ATOMIC_AND || opcode == SPV_OP_ATOMIC_OR || opcode == SPV_OP_ATOMIC_XOR);
    uint32_t id = spv_emit_id(e);
    instruction(&e->functions, opcode, 7);
    put(&e->functions, u32_type);
    put(&e->functions, id);
    put(&e->functions, pointer);
    put(&e->functions, spv_emit_constant_u32(e, u32_type, 2));     /* Workgroup scope */
    /* AcquireRelease | WorkgroupMemory */
    put(&e->functions, spv_emit_constant_u32(e, u32_type, spv_emit_memory_semantics(e, 0x108)));
    put(&e->functions, value);
    return id;
}

uint32_t spv_emit_atomic_load_u32(SpvEmitter* e, uint32_t u32_type, uint32_t pointer) {
    uint32_t id = spv_emit_id(e);
    instruction(&e->functions, 227, 6); /* OpAtomicLoad */
    put(&e->functions, u32_type);
    put(&e->functions, id);
    put(&e->functions, pointer);
    put(&e->functions, spv_emit_constant_u32(e, u32_type, 1)); /* Device scope */
    put(&e->functions, spv_emit_constant_u32(e, u32_type, 0)); /* Relaxed */
    return id;
}

void spv_emit_atomic_store_u32(SpvEmitter* e, uint32_t u32_type, uint32_t pointer, uint32_t value) {
    instruction(&e->functions, 228, 5); /* OpAtomicStore */
    put(&e->functions, pointer);
    put(&e->functions, spv_emit_constant_u32(e, u32_type, 1)); /* Device scope */
    put(&e->functions, spv_emit_constant_u32(e, u32_type, 0)); /* Relaxed */
    put(&e->functions, value);
}

uint32_t spv_emit_atomic_load_shared_u32(SpvEmitter* e, uint32_t u32_type, uint32_t pointer) {
    uint32_t id = spv_emit_id(e);
    instruction(&e->functions, 227, 6); /* OpAtomicLoad */
    put(&e->functions, u32_type);
    put(&e->functions, id);
    put(&e->functions, pointer);
    put(&e->functions, spv_emit_constant_u32(e, u32_type, 2));     /* Workgroup scope */
    /* Acquire | WorkgroupMemory */
    put(&e->functions, spv_emit_constant_u32(e, u32_type, spv_emit_memory_semantics(e, 0x102)));
    return id;
}

uint32_t spv_emit_atomic_compare_exchange_shared_u32(SpvEmitter* e, uint32_t u32_type, uint32_t pointer,
                                                     uint32_t desired, uint32_t expected) {
    uint32_t id = spv_emit_id(e);
    instruction(&e->functions, 230, 9); /* OpAtomicCompareExchange */
    put(&e->functions, u32_type);
    put(&e->functions, id);
    put(&e->functions, pointer);
    put(&e->functions, spv_emit_constant_u32(e, u32_type, 2));     /* Workgroup scope */
    /* AcquireRelease | WorkgroupMemory */
    put(&e->functions, spv_emit_constant_u32(e, u32_type, spv_emit_memory_semantics(e, 0x108)));
    /* Acquire | WorkgroupMemory */
    put(&e->functions, spv_emit_constant_u32(e, u32_type, spv_emit_memory_semantics(e, 0x102)));
    put(&e->functions, desired);
    put(&e->functions, expected);
    return id;
}

uint32_t spv_emit_atomic_compare_exchange_u32(SpvEmitter* e, uint32_t u32_type, uint32_t pointer, uint32_t desired,
                                              uint32_t expected) {
    uint32_t id = spv_emit_id(e);
    instruction(&e->functions, 230, 9); /* OpAtomicCompareExchange */
    put(&e->functions, u32_type);
    put(&e->functions, id);
    put(&e->functions, pointer);
    put(&e->functions, spv_emit_constant_u32(e, u32_type, 1)); /* Device scope */
    put(&e->functions, spv_emit_constant_u32(e, u32_type, 0)); /* Relaxed success */
    put(&e->functions, spv_emit_constant_u32(e, u32_type, 0)); /* Relaxed failure */
    put(&e->functions, desired);
    put(&e->functions, expected);
    return id;
}

void spv_emit_control_barrier(SpvEmitter* e, uint32_t u32_type) {
    instruction(&e->functions, 224, 4);                            /* OpControlBarrier */
    put(&e->functions, spv_emit_constant_u32(e, u32_type, 2));     /* Workgroup execution scope */
    put(&e->functions, spv_emit_constant_u32(e, u32_type, 2));     /* Workgroup memory scope */
    /* AcquireRelease | WorkgroupMemory */
    put(&e->functions, spv_emit_constant_u32(e, u32_type, spv_emit_memory_semantics(e, 0x108)));
}
void spv_emit_memory_barrier(SpvEmitter* e, uint32_t u32_type, uint32_t scope, uint32_t semantics) {
    instruction(&e->functions, SPV_OP_MEMORY_BARRIER, 3);
    put(&e->functions, spv_emit_constant_u32(e, u32_type, scope));
    put(&e->functions, spv_emit_constant_u32(e, u32_type, spv_emit_memory_semantics(e, semantics)));
}

uint32_t spv_emit_bitfield_insert(SpvEmitter* e, uint32_t u32_type, uint32_t base, uint32_t insert, uint32_t offset,
                                  uint32_t count) {
    uint32_t id = spv_emit_id(e);
    EMIT(&e->functions, SPV_OP_BIT_FIELD_INSERT, u32_type, id, base, insert, offset, count);
    return id;
}

/* The common forms: an instruction of op, its result type, a new id and one, two or three operands. Each takes the ops
   its switch names, whose operands are of that shape */
static int unary_op(uint32_t op) {
    switch (op) {
        case SPV_OP_BITCAST:
        case SPV_OP_BIT_COUNT:
        case SPV_OP_BIT_REVERSE:
        case SPV_OP_CONVERT_F_TO_S:
        case SPV_OP_CONVERT_F_TO_U:
        case SPV_OP_CONVERT_S_TO_F:
        case SPV_OP_CONVERT_U_TO_F:
        case SPV_OP_F_CONVERT:
        case SPV_OP_F_NEGATE:
        case SPV_OP_IS_NAN:
        case SPV_OP_LOGICAL_NOT:
        case SPV_OP_NOT:
        case SPV_OP_READ_CLOCK_KHR: return 1;
        default: return 0;
    }
}
static int binary_op(uint32_t op) {
    switch (op) {
        case SPV_OP_BITWISE_AND:
        case SPV_OP_BITWISE_OR:
        case SPV_OP_BITWISE_XOR:
        case SPV_OP_F_ADD:
        case SPV_OP_F_DIV:
        case SPV_OP_F_MUL:
        case SPV_OP_I_EQUAL:
        case SPV_OP_I_MUL:
        case SPV_OP_I_NOT_EQUAL:
        case SPV_OP_I_SUB:
        case SPV_OP_SHIFT_LEFT_LOGICAL:
        case SPV_OP_SHIFT_RIGHT_ARITHMETIC:
        case SPV_OP_SHIFT_RIGHT_LOGICAL:
        case SPV_OP_S_DIV:
        case SPV_OP_S_GREATER_THAN:
        case SPV_OP_U_LESS_THAN:
        case SPV_OP_VECTOR_TIMES_SCALAR: return 1;
        default: return 0;
    }
}
static int ternary_op(uint32_t op) {
    switch (op) {
        case SPV_OP_BIT_FIELD_S_EXTRACT:
        case SPV_OP_BIT_FIELD_U_EXTRACT:
        case SPV_OP_SELECT: return 1;
        default: return 0;
    }
}
uint32_t spv_emit_unary(SpvEmitter* e, uint32_t op, uint32_t result_type, uint32_t a) {
    assert(unary_op(op));
    uint32_t id = spv_emit_id(e);
    EMIT(&e->functions, op, result_type, id, a);
    return id;
}
uint32_t spv_emit_binary(SpvEmitter* e, uint32_t op, uint32_t result_type, uint32_t a, uint32_t b) {
    assert(binary_op(op));
    return binary(e, op, result_type, a, b);
}
uint32_t spv_emit_ternary(SpvEmitter* e, uint32_t op, uint32_t result_type, uint32_t a, uint32_t b, uint32_t c) {
    assert(ternary_op(op));
    uint32_t id = spv_emit_id(e);
    EMIT(&e->functions, op, result_type, id, a, b, c);
    return id;
}

uint32_t spv_emit_fcompare(SpvEmitter* e, uint32_t bool_type, uint32_t condition, uint32_t left, uint32_t right) {
    static const uint32_t opcode[16] = {0, 184, 180, 188, 186, 182, 190, 0, 0, 185, 181, 189, 187, 183, 191, 0};
    assert(condition < 16);
    if (condition == 0 || condition == 15) return spv_emit_constant_bool(e, bool_type, condition == 15);
    if (condition == 7 || condition == 8) {
        uint32_t nan = spv_emit_bool_binary(e, bool_type, 1, spv_emit_unary(e, SPV_OP_IS_NAN, bool_type, left),
                                            spv_emit_unary(e, SPV_OP_IS_NAN, bool_type, right));
        return condition == 7 ? spv_emit_unary(e, SPV_OP_LOGICAL_NOT, bool_type, nan) : nan;
    }
    return binary(e, opcode[condition], bool_type, left, right);
}

uint32_t spv_emit_flush_subnormal(SpvEmitter* e, uint32_t f32_type, uint32_t u32_type, uint32_t bool_type,
                                  uint32_t glsl, uint32_t value, int signed_zeros) {
    if (signed_zeros) { /* below the smallest normal, times zero: the zero of its sign; NaN fails the test */
        uint32_t magnitude = spv_emit_glsl_unary(e, f32_type, glsl, SPV_GLSL_F_ABS, value);
        uint32_t tiny = spv_emit_fcompare(e, bool_type, 1, magnitude, spv_emit_constant_u32(e, f32_type, 0x00800000u));
        uint32_t zero = spv_emit_binary(e, SPV_OP_F_MUL, f32_type, value, spv_emit_constant_u32(e, f32_type, 0));
        return spv_emit_ternary(e, SPV_OP_SELECT, f32_type, tiny, zero, value);
    }
    uint32_t bits = spv_emit_unary(e, SPV_OP_BITCAST, u32_type, value);
    uint32_t exponent =
        spv_emit_binary(e, SPV_OP_BITWISE_AND, u32_type, bits, spv_emit_constant_u32(e, u32_type, 0x7f800000u));
    uint32_t sign =
        spv_emit_binary(e, SPV_OP_BITWISE_AND, u32_type, bits, spv_emit_constant_u32(e, u32_type, 0x80000000u));
    uint32_t flushed = spv_emit_ternary(
        e, SPV_OP_SELECT, u32_type,
        spv_emit_binary(e, SPV_OP_I_EQUAL, bool_type, exponent, spv_emit_constant_u32(e, u32_type, 0)), sign, bits);
    return spv_emit_unary(e, SPV_OP_BITCAST, f32_type, flushed);
}

/* PTX's add, sub, mul, fma and cvt .sat flush a NaN result to +0, and GLSL.std.450 leaves FClamp's result undefined for
   a NaN. The NaN is found on the bits: without SignedZeroInfNanPreserve a driver may take no float for a NaN, and an
   OpIsNan for false */
uint32_t spv_emit_saturate(SpvEmitter* e, uint32_t f32_type, uint32_t u32_type, uint32_t bool_type, uint32_t glsl,
                           uint32_t value) {
    uint32_t zero = spv_emit_constant_u32(e, f32_type, 0), one = spv_emit_constant_u32(e, f32_type, 0x3f800000u);
    uint32_t clamped = spv_emit_glsl_ternary(e, f32_type, glsl, SPV_GLSL_F_CLAMP, value, zero, one);
    uint32_t magnitude =
        spv_emit_binary(e, SPV_OP_BITWISE_AND, u32_type, spv_emit_unary(e, SPV_OP_BITCAST, u32_type, value),
                        spv_emit_constant_u32(e, u32_type, 0x7fffffffu));
    uint32_t nan =
        spv_emit_binary(e, SPV_OP_U_LESS_THAN, bool_type, spv_emit_constant_u32(e, u32_type, 0x7f800000u), magnitude);
    return spv_emit_ternary(e, SPV_OP_SELECT, f32_type, nan, zero, clamped);
}

uint32_t spv_emit_bool_binary(SpvEmitter* e, uint32_t bool_type, uint32_t op, uint32_t left, uint32_t right) {
    static const uint32_t opcode[3] = {SPV_OP_LOGICAL_AND, SPV_OP_LOGICAL_OR, SPV_OP_LOGICAL_NOT_EQUAL};
    assert(op < 3);
    return binary(e, opcode[op], bool_type, left, right);
}

uint32_t spv_emit_derivative(SpvEmitter* e, uint32_t f32_type, uint32_t value, int y_axis) {
    uint32_t id = spv_emit_id(e);
    EMIT(&e->functions, y_axis ? SPV_OP_DPDY : SPV_OP_DPDX, f32_type, id, value);
    return id;
}

uint32_t spv_emit_derivative_fine(SpvEmitter* e, uint32_t type, uint32_t value, int y_axis) {
    uint32_t id = spv_emit_id(e);
    spv_emit_capability(e, SPV_CAPABILITY_DERIVATIVE_CONTROL);
    EMIT(&e->functions, y_axis ? SPV_OP_DPDY_FINE : SPV_OP_DPDX_FINE, type, id, value);
    return id;
}

uint32_t spv_emit_glsl_unary(SpvEmitter* e, uint32_t f32_type, uint32_t import, SpvGlsl450Op op, uint32_t value) {
    uint32_t id = spv_emit_id(e);
    EMIT(&e->functions, SPV_OP_EXT_INST, f32_type, id, import, (uint32_t)op, value);
    return id;
}

uint32_t spv_emit_glsl_binary(SpvEmitter* e, uint32_t f32_type, uint32_t import, SpvGlsl450Op op, uint32_t left,
                              uint32_t right) {
    uint32_t id = spv_emit_id(e);
    EMIT(&e->functions, SPV_OP_EXT_INST, f32_type, id, import, (uint32_t)op, left, right);
    return id;
}

uint32_t spv_emit_glsl_ternary(SpvEmitter* e, uint32_t f32_type, uint32_t import, SpvGlsl450Op op, uint32_t a,
                               uint32_t b, uint32_t c) {
    uint32_t id = spv_emit_id(e);
    EMIT(&e->functions, SPV_OP_EXT_INST, f32_type, id, import, (uint32_t)op, a, b, c);
    return id;
}

uint32_t spv_emit_vec4_extract(SpvEmitter* e, uint32_t component_type, uint32_t vector, uint32_t component) {
    assert(component < 4);
    uint32_t id = spv_emit_id(e);
    EMIT(&e->functions, SPV_OP_COMPOSITE_EXTRACT, component_type, id, vector, component);
    return id;
}

uint32_t spv_emit_subgroup_ballot(SpvEmitter* e, uint32_t vec4_type, uint32_t u32_type, uint32_t predicate) {
    uint32_t id = spv_emit_id(e);
    instruction(&e->functions, SPV_OP_GROUP_NON_UNIFORM_BALLOT, 5);
    put(&e->functions, vec4_type);
    put(&e->functions, id);
    put(&e->functions, spv_emit_constant_u32(e, u32_type, 3)); /* Subgroup scope */
    put(&e->functions, predicate);
    return id;
}

/* the smallest value among the subgroup's active invocations (OpGroupNonUniformUMin, Reduce) */
uint32_t spv_emit_subgroup_umin_u32(SpvEmitter* e, uint32_t u32_type, uint32_t value) {
    uint32_t id = spv_emit_id(e);
    instruction(&e->functions, 354, 6); /* OpGroupNonUniformUMin */
    put(&e->functions, u32_type);
    put(&e->functions, id);
    put(&e->functions, spv_emit_constant_u32(e, u32_type, 3)); /* Subgroup scope */
    put(&e->functions, 0);                                     /* Reduce */
    put(&e->functions, value);
    return id;
}


uint32_t spv_emit_subgroup_shuffle_u32(SpvEmitter* e, uint32_t u32_type, uint32_t value, uint32_t lane) {
    uint32_t id = spv_emit_id(e);
    instruction(&e->functions, SPV_OP_GROUP_NON_UNIFORM_SHUFFLE, 6);
    put(&e->functions, u32_type);
    put(&e->functions, id);
    put(&e->functions, spv_emit_constant_u32(e, u32_type, 3)); /* Subgroup scope */
    put(&e->functions, value);
    put(&e->functions, lane);
    return id;
}

uint32_t spv_emit_subgroup_quad_u32(SpvEmitter* e, uint32_t u32_type, uint32_t value, int swap, uint32_t operand) {
    uint32_t id = spv_emit_id(e);
    instruction(&e->functions, swap ? SPV_OP_GROUP_NON_UNIFORM_QUAD_SWAP : SPV_OP_GROUP_NON_UNIFORM_QUAD_BROADCAST, 6);
    put(&e->functions, u32_type);
    put(&e->functions, id);
    put(&e->functions, spv_emit_constant_u32(e, u32_type, 3)); /* Subgroup scope */
    put(&e->functions, value);
    put(&e->functions, operand);
    return id;
}

uint32_t spv_emit_vec4_insert(SpvEmitter* e, uint32_t vec4_type, uint32_t vector, uint32_t component_value,
                              uint32_t component) {
    assert(component < 4);
    uint32_t id = spv_emit_id(e);
    EMIT(&e->functions, SPV_OP_COMPOSITE_INSERT, vec4_type, id, component_value, vector, component);
    return id;
}

uint32_t spv_emit_vec4_construct(SpvEmitter* e, uint32_t vec4_type, const uint32_t components[4]) {
    uint32_t id = spv_emit_id(e);
    instruction(&e->functions, SPV_OP_COMPOSITE_CONSTRUCT, 7);
    put(&e->functions, vec4_type);
    put(&e->functions, id);
    put_n(&e->functions, components, 4);
    return id;
}

uint32_t spv_emit_vec2_construct(SpvEmitter* e, uint32_t vec2_type, uint32_t x, uint32_t y) {
    uint32_t id = spv_emit_id(e);
    EMIT(&e->functions, SPV_OP_COMPOSITE_CONSTRUCT, vec2_type, id, x, y);
    return id;
}

uint32_t spv_emit_vec3_construct(SpvEmitter* e, uint32_t vec3_type, uint32_t x, uint32_t y, uint32_t z) {
    uint32_t id = spv_emit_id(e);
    EMIT(&e->functions, SPV_OP_COMPOSITE_CONSTRUCT, vec3_type, id, x, y, z);
    return id;
}

uint32_t spv_emit_sample2d(SpvEmitter* e, uint32_t result_type, uint32_t sampled_type, uint32_t sampler,
                           uint32_t coordinates, uint32_t depth_reference) {
    uint32_t image = spv_emit_load(e, sampled_type, sampler);
    uint32_t id = spv_emit_id(e);
    instruction(&e->functions,
                depth_reference ? SPV_OP_IMAGE_SAMPLE_DREF_IMPLICIT_LOD : SPV_OP_IMAGE_SAMPLE_IMPLICIT_LOD,
                depth_reference ? 6 : 5);
    put(&e->functions, result_type);
    put(&e->functions, id);
    put(&e->functions, image);
    put(&e->functions, coordinates);
    if (depth_reference) put(&e->functions, depth_reference);
    return id;
}

uint32_t spv_emit_sample2d_lod(SpvEmitter* e, uint32_t result_type, uint32_t sampled_type, uint32_t sampler,
                               uint32_t coordinates, uint32_t depth_reference, uint32_t lod) {
    uint32_t image = spv_emit_load(e, sampled_type, sampler);
    uint32_t id = spv_emit_id(e);
    instruction(&e->functions,
                depth_reference ? SPV_OP_IMAGE_SAMPLE_DREF_EXPLICIT_LOD : SPV_OP_IMAGE_SAMPLE_EXPLICIT_LOD,
                depth_reference ? 8 : 7);
    put(&e->functions, result_type);
    put(&e->functions, id);
    put(&e->functions, image);
    put(&e->functions, coordinates);
    if (depth_reference) put(&e->functions, depth_reference);
    put(&e->functions, 2); /* ImageOperands Lod */
    put(&e->functions, lod);
    return id;
}

uint32_t spv_emit_image_query_size_lod(SpvEmitter* e, uint32_t result_type, uint32_t sampled_type, uint32_t sampler,
                                       uint32_t lod) {
    uint32_t image_type = image_of(e, sampled_type);
    uint32_t sampled = spv_emit_load(e, sampled_type, sampler);
    uint32_t image = spv_emit_id(e);
    EMIT(&e->functions, SPV_OP_IMAGE, image_type, image, sampled);
    if (e->nonuniform_count && is_nonuniform(e, sampled)) nonuniform(e, image);
    uint32_t id = spv_emit_id(e);
    instruction(&e->functions, lod ? SPV_OP_IMAGE_QUERY_SIZE_LOD : SPV_OP_IMAGE_QUERY_SIZE, lod ? 5 : 4);
    put(&e->functions, result_type);
    put(&e->functions, id);
    put(&e->functions, image);
    if (lod) put(&e->functions, lod);
    return id;
}

uint32_t spv_emit_image_query_lod(SpvEmitter* e, uint32_t result_type, uint32_t sampled_type, uint32_t sampler,
                                  uint32_t coordinates) {
    uint32_t sampled = spv_emit_load(e, sampled_type, sampler);
    uint32_t id = spv_emit_id(e);
    EMIT(&e->functions, SPV_OP_IMAGE_QUERY_LOD, result_type, id, sampled, coordinates);
    return id;
}

uint32_t spv_emit_image_query_levels(SpvEmitter* e, uint32_t u32_type, uint32_t sampled_type, uint32_t sampler) {
    uint32_t image_type = image_of(e, sampled_type);
    uint32_t sampled = spv_emit_load(e, sampled_type, sampler);
    uint32_t image = spv_emit_id(e);
    EMIT(&e->functions, SPV_OP_IMAGE, image_type, image, sampled);
    if (e->nonuniform_count && is_nonuniform(e, sampled)) nonuniform(e, image);
    uint32_t id = spv_emit_id(e);
    EMIT(&e->functions, 106, u32_type, id, image); /* OpImageQueryLevels */
    return id;
}

uint32_t spv_emit_image_fetch(SpvEmitter* e, uint32_t result_type, uint32_t sampled_type, uint32_t sampler,
                              uint32_t coordinates, uint32_t lod) {
    uint32_t image_type = image_of(e, sampled_type);
    uint32_t sampled = spv_emit_load(e, sampled_type, sampler);
    uint32_t image = spv_emit_id(e);
    EMIT(&e->functions, SPV_OP_IMAGE, image_type, image, sampled);
    if (e->nonuniform_count && is_nonuniform(e, sampled)) nonuniform(e, image);
    uint32_t id = spv_emit_id(e);
    EMIT(&e->functions, SPV_OP_IMAGE_FETCH, result_type, id, image, coordinates, 2, lod); /* ImageOperands Lod */
    return id;
}

uint32_t spv_emit_image_fetch_sample(SpvEmitter* e, uint32_t result_type, uint32_t sampled_type, uint32_t sampler,
                                     uint32_t coordinates, uint32_t sample) {
    uint32_t image_type = image_of(e, sampled_type);
    uint32_t sampled = spv_emit_load(e, sampled_type, sampler);
    uint32_t image = spv_emit_id(e);
    EMIT(&e->functions, SPV_OP_IMAGE, image_type, image, sampled);
    if (e->nonuniform_count && is_nonuniform(e, sampled)) nonuniform(e, image);
    uint32_t id = spv_emit_id(e);
    EMIT(&e->functions, SPV_OP_IMAGE_FETCH, result_type, id, image, coordinates, 64, sample); /* ImageOperands Sample */
    return id;
}

uint32_t spv_emit_image_gather_ex(SpvEmitter* e, uint32_t vec4_type, uint32_t sampled_type, uint32_t sampler,
                                  uint32_t coordinates, uint32_t component, uint32_t dref, uint32_t offset) {
    uint32_t image = spv_emit_load(e, sampled_type, sampler);
    uint32_t id = spv_emit_id(e);
    instruction(&e->functions, dref ? SPV_OP_IMAGE_DREF_GATHER : SPV_OP_IMAGE_GATHER, offset ? 8 : 6);
    put(&e->functions, vec4_type);
    put(&e->functions, id);
    put(&e->functions, image);
    put(&e->functions, coordinates);
    put(&e->functions, dref ? dref : component);
    if (offset) {
        put(&e->functions, 16); /* ImageOperands Offset */
        put(&e->functions, offset);
    }
    return id;
}

uint32_t spv_emit_image_gather_offset(SpvEmitter* e, uint32_t vec4_type, uint32_t sampled_type, uint32_t sampler,
                                      uint32_t coordinates, uint32_t component, uint32_t offset, int dynamic_offset) {
    if (offset && dynamic_offset) spv_emit_capability(e, SPV_CAPABILITY_IMAGE_GATHER_EXTENDED);
    uint32_t image = spv_emit_load(e, sampled_type, sampler);
    uint32_t id = spv_emit_id(e);
    instruction(&e->functions, SPV_OP_IMAGE_GATHER, offset ? 8 : 6);
    put(&e->functions, vec4_type);
    put(&e->functions, id);
    put(&e->functions, image);
    put(&e->functions, coordinates);
    put(&e->functions, component);
    if (offset) {
        put(&e->functions, dynamic_offset ? 16 : 8); /* ImageOperands Offset, or ConstOffset */
        put(&e->functions, offset);
    }
    return id;
}

uint32_t spv_emit_sample_image_min_lod(SpvEmitter* e, uint32_t result_type, uint32_t sampled_type, uint32_t sampler,
                                       uint32_t coordinates, uint32_t depth_reference, uint32_t mode, uint32_t level,
                                       uint32_t offset, int dynamic_offset, uint32_t min_lod) {
    assert(!min_lod || mode != 1);
    if (min_lod) spv_emit_capability(e, 42); /* MinLod */
    if (offset && dynamic_offset) spv_emit_capability(e, 25); /* Offset requires ImageGatherExtended. */
    uint32_t image = spv_emit_load(e, sampled_type, sampler);
    uint32_t id = spv_emit_id(e);
    uint32_t operands = mode == 1 ? 2 : mode == 2 ? 1 : 0;
    uint32_t opcode;
    if (offset) operands |= dynamic_offset ? 16 : 8;
    if (min_lod) operands |= 128;
    opcode = mode == 1 ? depth_reference ? SPV_OP_IMAGE_SAMPLE_DREF_EXPLICIT_LOD : SPV_OP_IMAGE_SAMPLE_EXPLICIT_LOD
             : depth_reference ? SPV_OP_IMAGE_SAMPLE_DREF_IMPLICIT_LOD
                               : SPV_OP_IMAGE_SAMPLE_IMPLICIT_LOD;
    instruction(&e->functions, opcode, 5 + !!depth_reference + !!operands + !!level + !!offset + !!min_lod);
    put(&e->functions, result_type);
    put(&e->functions, id);
    put(&e->functions, image);
    put(&e->functions, coordinates);
    if (depth_reference) put(&e->functions, depth_reference);
    if (operands) put(&e->functions, operands);
    if (level) put(&e->functions, level);
    if (offset) put(&e->functions, offset);
    if (min_lod) put(&e->functions, min_lod);
    return id;
}


uint32_t spv_emit_sample_grad_offset(SpvEmitter* e, uint32_t result_type, uint32_t sampled_type, uint32_t sampler,
                                     uint32_t coordinates, uint32_t dx, uint32_t dy, uint32_t offset,
                                     int dynamic_offset, uint32_t min_lod) {
    if (min_lod) spv_emit_capability(e, 42); /* MinLod */
    if (offset && dynamic_offset) spv_emit_capability(e, 25); /* Offset requires ImageGatherExtended. */
    uint32_t image = spv_emit_load(e, sampled_type, sampler);
    uint32_t id = spv_emit_id(e);
    instruction(&e->functions, SPV_OP_IMAGE_SAMPLE_EXPLICIT_LOD, 8 + !!offset + !!min_lod);
    put(&e->functions, result_type);
    put(&e->functions, id);
    put(&e->functions, image);
    put(&e->functions, coordinates);
    put(&e->functions, 4 | (offset ? dynamic_offset ? 16 : 8 : 0) | (min_lod ? 128 : 0));
    put(&e->functions, dx);
    put(&e->functions, dy);
    if (offset) put(&e->functions, offset);
    if (min_lod) put(&e->functions, min_lod);
    return id;
}

void spv_emit_no_contraction(SpvEmitter* e, uint32_t result) {
    EMIT(&e->decorations, SPV_OP_DECORATE, result, 42); /* NoContraction */
}

uint32_t spv_emit_storage_access_u32(SpvEmitter* e, uint32_t pointer_type, uint32_t buffer, uint32_t member_index,
                                     uint32_t element_index) {
    uint32_t id = spv_emit_id(e);
    EMIT(&e->functions, SPV_OP_ACCESS_CHAIN, pointer_type, id, buffer, member_index, element_index);
    return id;
}

uint32_t spv_emit_iadd_u32(SpvEmitter* e, uint32_t u32_type, uint32_t left, uint32_t right) {
    uint32_t id = spv_emit_id(e);
    EMIT(&e->functions, SPV_OP_I_ADD, u32_type, id, left, right);
    return id;
}

uint32_t spv_emit_umul_high_u32(SpvEmitter* e, uint32_t u32_type, uint32_t left, uint32_t right) {
    uint32_t pair_type = spv_emit_id(e);
    EMIT(&e->types, SPV_OP_TYPE_STRUCT, pair_type, u32_type, u32_type);
    uint32_t pair = binary(e, SPV_OP_U_MUL_EXTENDED, pair_type, left, right);
    uint32_t high = spv_emit_id(e);
    EMIT(&e->functions, SPV_OP_COMPOSITE_EXTRACT, u32_type, high, pair, 1);
    return high;
}

void spv_emit_store(SpvEmitter* e, uint32_t pointer, uint32_t value) {
    EMIT(&e->functions, SPV_OP_STORE, pointer, value);
}
uint32_t spv_emit_load_nonprivate(SpvEmitter* e, uint32_t result_type, uint32_t pointer) {
    if (!e->vulkan_memory_model) return spv_emit_load(e, result_type, pointer);
    uint32_t id = spv_emit_id(e);
    EMIT(&e->functions, SPV_OP_LOAD, result_type, id, pointer, 0x20); /* NonPrivatePointer */
    if (e->nonuniform_count && is_nonuniform(e, pointer)) nonuniform(e, id);
    return id;
}
void spv_emit_store_nonprivate(SpvEmitter* e, uint32_t pointer, uint32_t value) {
    if (!e->vulkan_memory_model) {
        spv_emit_store(e, pointer, value);
        return;
    }
    EMIT(&e->functions, SPV_OP_STORE, pointer, value, 0x20); /* NonPrivatePointer */
}


void spv_emit_return(SpvEmitter* e) { instruction(&e->functions, SPV_OP_RETURN, 1); }
void spv_emit_kill(SpvEmitter* e) { instruction(&e->functions, SPV_OP_KILL, 1); }
void spv_emit_demote(SpvEmitter* e) { instruction(&e->functions, SPV_OP_DEMOTE_TO_HELPER_INVOCATION, 1); }
uint32_t spv_emit_is_helper(SpvEmitter* e, uint32_t bool_type) {
    uint32_t id = spv_emit_id(e);
    EMIT(&e->functions, SPV_OP_IS_HELPER_INVOCATION, bool_type, id);
    return id;
}
static void block_ends(SpvEmitter* e) {
    if (e->block_end) e->block_end(e->block_end_user);
}

void spv_emit_selection_merge(SpvEmitter* e, uint32_t merge_label) {
    block_ends(e);
    EMIT(&e->functions, SPV_OP_SELECTION_MERGE, merge_label, 0); /* SelectionControlMaskNone */
}

void spv_emit_branch_conditional(SpvEmitter* e, uint32_t condition, uint32_t yes_label, uint32_t no_label) {
    block_ends(e);
    EMIT(&e->functions, SPV_OP_BRANCH_CONDITIONAL, condition, yes_label, no_label);
}

void spv_emit_branch(SpvEmitter* e, uint32_t label) {
    block_ends(e);
    EMIT(&e->functions, SPV_OP_BRANCH, label);
}

void spv_emit_loop_merge(SpvEmitter* e, uint32_t merge_label, uint32_t continue_label) {
    block_ends(e);
    EMIT(&e->functions, SPV_OP_LOOP_MERGE, merge_label, continue_label, 0); /* LoopControlMaskNone */
}

void spv_emit_switch_u32(SpvEmitter* e, uint32_t selector, uint32_t default_label, const uint32_t* literals,
                         const uint32_t* labels, size_t count) {
    assert(count <= (UINT16_MAX - 3) / 2);
    block_ends(e);
    instruction(&e->functions, SPV_OP_SWITCH, (uint32_t)(3 + count * 2));
    put(&e->functions, selector);
    put(&e->functions, default_label);
    for (size_t i = 0; i < count; i++) {
        put(&e->functions, literals[i]);
        put(&e->functions, labels[i]);
    }
}
void spv_emit_function_end(SpvEmitter* e) {
    if (e->locals.count) { /* spv_emit_local's variables, into the first block */
        assert(e->body_start && e->body_start <= e->functions.count);
        reserve(&e->functions, e->locals.count);
        uint32_t* at = e->functions.words + e->body_start;
        memmove(at + e->locals.count, at, (e->functions.count - e->body_start) * sizeof *at);
        memcpy(at, e->locals.words, e->locals.count * sizeof *at);
        e->functions.count += e->locals.count;
        e->locals.count = 0;
    }
    e->body_start = 0;
    instruction(&e->functions, SPV_OP_FUNCTION_END, 1);
}

/* ---- the generic layer ---- */

void spv_emit_extension(SpvEmitter* e, const char* name) {
    size_t n = strlen(name) + 1, words = string_words(name);
    for (size_t i = 0; i < e->extensions.count; i += e->extensions.words[i] >> 16) {
        size_t j = 0;
        if (e->extensions.words[i] >> 16 != 1 + words) continue;
        while (j < words && e->extensions.words[i + 1 + j] == string_word(name, n, j)) j++;
        if (j == words) return;
    }
    assert(words <= UINT16_MAX - 1);
    instruction(&e->extensions, SPV_OP_EXTENSION, (uint32_t)(1 + words));
    put_str(&e->extensions, name);
}

uint32_t spv_emit_type_int(SpvEmitter* e, uint32_t width, int is_signed) {
    uint32_t operands[2] = {width, is_signed ? 1u : 0u};
    return known(e, SPV_OP_TYPE_INT, 1, operands, 2);
}
uint32_t spv_emit_type_float(SpvEmitter* e, uint32_t width) { return known(e, SPV_OP_TYPE_FLOAT, 1, &width, 1); }
uint32_t spv_emit_type_vector(SpvEmitter* e, uint32_t component, uint32_t count) {
    uint32_t operands[2] = {component, count};
    return known(e, SPV_OP_TYPE_VECTOR, 1, operands, 2);
}
uint32_t spv_emit_type_array(SpvEmitter* e, uint32_t element, uint32_t length) {
    assert(length);
    uint32_t operands[2] = {element, spv_emit_const_u32(e, length)};
    return known(e, SPV_OP_TYPE_ARRAY, 1, operands, 2);
}
uint32_t spv_emit_type_pointer(SpvEmitter* e, uint32_t storage, uint32_t type) {
    uint32_t operands[2] = {storage, type};
    return known(e, SPV_OP_TYPE_POINTER, 1, operands, 2);
}
uint32_t spv_emit_type_cooperative_matrix(SpvEmitter* e, uint32_t component, uint32_t scope, uint32_t rows,
                                          uint32_t columns, uint32_t use) {
    uint32_t operands[5] = {component, spv_emit_const_u32(e, scope), spv_emit_const_u32(e, rows),
                            spv_emit_const_u32(e, columns), spv_emit_const_u32(e, use)};
    return known(e, SPV_OP_TYPE_COOPERATIVE_MATRIX_KHR, 1, operands, 5);
}
uint32_t spv_emit_type_function(SpvEmitter* e, uint32_t result, const uint32_t* params, size_t count) {
    uint32_t operands[17];
    assert(count <= 16);
    operands[0] = result;
    for (size_t i = 0; i < count; i++) operands[i + 1] = params[i];
    return known(e, SPV_OP_TYPE_FUNCTION, 1, operands, count + 1);
}
uint32_t spv_emit_type_image(SpvEmitter* e, uint32_t sampled_type, uint32_t dim, int depth, int arrayed,
                             int multisampled, uint32_t sampled, uint32_t format) {
    uint32_t operands[7] = {sampled_type,           dim,     (uint32_t)depth, (uint32_t)arrayed,
                            (uint32_t)multisampled, sampled, format};
    return known(e, SPV_OP_TYPE_IMAGE, 1, operands, 7);
}
uint32_t spv_emit_type_sampled_image(SpvEmitter* e, uint32_t image) {
    return known(e, SPV_OP_TYPE_SAMPLED_IMAGE, 1, &image, 1);
}
uint32_t spv_emit_type_struct(SpvEmitter* e, const uint32_t* members, const uint32_t* offsets, size_t count) {
    uint32_t id = spv_emit_id(e);
    assert(count <= UINT16_MAX - 2);
    instruction(&e->types, SPV_OP_TYPE_STRUCT, (uint32_t)(count + 2));
    put(&e->types, id);
    put_n(&e->types, members, count);
    for (size_t i = 0; i < count; i++)
        spv_emit_member_decorate(e, id, (uint32_t)i, SPV_DECORATION_OFFSET, &offsets[i], 1);
    return id;
}

uint32_t spv_emit_const_u32(SpvEmitter* e, uint32_t value) {
    uint32_t operands[2] = {spv_emit_type_int(e, 32, 0), value};
    return known(e, SPV_OP_CONSTANT, 2, operands, 2);
}
uint32_t spv_emit_const_i32(SpvEmitter* e, int32_t value) {
    uint32_t operands[2] = {spv_emit_type_int(e, 32, 1), (uint32_t)value};
    return known(e, SPV_OP_CONSTANT, 2, operands, 2);
}
uint32_t spv_emit_const_f32(SpvEmitter* e, float value) {
    uint32_t operands[2] = {spv_emit_type_float(e, 32), 0};
    memcpy(&operands[1], &value, sizeof value);
    return known(e, SPV_OP_CONSTANT, 2, operands, 2);
}
uint32_t spv_emit_const_bool(SpvEmitter* e, int value) {
    uint32_t type = spv_emit_type_bool(e);
    return known(e, value ? SPV_OP_CONSTANT_TRUE : SPV_OP_CONSTANT_FALSE, 2, &type, 1);
}
uint32_t spv_emit_const_composite(SpvEmitter* e, uint32_t type, const uint32_t* parts, size_t count) {
    uint32_t operands[17];
    assert(count <= 16);
    operands[0] = type;
    for (size_t i = 0; i < count; i++) operands[i + 1] = parts[i];
    return known(e, SPV_OP_CONSTANT_COMPOSITE, 2, operands, count + 1);
}

void spv_emit_decorate(SpvEmitter* e, uint32_t id, uint32_t decoration, const uint32_t* operands, size_t count) {
    assert(count <= UINT16_MAX - 3);
    instruction(&e->decorations, SPV_OP_DECORATE, (uint32_t)(count + 3));
    put(&e->decorations, id);
    put(&e->decorations, decoration);
    put_n(&e->decorations, operands, count);
}
void spv_emit_member_decorate(SpvEmitter* e, uint32_t type, uint32_t member, uint32_t decoration,
                              const uint32_t* operands, size_t count) {
    assert(count <= UINT16_MAX - 4);
    instruction(&e->decorations, SPV_OP_MEMBER_DECORATE, (uint32_t)(count + 4));
    put(&e->decorations, type);
    put(&e->decorations, member);
    put(&e->decorations, decoration);
    put_n(&e->decorations, operands, count);
}
static uint32_t variable(SpvEmitter* e, uint32_t storage, uint32_t type, uint32_t initializer) {
    uint32_t pointer = spv_emit_type_pointer(e, storage, type), id = spv_emit_id(e);
    assert(storage != SPV_STORAGE_FUNCTION);
    instruction(&e->types, SPV_OP_VARIABLE, initializer ? 5 : 4);
    put(&e->types, pointer);
    put(&e->types, id);
    put(&e->types, storage);
    if (initializer) put(&e->types, initializer);
    if (storage == SPV_STORAGE_INPUT || storage == SPV_STORAGE_OUTPUT) {
        if (e->interface_count == e->interface_capacity) {
            size_t capacity = e->interface_capacity ? e->interface_capacity * 2 : 16;
            uint32_t* grown = realloc(e->interface, capacity * sizeof *grown);
            if (!grown) abort();
            e->interface = grown;
            e->interface_capacity = capacity;
        }
        e->interface[e->interface_count++] = id;
    }
    return id;
}
uint32_t spv_emit_variable(SpvEmitter* e, uint32_t storage, uint32_t type) { return variable(e, storage, type, 0); }
uint32_t spv_emit_variable_zeroed(SpvEmitter* e, uint32_t storage, uint32_t type) {
    uint32_t zero = spv_emit_id(e);
    EMIT(&e->types, SPV_OP_CONSTANT_NULL, type, zero);
    return variable(e, storage, type, zero);
}
uint32_t spv_emit_resource(SpvEmitter* e, uint32_t storage, uint32_t type, uint32_t set, uint32_t binding) {
    uint32_t id = spv_emit_variable(e, storage, type);
    spv_emit_decorate(e, id, SPV_DECORATION_DESCRIPTOR_SET, &set, 1);
    spv_emit_decorate(e, id, SPV_DECORATION_BINDING, &binding, 1);
    return id;
}
uint32_t spv_emit_location(SpvEmitter* e, uint32_t storage, uint32_t type, uint32_t location) {
    uint32_t id = spv_emit_variable(e, storage, type);
    spv_emit_decorate(e, id, SPV_DECORATION_LOCATION, &location, 1);
    return id;
}
uint32_t spv_emit_builtin(SpvEmitter* e, uint32_t storage, uint32_t type, uint32_t builtin) {
    uint32_t id = spv_emit_variable(e, storage, type);
    spv_emit_decorate(e, id, SPV_DECORATION_BUILT_IN, &builtin, 1);
    return id;
}

uint32_t spv_emit_function(SpvEmitter* e, uint32_t id, uint32_t result, const uint32_t* param_types, size_t count,
                           uint32_t* params) {
    uint32_t type = spv_emit_type_function(e, result, param_types, count);
    assert(!e->body_start && !e->locals.count);
    if (!id) id = spv_emit_id(e);
    EMIT(&e->functions, SPV_OP_FUNCTION, result, id, 0, type); /* FunctionControlMaskNone */
    for (size_t i = 0; i < count; i++) {
        params[i] = spv_emit_id(e);
        EMIT(&e->functions, SPV_OP_FUNCTION_PARAMETER, param_types[i], params[i]);
    }
    spv_emit_label(e, spv_emit_id(e));
    e->body_start = e->functions.count;
    return id;
}
uint32_t spv_emit_local(SpvEmitter* e, uint32_t type) {
    uint32_t pointer = spv_emit_type_pointer(e, SPV_STORAGE_FUNCTION, type), id = spv_emit_id(e);
    assert(e->body_start);
    EMIT(&e->locals, SPV_OP_VARIABLE, pointer, id, SPV_STORAGE_FUNCTION);
    return id;
}
uint32_t spv_emit_op(SpvEmitter* e, uint32_t opcode, uint32_t type, const uint32_t* operands, size_t count) {
    if (!type) {
        spv_emit_void_op(e, opcode, operands, count);
        return 0;
    }
    uint32_t id = spv_emit_id(e);
    assert(count <= UINT16_MAX - 3);
    instruction(&e->functions, opcode, (uint32_t)(count + 3));
    put(&e->functions, type);
    put(&e->functions, id);
    put_n(&e->functions, operands, count);
    return id;
}
void spv_emit_void_op(SpvEmitter* e, uint32_t opcode, const uint32_t* operands, size_t count) {
    assert(count <= UINT16_MAX - 1);
    if (opcode == SPV_OP_LOOP_MERGE || opcode == SPV_OP_SELECTION_MERGE || opcode == SPV_OP_BRANCH ||
        opcode == SPV_OP_BRANCH_CONDITIONAL || opcode == SPV_OP_SWITCH)
        block_ends(e);
    instruction(&e->functions, opcode, (uint32_t)(count + 1));
    put_n(&e->functions, operands, count);
}
uint32_t spv_emit_ext(SpvEmitter* e, uint32_t type, uint32_t instruction_number, const uint32_t* operands,
                      size_t count) {
    uint32_t import = spv_emit_glsl450_import(e), id = spv_emit_id(e);
    assert(count <= UINT16_MAX - 5);
    instruction(&e->functions, SPV_OP_EXT_INST, (uint32_t)(count + 5));
    put(&e->functions, type);
    put(&e->functions, id);
    put(&e->functions, import);
    put(&e->functions, instruction_number);
    put_n(&e->functions, operands, count);
    return id;
}

uint32_t* spv_emit_finish(const SpvEmitter* e, size_t* word_count) {
    const SpvWords* sections[] = {&e->capabilities, &e->extensions,  &e->imports, &e->entry_points, &e->execution_modes,
                                  &e->debug,        &e->decorations, &e->types,   &e->functions};
    size_t n = 8; /* header, memory model */
    for (size_t i = 0; i < sizeof sections / sizeof sections[0]; i++) {
        assert(sections[i]->count <= SIZE_MAX - n);
        n += sections[i]->count;
    }
    assert(n <= SIZE_MAX / sizeof(uint32_t));
    uint32_t* out = malloc(n * sizeof(uint32_t));
    if (!out) abort();
    size_t p = 0;
    out[p++] = 0x07230203; /* magic */
    out[p++] = e->version;
    out[p++] = 0; /* generator */
    out[p++] = e->next_id;
    out[p++] = 0; /* schema */
    if (e->capabilities.count) memcpy(out + p, e->capabilities.words, e->capabilities.count * sizeof(uint32_t));
    p += e->capabilities.count;
    if (e->extensions.count) memcpy(out + p, e->extensions.words, e->extensions.count * sizeof(uint32_t));
    p += e->extensions.count;
    if (e->imports.count) memcpy(out + p, e->imports.words, e->imports.count * sizeof(uint32_t));
    p += e->imports.count;
    out[p++] = (3u << 16) | SPV_OP_MEMORY_MODEL;
    out[p++] = 0; /* Logical */
    out[p++] = e->vulkan_memory_model ? 3 : 1; /* Vulkan, GLSL450 */
    for (size_t i = 3; i < sizeof sections / sizeof sections[0]; i++) {
        if (sections[i]->count) memcpy(out + p, sections[i]->words, sections[i]->count * sizeof(uint32_t));
        p += sections[i]->count;
    }
    assert(p == n);
    *word_count = n;
    return out;
}
