/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "nx/gpu/shader/shader.h"
#include "nx/gpu/vk/xfb_stores.h"
#include "common/utilities/endian.h"
#include "support/spv_eval.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCHED          0x001F8000FC0007E0ull
#define EXIT           0xE30000000007000Full
#define HANDLE_CBUF_R4 ((0x4c98078805a7001eull & ~0xffull) | 4ull)
#define F2I            0x5CB0118000070A00ull
#define R2P_R          0x5CF0000000000000ull
#define P2R_R          0x5CE8000000000000ull
#define FMUL_FTZ       0x5C68100000070000ull
#define FMUL_KEEP      0x5C68000000070000ull
#define FADD_KEEP      0x5C58000000070000ull
#define FMUL_IMM       0x3868003F00070000ull
#define FFMA_FMZ       0x59C0000000070000ull
#define FFMA_KEEP      0x5980000000070000ull

static int failures;
static const char* storage_paths[4];
static const char* storage_signed_paths[9];
static const char* storage_volume_atomic_paths[4];
static const char* storage_layer_load_paths[8];
static const char* nro_paths[4];
static const char* dc_color_path;
static const char* audit_paths[7];
static const char* issue126_paths[17];
static const char* stage_paths[3];
static const char* query_paths[2];
static const char* opcode_paths[3];
static const char* gradient_paths[3];
static const char* gradient_cube_path;
static const char* draw_buffer_paths[10];
static const char* texture_paths[6];
static const char* arithmetic_path;
static const char* beyond_paths[3];
static const char* attribute_paths[7];
static const char* pexit_path;
static const char* global_dynamic_path;
static const char* xfb_path;
static const char* xfb_stores_path;
static const char* passthrough_paths[6];
static const char* structured_paths[2];
static const char* fswzadd_path;
static const char* surface_op_paths[7];
static const char* legacy_paths[4];
static const char* rect_paths[3];
static const char* window_paths[3];
static const char* lone_lane_paths[9];
static struct {
    char** paths;
    size_t count, capacity;
    int failed;
} g_fixtures;

static const char* fixture_path(const char* directory, const char* name) {
    size_t dir_len = strlen(directory), name_len = strlen(name);
    if (name_len > SIZE_MAX - 6 || dir_len > SIZE_MAX - name_len - 6) {
        g_fixtures.failed = 1;
        return NULL;
    }
    if (g_fixtures.count == g_fixtures.capacity) {
        size_t capacity = g_fixtures.capacity ? g_fixtures.capacity * 2 : 128;
        if (g_fixtures.capacity > SIZE_MAX / 2 || capacity > SIZE_MAX / sizeof *g_fixtures.paths) {
            g_fixtures.failed = 1;
            return NULL;
        }
        char** paths = realloc(g_fixtures.paths, capacity * sizeof *paths);
        if (!paths) {
            g_fixtures.failed = 1;
            return NULL;
        }
        g_fixtures.paths = paths;
        g_fixtures.capacity = capacity;
    }
    char* path = malloc(dir_len + name_len + 6);
    if (!path) {
        g_fixtures.failed = 1;
        return NULL;
    }
    snprintf(path, dir_len + name_len + 6, "%s/%s.spv", directory, name);
    g_fixtures.paths[g_fixtures.count++] = path;
    return path;
}
static void check(int condition, const char* what) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", what);
        failures++;
    }
}

static void check_translated(const u32* code, const ShaderInfo* info, const char* what) {
    if (!code || info->unsupported) fprintf(stderr, "%s: %s\n", what, info->first_unsupported);
    check(code && !info->unsupported, what);
}

static void check_refused(const u32* code, const ShaderInfo* info, const char* reason, const char* what) {
    if (code || !info->unsupported || strncmp(info->first_unsupported, reason, strlen(reason)))
        fprintf(stderr, "%s: unexpected refusal reason '%s'\n", what, info->first_unsupported);
    check(!code && info->unsupported && !strncmp(info->first_unsupported, reason, strlen(reason)), what);
}

/* The programs here are built as 64-bit words in host numbers. The translator takes guest memory's bytes, where
   the words are little-endian, so every call gets a little-endian copy; the header's 32-bit words are halves of the
   first ten (word 2k the low half of program word k, as in guest memory). */
static void set_sph(u64* code, u32 index, u32 value) {
    u32 shift = index % 2 * 32;
    code[index / 2] = (code[index / 2] & ~(0xFFFFFFFFull << shift)) | (u64)value << shift;
}
static u8* guest_bytes(const u64* code, size_t bytes) {
    u8* guest = malloc(bytes ? bytes : 1);
    if (!guest) return NULL;
    for (size_t i = 0; i < bytes / 8; i++) snx_st_le64(guest + i * 8, code[i]);
    return guest;
}
/* The OpBitcasts of a module that give a 32-bit float back from its own bits: OpBitcast %float of an OpBitcast (to
   another type) of a value that is a float already. The translator keeps what a register holds as the float it was
   written as, so every program this file translates has none (spirv_of) */
static int float_round_trips(const u32* code, size_t words) {
    u32 f32 = 0;
    int found = 0;
    for (size_t i = 5; i < words && !f32;) {
        u32 width = code[i] >> 16;
        if (!width || i + width > words) return -1;
        if ((code[i] & 0xffff) == 22 && width == 3 && code[i + 2] == 32) f32 = code[i + 1];
        i += width;
    }
    for (size_t i = 5; f32 && i < words;) {
        u32 width = code[i] >> 16;
        if (!width || i + width > words) return -1;
        if ((code[i] & 0xffff) == 124 && width == 4 && code[i + 1] == f32)
            for (size_t j = 5; j < words;) {
                u32 inner = code[j] >> 16;
                if (!inner || j + inner > words) return -1;
                if ((code[j] & 0xffff) == 124 && inner == 4 && code[j + 2] == code[i + 3]) /* the bits it reads */
                    for (size_t k = 5; k < words;) {
                        u32 source = code[k] >> 16, op = code[k] & 0xffff;
                        if (!source || k + source > words) return -1;
                        /* what those bits were made from: a value of a function (after the types and constants,
                           whose instructions carry no result type at this place) or a constant, of type float */
                        if (source >= 3 && code[k + 2] == code[j + 3] && code[k + 1] == f32 &&
                            (op == 43 || (op >= 61 && op != 62 && op != 71 && op != 72)))
                            found++;
                        k += source;
                    }
                j += inner;
            }
        i += width;
    }
    return found;
}
/* The OpLoads of a whole Input variable in a module, of the variables decorated BuiltIn builtin (-1: those with no
   BuiltIn, the generic inputs; -2: every Input variable), and in *again those of a variable its block already
   loaded */
static int input_loads(const u32* code, size_t words, int builtin, int* again) {
    u32 inputs[96], loaded[96];
    int count = 0, in_block = 0, loads = 0;
    *again = 0;
    for (size_t i = 5; i < words;) {
        u32 width = code[i] >> 16, op = code[i] & 0xffff;
        if (!width || i + width > words) return -1;
        if (op == 59 && width >= 4 && code[i + 3] == 1 && count < 96) { /* OpVariable, Input */
            int decorated = -1;
            for (size_t k = 5; k < i; k += code[k] >> 16) /* OpDecorate variable BuiltIn */
                if ((code[k] & 0xffff) == 71 && code[k] >> 16 == 4 && code[k + 1] == code[i + 2] && code[k + 2] == 11)
                    decorated = (int)code[k + 3];
            if (builtin == -2 || builtin == decorated) inputs[count++] = code[i + 2];
        }
        if (op == 248) in_block = 0; /* OpLabel */
        if (op == 61 && width >= 4) {
            int input = 0, seen = 0;
            for (int k = 0; k < count; k++) input |= inputs[k] == code[i + 3];
            for (int k = 0; k < in_block; k++) seen |= loaded[k] == code[i + 3];
            loads += input;
            *again += input && seen;
            if (input && !seen && in_block < 96) loaded[in_block++] = code[i + 3];
        }
        i += width;
    }
    return loads;
}
static u32* spirv_of(const u64* code, size_t bytes, int stage, ShaderInfo* info, size_t* words, int flush,
                     int can_preserve, int can_preserve_f64, int can_subgroup) {
    u8* guest = guest_bytes(code, bytes);
    u32* result = guest ? shader_to_spirv(guest, (u32)bytes, stage, info, words, flush, can_preserve, can_preserve_f64,
                                          can_subgroup)
                        : NULL;
    free(guest);
    if (result) {
        int trips = float_round_trips(result, *words);
        if (trips) fprintf(stderr, "a module of %zu words has %d float round trips through its bits\n", *words, trips);
        check(!trips, "no float is cast to its bits and back");
        int again = 0;
        input_loads(result, *words, -2, &again);
        if (again)
            fprintf(stderr, "a module of %zu words loads an Input variable its block has loaded %d times\n", *words,
                    again);
        check(!again, "an Input variable is loaded once a block");
    }
    return result;
}
static u32* compute_spirv_of(const u64* code, size_t bytes, ShaderInfo* info, size_t* words, int flush,
                             int can_preserve) {
    u8* guest = guest_bytes(code, bytes);
    u32* result = guest ? shader_compute_to_spirv(guest, (u32)bytes, info, words, flush, can_preserve) : NULL;
    free(guest);
    return result;
}
static void hash_of(const u64* code, size_t bytes, u32* len) {
    u8* guest = guest_bytes(code, bytes);
    if (guest) shader_hash(guest, (u32)bytes, len);
    free(guest);
}

static u64* guest_program(const u64* instructions, int count, const u32* sph, size_t* slots_out) {
    if (count < 0 || count > (INT_MAX - 14) / 2) return NULL;
    size_t slots = 10 + (size_t)count + 1 + ((size_t)count + 3) / 3;
    u64* code = calloc(slots, sizeof *code);
    if (!code) return NULL;
    for (u32 i = 0; sph && i < 3; i++) set_sph(code, 2 + i, sph[i]);
    int at = 10;
    for (int i = 0; i <= count; i++) {
        if ((at - 10) % 4 == 0) code[at++] = SCHED;
        code[at++] = i < count ? instructions[i] : EXIT;
    }
    *slots_out = slots;
    return code;
}

static u32* translate_with_rt_masks(const u64* instructions, int count, int stage, ShaderInfo* info, size_t* words,
                                    int flush, u32 rt_mask, u32 rt_signed) {
    size_t slots;
    u64* code = guest_program(instructions, count, NULL, &slots);
    if (!code) return NULL;
    memset(info, 0, sizeof *info);
    info->rt_int_mask = rt_mask;
    info->rt_sint_mask = rt_signed;
    info->img_int_mask = rt_mask >> 8;
    info->img_sint_mask = rt_signed >> 8;
    if (stage == ST_VS) info->clip_enable = rt_mask;
    if (stage == ST_CS) {
        info->cs_local[0] = info->cs_local[1] = info->cs_local[2] = 1;
        info->shared_size = 16;
    }
    u32* result = spirv_of(code, slots * sizeof *code, stage, info, words, flush, 1, 1, 1);
    free(code);
    return result;
}
static u32* translate_with_rt_mask(const u64* instructions, int count, int stage, ShaderInfo* info, size_t* words,
                                   int flush, u32 rt_mask) {
    return translate_with_rt_masks(instructions, count, stage, info, words, flush, rt_mask, 0);
}
static u32* translate(const u64* instructions, int count, int stage, ShaderInfo* info, size_t* words, int flush) {
    return translate_with_rt_mask(instructions, count, stage, info, words, flush, 0);
}
static u32* translate_stage(const u64* instructions, int count, int stage, const u32 sph[3], u32 primitive,
                            ShaderInfo* info, size_t* words);

static int count_opcode(const u32* code, size_t words, u32 opcode, u32 operand) {
    int found = 0;
    for (size_t i = 5; i < words;) {
        u32 width = code[i] >> 16, op = code[i] & 0xffff;
        if (!width || i + width > words) {
            check(0, "malformed SPIR-V instruction while counting opcodes");
            return -1;
        }
        u32 value_index = op == 17 ? 1 : 2;
        if (op == opcode && (operand == 0xffffffffu || (width > value_index && code[i + value_index] == operand)))
            found++;
        i += width;
    }
    return found;
}

/* Every boolean OpLoad reads a variable whose pointer type points to bool. A predicate or flag whose variable the
   module leaves out is loaded through id 0 (the flags form of R2P and P2R used to take the flag variables' ids as
   their indexes), and radv crashed on that module while making the pipeline. */
static int bool_loads_read_variables(const u32* code, size_t words) {
    u32 bool_type = 0, pointer_types[8], variables[64];
    int pointer_count = 0, variable_count = 0;
    for (size_t i = 5; i < words;) {
        u32 width = code[i] >> 16, op = code[i] & 0xffff;
        if (!width || i + width > words) return 0;
        if (op == 20 && width == 2) bool_type = code[i + 1]; /* OpTypeBool */
        if (op == 32 && width == 4 && pointer_count < 8 && code[i + 3] == bool_type)
            pointer_types[pointer_count++] = code[i + 1]; /* OpTypePointer to bool */
        i += width;
    }
    if (!bool_type || !pointer_count) return 0;
    for (size_t i = 5; i < words;) {
        u32 width = code[i] >> 16, op = code[i] & 0xffff;
        if (!width || i + width > words) return 0;
        if (op == 59 && width >= 4 && variable_count < 64) { /* OpVariable, with an initializer or not */
            for (int k = 0; k < pointer_count; k++)
                if (code[i + 1] == pointer_types[k]) variables[variable_count++] = code[i + 2];
        }
        i += width;
    }
    int loads = 0;
    for (size_t i = 5; i < words;) {
        u32 width = code[i] >> 16, op = code[i] & 0xffff;
        if (!width || i + width > words) return 0;
        if (op == 61 && width >= 4 && code[i + 1] == bool_type) { /* OpLoad, of bool */
            int found = 0;
            for (int k = 0; k < variable_count; k++) found |= variables[k] == code[i + 3];
            if (!found) return 0;
            loads++;
        }
        i += width;
    }
    return loads > 0;
}

/* whether every OpExtInst names an instruction set the module imports: one naming id 0 crashed NVIDIA's SPIR-V front
   end (615.78) */
static int ext_sets_imported(const u32* code, size_t words) {
    u32 imported[4];
    int n = 0;
    for (size_t i = 5; i < words;) {
        u32 width = code[i] >> 16;
        if (!width || i + width > words) return 0;
        if ((code[i] & 0xffff) == 11 && n < 4) imported[n++] = code[i + 1];
        if ((code[i] & 0xffff) == 12 && width > 3) {
            int found = 0;
            for (int k = 0; k < n; k++) found |= imported[k] == code[i + 3];
            if (!found) return 0;
        }
        i += width;
    }
    return 1;
}

/* an OpTypeImage of that Dim read and written without a sampler (Sampled 2): a storage image */
static int has_storage_image_dim(const u32* code, size_t words, u32 dim) {
    for (size_t i = 5; i < words;) {
        u32 width = code[i] >> 16;
        if (!width || i + width > words) return 0;
        if ((code[i] & 0xffff) == 25 && width > 7 && code[i + 3] == dim && code[i + 7] == 2) return 1;
        i += width;
    }
    return 0;
}

/* Formatted stores use the descriptor's format; atomics still require R32ui. */
static int has_storage_image_format(const u32* code, size_t words, u32 format) {
    for (size_t i = 5; i < words;) {
        u32 width = code[i] >> 16;
        if (!width || i + width > words) return 0;
        if ((code[i] & 0xffff) == 25 && width > 8 && code[i + 7] == 2 && code[i + 8] == format) return 1;
        i += width;
    }
    return 0;
}

static int has_image_operands(const u32* code, size_t words, u32 opcode, u32 mask) {
    u32 at = opcode == 87 || opcode == 88 || opcode == 95 ? 5 : 6;
    for (size_t i = 5; i < words;) {
        u32 width = code[i] >> 16;
        if (!width || i + width > words) return 0;
        if ((code[i] & 0xffff) == opcode && width > at && code[i + at] == mask) return 1;
        i += width;
    }
    return 0;
}

static int has_conditional_emit(const u32* code, size_t words) {
    u32 label = 0, emit_label = 0;
    for (size_t i = 5; i < words;) {
        u32 width = code[i] >> 16;
        if (!width || i + width > words) return 0;
        if ((code[i] & 0xffff) == 248 && width == 2) label = code[i + 1];
        if ((code[i] & 0xffff) == 218) emit_label = label;
        i += width;
    }
    if (!emit_label) return 0;
    for (size_t i = 5; i < words;) {
        u32 width = code[i] >> 16;
        if (!width || i + width > words) return 0;
        if ((code[i] & 0xffff) == 250 && width == 4 && (code[i + 2] == emit_label || code[i + 3] == emit_label))
            return 1;
        i += width;
    }
    return 0;
}

static int is_constant_value(const u32* code, size_t words, u32 id, u32 value);

static int emit_only_on_zero_stream(const u32* code, size_t words) {
    u32 found = 0;
    for (size_t i = 5; i < words;) {
        u32 width = code[i] >> 16;
        if (!width || i + width > words) return 0;
        if ((code[i] & 0xffff) == 218) {
            int selected = 0;
            for (size_t branch = 5; branch < words;) {
                u32 length = code[branch] >> 16;
                if (!length || branch + length > words) return 0;
                if ((code[branch] & 0xffff) == 250 && length == 4) {
                    u32 condition = code[branch + 1];
                    int zero_test = 0;
                    size_t body = words, merge = 0;
                    for (size_t test = 5; test < words;) {
                        u32 span = code[test] >> 16;
                        if (!span || test + span > words) return 0;
                        if ((code[test] & 0xffff) == 170 && span == 5 && code[test + 2] == condition &&
                            is_constant_value(code, words, code[test + 4], 0))
                            zero_test = 1;
                        if ((code[test] & 0xffff) == 248 && span == 2) {
                            if (code[test + 1] == code[branch + 2]) body = test;
                            if (code[test + 1] == code[branch + 3]) merge = test;
                        }
                        test += span;
                    }
                    /* Position fixup can insert its own selection inside this block. The vertex must remain
                       inside the enclosing stream-zero selection, before its false/merge target. */
                    if (zero_test && body < i && i < merge) selected = 1;
                }
                branch += length;
            }
            if (!selected) return 0;
            found++;
        }
        i += width;
    }
    return found != 0;
}

static int shift_uses_amount(const u32* code, size_t words, u32 amount) {
    for (size_t i = 5; i < words;) {
        u32 width = code[i] >> 16;
        if (!width || i + width > words) return 0;
        if ((code[i] & 0xffff) == 196 && width == 5 && is_constant_value(code, words, code[i + 4], amount)) return 1;
        i += width;
    }
    return 0;
}

static int count_shifts_by(const u32* code, size_t words, u32 amount) {
    int count = 0;
    for (size_t i = 5; i < words;) {
        u32 width = code[i] >> 16;
        if (!width || i + width > words) return -1;
        if ((code[i] & 0xffff) == 196 && width == 5 && is_constant_value(code, words, code[i + 4], amount)) count++;
        i += width;
    }
    return count;
}

static int has_zero_low_double_word(const u32* code, size_t words) {
    for (size_t i = 5; i < words;) {
        u32 width = code[i] >> 16;
        if (!width || i + width > words) return 0;
        if ((code[i] & 0xffff) == 80 && width == 5 && is_constant_value(code, words, code[i + 3], 0)) return 1;
        i += width;
    }
    return 0;
}

static int sprite_t_selects_flipped_on_true(const u32* code, size_t words) {
    u32 negated = 0, flipped = 0;
    for (size_t i = 5; i < words;) {
        u32 width = code[i] >> 16;
        if (!width || i + width > words) return 0;
        if ((code[i] & 0xffff) == 127 && width == 4) negated = code[i + 2];
        if ((code[i] & 0xffff) == 129 && width == 5 && negated &&
            ((code[i + 3] == negated && is_constant_value(code, words, code[i + 4], 0x3f800000)) ||
             (code[i + 4] == negated && is_constant_value(code, words, code[i + 3], 0x3f800000))))
            flipped = code[i + 2];
        if ((code[i] & 0xffff) == 169 && width == 6 && flipped && code[i + 4] == flipped && code[i + 5] != flipped)
            return 1;
        i += width;
    }
    return 0;
}

/* the fragment's y read as the pushed clip height less it (FNegate, then FAdd with a ConvertUToF) when the pushed
   lower-left word isn't zero (INotEqual feeding the Select whose true side that is), as it is otherwise */
static int frag_y_selects_clip_less_y(const u32* code, size_t words) {
    u32 negated = 0, height = 0, up = 0, lower_left = 0;
    for (size_t i = 5; i < words;) {
        u32 width = code[i] >> 16;
        if (!width || i + width > words) return 0;
        u32 op = code[i] & 0xffff;
        if (op == 127 && width == 4) negated = code[i + 2];
        if (op == 112 && width == 4) height = code[i + 2];
        if (op == 129 && width == 5 && negated && height &&
            ((code[i + 3] == height && code[i + 4] == negated) || (code[i + 3] == negated && code[i + 4] == height)))
            up = code[i + 2];
        if (op == 171 && width == 5) lower_left = code[i + 2];
        if (op == 169 && width == 6 && up && lower_left && code[i + 3] == lower_left && code[i + 4] == up &&
            code[i + 5] != up)
            return 1;
        i += width;
    }
    return 0;
}

static int clock_pair_extracts_both_words(const u32* code, size_t words) {
    u32 clock = 0;
    for (size_t i = 5; i < words;) {
        u32 width = code[i] >> 16;
        if (!width || i + width > words) return 0;
        if ((code[i] & 0xffff) == 5056 && width == 4) clock = code[i + 2];
        i += width;
    }
    if (!clock) return 0;
    int low = 0, high = 0;
    for (size_t i = 5; i < words;) {
        u32 width = code[i] >> 16;
        if (!width || i + width > words) return 0;
        if ((code[i] & 0xffff) == 81 && width == 5 && code[i + 3] == clock) {
            low |= code[i + 4] == 0;
            high |= code[i + 4] == 1;
        }
        i += width;
    }
    return low && high;
}

static int vset_selects_comparison(const u32* code, size_t words, int greater) {
    u32 comparisons[2] = {0};
    u32 operands[2][2] = {{0}};
    int count = 0;
    for (size_t i = 5; i < words;) {
        u32 width = code[i] >> 16;
        if (!width || i + width > words) return 0;
        if ((code[i] & 0xffff) == 173 && width == 5 && count < 2) {
            comparisons[count] = code[i + 2];
            operands[count][0] = code[i + 3];
            operands[count][1] = code[i + 4];
            count++;
        }
        i += width;
    }
    if (count != 2 || operands[0][0] != operands[1][1] || operands[0][1] != operands[1][0]) return 0;
    for (size_t i = 5; i < words;) {
        u32 width = code[i] >> 16;
        if (!width || i + width > words) return 0;
        if ((code[i] & 0xffff) == 169 && width == 6 && code[i + 3] == comparisons[greater ? 1 : 0]) return 1;
        i += width;
    }
    return 0;
}

static int is_constant_value(const u32* code, size_t words, u32 id, u32 value) {
    for (size_t i = 5; i < words;) {
        u32 width = code[i] >> 16;
        if (!width || i + width > words) return 0;
        if ((code[i] & 0xffff) == 43 && width == 4 && code[i + 2] == id && code[i + 3] == value) return 1;
        i += width;
    }
    return 0;
}

static int has_constant64(const u32* code, size_t words, u64 value) {
    for (size_t i = 5; i < words;) {
        u32 width = code[i] >> 16;
        if (!width || i + width > words) return 0;
        if ((code[i] & 0xffff) == 43 && width == 5 && (((u64)code[i + 4] << 32) | code[i + 3]) == value) return 1;
        i += width;
    }
    return 0;
}

static int has_glsl_instruction(const u32* code, size_t words, u32 instruction) {
    for (size_t i = 5; i < words;) {
        u32 width = code[i] >> 16;
        if (!width || i + width > words) return 0;
        if ((code[i] & 0xffff) == 12 && width > 4 && code[i + 4] == instruction) return 1;
        i += width;
    }
    return 0;
}

static int has_insert_range(const u32* code, size_t words, u32 offset, u32 width_bits) {
    for (size_t i = 5; i < words;) {
        u32 width = code[i] >> 16;
        if (!width || i + width > words) return 0;
        if ((code[i] & 0xffff) == 201 && width == 7 && is_constant_value(code, words, code[i + 5], offset) &&
            is_constant_value(code, words, code[i + 6], width_bits))
            return 1;
        i += width;
    }
    return 0;
}

static int switch_selects_current_pc(const u32* code, size_t words) {
    u32 selector = 0;
    for (size_t i = 5; i < words;) {
        u32 width = code[i] >> 16;
        if (!width || i + width > words) return 0;
        if ((code[i] & 0xffff) == 251 && width >= 3) selector = code[i + 1];
        i += width;
    }
    if (!selector) return 0;
    for (size_t i = 5; i < words;) {
        u32 width = code[i] >> 16;
        if (!width || i + width > words) return 0;
        if ((code[i] & 0xffff) == 169 && width == 6 && code[i + 2] == selector &&
            is_constant_value(code, words, code[i + 5], UINT32_MAX - 1)) {
            u32 condition = code[i + 3], current_pc = code[i + 4];
            for (size_t j = 5; j < words;) {
                u32 span = code[j] >> 16;
                if (!span || j + span > words) return 0;
                if ((code[j] & 0xffff) == 170 && span == 5 && code[j + 2] == condition &&
                    (code[j + 3] == current_pc || code[j + 4] == current_pc))
                    return 1;
                j += span;
            }
        }
        i += width;
    }
    return 0;
}

/* The values a register variable carries: what is loaded from it, and what is stored to it (the translator reuses a
   value stored or loaded earlier in the same block in place of a load), up to max */
static size_t register_values(const u32* code, size_t words, u32 pointer, u32* values, size_t max) {
    size_t n = 0;
    for (size_t i = 5; i < words && n < max;) {
        u32 width = code[i] >> 16;
        if (!width || i + width > words) return n;
        if ((code[i] & 0xffff) == 61 && width == 4 && code[i + 3] == pointer) values[n++] = code[i + 2];
        if ((code[i] & 0xffff) == 62 && width == 3 && code[i + 1] == pointer) values[n++] = code[i + 2];
        i += width;
    }
    return n;
}

/* the values a register seeded with a constant takes: the loads and stores of the variable the constant is stored
   to, or the constant itself, which a read in the block of its write takes with no store or load between */
static size_t seeded_values(const u32* code, size_t words, u32 seed, u32* values, size_t max) {
    u32 pointer = 0;
    size_t n = 0;
    for (size_t i = 5; i < words;) {
        u32 width = code[i] >> 16;
        if (!width || i + width > words) return 0;
        if ((code[i] & 0xffff) == 62 && width == 3 && is_constant_value(code, words, code[i + 2], seed))
            pointer = code[i + 1];
        if ((code[i] & 0xffff) == 43 && width == 4 && code[i + 3] == seed && n < max) values[n++] = code[i + 2];
        i += width;
    }
    return n + (pointer ? register_values(code, words, pointer, values + n, max - n) : 0);
}

static int prmt_selector_uses_seeded_register(const u32* code, size_t words) {
    u32 values[64];
    size_t nvalues = seeded_values(code, words, 0x1234, values, 64);
    for (size_t v = 0; v < nvalues; v++) {
        u32 loaded = values[v];
        for (size_t j = 5; j < words;) {
            u32 w = code[j] >> 16;
            if (!w || j + w > words) return 0;
            if ((code[j] & 0xffff) == 199 && w == 5 &&
                ((code[j + 3] == loaded && is_constant_value(code, words, code[j + 4], 3)) ||
                 (code[j + 4] == loaded && is_constant_value(code, words, code[j + 3], 3))))
                return 1;
            j += w;
        }
    }
    return 0;
}

static int seeded_register_feeds(const u32* code, size_t words, u32 seed, u32 opcode, u32 operand_index) {
    u32 values[64];
    size_t nvalues = seeded_values(code, words, seed, values, 64);
    for (size_t v = 0; v < nvalues; v++) {
        u32 loaded = values[v];
        for (size_t j = 5; j < words;) {
            u32 span = code[j] >> 16;
            if (!span || j + span > words) return 0;
            u32 value = loaded;
            if ((code[j] & 0xffff) == 124 && span == 4 && code[j + 3] == loaded) value = code[j + 2];
            for (size_t k = 5; k < words;) {
                u32 target_span = code[k] >> 16;
                if (!target_span || k + target_span > words) return 0;
                if ((code[k] & 0xffff) == opcode && target_span > operand_index && code[k + operand_index] == value)
                    return 1;
                k += target_span;
            }
            j += span;
        }
    }
    return 0;
}

/* whether value is component of a vector that an explicit-gradient sample takes as its dx (gradient 0) or dy (1) */
static int gradient_takes(const u32* code, size_t words, u32 value, u32 gradient, u32 component) {
    for (size_t k = 5; k < words;) {
        u32 construct_span = code[k] >> 16;
        if (!construct_span || k + construct_span > words) return 0;
        if ((code[k] & 0xffff) == 80 && construct_span > 3 + component && code[k + 3 + component] == value) {
            u32 vector = code[k + 2];
            for (size_t m = 5; m < words;) {
                u32 sample_span = code[m] >> 16;
                if (!sample_span || m + sample_span > words) return 0;
                if ((code[m] & 0xffff) == 88 && sample_span > 6 + gradient && (code[m + 5] & ~8u) == 4 &&
                    code[m + 6 + gradient] == vector)
                    return 1;
                m += sample_span;
            }
        }
        k += construct_span;
    }
    return 0;
}
/* a register seeded with a constant is that gradient component: as its bits' bitcast, or as the float constant of
   those bits, which a read in the block of its write takes */
static int seeded_gradient_component(const u32* code, size_t words, u32 seed, u32 gradient, u32 component) {
    u32 values[64];
    size_t nvalues = seeded_values(code, words, seed, values, 64);
    for (size_t v = 0; v < nvalues; v++) {
        u32 loaded = values[v];
        if (gradient_takes(code, words, loaded, gradient, component)) return 1;
        for (size_t j = 5; j < words;) {
            u32 span = code[j] >> 16;
            if (!span || j + span > words) return 0;
            if ((code[j] & 0xffff) == 124 && span == 4 && code[j + 3] == loaded &&
                gradient_takes(code, words, code[j + 2], gradient, component))
                return 1;
            j += span;
        }
    }
    return 0;
}

static int count_builtin(const u32* code, size_t words, u32 builtin) {
    int found = 0;
    for (size_t i = 5; i < words;) {
        u32 width = code[i] >> 16;
        if (!width || i + width > words) return -1;
        if ((code[i] & 0xffff) == 71 && width == 4 && code[i + 2] == 11 && code[i + 3] == builtin) found++;
        i += width;
    }
    return found;
}

static int has_execution_mode_width(const u32* code, size_t words, u32 mode, u32 width_bits) {
    for (size_t i = 5; i < words;) {
        u32 width = code[i] >> 16;
        if (!width || i + width > words) return 0;
        if ((code[i] & 0xffff) == 16 && width == 4 && code[i + 2] == mode && code[i + 3] == width_bits) return 1;
        i += width;
    }
    return 0;
}

static int memory_barrier_scope(const u32* code, size_t words) {
    u32 scope_id = 0;
    for (size_t i = 5; i < words;) {
        u32 width = code[i] >> 16;
        if (!width || i + width > words) return -1;
        if ((code[i] & 0xffff) == 225 && width == 3) scope_id = code[i + 1];
        i += width;
    }
    if (!scope_id) return -1;
    for (size_t i = 5; i < words;) {
        u32 width = code[i] >> 16;
        if (!width || i + width > words) return -1;
        if ((code[i] & 0xffff) == 43 && width == 4 && code[i + 2] == scope_id) return (int)code[i + 3];
        i += width;
    }
    return -1;
}

static void save_fixture(const char* path, const u32* code, size_t words) {
    if (!path || !code) return;
    FILE* out = fopen(path, "wb");
    if (!out) {
        check(0, "shader fixture opened");
        return;
    }
    int written = 1; /* a .spv file is little-endian, the same bytes from any host */
    for (size_t i = 0; i < words && written; i++) {
        u8 le[4];
        snx_st_le32(le, code[i]);
        written = fwrite(le, 1, sizeof le, out) == sizeof le;
    }
    int closed = fclose(out) == 0;
    check(written && closed, "shader fixture written");
}

static int has_glsl_abs(const u32* code, size_t words) {
    for (size_t i = 5; i < words;) {
        u32 width = code[i] >> 16;
        if (!width || i + width > words) return 0;
        if ((code[i] & 0xffff) == 12 && width >= 6 && code[i + 4] == 4) return 1; /* GLSL.std.450 FAbs */
        i += width;
    }
    return 0;
}

static int abs_feeds_negate(const u32* code, size_t words) {
    u32 abs_id = 0;
    for (size_t i = 5; i < words;) {
        u32 width = code[i] >> 16;
        if (!width || i + width > words) return 0;
        if ((code[i] & 0xffff) == 12 && width >= 6 && code[i + 4] == 4) abs_id = code[i + 2];
        if ((code[i] & 0xffff) == 127 && width >= 4 && code[i + 3] == abs_id && abs_id) return 1;
        i += width;
    }
    return 0;
}

static void f2i_modifiers(void) {
    const u64 cases[] = {F2I | (1ull << 45), F2I | (1ull << 49), F2I | (1ull << 45) | (1ull << 49)};
    for (int i = 0; i < 3; i++) {
        ShaderInfo info;
        size_t words = 0;
        u32* code = translate(&cases[i], 1, ST_FS, &info, &words, 1);
        check_translated(code, &info, "F2I modifier translates");
        if (code) {
            check((count_opcode(code, words, 127, 0xffffffffu) > 0) == (i != 1), "F2I bit 45 negates");
            check(has_glsl_abs(code, words) == (i != 0), "F2I bit 49 takes absolute value");
            if (i == 2) check(abs_feeds_negate(code, words), "F2I applies absolute value before negation");
        }
        free(code);
    }
}

/* the instruction in a function whose result is id: its offset in code, 0 for none. A value is made before an
   instruction without a result names it in the same word (OpStore's object), as blocks come in dominance order */
static size_t defined_at(const u32* code, size_t words, u32 id) {
    int in_function = 0;
    for (size_t i = 5; i < words;) {
        u32 width = code[i] >> 16;
        if (!width || i + width > words) return 0;
        in_function |= (code[i] & 0xffff) == 54; /* OpFunction */
        if (in_function && width >= 3 && code[i + 2] == id) return i;
        i += width;
    }
    return 0;
}
/* the module's OpConvertSToF and OpConvertUToF: in halves, those whose operand is an OpBitwiseAnd with 0xffff0000 or
   0xffff (through an OpBitcast or not), a value with 16 significant bits at most, which every driver converts exactly;
   in whole, the others */
static void count_conversions(const u32* code, size_t words, int* halves, int* whole) {
    *halves = *whole = 0;
    for (size_t i = 5; i < words;) {
        u32 width = code[i] >> 16, op = code[i] & 0xffff;
        if (!width || i + width > words) return;
        if ((op == 111 || op == 112) && width == 4) {
            size_t at = defined_at(code, words, code[i + 3]);
            if (at && (code[at] & 0xffff) == 124 /* OpBitcast */) at = defined_at(code, words, code[at + 3]);
            int masked = at && (code[at] & 0xffff) == 199 /* OpBitwiseAnd */ && (code[at] >> 16) == 5 &&
                         (is_constant_value(code, words, code[at + 4], 0xffff0000u) ||
                          is_constant_value(code, words, code[at + 4], 0xffffu));
            ++*(masked ? halves : whole);
        }
        i += width;
    }
}

/* I2F.F32 of a 32-bit integer where the driver's conversions truncate (g_shader_i2f_nearest, a Turnip on an Adreno
   610): no conversion is left a value with more than 16 significant bits, so the only rounding is the OpFAdd's, to
   nearest even as the console's. A 16-bit integer converts exactly, and as it did */
static void i2f_by_halves(void) {
    int was = g_shader_i2f_nearest;
    for (u32 size = 1; size < 3; size++)
        for (u32 is_signed = 0; is_signed < 2; is_signed++)
            for (int nearest = 0; nearest < 2; nearest++) {
                g_shader_i2f_nearest = nearest ? 3 : 0;
                /* I2F.F32.{U,S}{16,32} R9, R9 */
                const u64 program[] = {(0x5cb8000000970209ull | (u64)size << 10 | (u64)is_signed << 13)};
                ShaderInfo info;
                size_t words = 0;
                u32* code = translate(program, 1, ST_FS, &info, &words, 1);
                char what[96];
                snprintf(what, sizeof what, "I2F.F32.%c%u with i2f_nearest %d translates", is_signed ? 'S' : 'U',
                         8u << size, g_shader_i2f_nearest);
                check_translated(code, &info, what);
                if (!code) continue;
                int halves, whole;
                count_conversions(code, words, &halves, &whole);
                int split = size == 2 && nearest;
                if (halves != (split ? 2 : 0) || whole != (split ? 0 : 1))
                    fprintf(stderr, "I2F.F32.%c%u i2f_nearest %d: %d conversions of halves, %d of a whole value\n",
                            is_signed ? 'S' : 'U', 8u << size, g_shader_i2f_nearest, halves, whole);
                snprintf(what, sizeof what, "I2F.F32.%c%u with i2f_nearest %d converts %s", is_signed ? 'S' : 'U',
                         8u << size, g_shader_i2f_nearest, split ? "two halves" : "the whole value");
                check(halves == (split ? 2 : 0) && whole == (split ? 0 : 1), what);
                if (split) check(count_opcode(code, words, 129, 0xffffffffu) == 1, "one OpFAdd sums the halves");
                free(code);
            }
    g_shader_i2f_nearest = was;
}

static u64 mov32i(u32 reg, u32 value) { return 0x0100000000000000ull | ((u64)value << 20) | 0x71000ull | reg; }

/* R2P and P2R transfer the condition flags too (bit 40): the flags are variables of their own (z, s, c, o), read
   and written by their VAR_FLAG + p index, not by their variable id. A program that doesn't also use registers at
   those ids left R2P loading id 0, invalid SPIR-V that radv crashed on while making the pipeline. */
static void flag_transfers(void) {
    const u64 program[] = {
        mov32i(5, 15),                                         /* r5 = 15: the flags' bits */
        mov32i(6, 15),                                         /* r6 = 15: the mask */
        R2P_R | (1ull << 40) | (5ull << 8) | (6ull << 20),     /* R2P: z,s,c,o = r5 & r6 */
        P2R_R | (1ull << 40) | 7 | (5ull << 8) | (6ull << 20), /* P2R: r7 = r5 with the flags */
    };
    ShaderInfo info;
    size_t words = 0;
    u32* code = translate(program, 4, ST_FS, &info, &words, 1);
    check_translated(code, &info, "R2P and P2R's flags translate");
    if (code) check(bool_loads_read_variables(code, words), "the flags' transfers load their variables");
    free(code);
}

static void global_buffers(void) {
    static const struct {
        u64 instructions[3];
        int count;
        u32 offset;
        int write;
    } cases[] = {
        {{0xEF9500007307FF00ull, 0xEED4200000070000ull}, 2, 0x730, 0},
        {{0x4C1080001CC70100ull, 0x4C1008001CD7FF01ull, 0xEEDC200000070002ull}, 3, 0x730, 1},
        {{0x4C98078014470002ull, 0xEED4200000070200ull}, 2, 0x510, 0},
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        ShaderInfo info;
        size_t words = 0;
        u32* code = translate(cases[i].instructions, cases[i].count, ST_FS, &info, &words, 1);
        if (!code || info.unsupported || info.ngbuf != 1 || info.gbuf_cbuf_off[0] != cases[i].offset ||
            !!(info.gbuf_wmask & 1) != cases[i].write)
            fprintf(stderr, "global case %zu: %s, buffers %d, offset %x, writes %x\n", i, info.first_unsupported,
                    info.ngbuf, info.gbuf_cbuf_off[0], info.gbuf_wmask);
        check(code && !info.unsupported && info.ngbuf == 1 && info.gbuf_cbuf_off[0] == cases[i].offset &&
                  !!(info.gbuf_wmask & 1) == cases[i].write,
              "global buffer found at first use");
        free(code);
    }
    const u64 unused[] = {0x4C98078018870002ull};
    ShaderInfo info;
    size_t words = 0;
    u32* code = translate(unused, 1, ST_FS, &info, &words, 1);
    check(code && !info.unsupported && info.ngbuf == 0, "unused constant is not a global buffer");
    free(code);
    /* deko3d's vertex stage keeps its storage buffers' descriptors from c[0][0xb0], each address followed by its size,
       below NVN's 0x110: a vertex program uam compiles to read word 32 of the first one. It wasn't translated */
    const u64 deko_vertex[] = {
        0xEF9500000B07FF00ull, /* LDC.64 R0, c[0x0][0xb0] */
        0xEED4200008070000ull, /* LDG.E R0, [R0+0x80] */
    };
    code = translate(deko_vertex, 2, ST_VS, &info, &words, 1);
    if (!code || info.unsupported) fprintf(stderr, "deko3d vertex storage read: %s\n", info.first_unsupported);
    check(code && !info.unsupported && info.ngbuf == 1 && info.gbuf_cbuf_off[0] == 0xb0 && (info.gbuf_rmask & 1) &&
              shader_gbuf_sized(ST_VS, 0xb0) && shader_gbuf_sized(ST_VS, 0x100) && !shader_gbuf_sized(ST_FS, 0xb0) &&
              !shader_gbuf_sized(ST_VS, 0xa0),
          "a deko3d vertex program reads its storage buffer past 16 bytes, its size beside the address");
    free(code);
    /* an LDG whose base comes from arithmetic the tracking doesn't follow names that instruction (issue #440) */
    const u64 untracked[] = {FMUL_KEEP | 2, 0xEED4200000070200ull};
    code = translate(untracked, 2, ST_FS, &info, &words, 1);
    check_refused(code, &info, "global memory base R2 not tracked at 60: FMUL (reg) at 58",
                  "an untracked global base names its last writer");
    free(code);
    code = translate(untracked + 1, 1, ST_FS, &info, &words, 1);
    check_refused(code, &info, "global memory base R2 not tracked at 58: no write before",
                  "an untracked global base without a writer says so");
    free(code);
    /* an index plus a bare address in a constant buffer of the program's own, with no NVN descriptor behind it: one
       game's terrain vertex programs read their patch data so, and weren't translated (their draws all skipped) */
    const u64 bare[] = {
        0x4C10801800070A0Aull, /* IADD.CC R10, R10, c[6][0x0] */
        0x4C1008180017FF0Bull, /* IADD.X R11, RZ, c[6][0x4] */
        0xEED4A00000070A0Bull, /* LDG.E R11, [R10] */
    };
    code = translate(bare, 3, ST_FS, &info, &words, 1);
    check(code && !info.unsupported && info.ngbuf == 1 && info.gbuf_cbuf_off[0] == (6u << 16) &&
              SH_GBUF_BANK(info.gbuf_cbuf_off[0]) == 6 && SH_GBUF_OFFSET(info.gbuf_cbuf_off[0]) == 0 &&
              info.cbuf_max[6] >= 16,
          "a bare address in a program's own constant buffer is a global buffer");
    free(code);
    /* an offset from one constant buffer, then the address from another: the address, added last, is the base */
    const u64 offset_then_bare[] = {
        0x4C1801C000470200ull, /* ISCADD R0, R2, c[16][0x10], 3 */
        0x4C10801800070000ull, /* IADD.CC R0, R0, c[6][0x0] */
        0x4C1008180017FF01ull, /* IADD.X R1, RZ, c[6][0x4] */
        0xEED5A00000070000ull, /* LDG.E.64 R0, [R0] */
    };
    code = translate(offset_then_bare, 4, ST_FS, &info, &words, 1);
    check(code && !info.unsupported && info.ngbuf == 1 && info.gbuf_cbuf_off[0] == (6u << 16),
          "of an offset and a bare address, the one added last is the base");
    free(code);
    /* LEA.LO R2, R0, c[0][0x510], 4: the base plus an index, as a 64-bit address's low word (Pokemon Legends Z-A) */
    const u64 lea[] = {0x4BD7020014470002ull, 0xEED4200000070200ull};
    code = translate(lea, 2, ST_FS, &info, &words, 1);
    check(code && !info.unsupported && info.ngbuf == 1 && info.gbuf_cbuf_off[0] == 0x510,
          "an LEA of a buffer's base keeps the buffer");
    free(code);
    /* A load and a store through bases the tracking lost (R6 from an FMUL, as a switch's case reaches a base in
       Pokemon Legends Z-A, issue #440) find their buffer among the tracked one when they run */
    const u64 dynamic[] = {
        0x4C98078014470002ull, /* MOV R2, c[0][0x510] */
        0xEED4200000070200ull, /* LDG.E R0, [R2] */
        FMUL_KEEP | 6,         /* R6 = an address the tracking doesn't follow */
        0xEED4200000070608ull, /* LDG.E R8, [R6] */
        0xEEDC200000070608ull, /* STG.E [R6], R8 */
    };
    code = translate(dynamic, 5, ST_FS, &info, &words, 1);
    if (!code || info.unsupported) fprintf(stderr, "dynamic global access: %s\n", info.first_unsupported);
    check(code && !info.unsupported && info.ngbuf == 1 && (info.gbuf_wmask & 1),
          "untracked global accesses use the tracked buffer, which a store through them writes");
    if (code) {
        check(count_opcode(code, words, 247, 0xffffffffu) > 0, "an untracked access tests the buffer's range");
        save_fixture(global_dynamic_path, code, words);
    }
    free(code);
    /* an atomic, or an access with no tracked buffer to be in, is still refused */
    const u64 dynamic_atomic[] = {0x4C98078014470002ull, 0xEED4200000070200ull, FMUL_KEEP | 6, 0xED03000000600606ull};
    code = translate(dynamic_atomic, 4, ST_FS, &info, &words, 1);
    check_refused(code, &info, "global memory base R6 not tracked at 78: FMUL (reg) at 68",
                  "an untracked global atomic is refused");
    free(code);
}

/* A compute program's stores and atomics to a global buffer log the bytes they write (write_log.c, SH_WLOG_*), as the
   SM86 translator's do: the buffer it writes takes the log's first part, one it only loads from none, and each store
   and atomic ORs its bits into the log (OpAtomicOr, 241). A graphics program logs nothing. shader_sm86_gpu runs such
   programs and checks the bits */
static void compute_write_log(void) {
    const u64 program[] = {
        0x4C9807800C870004ull, /* MOV R4, c[0][0x320] */
        0x4C9807800C470002ull, /* MOV R2, c[0][0x310] */
        0xEED4200000070410ull, /* LDG.E R16, [R4] */
        0xEED8200000270208ull, /* STG.E.U8 [R2 + 0x2], R8 */
        0xEBF9000000070208ull, /* RED.E.ADD [R2], R8 */
        0xED01000040870209ull, /* ATOM.E.ADD R9, [R2 + 0x4], R8 */
        0xEEF100008087020Aull, /* ATOM.E.CAS R10, [R2 + 0x8], R8 */
    };
    ShaderInfo info;
    size_t words = 0;
    u32* code = translate(program, 7, ST_CS, &info, &words, 1);
    check_translated(code, &info, "a compute program's stores and atomics translate");
    if (code) {
        int out = info.ngbuf == 2 && info.gbuf_cbuf_off[1] == 0x310 ? 1 : 0;
        check(info.ngbuf == 2 && out == 1 && info.gbuf_wmask == 2 && info.gbuf_rmask == 3 && info.gbuf_marks == 2 &&
                  info.nmarks == 1 && info.gbuf_mark[1] == 1 && !info.gbuf_mark[0],
              "the buffer a compute program writes takes a part of its write log, one it only reads none");
        check(count_opcode(code, words, 241, 0xffffffffu) == 4, "each store and atomic ORs its bytes into the log");
    }
    free(code);
    const u64 store[] = {0x4C9807800C470002ull, 0xEEDC200000070208ull}; /* MOV R2, c[0][0x310]; STG.E [R2], R8 */
    code = translate(store, 2, ST_CS, &info, &words, 1);
    check(code && !info.unsupported && info.gbuf_marks == 1 && info.nmarks == 1 && info.gbuf_mark[0] == 1 &&
              !info.gbuf_rmask && count_opcode(code, words, 241, 0xffffffffu) == 1,
          "the simple compute translator's store logs its bytes, its buffer not read");
    free(code);
    const u64 fragment[] = {0x4C98078014470002ull, 0xEEDC200000070208ull}; /* MOV R2, c[0][0x510]; STG.E [R2], R8 */
    code = translate(fragment, 2, ST_FS, &info, &words, 1);
    check(code && !info.unsupported && (info.gbuf_wmask & 1) && !info.gbuf_marks && !info.nmarks &&
              count_opcode(code, words, 241, 0xffffffffu) == 0,
          "a graphics program's store logs nothing");
    free(code);
}

static void float_controls(void) {
    const u64 cases[][2] = {{FMUL_FTZ, FMUL_FTZ},
                            {FMUL_FTZ, FADD_KEEP},
                            {FMUL_KEEP, FMUL_KEEP},
                            {FFMA_FMZ, FFMA_FMZ},
                            {FFMA_FMZ, FFMA_KEEP}};
    /* available: SH_FTZ_EMULATE, SH_FTZ_MODE, or SH_FTZ_NATIVE (a device that flushes by itself: neither the mode nor a
       flush of its own) */
    for (int i = 0; i < 5; i++) {
        for (int available = SH_FTZ_EMULATE; available <= SH_FTZ_NATIVE; available++) {
            ShaderInfo info;
            size_t words = 0;
            u32* code = translate(cases[i], 2, ST_FS, &info, &words, available);
            check_translated(code, &info, "FP32 mode translates");
            if (code) {
                int flush = i == 0 && available == SH_FTZ_MODE;
                int soft_ftz = available != SH_FTZ_NATIVE && ((i == 0 && available == SH_FTZ_EMULATE) || i == 1);
                check(info.fp32_flush == (i == 0 || i == 1) && info.fp32_preserve == (i != 0), "FP32 modes recorded");
                check((count_opcode(code, words, 17, 4465) > 0) == flush, "FP32 flush capability");
                check(ext_sets_imported(code, words), "every OpExtInst names an imported instruction set");
                /* the translation keeps signed zeros here: the flush is a float test (FOrdLessThan on the magnitude) */
                check((count_opcode(code, words, 184, 0xffffffffu) > 0) == soft_ftz &&
                          count_opcode(code, words, 199, 0xffffffffu) == 0,
                      "FTZ instructions flush with a float test when a module mode cannot apply");
                int mode = 0;
                for (size_t k = 5; k < words;) {
                    u32 width = code[k] >> 16;
                    if (!width || k + width > words) break;
                    if ((code[k] & 0xffff) == 16 && width >= 4 && code[k + 2] == 4460) mode = 1;
                    k += width;
                }
                check(mode == flush, "FP32 flush execution mode");
            }
            free(code);
        }
    }
    /* without signed zeros kept, the flush is done on the bits */
    size_t slots, words = 0;
    u64* program = guest_program(cases[1], 2, NULL, &slots);
    ShaderInfo info;
    memset(&info, 0, sizeof info);
    u32* code = program ? spirv_of(program, slots * sizeof *program, ST_FS, &info, &words, 1, 0, 1, 1) : NULL;
    free(program);
    check_translated(code, &info, "FP32 mixed modes translate without signed zeros kept");
    if (code)
        check(count_opcode(code, words, 199, 0xffffffffu) > 0 && count_opcode(code, words, 184, 0xffffffffu) == 0,
              "FTZ instructions flush on the bits when signed zeros aren't kept");
    free(code);
}

static void precise_math(void) {
    const u64 math[] = {FMUL_KEEP, FMUL_IMM};
    const int stages[] = {ST_VS, ST_FS, ST_CS};
    for (size_t i = 0; i < sizeof stages / sizeof *stages; i++) {
        int stage = stages[i];
        ShaderInfo info;
        size_t words = 0;
        u32* code = translate(math, 2, stage, &info, &words, 1);
        if (!code || info.unsupported) fprintf(stderr, "precise stage %d: %s\n", stage, info.first_unsupported);
        check_translated(code, &info, "precise float math translates");
        if (code) check(count_opcode(code, words, 71, 42) >= 2, "float math has NoContraction");
        free(code);
    }
}

/* F2F between two halves with bit 42 rounds to an integer: DREDGE's colour grading takes frac() of a half as x minus
   F2F.F16.F16.FLOOR of it, and without the floor every colour came out grey */
static int glsl_ext_count(const u32* code, size_t words, u32 inst) {
    int found = 0;
    for (size_t i = 5; i < words;) {
        u32 width = code[i] >> 16, op = code[i] & 0xffff;
        if (!width || i + width > words) return -1;
        if (op == 12 && width >= 5 && code[i + 4] == inst) found++;
        i += width;
    }
    return found;
}
static void f2f_half_rounding(void) {
    static const struct {
        u64 insn;
        u32 glsl; /* GLSL.std.450: 1 Round, 2 RoundEven, 3 Trunc, 8 Floor, 9 Ceil */
        const char* what;
    } cases[] = {
        {0x5ca8068000270508ull, 8, "F2F.F16.F16.FLOOR of the high half floors"},
        {0x5ca8048000570507ull, 8, "F2F.F16.F16.FLOOR of the low half floors"},
        {0x5ca8068000270508ull & ~(1ull << 39), 2, "F2F.F16.F16.ROUND rounds to even"},
        {0x5ca8068000270508ull | (1ull << 40), 3, "F2F.F16.F16.TRUNC truncates"},
        {(0x5ca8068000270508ull & ~(1ull << 39)) | (1ull << 40), 9, "F2F.F16.F16.CEIL rounds up"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        ShaderInfo info;
        size_t words = 0;
        u32* code = translate(&cases[i].insn, 1, ST_FS, &info, &words, 1);
        check_translated(code, &info, "F2F between halves translates");
        if (code) check(glsl_ext_count(code, words, cases[i].glsl) > 0, cases[i].what);
        free(code);
    }
    /* without bit 42 it stays a plain move */
    u64 plain = 0x5ca8068000270508ull & ~(1ull << 42);
    ShaderInfo info;
    size_t words = 0;
    u32* code = translate(&plain, 1, ST_FS, &info, &words, 1);
    check_translated(code, &info, "F2F between halves without rounding translates");
    if (code) check(glsl_ext_count(code, words, 8) == 0, "F2F between halves without bit 42 does not floor");
    free(code);
}

/* the word at which the instruction whose first word is head (its width and opcode) defines id, its result, or 0 */
static size_t defined_as(const u32* code, size_t words, u32 id, u32 head) {
    for (size_t i = 5; i < words;) {
        u32 width = code[i] >> 16;
        if (!width || i + width > words) return 0;
        if (code[i] == head && width > 2 && code[i + 2] == id) return i;
        i += width;
    }
    return 0;
}
/* whether id is OpBitwiseAnd(OpBitcast(x), 0x7fffffff) in either order: x's bits without the sign */
static int magnitude_bits_of(const u32* code, size_t words, u32 id, u32 x) {
    size_t at = defined_as(code, words, id, 5u << 16 | 199);
    if (!at) return 0;
    for (int k = 0; k < 2; k++) {
        size_t cast = defined_as(code, words, code[at + 3 + k], 4u << 16 | 124);
        if (cast && code[cast + 3] == x && is_constant_value(code, words, code[at + 4 - k], 0x7fffffffu)) return 1;
    }
    return 0;
}
/* .SAT's FClamp(x, 0, 1) results (*clamps), and how many of them an OpSelect replaces with +0 where x's bits are a
   NaN's, 0x7f800000 < (x & 0x7fffffff): GLSL.std.450 leaves FClamp's result undefined for a NaN, and the console's
   saturate gives +0 */
static void saturate_guards(const u32* code, size_t words, int* clamps, int* guarded) {
    *clamps = *guarded = 0;
    for (size_t i = 5; i < words;) {
        u32 width = code[i] >> 16;
        if (!width || i + width > words) return;
        if ((code[i] & 0xffff) == 12 && width == 8 && code[i + 4] == 43 &&
            is_constant_value(code, words, code[i + 6], 0) &&
            is_constant_value(code, words, code[i + 7], 0x3f800000u)) {
            u32 clamp = code[i + 2], x = code[i + 5];
            ++*clamps;
            for (size_t s = 5; s < words;) {
                u32 sw = code[s] >> 16;
                if (!sw || s + sw > words) return;
                if (code[s] == (6u << 16 | 169) && code[s + 5] == clamp &&
                    is_constant_value(code, words, code[s + 4], 0)) {
                    size_t test = defined_as(code, words, code[s + 3], 5u << 16 | 176);
                    if (test && is_constant_value(code, words, code[test + 3], 0x7f800000u) &&
                        magnitude_bits_of(code, words, code[test + 4], x)) {
                        ++*guarded;
                        break;
                    }
                }
                s += sw;
            }
        }
        i += width;
    }
}
static const char* saturate_path;
static const char* cbuf_storage_path;
/* the OpVariables of a storage class, and the NonWritable decorations */
static int count_variables(const u32* code, size_t words, u32 storage_class) {
    int found = 0;
    for (size_t i = 5; i < words && code[i] >> 16; i += code[i] >> 16)
        if ((code[i] & 0xffff) == 59 && code[i + 3] == storage_class) found++;
    return found;
}
static int count_nonwritable(const u32* code, size_t words) {
    int found = 0;
    for (size_t i = 5; i < words && code[i] >> 16; i += code[i] >> 16)
        if ((code[i] & 0xffff) == 71 && code[i + 2] == 24) found++;
    return found;
}
/* A device with fewer uniform buffers a stage than the 18 slots: a slot past them is a read-only storage buffer, which
   only the descriptor's type tells the renderer. The fixture goes through spirv-val */
static void constant_buffers_past_the_limit(void) {
    const u64 ops[] = {0x4c9807880e070002ull}; /* MOV R2, c[0x2][0x380] */
    ShaderInfo info;
    size_t words = 0;
    u32* code = translate(ops, 1, ST_FS, &info, &words, 1);
    check_translated(code, &info, "a constant buffer read translates");
    int uniform = code ? count_variables(code, words, 2) : 0;
    if (code) {
        check(uniform >= 1 && !count_variables(code, words, 12) && !count_nonwritable(code, words),
              "a constant buffer below the limit is a uniform buffer");
    }
    free(code);
    g_shader_uniform_cbufs = 2;
    code = translate(ops, 1, ST_FS, &info, &words, 1);
    g_shader_uniform_cbufs = 18;
    check_translated(code, &info, "a constant buffer past the limit translates");
    if (code) {
        check(count_variables(code, words, 2) == uniform - 1 && count_variables(code, words, 12) == 1 &&
                  count_nonwritable(code, words) == 1,
              "a constant buffer past the limit is a read-only storage buffer");
        check(info.cbuf_mask == 1u << 2, "a constant buffer past the limit still reports its slot");
        save_fixture(cbuf_storage_path, code, words);
    }
    free(code);
}
/* Every float .SAT (FADD, FMUL, FFMA, MUFU, F2F, IPA and each half of HADD2) gives +0 for a NaN, with or without the
   module keeping NaNs (SignedZeroInfNanPreserve), and an instruction without .SAT clamps nothing (issue #865) */
static void saturate_nan(void) {
    static const struct {
        u64 insn;
        int clamps;
        const char* what;
    } cases[] = {
        {0x5C58000000070000ull | 1ull << 50, 1, "FADD.SAT R0, R0, R0"},
        {0x5C68000000070000ull | 1ull << 50, 1, "FMUL.SAT R0, R0, R0"},
        {0x5980000000070000ull | 1ull << 50, 1, "FFMA.SAT R0, R0, R0, R0"},
        {0x5080000000470000ull | 1ull << 50, 1, "MUFU.RCP.SAT R0, R0"},
        {0x5CA8000000070A00ull | 1ull << 50, 1, "F2F.F32.F32.SAT R0, R0"},
        {0xe003ff880ff7ff00ull | 1ull << 51, 1, "IPA.SAT R0, a[0x80]"},
        {0x5D10000100070000ull, 2, "HADD2.SAT R0, R0, R0: each half"},
        {0x5C58000000070000ull, 0, "FADD R0, R0, R0 without .SAT"},
    };
    u64 all[sizeof cases / sizeof *cases];
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        all[i] = cases[i].insn;
        for (int preserve = 0; preserve < 2; preserve++) {
            size_t slots, words = 0;
            u64* program = guest_program(&cases[i].insn, 1, NULL, &slots);
            ShaderInfo info;
            memset(&info, 0, sizeof info);
            u32* code =
                program ? spirv_of(program, slots * sizeof *program, ST_FS, &info, &words, 1, preserve, 1, 1) : NULL;
            free(program);
            check_translated(code, &info, cases[i].what);
            if (code) {
                int clamps, guarded;
                saturate_guards(code, words, &clamps, &guarded);
                if (clamps != cases[i].clamps || guarded != clamps)
                    fprintf(stderr, "%s (NaNs %s): %d clamps to [0, 1] (%d expected), %d of them +0 for a NaN\n",
                            cases[i].what, preserve ? "kept" : "not kept", clamps, cases[i].clamps, guarded);
                check(clamps == cases[i].clamps && guarded == clamps, "a float .SAT gives +0 for a NaN");
            }
            free(code);
        }
    }
    ShaderInfo info;
    size_t words = 0;
    u32* code = translate(all, (int)(sizeof all / sizeof *all), ST_FS, &info, &words, 1);
    check_translated(code, &info, "every float .SAT in one program translates");
    if (code) {
        int clamps, guarded;
        saturate_guards(code, words, &clamps, &guarded);
        check(clamps == 8 && guarded == 8, "every float .SAT in one program gives +0 for a NaN");
        if (saturate_path) save_fixture(saturate_path, code, words);
    }
    free(code);
}

/* HFMA2 with an immediate B (DREDGE's fragment shaders): the two halves of B are 9-bit immediates at 20 and 30 with
   their signs at 29 and 56, C is the register at 39 with its swizzle at 53 */
static int has_constant(const u32* code, size_t words, u32 value) {
    for (size_t i = 5; i < words;) {
        u32 width = code[i] >> 16, op = code[i] & 0xffff;
        if (!width || i + width > words) return 0;
        if (op == 43 && width == 4 && code[i + 3] == value) return 1;
        i += width;
    }
    return 0;
}
static int has_glsl_fma(const u32* code, size_t words) {
    for (size_t i = 5; i < words;) {
        u32 width = code[i] >> 16, op = code[i] & 0xffff;
        if (!width || i + width > words) return 0;
        if (op == 12 && width >= 5 && code[i + 4] == 50) return 1;
        i += width;
    }
    return 0;
}
static void hfma2_immediate(void) {
    static const struct {
        u64 insn;
        u32 imm;
    } cases[] = {
        {0x704384c611870b05ull, 0x46004600u}, /* HFMA2 R5, R11.H1_H1, 6, 6, R9.H0_H0 */
        {0x716287c230871206ull, 0xc200c200u}, /* HFMA2 R6, R18.H0_H0, -3, -3, R15.H1_H1 */
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        ShaderInfo info;
        size_t words = 0;
        u32* code = translate(&cases[i].insn, 1, ST_FS, &info, &words, 1);
        if (!code || info.unsupported) fprintf(stderr, "HFMA2 immediate: %s\n", info.first_unsupported);
        check_translated(code, &info, "HFMA2 with an immediate translates");
        if (code) {
            check(has_constant(code, words, cases[i].imm), "HFMA2's immediate halves keep their values and signs");
            check(has_glsl_fma(code, words), "HFMA2 with an immediate is a fused multiply-add");
        }
        free(code);
    }
}

static void fragment_storage_images(void) {
    /* the same store and atomic on 2D array surfaces (bits 33-35 set to 4), as Pokemon Legends Z-A's fragment
       programs write them (issue #440): the layer in the coordinates' third register */
#define SURFACE_2D_ARRAY(w) (((w) & ~(7ull << 33)) | 4ull << 33)
    static const struct {
        u64 instructions[2];
        u8 kind, type;
        u32 opcode;
    } cases[] = {
        {{0x4C98078804A70002ull, 0xEB20010600F70004ull}, 2, 3, 99},
        {{0x4C98078804A70001ull, 0xEB50008602070A00ull}, 1, 3, 239},
        {{0x4C98078804A70002ull, SURFACE_2D_ARRAY(0xEB20010600F70004ull)}, 2, 4, 99},
        {{0x4C98078804A70001ull, SURFACE_2D_ARRAY(0xEB50008602070A00ull)}, 1, 4, 239},
    };
#undef SURFACE_2D_ARRAY
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        ShaderInfo info;
        size_t words = 0;
        u32* code = translate(cases[i].instructions, 2, ST_FS, &info, &words, 1);
        check(code && !info.unsupported && info.nimg == 1, "fragment storage image translates");
        if (code) {
            save_fixture(storage_paths[i], code, words);
            check(info.img[0].kind == cases[i].kind && info.img[0].type == cases[i].type && info.img[0].bindless &&
                      info.img[0].bl_cbuf == 2 && info.img[0].cbuf_off == 74,
                  "fragment storage image resolves its handle, dimension and integer format");
            check(count_opcode(code, words, cases[i].opcode, 0xffffffffu) > 0,
                  "fragment storage image emits the write or atomic operation");
            check(count_opcode(code, words, 5381, 0xffffffffu) > 0,
                  "fragment storage effects check for helper invocations");
            if (cases[i].kind == 1)
                check(count_opcode(code, words, 60, 0xffffffffu) > 0 && has_storage_image_format(code, words, 33),
                      "image atomic uses a texel pointer and an R32ui declaration");
            else
                check(has_storage_image_format(code, words, 0) && count_opcode(code, words, 17, 56) == 1,
                      "unsigned image stores use the descriptor format and declare unformatted writes");
        }
        free(code);
    }
    ShaderInfo alternate;
    size_t words = 0;
    u32* code = translate_with_rt_mask(cases[0].instructions, 2, ST_FS, &alternate, &words, 1, 1u << 8);
    check(code && !alternate.unsupported && alternate.nimg == 1 && alternate.img[0].kind == 0,
          "fragment surface store selects a float image from its runtime format key");
    free(code);
    const int signed_stages[] = {ST_FS, ST_CS, ST_VS};
    for (int stage = 0; stage < 3; stage++)
        for (u32 dim = 3; dim <= 5; dim++) {
            u64 instructions[] = {cases[0].instructions[0],
                                  (cases[0].instructions[1] & ~(7ull << 33)) | (u64)dim << 33};
            code = translate_with_rt_masks(instructions, 2, signed_stages[stage], &alternate, &words, 1, 0, 1u << 8);
            check_translated(code, &alternate, "signed surface store translates");
            if (code) {
                check(alternate.nimg == 1 && alternate.img[0].kind == 3 && has_storage_image_format(code, words, 0),
                      "a signed image store follows the signed runtime format key");
                save_fixture(storage_signed_paths[stage * 3 + dim - 3], code, words);
            }
            free(code);
        }
    const u64 clamped[] = {cases[1].instructions[0], cases[1].instructions[1] | (1ull << 49)};
    code = translate(clamped, 2, ST_FS, &alternate, &words, 1);
    check(!code && alternate.unsupported, "unsupported surface atomic clamp mode is rejected");
    free(code);
}

/* w without its predicate guard, with these destination, coordinate and handle registers */
static u64 unguarded(u64 w, u32 d, u32 a, u32 handle) {
    return (w & ~(0xffull | 0xffull << 8 | 0xfull << 16 | 0xffull << 39)) | d | (u64)a << 8 | 7ull << 16 |
           (u64)handle << 39;
}

/* A compute program's formatted load from a buffer surface, SULD.P.R on dimension 1 through the handle MOV R5,
   c[0x2][0x128], as one game's lighting reads its tiles: the header's format converts it, so the texel buffer is
   read with the number type the program is keyed on (as a 2D surface's) and declared without a format. A raw
   SULD.D.32 stays an R32UI read */
static void buffer_formatted_loads(void) {
    const u64 load[] = {0x4c98078804a70005ull, 0xeb00028200170202ull};
    const u64 raw[] = {0x4c98078804a70004ull, unguarded(0xeb10020200480370ull, 8, 0, 4)};
    /* unkeyed (a bindless header taken as integer), keyed float, keyed signed */
    const u32 rt_mask[3] = {0, 1u << 8, 0}, rt_signed[3] = {0, 0, 1u << 8};
    const u8 kinds[3] = {2, 0, 3};
    for (int i = 0; i < 3; i++) {
        ShaderInfo info;
        size_t words = 0;
        u32* code = translate_with_rt_masks(load, 2, ST_CS, &info, &words, 1, rt_mask[i], rt_signed[i]);
        check_translated(code, &info, "a formatted buffer surface load translates");
        if (code)
            check(info.nimg == 1 && info.img[0].type == 1 && info.img[0].kind == kinds[i] &&
                      has_storage_image_dim(code, words, 5) && has_storage_image_format(code, words, 0) &&
                      count_opcode(code, words, 98, 0xffffffffu) == 1 && count_opcode(code, words, 17, 55) == 1,
                  "a formatted buffer surface load reads a texel buffer of its keyed type without a format");
        free(code);
    }
    ShaderInfo info;
    size_t words = 0;
    u32* code = translate(raw, 2, ST_CS, &info, &words, 1);
    check_translated(code, &info, "a raw buffer surface load translates");
    if (code)
        check(info.nimg == 1 && info.img[0].kind == 1 && has_storage_image_format(code, words, 33) &&
                  count_opcode(code, words, 17, 55) == 0,
              "a raw buffer surface load stays an R32UI read");
    free(code);
}

/* A bindless handle whose sampler half is a constant, as one game's fragment program builds it: the texture half
   from MOV R2, c[0x2][0x380], ORed with MOV32I R1, 0x3f000000 (sampler 0x3f0). The constant is kept as the second
   half (SH_HANDLE_IMM), in either operand */
static void immediate_sampler_handle(void) {
    for (int swap = 0; swap < 2; swap++) {
        const u64 ops[] = {
            0x4c9807880e070002ull, /* MOV R2, c[0x2][0x380] */
            0x0103f0000007f001ull, /* MOV32I R1, 0x3f000000 */
            0x5c4000000007000aull | (swap ? 1ull : 2ull) << 8 | (swap ? 2ull : 1ull) << 20 |
                1ull << 41,                                                      /* LOP.OR R10 */
            0xdeb8000000070008ull | (2ull << 28) | (1ull << 37) | (10ull << 20), /* TEX.B R8, [R0], R10 */
        };
        ShaderInfo info;
        size_t words = 0;
        u32* code = translate(ops, 4, ST_FS, &info, &words, 1);
        check_translated(code, &info, "a handle with a constant sampler half translates");
        if (code)
            check(info.ntex == 1 && info.tex[0].bindless && info.tex[0].bl_cbuf == 2 &&
                      info.tex[0].cbuf_off == 0x380 / 4 && info.tex[0].bl2 && info.tex[0].bl2_cbuf == SH_HANDLE_IMM &&
                      info.tex[0].bl2_off == 0x3f000000u,
                  "a handle with a constant sampler half keeps the read and the constant");
        free(code);
    }
}

/* Handles written under each side of a predicate, as one game's fragment and compute programs write them. The
   instruction i lies at word 11 + i + i / 3 (guest_program), which the branch offsets count from */
static void guarded_handle_merges(void) {
    const u64 tex_b = 0xdeb8000000070008ull | (2ull << 28) | (1ull << 37) | (6ull << 20);
    const u64 load = 0x4c98078800070006ull, other_load = 0x4c98078800170006ull; /* MOV R6, c[2][0] / c[2][4] */
    const u64 under_p0 = ~(0xfull << 16), not_p0 = 8ull << 16, fsetp_p0 = 0x4bb1838c30f7ff07ull;
    const u64 fmul_r6 = FMUL_KEEP | 6, fmul_r5 = FMUL_KEEP | 5, sync = 0xf0f800000007000full;
    ShaderInfo info;
    size_t words = 0;
    u32* code;
    /* a write under P0 runs in none of the lanes of a use under !P0, P0 kept since: the handle loaded under !P0
       reaches it, unless P0 changes between or the write is under !P0 too */
    const u64 complement[] = {(load & under_p0) | not_p0, fmul_r6 & under_p0, (tex_b & under_p0) | not_p0};
    code = translate(complement, 3, ST_FS, &info, &words, 1);
    check_translated(code, &info, "a write under the use's complement guard leaves the handle");
    check(code && info.ntex == 1 && info.tex[0].bl_cbuf == 2 && info.tex[0].cbuf_off == 0,
          "the handle under the use's own guard is the one bound");
    free(code);
    const u64 same_guard[] = {(load & under_p0) | not_p0, (fmul_r6 & under_p0) | not_p0, (tex_b & under_p0) | not_p0};
    code = translate(same_guard, 3, ST_FS, &info, &words, 1);
    check_refused(code, &info, "untracked texture handle", "a write under the use's own guard replaces the handle");
    free(code);
    const u64 rewritten[] = {(load & under_p0) | not_p0, fmul_r6 & under_p0, fsetp_p0, (tex_b & under_p0) | not_p0};
    code = translate(rewritten, 4, ST_FS, &info, &words, 1);
    check_refused(code, &info, "untracked texture handle", "a complement write counts again once P0 changes");
    free(code);
    /* the handle under !P0; under P0 something else, whose lanes leave by @P0 BRA (word 13) to the EXIT (word 20);
       P0 rewritten, the same handle loaded under it, P0 rewritten again, then the sample under P0 */
    const u64 leave = 0xe24000000000000full | (48ull << 20);
    const u64 both_sides[] = {
        (load & under_p0) | not_p0, fmul_r6 & under_p0, leave, fsetp_p0, load & under_p0, fsetp_p0, tex_b & under_p0};
    code = translate(both_sides, 7, ST_FS, &info, &words, 1);
    check_translated(code, &info, "one handle written under each side of a rewritten predicate reaches the sample");
    check(code && info.ntex == 1 && info.tex[0].bl_cbuf == 2 && info.tex[0].cbuf_off == 0,
          "the handle written under each side is the one bound");
    free(code);
    u64 variant[7];
    memcpy(variant, both_sides, sizeof variant);
    variant[4] = other_load & under_p0;
    code = translate(variant, 7, ST_FS, &info, &words, 1);
    check_refused(code, &info, "untracked texture handle", "different handles under each side stay refused");
    free(code);
    memcpy(variant, both_sides, sizeof variant);
    variant[2] = fmul_r5;
    code = translate(variant, 7, ST_FS, &info, &words, 1);
    check_refused(code, &info, "untracked texture handle",
                  "the other side's value reaches the sample without the branch");
    free(code);
    memcpy(variant, both_sides, sizeof variant);
    variant[0] = fmul_r5;
    code = translate(variant, 7, ST_FS, &info, &words, 1);
    check_refused(code, &info, "untracked texture handle", "lanes that load no handle reach the sample");
    free(code);
    /* if and else between an SSY (word 11, to the sample at word 19) and their SYNCs: @!P0 BRA (word 12) to the else
       side (word 16), each side loading the handle */
    const u64 ssy = 0xe290000000000000ull | (56ull << 20), to_else = 0xe24000000008000full | (24ull << 20);
    const u64 if_else[] = {ssy, to_else, load, sync, load, sync, tex_b};
    code = translate(if_else, 7, ST_FS, &info, &words, 1);
    check_translated(code, &info, "one handle loaded on each side of an SSY's branch reaches its reconvergence point");
    check(code && info.ntex == 1 && info.tex[0].bl_cbuf == 2 && info.tex[0].cbuf_off == 0,
          "the handle loaded on each side is the one bound");
    free(code);
    memcpy(variant, if_else, sizeof variant);
    variant[2] = other_load;
    code = translate(variant, 7, ST_FS, &info, &words, 1);
    check_refused(code, &info, "untracked texture handle", "different handles on each side of an SSY stay refused");
    free(code);
    memcpy(variant, if_else, sizeof variant);
    variant[2] = fmul_r5;
    code = translate(variant, 7, ST_FS, &info, &words, 1);
    check_refused(code, &info, "untracked texture handle", "a side that loads no handle reaches the sample");
    free(code);
}

static void volume_surface_atomics(void) {
    const u64 operations[] = {0xea40020a00870009ull, 0xea48020a20870009ull, 0xeb50020a00070008ull,
                              0xeb50020a01170008ull};
    for (u32 i = 0; i < 4; i++) {
        const u64 instructions[] = {0x4c98078804a70004ull, operations[i]};
        ShaderInfo info;
        size_t words = 0;
        u32* code = translate(instructions, 2, ST_CS, &info, &words, 1);
        check_translated(code, &info, "3D surface atomic or reduction translates");
        if (code) {
            check(info.nimg == 1 && info.img[0].kind == 1 && info.img[0].type == 5 &&
                      has_storage_image_dim(code, words, 2) && has_storage_image_format(code, words, 33),
                  "3D atomics use a 3D R32ui image");
            check(count_opcode(code, words, 60, 0xffffffffu) == 1,
                  "3D atomics address one voxel through a texel pointer");
            save_fixture(storage_volume_atomic_paths[i], code, words);
        }
        free(code);
    }
    const u64 refused[] = {operations[0] | 1ull << 49, operations[0] | 9ull << 29, operations[0] | 2ull << 51,
                           operations[2] | 1ull << 23};
    for (u32 i = 0; i < 4; i++) {
        const u64 instructions[] = {0x4c98078804a70004ull, refused[i]};
        ShaderInfo info;
        size_t words = 0;
        u32* code = translate(instructions, 2, ST_CS, &info, &words, 1);
        check_refused(code, &info, i == 3 ? "SURED" : "SUATOM",
                      "unimplemented 3D atomic widths, clamp modes and operation 9 remain rejected");
        free(code);
    }
}

/* One game's compute program ORs flags into a buffer surface: SURED.OR with the value in R5 at the texel R4, the
   handle in R6 (MOV R6, c[0x2][0x128]), the word its log names. Every operation of SURED (bits 24-26) and SUATOM
   (bits 29-32) translates: AND, OR and XOR as SPIR-V atomics; INC and DEC, which wrap at the operand, as a loop of
   compare and exchange that reads the texel atomically, and SUATOM returns what it held */
static void surface_atomic_operations(void) {
    static const u32 bitwise[3] = {240, 241, 242}; /* OpAtomicAnd, OpAtomicOr, OpAtomicXor */
    const u64 sured = 0xeb50030206070405ull & ~(7ull << 24);
    for (u32 operation = 3; operation <= 7; operation++) {
        const u64 instructions[] = {0x4c98078804a70006ull, sured | (u64)operation << 24};
        ShaderInfo info;
        size_t words = 0;
        u32* code = translate(instructions, 2, ST_CS, &info, &words, 1);
        check_translated(code, &info, "a buffer surface reduction of every operation translates");
        if (code) {
            int wraps = operation <= 4;
            check(info.nimg == 1 && info.img[0].type == 1 && info.img[0].kind == 1,
                  "a buffer surface reduction reaches one R32UI texel buffer");
            check(wraps ? count_opcode(code, words, 230, 0xffffffffu) == 1 &&
                              count_opcode(code, words, 227, 0xffffffffu) == 1 &&
                              count_opcode(code, words, 246, 0xffffffffu) == 1
                        : count_opcode(code, words, bitwise[operation - 5], 0xffffffffu) == 1 &&
                              count_opcode(code, words, 230, 0xffffffffu) == 0,
                  wraps ? "SURED.INC and .DEC loop on a compare and exchange from an atomic load"
                        : "SURED.AND, .OR and .XOR are one SPIR-V atomic each");
            save_fixture(surface_op_paths[operation - 3], code, words);
        }
        free(code);
    }
    for (u32 k = 0; k < 2; k++) {
        const u64 instructions[] = {0x4c98078804a70004ull, (0xea40020a00870009ull & ~(0xfull << 29)) | (u64)(3 + k)
                                                                                                           << 29};
        ShaderInfo info;
        size_t words = 0;
        u32* code = translate(instructions, 2, ST_CS, &info, &words, 1);
        check_translated(code, &info, "SUATOM.INC and .DEC on a 3D surface translate");
        if (code) {
            check(info.nimg == 1 && info.img[0].type == 5 && count_opcode(code, words, 230, 0xffffffffu) == 1 &&
                      count_opcode(code, words, 227, 0xffffffffu) == 1,
                  "SUATOM.INC and .DEC loop on a compare and exchange of one voxel");
            save_fixture(surface_op_paths[5 + k], code, words);
        }
        free(code);
    }
}

/* SUST.P.RGBA feedback stores and SULD.D.32 buffer loads use storage images in every graphics stage. Only fragment
   stages have helper invocations to guard. */
static void draw_buffer_surfaces(void) {
    const u64 store[] = {0x4c9807880487001cull, unguarded(0xeb200e0200f00f18ull, 24, 15, 28)};
    /* SULD.D.32 on the handle MOV R4, c[0x2][0x128], as Ender Magnolia's compute loads it */
    const u64 load[] = {0x4c98078804a70004ull, unguarded(0xeb10020200480370ull, 8, 0, 4)};
    for (int stage = ST_VS; stage <= ST_FS; stage++)
        for (int write = 0; write < 2; write++) {
            u32 header[3] = {stage == ST_TCS ? 3u << 24 : 1u << 24, 7u << 24, 3};
            ShaderInfo info;
            size_t words = 0;
            u32* code = translate_stage(write ? store : load, 2, stage, header, stage == ST_GS ? 3 : 0, &info, &words);
            check_translated(code, &info, "a graphics stage's buffer surface operation translates");
            if (code) {
                check(info.nimg == 1 && info.img[0].type == 1 && has_storage_image_dim(code, words, 5) &&
                          count_opcode(code, words, write ? 99 : 98, 0xffffffffu) == 1,
                      "a graphics stage reads or writes exactly one storage texel buffer");
                check(info.img[0].kind == (write ? 2 : 1) && has_storage_image_format(code, words, write ? 0 : 33),
                      "buffer loads keep R32UI while formatted stores retain the descriptor format");
                check(count_opcode(code, words, 5381, 0xffffffffu) == (write && stage == ST_FS),
                      "only fragment stores guard helper invocations");
                save_fixture(draw_buffer_paths[stage * 2 + write], code, words);
            }
            free(code);
        }
}

static void layered_surface_loads(void) {
    for (u32 stage = 0; stage < 2; stage++)
        for (u32 dim = 4; dim <= 5; dim++)
            for (u32 raw = 0; raw < 2; raw++) {
                const u64 load = 0xeb00000000170008ull | (u64)dim << 33 | 4ull << 39;
                const u64 instructions[] = {0x4c98078804a70004ull,
                                            raw ? (load & ~(15ull << 20)) | 4ull << 20 | 1ull << 52 : load};
                ShaderInfo info;
                size_t words = 0;
                u32* code = translate(instructions, 2, stage ? ST_CS : ST_FS, &info, &words, 1);
                check_translated(code, &info, "layered or volume surface load translates");
                if (code) {
                    check(info.ntex == 1 && info.tex[0].type == (dim == 5 ? 2 : 1) && info.tex[0].array == (dim == 4) &&
                              (info.tex[0].flags & 1) == raw,
                          "surface load preserves the image dimension, array layers and raw mode");
                    check(count_opcode(code, words, 95, 0xffffffffu) == 1,
                          "surface load fetches one texel from its typed image");
                    save_fixture(storage_layer_load_paths[stage * 4 + (dim - 4) * 2 + raw], code, words);
                }
                free(code);
            }
}

static int brx_table(u32 bank, u32 offset, u32* value) {
    if (bank || offset > 8) return 0;
    *value = offset == 0 ? 16 : 1024; /* second word belongs to another constant, not this jump table */
    return 1;
}

static int clamped_brx_table(u32 bank, u32 offset, u32* value) {
    if (bank || offset > 8) return 0;
    *value = offset < 8 ? 24 : 1024;
    return 1;
}

static int invalid_brx_table(u32 bank, u32 offset, u32* value) {
    if (bank || offset) return 0;
    *value = 1024;
    return 1;
}

static int three_way_table(u32 bank, u32 offset, u32* value) {
    if (bank || offset > 8) return 0;
    *value = offset == 0 ? 8 : offset == 4 ? 32 : 56; /* words 17, 20 and 23, after the BRX at 15 */
    return 1;
}

/* the cases of each OpSwitch in a module, added up, and the switches */
static int switch_cases(const u32* code, size_t words, int* switches) {
    int cases = 0;
    *switches = 0;
    for (size_t i = 5; i < words && code[i] >> 16;) {
        if ((code[i] & 0xffff) == 251) {
            cases += (int)((code[i] >> 16) - 3) / 2;
            ++*switches;
        }
        i += code[i] >> 16;
    }
    return cases;
}

/* Structured control flow: a jump table becomes an OpSwitch of its own targets and a loop its own loop construct, in
   place of the dispatch loop's switch over every block */
static void structured_flow(void) {
    u64 code[32] = {0};
    ShaderInfo info;
    size_t words = 0;
    int switches;
    for (u32 i = 10; i < 32; i += 4) code[i] = SCHED;
    code[11] = 0x3820000000270b0bull; /* IMNMX R11, R11, 2: three entries */
    code[12] = 0x3848000000270b0dull; /* SHL R13, R11, 2 */
    code[13] = 0xef94000000070d0dull; /* LDC.B32 R13, c[0][R13] */
    code[15] = 0xe250000000070d0full; /* BRX R13 */
    code[16] = EXIT;
    code[17] = 0x010000000007F001ull; /* MOV32I R1 */
    code[19] = EXIT;
    code[20] = 0x010000000007F001ull;
    code[21] = EXIT;
    code[23] = 0x50B0000000070F00ull; /* NOP */
    code[24] = EXIT;
    int (*old)(u32, u32, u32*) = g_shader_cb_read;
    g_shader_cb_read = three_way_table;
    memset(&info, 0, sizeof info);
    u32* spv = spirv_of(code, 25 * 8, ST_FS, &info, &words, 1, 1, 1, 1);
    g_shader_cb_read = old;
    check(spv != NULL, "a program with a jump table translates");
    if (spv) {
        check(switch_cases(spv, words, &switches) == 3 && switches == 1,
              "a jump table is a switch of its three targets, with no dispatch loop");
        save_fixture(structured_paths[0], spv, words);
    }
    free(spv);
    /* a loop (@P0 BRA back to 12) and an if after it (@P1 BRA over a NOP) */
    memset(code, 0, sizeof code);
    for (u32 i = 10; i < 32; i += 4) code[i] = SCHED;
    code[11] = 0x010000000007F001ull;
    code[12] = 0x50B0000000070F00ull;
    code[13] = (0xE24ull << 52 | (u64)(((12u - 14u) * 8) & 0xFFFFFF) << 20 | 0x7000Full) & ~0xF0000ull;
    code[15] = (0xE24ull << 52 | (u64)(((17u - 16u) * 8) & 0xFFFFFF) << 20 | 0x7000Full) & ~0xE0000ull;
    code[16] = 0x50B0000000070F00ull;
    code[17] = EXIT;
    memset(&info, 0, sizeof info);
    spv = spirv_of(code, 18 * 8, ST_FS, &info, &words, 1, 1, 1, 1);
    check(spv != NULL, "a program with a loop translates");
    if (spv) {
        check(switch_cases(spv, words, &switches) == 0 && count_opcode(spv, words, 246, 0xffffffffu) >= 2,
              "a loop and an if are loop constructs, with no dispatch loop");
        save_fixture(structured_paths[1], spv, words);
    }
    free(spv);
}

/* Indirect calls read compiler constants at execution time, so a changed selection can reuse the same module.
   Absolute calls include their shader-pool base in the key. Hashing must retain functions past the main EXIT. */
static void subroutine_calls(void) {
    const u64 forms[] = {0xe26000100007002full, 0xe220001000070020ull, 0xe21000100007002full, 0xe20000000007040full};
    for (u32 form = 0; form < sizeof forms / sizeof *forms; form++) {
        const u64 instructions[] = {forms[form], EXIT, 0x010000001007f000ull, 0xe32000000007000full};
        size_t slots, words = 0;
        u64* program = guest_program(instructions, 4, NULL, &slots);
        check(program != NULL, "allocate an indirect-call program");
        if (!program) continue;
        ShaderInfo info = {.flow_base = 0x400000};
        u32* code = spirv_of(program, slots * 8, ST_FS, &info, &words, 0, 1, 1, 0);
        check_translated(code, &info, "indirect call/jump translates to the dispatch loop");
        if (code) {
            check(info.flow_base == 0x400000, "absolute-flow base survives shader metadata output");
            if (form < 3)
                check((info.cbuf_mask & 2) && info.cbuf_max[1] == 16,
                      "compiler jump-table word has a bounded integer constant-buffer binding");
            check(count_opcode(code, words, 251, 0xffffffffu) != 0,
                  "dynamic call/jump dispatch contains cases after the main EXIT");
        }
        u32 length = 0;
        hash_of(program, slots * 8, &length);
        check(length > 13 * 8, "indirect flow retains target functions after an early main EXIT");
        u8* bytes = guest_bytes(program, slots * 8);
        check(bytes && shader_absolute_flow(bytes, (u32)(slots * 8)) == (form != 0),
              "absolute-flow cache key is required only for absolute call/jump opcodes");
        free(bytes);
        free(code);
        free(program);
    }
}

static void brx_length(void) {
    u64 code[64] = {0};
    code[10] = 0x001F8000FC0007E0ull;
    code[11] = 0xef94000000070001ull; /* LDC.B32 R1, c[0][0] */
    code[12] = 0xe25000000007010full; /* BRX R1 */
    code[13] = 0xe30000000007000full;
    code[14] = code[10];
    code[15] = code[13];
    u32 targets[64], len = 0;
    int (*old)(u32, u32, u32*) = g_shader_cb_read;
    g_shader_cb_read = brx_table;
    hash_of(code, sizeof code, &len);
    int count = shader_brx_targets(code, 12, 64, targets, 64);
    check(count == 1 && targets[0] == 15, "BRX table stops at out-of-range trailing constant");
    check(len == 16 * 8, "BRX program length ends after its last reachable exit");
    g_shader_cb_read = invalid_brx_table;
    check(shader_brx_targets(code, 12, 64, targets, 64) == -1, "BRX rejects a table with no valid targets");
    memset(code, 0, sizeof code);
    code[10] = 0x001F8000FC0007E0ull;
    code[11] = 0x3820000000170b0bull; /* IMNMX R11, R11, 1: two entries */
    code[12] = 0x3848000000270b0dull; /* SHL R13, R11, 2 */
    code[13] = 0xef94000000070d0dull; /* LDC R13, c[0][R13] */
    code[14] = code[10];
    code[15] = 0xe250000000070d0full; /* BRX R13 */
    code[16] = 0xe30000000007000full;
    code[17] = 0x50B0000000070F00ull; /* NOP: live code fills its bundles, zeros are padding after the program */
    code[18] = code[10];
    code[19] = code[16];
    g_shader_cb_read = clamped_brx_table;
    hash_of(code, sizeof code, &len);
    count = shader_brx_targets(code, 15, 64, targets, 64);
    check(count == 2 && targets[0] == 19 && targets[1] == 19,
          "BRX reads only the entries permitted by its IMNMX clamp");
    check(len == 20 * 8, "clamped BRX bounds program length before padding");
    memset(code, 0, sizeof code);
    code[10] = 0x001F8000FC0007E0ull;
    code[11] = 0xe250000000070d0full; /* dynamic BRX with no readable jump table */
    code[12] = 0xe30000000007000full;
    g_shader_cb_read = NULL;
    hash_of(code, sizeof code, &len);
    check(len == 13 * 8, "unresolved BRX stops at the exit before an all-zero tail");
    code[13] = 0x50B0000000070F00ull; /* NOP */
    code[15] = code[12];
    hash_of(code, sizeof code, &len);
    check(len == 16 * 8, "unresolved BRX keeps nonzero instructions beyond an earlier exit");
    code[13] = 0; /* padding, then the next program of the pool */
    hash_of(code, sizeof code, &len);
    check(len == 13 * 8, "unresolved BRX ends at the zero padding before the next program");
    g_shader_cb_read = old;
}

static void invalid_ldc_bank(void) {
    const u64 ldc = 0xef94000000070d0dull | (31ull << 36);
    ShaderInfo info;
    size_t words = 0;
    u32* code = translate(&ldc, 1, ST_FS, &info, &words, 1);
    check(!code && info.unsupported, "LDC rejects constant buffer banks outside the descriptor set");
    free(code);
}

static void nro_shader_modes(void) {
    const u64 raw[] = {0x4c98078800070004ull, mov32i(4, 1), mov32i(5, 3), 0xeb18048600470400ull};
    const u64 ms[] = {0x4c98078800070004ull, mov32i(5, 4), mov32i(0, 3), mov32i(1, 5), 0xdd3c0007a0470000ull};
    const u64 ms_array[] = {0x4c98078800070004ull, mov32i(5, 2), mov32i(0, 3),
                            mov32i(1, 5),          mov32i(2, 5), 0xdd3c0007b0470000ull};
    const u64 exchange[] = {mov32i(2, 1), mov32i(3, 2), 0xef5d00000007ff02ull, 0xec8000002027ff02ull};
    struct {
        const u64* instructions;
        int count;
        u8 flags;
    } cases[] = {{raw, 4, 1}, {ms, 5, 2}, {ms_array, 6, 2}, {exchange, 4, 0}};
    for (int i = 0; i < 4; i++) {
        ShaderInfo info = {0};
        info.shared_size = 16;
        size_t words = 0;
        u32* code = translate(cases[i].instructions, cases[i].count, ST_CS, &info, &words, 1);
        check_translated(code, &info, "NRO shader mode translates");
        if (code) {
            if (i < 3)
                check(info.ntex == 1 && (info.tex[0].flags & 3) == cases[i].flags, "NRO texture mode is recorded");
            save_fixture(nro_paths[i], code, words);
        }
        free(code);
    }
}

static void issue126_shader_modes(void) {
    const u64 intmath[] = {
        0x1f00000000070000ull | (7ull << 20) | (1ull << 8),                               /* IMUL32I low */
        0x1f00000000070002ull | (1ull << 53) | (1ull << 54) | (3ull << 20) | (1ull << 8), /* signed high */
        0x8000000000070004ull | (3ull << 20) | (1ull << 8),                               /* IMAD32I */
    };
    const u64 clip[] = {0xeff000000007ff00ull | (0x2c0ull << 20)};
    const u64 bindless[] = {
        0x4c98078800070004ull, /* MOV R4, c[0][...] supplies the texture handle */
        0xdeb8000000070008ull | (6ull << 28) | (1ull << 37) | (4ull << 20), /* TEX.B cube LZ */
    };
    const u64 indexed_handle[] = {
        0xef94002000070004ull, /* LDC R4, c[0][R0 + 0x20] */
        0xdd380007a0470000ull, /* TLD.B using the handle loaded above */
    };
    const u64 tex3d[] = {0x4c98078800070004ull, 0xdeb8000000070008ull | (4ull << 28) | (3ull << 37) | (4ull << 20)};
    const u64 cube_array_bias[] = {0x4c98078800070004ull,
                                   0xdeb8000000070008ull | (7ull << 28) | (2ull << 37) | (4ull << 20)};
    const u64 tex1d_array[] = {0x4c98078800070004ull,
                               0xdeb8000000070008ull | (1ull << 28) | (1ull << 37) | (4ull << 20)};
    const u64 split_handle[] = {
        0x4c98078800070004ull,
        0x4c98078800470005ull,
        0x5c40000000070006ull | (4ull << 8) | (5ull << 20) | (1ull << 41), /* LOP.OR R6, R4, R5 */
        0xdeb8000000070008ull | (2ull << 28) | (1ull << 37) | (6ull << 20),
    };
    const u64 dynamic_3d_offset[] = {0x4c98078800070004ull,
                                     0xdeb8000000070008ull | (4ull << 28) | (1ull << 36) | (4ull << 20)};
    /* Ender Magnolia ORs the sampler half in from a constant buffer operand. */
    const u64 split_cbuf_handle[] = {
        0x4c98078805c70006ull, /* MOV R6, c[0x2][0x170] */
        0x4c47020815c70606ull, /* LOP.OR R6, R6, c[0x2][0x570] */
        0xdeb8000000070008ull | (2ull << 28) | (1ull << 37) | (6ull << 20),
    };
    /* Ender Magnolia's buffer images: SULD.D.32, SUATOM.ADD and SUST.P on the handle MOV R4, c[0x2][0x128] */
    const u64 buffer_surfaces[] = {
        0x4c98078804a70004ull,
        unguarded(0xeb10020200480370ull, 8, 0, 4),
        (unguarded(0xea70070200980b0aull, 9, 0, 4) & ~(0xffull << 20)) | (1ull << 20),
        unguarded(0xeb203a0200f77614ull, 12, 0, 4),
    };
    /* TXQ.B: the handle in R4, the level in R5 */
    const u64 bindless_query[] = {HANDLE_CBUF_R4, 0x5c9807800ff70005ull, 0xdf50000180470400ull};
    /* A 2D texture offset from an unresolved register uses exact selected-mip sample offsets. */
    const u64 dynamic_2d_offset[] = {0x4c98078800070004ull,
                                     0xdeb8000000070008ull | (2ull << 28) | (1ull << 36) | (4ull << 20)};
    /* the lane ID after a branch: lanes reconverge on the subgroup's lowest pc */
    const u64 lane_after_branch[] = {0xe24000000000000full | (16ull << 20), FMUL_KEEP | 5, 0xf0c8000000070003ull};
    /* a clip distance and flow: every OpVariable before the clip distance's store */
    const u64 clip_flow[] = {0xe24000000007000full, 0xeff000000007ff00ull | (0x2c0ull << 20)};
    /* Single-register writers and a TEX.B masked to one result write just below the handle. */
    const u64 handle_past_writers[] = {
        0x4c98078800070006ull,
        FMUL_KEEP | 5,
        FFMA_KEEP | 3,
        0xdeb8000000070004ull | (2ull << 28) | (1ull << 37) | (6ull << 20) | (1ull << 31),
        0xdeb8000000070008ull | (2ull << 28) | (1ull << 37) | (6ull << 20),
    };
    /* edenprobe's indexed handle: the table index from an attribute, shifted to a 16-byte stride, then LDC */
    const u64 indexed_table[] = {
        0xe003ff870ff7ff00ull, /* IPA R0, a[0x70] */
        0x5cb0118000070a00ull, /* F2I.U32 R0, R0 */
        0x3848000000470000ull, /* SHL R0, R0, 4 */
        0xef94002000070004ull, /* LDC R4, c[2][R0] */
        0x5c9807800ff70000ull, /* MOV R0, RZ */
        0x5c9807800ff70001ull, /* MOV R1, RZ */
        0xdd380007a0470000ull, /* TLD.B using the handle loaded above */
    };
    struct {
        const u64* instructions;
        int count, stage;
        u32 mask;
        u32 opcode;
    } cases[] = {{intmath, 3, ST_FS, 0, 151},
                 {clip, 1, ST_VS, 1, 65},
                 {bindless, 2, ST_FS, 0, 88},
                 {indexed_handle, 2, ST_FS, 0, 95},
                 {tex3d, 2, ST_FS, 0, 88},
                 {cube_array_bias, 2, ST_FS, 0, 87},
                 {tex1d_array, 2, ST_FS, 0, 88},
                 {split_handle, 4, ST_FS, 0, 88},
                 {dynamic_3d_offset, 2, ST_FS, 0, 87},
                 {split_cbuf_handle, 3, ST_FS, 0, 88},
                 {handle_past_writers, 5, ST_FS, 0, 88},
                 {buffer_surfaces, 4, ST_CS, 0, 98},
                 {bindless_query, 3, ST_FS, 0, 103},
                 {clip_flow, 2, ST_VS, 1, 65},
                 {dynamic_2d_offset, 2, ST_FS, 0, 87},
                 {lane_after_branch, 3, ST_FS, 0, 354},
                 {indexed_table, 7, ST_FS, 0, 95}};
    for (int i = 0; i < 17; i++) {
        ShaderInfo info;
        size_t words = 0;
        g_shader_texture_arrays = i == 16; /* the device's non-uniform indexing, for the table */
        u32* code = translate_with_rt_mask(cases[i].instructions, cases[i].count, cases[i].stage, &info, &words, 1,
                                           cases[i].mask);
        check_translated(code, &info, "issue 126 shader translates");
        if (code) {
            check(count_opcode(code, words, cases[i].opcode, 0xffffffffu) > 0,
                  "issue 126 emits the expected SPIR-V instruction");
            if (i == 8 || i == 14)
                check(!count_opcode(code, words, 103, 0xffffffffu) &&
                          !count_opcode(code, words, 136, 0xffffffffu) && has_image_operands(code, words, 87, 8),
                      "ordinary varying TEX keeps exact ConstOffset without base-level query/division");
            if (i == 15)
                check(switch_selects_current_pc(code, words),
                      "reconverged flow dispatches the lane's own pc or the sentinel");
            if (i == 1) check(info.uses_clip_mask == 1, "enabled clip distance is reported");
            if (i == 7) check(info.ntex == 1 && info.tex[0].bl2, "split bindless handle retains both descriptor reads");
            if (i == 9)
                check(info.ntex == 1 && info.tex[0].bl_cbuf == 2 && info.tex[0].cbuf_off == 0x170 / 4 &&
                          info.tex[0].bl2 && info.tex[0].bl2_cbuf == 2 && info.tex[0].bl2_off == 0x570 / 4,
                      "a handle ORed with a constant buffer operand keeps both reads");
            if (i == 10) check(info.ntex == 1, "a handle is tracked past writes to the registers below it");
            if (i == 11)
                check(info.nimg == 2 && info.img[0].type == 1 && info.img[0].kind == 1 && info.img[1].type == 1 &&
                          info.img[1].kind == 2,
                      "buffer loads and atomics share an R32ui image, stores take a typeless one");
            if (i == 12) check(info.ntex == 1 && info.tex[0].bindless, "TXQ.B queries its tracked handle");
            if (i == 16)
                check(info.ntex == 1 && info.tex[0].flags == (SNX_TEX_INDEXED | 4 << 4) &&
                          count_opcode(code, words, 17, 5307) == 1,
                      "an indexed handle samples an element of the texture array, 16 bytes apart");
            save_fixture(issue126_paths[i], code, words);
        }
        free(code);
    }
    g_shader_texture_arrays = 0;
    /* One game's froxel volume: SUST.P.RGBA on a 3D surface (dimension 5), x, y and z in R0-R2, the texel from
       R8-R11, on the bindless handle MOV R4, c[0x2][0x128] */
    {
        const u64 volume_store[] = {
            0x4c98078804a70004ull,
            (unguarded(0xeb203a0200f77614ull, 8, 0, 4) & ~(7ull << 33)) | (5ull << 33),
        };
        ShaderInfo info;
        size_t words = 0;
        u32* code = translate(volume_store, 2, ST_CS, &info, &words, 1);
        check_translated(code, &info, "a 3D surface store translates");
        if (code)
            check(info.nimg == 1 && info.img[0].type == 5 && count_opcode(code, words, 99, 0xffffffffu) == 1 &&
                      has_storage_image_dim(code, words, 2) && has_storage_image_format(code, words, 0),
                  "a 3D surface store writes a 3D storage image in its descriptor format");
        free(code);
    }
    /* UE4's vertex factories fetch texture coordinates from buffer textures of tens of thousands of texels: a 1D
       TLD.B reads (x mod row, x / row) of the 2D image they are bound as, a 2D one its coordinates as they are */
    {
        const u64 tld_b = (0xdd3a0007a0070200ull & ~(0xffull << 20)) | (6ull << 20); /* TLD.B R0, R2, R6, 2D */
        const u64 fetch_1d[] = {0x4c98078800070006ull, tld_b & ~(7ull << 28)};
        const u64 fetch_2d[] = {0x4c98078800070006ull, tld_b};
        ShaderInfo info;
        size_t words = 0;
        u32* code = translate(fetch_1d, 2, ST_VS, &info, &words, 1);
        check_translated(code, &info, "a 1D buffer texel fetch translates");
        if (code)
            check(count_opcode(code, words, 195, 0xffffffffu) == 1 && count_opcode(code, words, 95, 0xffffffffu) == 1,
                  "a 1D texel fetch splits x into the row and column of its 2D image");
        free(code);
        code = translate(fetch_2d, 2, ST_VS, &info, &words, 1);
        check_translated(code, &info, "a 2D texel fetch translates");
        if (code) check(count_opcode(code, words, 195, 0xffffffffu) == 0, "a 2D texel fetch keeps its coordinates");
        free(code);
    }
    /* One game's histogram: SURED.ADD into a buffer surface, U32 and then S32 (bit 20), on the handle
       MOV R5, c[0x2][0x128] */
    {
        const u64 buffer_reductions[] = {0x4c98078804a70005ull, 0xeb50028200070700ull, 0xeb50028200170701ull};
        ShaderInfo info;
        size_t words = 0;
        u32* code = translate(buffer_reductions, 3, ST_CS, &info, &words, 1);
        check_translated(code, &info, "buffer surface reductions translate");
        if (code)
            check(info.nimg == 1 && info.img[0].type == 1 && info.img[0].kind == 1 &&
                      count_opcode(code, words, 234, 0xffffffffu) == 2,
                  "buffer surface reductions add atomically into one R32UI texel buffer");
        free(code);
    }
    /* One game's title screen queries a 3D texture's depth with TXQ.B (component 2) on the handle its TEX.B samples
       in 3D: the query takes the 3D slot, which binds, where a 2D one bound nothing and read a size of zero */
    {
        const u64 txq_3d[] = {0x4c98078800070008ull, 0xdf50000200470801ull,
                              (0xdeba0003c1d70000ull & ~(0xffull << 20)) | (8ull << 20)};
        ShaderInfo info;
        size_t words = 0;
        u32* code = translate(txq_3d, 3, ST_FS, &info, &words, 1);
        check_translated(code, &info, "a TXQ of a texture sampled in 3D translates");
        if (code)
            check(info.ntex == 1 && info.tex[0].type == 2 && !info.tex[0].array,
                  "a TXQ takes the type of the texture another instruction samples through its handle");
        free(code);
    }
    /* A TEX.B with all four results (mask 15) to R4-R7 overwrites the handle in R6. */
    const u64 clobbered_handle[] = {
        0x4c98078800070006ull,
        0xdeb8000000070004ull | (0xfull << 31) | (2ull << 28) | (1ull << 37) | (6ull << 20),
        0xdeb8000000070008ull | (0xfull << 31) | (2ull << 28) | (1ull << 37) | (6ull << 20),
    };
    ShaderInfo info;
    size_t words = 0;
    u32* code = translate(clobbered_handle, 3, ST_FS, &info, &words, 1);
    if (code || strcmp(info.first_unsupported, "untracked texture handle R6 of TEX.B at 68: TEX.B at 60"))
        fprintf(stderr, "overwritten handle: '%s'\n", info.first_unsupported);
    check(!code && info.unsupported &&
              !strcmp(info.first_unsupported, "untracked texture handle R6 of TEX.B at 68: TEX.B at 60"),
          "a handle overwritten by a texture result is not tracked, and the refusal names the register, the "
          "instruction, its offset and the last write of the register");
    free(code);
    /* a handle no constant buffer holds: the refusal says where it came from, and its value when that is a constant
       (a MOV of RZ, a MOV32I's handle, a flat varying, no write at all) */
    const u64 tex_r6 = 0xdeb8000000070004ull | (0xfull << 31) | (2ull << 28) | (1ull << 37) | (6ull << 20);
    const u64 zero_handle[] = {0x5c9807800ff70006ull, tex_r6};
    const u64 imm_handle[] = {mov32i(6, 0x00100105u), tex_r6};
    const u64 varying_handle[] = {0xe083ff8a0ff7ff06ull, tex_r6};
    const struct {
        const u64* program;
        u32 count;
        const char* reason;
    } from[] = {{zero_handle, 2, "untracked texture handle R6 of TEX.B at 60: MOV (reg) at 58 (the constant 0)"},
                {imm_handle, 2, "untracked texture handle R6 of TEX.B at 60: MOV32I at 58 (the constant 0x100105)"},
                {varying_handle, 2, "untracked texture handle R6 of TEX.B at 60: IPA at 58"},
                {&tex_r6, 1, "untracked texture handle R6 of TEX.B at 58: no write before"}};
    for (int k = 0; k < 4; k++) {
        code = translate(from[k].program, from[k].count, ST_FS, &info, &words, 1);
        if (code || strcmp(info.first_unsupported, from[k].reason))
            fprintf(stderr, "handle %d: '%s'\n", k, info.first_unsupported);
        check(!code && info.unsupported && !strcmp(info.first_unsupported, from[k].reason),
              "an untracked handle's refusal names its last write, and the value a constant write gives");
        free(code);
    }
    const u64 zero_mask_handle[] = {
        0x4c98078800070006ull,
        0xdeb8000000070004ull | (2ull << 28) | (1ull << 37) | (6ull << 20),
        0xdeb8000000070008ull | (0xfull << 31) | (2ull << 28) | (1ull << 37) | (6ull << 20),
    };
    code = translate(zero_mask_handle, 3, ST_FS, &info, &words, 1);
    check(code && !info.unsupported && info.ntex == 1, "a zero-mask TEX.B leaves its handle register intact");
    free(code);
    /* A handle loaded under P0 reaches a TEX.B under P0, and not one under !P0. */
    const u64 tex_b = 0xdeb8000000070008ull | (2ull << 28) | (1ull << 37) | (6ull << 20);
    const u64 guarded[] = {0x4c98078800000006ull, tex_b & ~(0xfull << 16)};
    code = translate(guarded, 2, ST_FS, &info, &words, 1);
    check_translated(code, &info, "a handle loaded under a predicate reaches a use under the same one");
    free(code);
    const u64 other_guard[] = {0x4c98078800000006ull, (tex_b & ~(0xfull << 16)) | (8ull << 16)};
    code = translate(other_guard, 2, ST_FS, &info, &words, 1);
    check_refused(code, &info, "untracked texture handle", "a handle loaded under P0 doesn't reach a use under !P0");
    free(code);
    /* A handle loaded before a forward branch reaches its target, unless the skipped code writes it. */
    const u64 branch = 0xe24000000000000full | (16ull << 20); /* @P0 BRA over the next instruction */
    const u64 across[] = {0x4c98078800070006ull, branch, FMUL_KEEP | 5, tex_b};
    code = translate(across, 4, ST_FS, &info, &words, 1);
    check_translated(code, &info, "a handle loaded before a forward branch reaches the branch target");
    free(code);
    /* IPA of a[0x7c] reads the interpolated 1/w as it is, FragCoord.w */
    {
        const u64 position_w[] = {0xe003ff87cff7ff1dull};
        code = translate(position_w, 1, ST_FS, &info, &words, 1);
        check(code && !info.unsupported && count_opcode(code, words, 136, 0xffffffffu) == 0,
              "IPA of a[0x7c] reads FragCoord.w without a division");
        free(code);
    }
    const u64 skipped_write[] = {0x4c98078800070006ull, branch, FMUL_KEEP | 6, tex_b};
    code = translate(skipped_write, 4, ST_FS, &info, &words, 1);
    check_refused(code, &info, "untracked texture handle",
                  "a handle written in code a branch skips doesn't reach the target");
    free(code);
    /* Both sides of a forward branch may independently assemble the same
       split descriptor. The merge is safe only when every source agrees. */
    const u64 choose = 0xe24000000000000full | (32ull << 20);
    const u64 merge = 0xe24000000007000full | (24ull << 20);
    const u64 pair_or = 0x4c47020800070006ull | (6ull << 8) | (0x100ull << 20);
    const u64 joined[] = {choose, 0x4c98078800070006ull, pair_or, merge, 0x4c98078800070006ull, pair_or, tex_b};
    code = translate(joined, 7, ST_FS, &info, &words, 1);
    check_translated(code, &info, "equal split handles merge across a forward branch");
    check(code && info.ntex == 1 && info.tex[0].bl2 && info.tex[0].bl_cbuf == 2 && info.tex[0].cbuf_off == 0 &&
              info.tex[0].bl2_cbuf == 2 && info.tex[0].bl2_off == 0x100,
          "a merged split handle retains both descriptor sources");
    free(code);
    u64 disagree[7];
    memcpy(disagree, joined, sizeof disagree);
    disagree[4] |= 1ull << 20;
    code = translate(disagree, 7, ST_FS, &info, &words, 1);
    check_refused(code, &info, "untracked texture handle", "different textures at a branch merge remain unsupported");
    free(code);
    memcpy(disagree, joined, sizeof disagree);
    disagree[5] |= 1ull << 20;
    code = translate(disagree, 7, ST_FS, &info, &words, 1);
    check_refused(code, &info, "untracked texture handle", "different samplers at a branch merge remain unsupported");
    free(code);
    memcpy(disagree, joined, sizeof disagree);
    disagree[1] = FMUL_KEEP | 5;
    code = translate(disagree, 7, ST_FS, &info, &words, 1);
    check_refused(code, &info, "untracked texture handle", "a branch path with no handle cannot reach the sample");
    free(code);
    /* One game's blur: the handle loaded before a loop, sampled in its body, @P0 BRA back to the sample. It reaches
       every iteration unless the body writes it after the sample */
    const u64 back = 0xe24000000000000full | ((u64)(0x1000000 - 32) << 20); /* @P0 BRA to the TEX.B */
    const u64 loop[] = {0x4c98078800070006ull, tex_b, FMUL_KEEP | 5, back};
    code = translate(loop, 4, ST_FS, &info, &words, 1);
    check_translated(code, &info, "a handle loaded before a loop reaches the sample in its body");
    free(code);
    /* A conditional exit kills only the lanes that take it. The other lanes carry the handle into the loop. */
    const u64 conditional_exits[] = {EXIT & ~(7ull << 16), (EXIT & ~31ull) | 13ull};
    for (size_t i = 0; i < sizeof conditional_exits / sizeof *conditional_exits; i++) {
        const u64 after_exit[] = {0x4c98078800070006ull, conditional_exits[i], tex_b, FMUL_KEEP | 5, back};
        code = translate(after_exit, 5, ST_FS, &info, &words, 1);
        check_translated(code, &info, "a handle survives a predicate or condition-code exit before a loop");
        check(code && info.ntex == 1 && info.tex[0].bindless && info.tex[0].bl_cbuf == 2 && info.tex[0].cbuf_off == 0,
              "the surviving lanes retain the original handle source");
        free(code);
        const u64 store = 0xeb20000a00f70c10ull | (6ull << 39);
        const u64 store_after_exit[] = {0x4c98078800070006ull, conditional_exits[i], store, FMUL_KEEP | 5, back};
        code = translate(store_after_exit, 5, ST_VS, &info, &words, 1);
        check_translated(code, &info, "a vertex image-store handle survives a conditional exit before a loop");
        check(code && info.nimg == 1 && info.img[0].bindless && info.img[0].bl_cbuf == 2 && info.img[0].cbuf_off == 0 &&
                  info.img[0].type == 5,
              "the vertex loop retains its 3D storage image descriptor");
        free(code);
        const u64 overwritten[] = {0x4c98078800070006ull, conditional_exits[i], store, FMUL_KEEP | 6, back};
        code = translate(overwritten, 5, ST_VS, &info, &words, 1);
        check_refused(code, &info, "untracked surface handle",
                      "a conditional exit does not hide a handle overwritten on the loop back edge");
        free(code);
    }
    /* A conditional KIL demotes only the lanes that take it, as a conditional EXIT ends only those. The other lanes
       carry the handle past it: into a loop, to a forward branch's target, and through a MOV to another register. */
    const u64 kil = 0xe33000000007000full;
    const u64 conditional_kills[] = {(kil & ~(0xfull << 16)) | (1ull << 16), (kil & ~31ull) | 13ull};
    const u64 tex_b_r7 = (tex_b & ~(0xffull << 20)) | (7ull << 20);
    for (size_t i = 0; i < sizeof conditional_kills / sizeof *conditional_kills; i++) {
        const u64 kill_loop[] = {0x4c98078800070006ull, conditional_kills[i], tex_b, FMUL_KEEP | 5, back};
        code = translate(kill_loop, 5, ST_FS, &info, &words, 1);
        check_translated(code, &info, "a handle survives a predicate or condition-code KIL before a loop");
        check(code && info.ntex == 1 && info.tex[0].bindless && info.tex[0].bl_cbuf == 2 && info.tex[0].cbuf_off == 0,
              "the lanes a conditional KIL keeps retain the original handle source");
        free(code);
        const u64 kill_across[] = {0x4c98078800070006ull, conditional_kills[i], branch, FMUL_KEEP | 5,
                                   0x5c98078000670007ull, tex_b_r7};
        code = translate(kill_across, 6, ST_FS, &info, &words, 1);
        check_translated(code, &info, "a handle crosses a conditional KIL and a forward branch, then a MOV");
        check(code && info.ntex == 1 && info.tex[0].bindless && info.tex[0].bl_cbuf == 2 && info.tex[0].cbuf_off == 0,
              "the handle after a conditional KIL and a MOV is the constant buffer's");
        free(code);
    }
    const u64 loop_rewrites[] = {0x4c98078800070006ull, tex_b, FMUL_KEEP | 6, back};
    code = translate(loop_rewrites, 4, ST_FS, &info, &words, 1);
    check_refused(code, &info, "untracked texture handle",
                  "a handle the loop body writes after the sample doesn't reach the next iteration");
    free(code);
    /* a handle loaded and sampled under P0 with another predicate written between, and with P0 itself written */
    const u64 fsetp_p4 = 0x4bb183900527ff27ull, fsetp_p0 = 0x4bb1838c30f7ff07ull;
    const u64 other_predicate[] = {0x4c98078800000006ull, fsetp_p4, tex_b & ~(0xfull << 16)};
    code = translate(other_predicate, 3, ST_FS, &info, &words, 1);
    check_translated(code, &info, "another predicate written between a guarded load and its use keeps the handle");
    free(code);
    const u64 guard_rewritten[] = {0x4c98078800000006ull, fsetp_p0, tex_b & ~(0xfull << 16)};
    code = translate(guard_rewritten, 3, ST_FS, &info, &words, 1);
    check_refused(code, &info, "untracked texture handle", "the guard written between a guarded load and its use");
    free(code);
    /* A saved predicate is a value, even after its original register is reused.
       Recomputing an equal compare is safe only if its operands stay intact. */
    const u64 save_p0 = 0x50880380e007000aull, restore_p0 = 0x5b6b03800ff70a07ull;
    const u64 pred_tex = tex_b & ~(0xfull << 16), load_p0 = 0x4c98078800000006ull;
    const u64 saved_guard[] = {load_p0, save_p0, fsetp_p0, restore_p0, FMUL_KEEP | 10, pred_tex};
    code = translate(saved_guard, 6, ST_FS, &info, &words, 1);
    check_translated(code, &info, "a saved guard survives predicate and saved-register reuse");
    free(code);
    const u64 renamed_guard[] = {load_p0, save_p0, fsetp_p0, (restore_p0 & ~63ull) | 15, pred_tex | (1ull << 16)};
    code = translate(renamed_guard, 5, ST_FS, &info, &words, 1);
    check_translated(code, &info, "a restored guard can use another predicate register");
    free(code);
    const u64 lost_guard[] = {load_p0, save_p0, fsetp_p0, FMUL_KEEP | 10, restore_p0, pred_tex};
    code = translate(lost_guard, 6, ST_FS, &info, &words, 1);
    check_refused(code, &info, "untracked texture handle", "overwriting a saved guard before restoration is refused");
    free(code);
    const u64 restore_inverse = (restore_p0 & ~(7ull << 49)) | (2ull << 49);
    const u64 inverse_guard[] = {load_p0, save_p0, fsetp_p0, restore_inverse, pred_tex};
    code = translate(inverse_guard, 5, ST_FS, &info, &words, 1);
    check_refused(code, &info, "untracked texture handle", "an inverted restored guard cannot read the handle");
    free(code);
    const u64 inverse_use[] = {load_p0, save_p0, fsetp_p0, restore_inverse, pred_tex | (8ull << 16)};
    code = translate(inverse_use, 5, ST_FS, &info, &words, 1);
    check_translated(code, &info, "inverting the restored guard and the use preserves the original guard");
    free(code);
    const u64 compare_p0 = 0x4b62038c00370307ull, compare_p1 = (compare_p0 & ~63ull) | 15;
    const u64 repeated_compare[] = {compare_p0, load_p0, compare_p1, pred_tex | (1ull << 16)};
    code = translate(repeated_compare, 4, ST_FS, &info, &words, 1);
    check_translated(code, &info, "equal integer compares guard the same handle");
    free(code);
    const u64 changed_compare[] = {compare_p0, load_p0, FMUL_KEEP | 3, compare_p1, pred_tex | (1ull << 16)};
    code = translate(changed_compare, 5, ST_FS, &info, &words, 1);
    check_refused(code, &info, "untracked texture handle", "a changed compare operand invalidates guard equivalence");
    free(code);
    /* SSY to the block that samples, the handle loaded right after it: the token's SYNCs come after the load. A SYNC
       between the SSY and the load goes around it */
    const u64 ssy = 0xe290000000000000ull | (40ull << 20), sync_not_p0 = 0xf0f800000008000full,
              sync = 0xf0f800000007000full;
    const u64 ssy_load[] = {ssy, 0x4c98078800070006ull, sync_not_p0, FMUL_KEEP | 5, sync, tex_b};
    code = translate(ssy_load, 6, ST_FS, &info, &words, 1);
    check_translated(code, &info, "a handle loaded right after an SSY reaches its reconvergence point");
    free(code);
    const u64 ssy_around[] = {ssy, sync_not_p0, 0x4c98078800070006ull, FMUL_KEEP | 5, sync, tex_b};
    code = translate(ssy_around, 6, ST_FS, &info, &words, 1);
    check_refused(code, &info, "untracked texture handle", "a SYNC before the load goes around it");
    free(code);
}

/* a tessellation or geometry program: the instructions after an SPH with words 2 to 4 set */
static u32* translate_stage(const u64* instructions, int count, int stage, const u32* sph, u32 primitive,
                            ShaderInfo* info, size_t* words) {
    size_t slots;
    u64* code = guest_program(instructions, count, sph, &slots);
    if (!code) return NULL;
    memset(info, 0, sizeof *info);
    info->rt_int_mask = primitive;
    if (stage == ST_TCS) info->clip_enable = SNX_SH_NOT_LAST;
    u32* result = spirv_of(code, slots * sizeof *code, stage, info, words, 1, 1, 1, 1);
    free(code);
    return result;
}

/* edenprobe's tessellated patch: a control program passing its 4 vertices through and writing every level from
   vertex 0's attribute, and an evaluation program mixing the corners by the tessellation coordinate and reading
   the outer level back. Then a geometry program passing a triangle's first vertex on as a strip of one. */
static void between_stages(void) {
    const u64 control[] = {
        0xf0c8000001170004ull, 0xf0c8000001d70000ull, 0x384700000ff70001ull, 0x3800000081070000ull,
        0x5b00020000170002ull, 0x5b007fa800170001ull, 0x5b30011800170000ull, 0xefd0000000070005ull,
        0xefd982800707ff00ull, 0xeff1ff800707ff00ull, 0xefd982800907ff00ull, 0xeff1ff800907ff00ull,
        0xe29000000c800000ull, 0x5b6403800ff70407ull, 0xf0f800000008000full, 0xf0c8000001d70000ull,
        0x384700000ff70001ull, 0x3800000081070000ull, 0x5b007f8000170002ull, 0x5b007fa800170001ull,
        0x5b30011800170000ull, 0xefd0000000070000ull, 0xefd8000009c7ff00ull, 0x386810437f070000ull,
        0x5ca8140000070a00ull, 0xeff07f808007ff00ull, 0xeff07f808047ff00ull, 0xeff07f808087ff00ull,
        0xeff07f8080c7ff00ull, 0xeff07f808107ff00ull, 0xeff07f808147ff00ull, 0xf0f800000007000full,
    };
    const u64 evaluation[] = {
        0xf0c8000000070002ull, 0xefd881012f07ff00ull, 0xf0c8000001d70002ull, 0x384700000ff70203ull,
        0xeff0ff800807ff00ull, 0x3800000081070202ull, 0x010000000017f004ull, 0x5b00020000370204ull,
        0x5b007fa800370211ull, 0x5b30021801170204ull, 0xefd0000000070408ull, 0xefd984000707ff04ull,
        0x5b007f8000370208ull, 0x5b30041801170208ull, 0xefd000000007080cull, 0xefd986000707ff08ull,
        0x5c58300000870404ull, 0x59a004000047000dull, 0x5c58300000970504ull, 0x59a004800047000eull,
        0x5c58300000a70604ull, 0x59a005000047000full, 0x5c58300000b70704ull, 0x59a0058000470010ull,
        0x010000000027f004ull, 0x5b00020000370204ull, 0x5b30021801170204ull, 0xefd0000000070408ull,
        0xefd984000707ff04ull, 0x010000000037f008ull, 0x5b00040000370203ull, 0x5b30019801170202ull,
        0xefd0000000070202ull, 0xefd981000707ff08ull, 0x5c58300000870402ull, 0x59a0040000270002ull,
        0xefd87f808007ff08ull, 0x5c58300000970503ull, 0x59a0048000370003ull, 0x5c58300000a70604ull,
        0x59a0050000470006ull, 0xefd806000987ff0aull, 0x5c58300000b70704ull, 0x59a0058000470007ull,
        0x5c58300000d70202ull, 0x59a0068000270104ull, 0x5c58300000e70302ull, 0x59a0070000270105ull,
        0x5c58300000f70602ull, 0x59a0078000270106ull, 0x5c58300001070702ull, 0x59a0080000270107ull,
        0xefd886000907ff02ull, 0x1e23b8080817080bull, 0xeff1ff800707ff04ull, 0x5c98078000270008ull,
        0x5c98078000370009ull, 0xeff1ff800907ff08ull,
    };
    const u64 geometry[] = {
        0xefd9ff800707ff00ull, /* ALD R0, a[0x70] of vertex RZ, 4 words */
        0xeff1ff800707ff00ull, /* AST a[0x70], R0, 4 words */
        0xfbe000800ff70404ull, /* OUT.EMIT R4, R4, RZ */
        0xfbe001000ff70404ull, /* OUT.CUT R4, R4, RZ */
    };
    const u32 control_sph[3] = {4u << 24, 0, 0};         /* 4 threads, each an output vertex */
    const u32 geometry_sph[3] = {1u << 24, 7u << 24, 3}; /* 1 invocation, a triangle strip of up to 3 vertices */
    const u32 none[3] = {0, 0, 0};
    struct {
        const u64* instructions;
        int count, stage;
        const u32* sph;
        u32 primitive, mode;
    } cases[] = {{control, (int)(sizeof control / sizeof *control), ST_TCS, control_sph, 0, 26},
                 /* quads, equal spacing, connected counterclockwise triangles */
                 {evaluation, (int)(sizeof evaluation / sizeof *evaluation), ST_TES, none, 0x302, 24},
                 {geometry, 4, ST_GS, geometry_sph, 3, 29}};
    for (int i = 0; i < 3; i++) {
        ShaderInfo info;
        size_t words = 0;
        u32* code = translate_stage(cases[i].instructions, cases[i].count, cases[i].stage, cases[i].sph,
                                    cases[i].primitive, &info, &words);
        check_translated(code, &info, "a tessellation or geometry program translates");
        if (code) {
            check(count_opcode(code, words, 16, cases[i].mode) == 1, "its execution mode says its primitive");
            if (i == 2) {
                check(count_opcode(code, words, 218, 0xffffffffu) == 1, "OUT.EMIT emits a vertex");
                check(count_builtin(code, words, 7) == 2, "geometry passes PrimitiveId to fragment shaders");
            }
            save_fixture(stage_paths[i], code, words);
        }
        free(code);
    }
    /* Citron's Peach geometry fix: SR_INVOCATION_INFO carries the input
       primitive's vertex count in bits 16-23, including adjacency vertices. */
    for (u32 vertices = 1; vertices <= 6; vertices++) {
        if (vertices == 5) continue;
        const u64 invocation[] = {0xf0c8000001d70000ull,  /* S2R R0, SR_INVOCATION_INFO */
                                  0xeff07f800807ff00ull}; /* AST a[0x80], R0 */
        ShaderInfo info;
        size_t words = 0;
        u32* code = translate_stage(invocation, 2, ST_GS, geometry_sph, vertices, &info, &words);
        check_translated(code, &info, "geometry invocation info translates for every input topology");
        if (code) {
            check(has_constant(code, words, vertices << 16), "geometry invocation info has the actual vertex count");
            check(!has_constant(code, words, 0x00ff0000), "geometry invocation info never uses the stub value");
        }
        free(code);
    }
}

/* whether the module decorates a variable BuiltIn builtin */
static int has_builtin(const u32* code, size_t words, u32 builtin) { return count_builtin(code, words, builtin) > 0; }

/* the layer takes ShaderLayer (the device's shaderOutputLayer) when a vertex program writes it, and a fragment
   program reads it with Geometry (geometryShader) or ShaderLayer. Without the capability the write is dropped and
   the read is 0, never a module the device can't take */
static void layer_capabilities(void) {
    const u64 write = 0xeff07f800647ff00ull; /* AST a[0x64], R0 */
    const u64 read = 0xe003ff864ff7ff00ull;  /* IPA R0, a[0x64] */
    for (int out = 0; out < 2; out++) {
        g_shader_output_layer = out;
        ShaderInfo info;
        size_t words = 0;
        u32* code = translate(&write, 1, ST_VS, &info, &words, 1);
        check_translated(code, &info, "a vertex program writing the layer translates");
        if (code) {
            check((count_opcode(code, words, 17, 69) == 1) == out, "a layer write takes ShaderLayer when it can");
            check(has_builtin(code, words, 9) == out, "a layer write is dropped without ShaderLayer");
            check(count_opcode(code, words, 17, 2) == 0, "a layer write never takes Geometry");
        }
        free(code);
        for (int geometry = 0; geometry < 2; geometry++) {
            g_shader_geometry = geometry;
            code = translate(&read, 1, ST_FS, &info, &words, 1);
            check_translated(code, &info, "a fragment program reading the layer translates");
            if (code) {
                check(count_opcode(code, words, 17, 2) == geometry, "a layer read takes Geometry when it can");
                check(count_opcode(code, words, 17, 69) == (!geometry && out),
                      "a layer read takes ShaderLayer without Geometry");
                check(has_builtin(code, words, 9) == (geometry || out), "a layer read is 0 without either");
            }
            free(code);
        }
    }
    g_shader_output_layer = g_shader_geometry = 1;
}

static void viewport_capabilities(void) {
    const u64 write = 0xeff07f800687ff00ull; /* AST a[0x68], R0 */
    const u32 geometry_sph[3] = {1u << 24, 7u << 24, 3};
    const u32 none[3] = {0};
    int stages[3] = {ST_VS, ST_TES, ST_GS};

    for (int i = 0; i < 3; i++) {
        ShaderInfo info = {0};
        size_t words = 0;
        u32* code = translate_stage(&write, 1, stages[i], i == 2 ? geometry_sph : none, i == 2 ? 3 : 0, &info, &words);
        check_translated(code, &info, "viewport index stores translate in vertex, evaluation and geometry stages");
        if (code) {
            check(info.writes_viewport, "viewport writes request indexed dynamic state");
            check(has_builtin(code, words, 10), "viewport index has its own scalar output");
            check(!has_builtin(code, words, 0), "viewport index is not a position component");
            check(count_opcode(code, words, 17, i == 2 ? 57 : 70) == 1,
                  "viewport output declares the stage capability");
        }
        free(code);
    }
    g_shader_viewport_index = 0;
    ShaderInfo info = {0};
    size_t words = 0;
    u32* code = translate(&write, 1, ST_VS, &info, &words, 1);
    check_refused(code, &info, "viewport index output unavailable", "unsupported devices refuse viewport output");
    free(code);
    g_shader_viewport_index = 3;
}

/* The viewport transform off (SNX_SH_WINDOW_XY): the last stage before rasterization takes its position's x and y
   from the surface clip's pixels, dividing 2 by the clip's width and height (from the push constant's w) where it
   ends or emits a vertex, which takes two OpFDiv more than without the key. A stage before the last one leaves its
   position as written. The vertex, evaluation and geometry fixtures go through spirv-val */
static void window_positions(void) {
    const u64 vertex[] = {0xeff1ff800707ff00ull};   /* AST a[0x70], R0..R3: the position */
    const u64 geometry[] = {0xefd9ff800707ff00ull,  /* ALD R0..R3, a[0x70] of vertex RZ */
                            0xeff1ff800707ff00ull,  /* AST a[0x70], R0..R3 */
                            0xfbe000800ff70404ull,  /* OUT.EMIT R4, R4, RZ */
                            0xfbe001000ff70404ull}; /* OUT.CUT R4, R4, RZ */
    const u32 geometry_sph[3] = {1u << 24, 7u << 24, 3}, none[3] = {0};
    const int stages[3] = {ST_VS, ST_TES, ST_GS};
    const u32 keys[3] = {0, SNX_SH_WINDOW_XY, SNX_SH_WINDOW_XY | SNX_SH_NOT_LAST};
    for (int i = 0; i < 3; i++) {
        int divisions[3] = {0};
        for (int k = 0; k < (i == 2 ? 2 : 3); k++) { /* a geometry program is the last stage */
            size_t slots, words = 0;
            const u32* sph = i == 0 ? NULL : i == 1 ? none : geometry_sph;
            u64* code = guest_program(i == 2 ? geometry : vertex, i == 2 ? 4 : 1, sph, &slots);
            ShaderInfo info = {0};
            info.rt_int_mask = i == 2 ? 3 : i ? 0x302 : 0; /* a triangle in; quads, equal spacing, counterclockwise */
            info.clip_enable = keys[k];
            u32* spv = code ? spirv_of(code, slots * sizeof *code, stages[i], &info, &words, 1, 1, 1, 1) : NULL;
            check_translated(spv, &info,
                             "a position from pixels translates in the vertex, evaluation and geometry stages");
            if (spv) {
                divisions[k] = count_opcode(spv, words, 136, 0xffffffffu); /* OpFDiv */
                if (k == 1) save_fixture(window_paths[i], spv, words);
            }
            free(spv);
            free(code);
        }
        check(divisions[1] == divisions[0] + 2, "the last stage divides by the surface clip's width and height");
        check(i == 2 || divisions[2] == divisions[0], "a stage before the last one leaves its position as written");
    }
}

/* Loads from a constant buffer (a Uniform variable bound below 18) by result type: u32 or anything else. */
static void cbuf_load_types(const u32* code, size_t words, int* integer, int* other) {
    u32 bound = words > 5 ? code[3] : 0, uint_type = 0;
    u32* binding = calloc((size_t)bound + 1, sizeof *binding); /* binding + 1, 0 for none */
    u8* cbuf = calloc((size_t)bound + 1, 1);                   /* a constant buffer variable or a pointer into one */
    *integer = *other = 0;
    for (size_t i = 5; binding && cbuf && i < words;) {
        u32 width = code[i] >> 16, op = code[i] & 0xffff;
        if (!width || i + width > words) break;
        if (op == 21 && width == 4 && code[i + 2] == 32 && !code[i + 3]) uint_type = code[i + 1];
        if (op == 71 && width == 4 && code[i + 2] == 33 && code[i + 1] < bound) binding[code[i + 1]] = code[i + 3] + 1;
        if (op == 59 && width >= 4 && code[i + 3] == 2 && code[i + 2] < bound && binding[code[i + 2]] &&
            binding[code[i + 2]] <= 18)
            cbuf[code[i + 2]] = 1;
        if (op == 65 && width >= 4 && code[i + 3] < bound && cbuf[code[i + 3]] && code[i + 2] < bound)
            cbuf[code[i + 2]] = 1;
        if (op == 61 && width >= 4 && code[i + 3] < bound && cbuf[code[i + 3]])
            ++*(code[i + 1] == uint_type ? integer : other);
        i += width;
    }
    free(binding);
    free(cbuf);
}

/* A constant buffer word is loaded as an integer, whatever reads it: a float load may flush integers whose bits look
   like a denormal, such as the 0x140-0x280 entries of a BRX jump table, which then sends the program back to its
   start forever. */
static void cbuf_integer_loads(void) {
    static const u64 graphics[] = {
        0x4C98079008070005ull, /* MOV R5, c[4][0x200] */
        0x3848000000270000ull, /* SHL R0, R0, 2 */
        0xEF94001000070000ull, /* LDC R0, c[1][R0] */
        0x4C68101001470C00ull, /* FMUL R0, R12, c[4][0x50] */
    };
    static const int stages[] = {ST_VS, ST_FS, ST_CS};
    for (size_t i = 0; i < sizeof stages / sizeof *stages; i++) {
        ShaderInfo info;
        size_t words = 0;
        int integer, other;
        u32* code = translate(graphics, 4, stages[i], &info, &words, 1);
        check_translated(code, &info, "constant buffer reads translate");
        if (code) {
            cbuf_load_types(code, words, &integer, &other);
            check(integer >= 3 && !other, "constant buffer words are loaded as integers");
        }
        free(code);
    }
    /* the compute translator for straight-line programs */
    u64 compute[64] = {0};
    compute[10] = SCHED;
    compute[11] = 0x4C98078000470004ull; /* MOV R4, c[0][0x10] */
    compute[12] = EXIT;
    ShaderInfo info;
    memset(&info, 0, sizeof info);
    info.cs_local[0] = info.cs_local[1] = info.cs_local[2] = 1;
    size_t words = 0;
    int integer, other;
    u32* code = compute_spirv_of(compute, sizeof compute, &info, &words, 1, 1);
    if (!code || info.unsupported) fprintf(stderr, "compute constant buffer read: %s\n", info.first_unsupported);
    check(code != NULL, "compute constant buffer read translates");
    if (code) {
        cbuf_load_types(code, words, &integer, &other);
        check(integer == 1 && !other, "compute constant buffer words are loaded as integers");
    }
    free(code);
}

/* TMML.B with its handle in Rb, on a cube array (the layer before the three coordinates), and IPA reading the point
   sprite's S and T (a[0x2e0] and a[0x2e4]), both as a game's fragment programs use them */
static void fragment_queries(void) {
    static const u64 tmml[] = {
        HANDLE_CBUF_R4, /* MOV R4, c[2][0x168]: the handle */
        0xdf60000000000000ull | 3ull << 31 | 7ull << 28 | 4ull << 20 | 7ull << 16 | 8ull << 8, /* TMML.B R0, R8, R4 */
    };
    static const u64 sprite[] = {
        0xe003ffae0ff7ff00ull, /* IPA R0, a[0x2e0] */
        0xe003ffae4ff7ff01ull, /* IPA R1, a[0x2e4] */
    };
    ShaderInfo info;
    size_t words = 0;
    u32* code = translate(tmml, 2, ST_FS, &info, &words, 0);
    check_translated(code, &info, "cube-array TMML.B translates");
    check(code && info.ntex == 1 && info.tex[0].bindless && info.tex[0].type == 3 && info.tex[0].array,
          "TMML.B queries the cube array its handle names");
    if (code) {
        check(count_opcode(code, words, 105, 0xffffffffu) == 1, "TMML.B emits OpImageQueryLod");
        save_fixture(query_paths[0], code, words);
    }
    free(code);
    const u64 seeded_cube[] = {tmml[0],           mov32i(8, 0x100),  mov32i(9, 0x200),
                               mov32i(10, 0x300), mov32i(11, 0x400), tmml[1]};
    code = translate(seeded_cube, 6, ST_FS, &info, &words, 0);
    check_translated(code, &info, "TMML.B cube array with distinct coordinates translates");
    if (code)
        check(seeded_register_feeds(code, words, 0x200, 80, 3) && seeded_register_feeds(code, words, 0x300, 80, 4) &&
                  seeded_register_feeds(code, words, 0x400, 80, 5) && !seeded_register_feeds(code, words, 0x100, 80, 3),
              "TMML.B skips the array layer and takes cube coordinates from Ra+1 through Ra+3");
    free(code);
    const u64 tmml_3d[] = {tmml[0], (tmml[1] & ~(7ull << 28)) | (4ull << 28)};
    code = translate(tmml_3d, 2, ST_FS, &info, &words, 0);
    check_translated(code, &info, "3D TMML.B translates");
    check(code && !info.unsupported && info.ntex == 1 && info.tex[0].type == 2 && !info.tex[0].array,
          "TMML.B queries a 3D texture");
    if (code) check(count_opcode(code, words, 105, 0xffffffffu) == 1, "3D TMML.B emits OpImageQueryLod");
    free(code);
    const u32 bound_types[] = {0, 4, 6}; /* 1D, 3D, cube */
    for (size_t i = 0; i < sizeof bound_types / sizeof *bound_types; i++) {
        u64 bound = (tmml[1] & ~((7ull << 28) | (0xfffull << 36))) | (u64)bound_types[i] << 28;
        bound = (bound & ~(0xffull << 20)) | 4ull << 20; /* bound TMML has no Rb handle */
        bound = (bound & ~(0xffffull << 48)) | (0xdf58ull << 48);
        code = translate(&bound, 1, ST_FS, &info, &words, 0);
        check_translated(code, &info, "bound TMML dimension translates");
        if (code) {
            check(info.ntex == 1 && !info.tex[0].bindless && info.tex[0].type == bound_types[i] / 2 &&
                      count_opcode(code, words, 105, 0xffffffffu) == 1,
                  "bound TMML preserves its dimension in OpImageQueryLod");
        }
        free(code);
    }
    code = translate(sprite, 2, ST_FS, &info, &words, 0);
    check_translated(code, &info, "IPA reads the point sprite's coordinates");
    if (code) {
        check(has_builtin(code, words, 16) == 1, "the point coordinate is the PointCoord built-in");
        check(count_opcode(code, words, 127, 0xffffffffu) == 1, "the point sprite's T is flipped by its origin");
        save_fixture(query_paths[1], code, words);
    }
    free(code);
    code = translate(&sprite[1], 1, ST_FS, &info, &words, 0);
    check_translated(code, &info, "point sprite T translates alone");
    if (code)
        check(sprite_t_selects_flipped_on_true(code, words),
              "point sprite T selects the flipped value for the flip condition");
    free(code);
    /* the fragment's y counts up from the surface clip's bottom in a lower-left window, both pushed; x stays */
    static const u64 position[] = {
        0xe003ff870ff7ff00ull, /* IPA R0, a[0x70] */
        0xe003ff874ff7ff01ull, /* IPA R1, a[0x74] */
    };
    code = translate(&position[1], 1, ST_FS, &info, &words, 0);
    check_translated(code, &info, "IPA reads the fragment's y");
    if (code)
        check(frag_y_selects_clip_less_y(code, words),
              "the fragment's y is the clip height less it when the window is lower left");
    free(code);
    code = translate(position, 1, ST_FS, &info, &words, 0);
    check_translated(code, &info, "IPA reads the fragment's x");
    if (code) check(!frag_y_selects_clip_less_y(code, words), "the fragment's x keeps its value in any window");
    free(code);
}

/* Forms compared with Eden's translator: the half-float immediates, MEMBAR, ISCADD32I, OUT on any stream, KIL as
   a demote and the sparse predicate, and the branch and program forms that are refused rather than mistranslated */
static void eden_forms(void) {
    static const u64 halves[] = {
        0x2c00000000070002ull | 0x3c003c00ull << 20,                     /* HADD2_32I R2, R0, (1, 1) */
        0x2a00000000070003ull | 2ull << 55 | 0x3c003c00ull << 20,        /* HMUL2_32I.FMZ R3, R0, (1, 1) */
        0x2800000000070004ull | 0x3c003c00ull << 20,                     /* HFMA2_32I R4, R0, (1, 1), R4 */
        0x7000000000070005ull | 2ull << 57 | 6ull << 39 | 0xf0ull << 20, /* HFMA2.FMZ R5, R0, imm, R6 */
        0x1400000000070100ull | 3ull << 53 | 0x1234ull << 20,            /* ISCADD32I R0, R1, 0x1234, 3 */
        0xef98000000070100ull,                                           /* MEMBAR.GL */
        0xc1020087a0970a00ull,                                           /* TEX.LB R0, R10, R9 with sparse P0 */
    };
    static const u64 geometry[] = {
        0xefd9ff800707ff00ull, /* ALD R0, a[0x70] of vertex RZ, 4 words */
        0xeff1ff800707ff00ull, /* AST a[0x70], R0, 4 words */
        0xfbe0008000570404ull, /* OUT.EMIT R4, R4, R5: the stream from a register */
        0xebe0010400470404ull, /* OUT.CUT R4, R4, c[1][0x10] */
        0xf6e0008000170404ull, /* OUT.EMIT R4, R4, 1: stream 1, which isn't rasterized */
    };
    static const u32 geometry_sph[3] = {1u << 24, 7u << 24, 3};
    ShaderInfo info;
    size_t words = 0;
    u32* code = translate(halves, 7, ST_FS, &info, &words, 0);
    check_translated(code, &info, "the half-float immediates, ISCADD32I, MEMBAR and a sparse TEX translate");
    if (code) {
        check(count_opcode(code, words, 225, 0xffffffffu) == 1, "MEMBAR is a memory barrier");
        check(count_opcode(code, words, 180, 0xffffffffu) >= 8,
              "HMUL2_32I and HFMA2_I FMZ check both half lanes for zero factors");
        save_fixture(opcode_paths[0], code, words);
    }
    free(code);
    const u64 iscadd_shift = halves[4];
    code = translate(&iscadd_shift, 1, ST_FS, &info, &words, 0);
    check_translated(code, &info, "ISCADD32I with shift 3 translates");
    if (code) check(shift_uses_amount(code, words, 3), "ISCADD32I uses bits 53-57 for its shift");
    free(code);
    /* A conditional EXIT after the TEX ends its block, where the block's last values are stored: the sparse
       predicate's among them */
    u64 sparse[8], no_sparse[8];
    memcpy(sparse, halves, sizeof halves);
    sparse[7] = 0xE30000000001000Full; /* @P1 EXIT */
    code = translate(sparse, 8, ST_FS, &info, &words, 0);
    int sparse_stores = code ? count_opcode(code, words, 62, 0xffffffffu) : -1;
    free(code);
    memcpy(no_sparse, sparse, sizeof no_sparse);
    no_sparse[6] = (no_sparse[6] & ~(7ull << 51)) | (7ull << 51);
    code = translate(no_sparse, 8, ST_FS, &info, &words, 0);
    check_translated(code, &info, "TEX with no sparse predicate translates");
    if (code)
        check(sparse_stores == count_opcode(code, words, 62, 0xffffffffu) + 1,
              "a sparse TEX writes its predicate false");
    free(code);
    for (int stage = ST_VS; stage <= ST_FS; stage += ST_FS) {
        u64 cta = 0xef98000000070000ull; /* MEMBAR.CTA */
        code = translate(&cta, 1, stage, &info, &words, 0);
        check_translated(code, &info, "graphics MEMBAR.CTA translates");
        if (code) check(memory_barrier_scope(code, words) == 1, "graphics MEMBAR.CTA uses Device scope");
        free(code);
    }
    code = translate_stage(geometry, 5, ST_GS, geometry_sph, 3, &info, &words);
    check_translated(code, &info, "OUT translates with a register, constant buffer or immediate stream");
    if (code) {
        check(count_opcode(code, words, 218, 0xffffffffu) == 2 && emit_only_on_zero_stream(code, words),
              "every rasterized OUT.EMIT is guarded by a zero-stream comparison");
        check(count_opcode(code, words, 219, 0xffffffffu) == 1, "OUT.CUT ends the primitive");
        save_fixture(opcode_paths[1], code, words);
    }
    free(code);
    const u64 conditional_out = 0xfbe0008000570404ull; /* OUT.EMIT with the stream in R5 */
    code = translate_stage(&conditional_out, 1, ST_GS, geometry_sph, 3, &info, &words);
    check_translated(code, &info, "register-selected geometry stream translates");
    if (code)
        check(count_opcode(code, words, 218, 0xffffffffu) == 1 && has_conditional_emit(code, words),
              "register-selected OUT emits only in a conditional block");
    if (code) check(emit_only_on_zero_stream(code, words), "OUT emits on the zero-stream branch");
    free(code);
    code = translate((const u64[]){0xe33000000007000full}, 1, ST_FS, &info, &words, 0); /* KIL */
    check(code != NULL, "KIL translates");
    if (code) {
        check(count_opcode(code, words, 252, 0xffffffffu) == 0 && count_opcode(code, words, 5380, 0xffffffffu) == 1 &&
                  count_opcode(code, words, 17, 5379) == 1,
              "KIL demotes the lane to a helper");
        save_fixture(opcode_paths[2], code, words);
    }
    free(code);
    code = translate((const u64[]){0xe24000000007002full}, 1, ST_FS, &info, &words, 0); /* BRA through c[] */
    check_refused(code, &info, "BRA", "a branch to a constant buffer target is refused");
    free(code);
    const u64 cbuf_flow[] = {0xe27000000007002full, 0xe29000000007002full, 0xe2a000000007002full,
                             0xe2b000000007002full}; /* PRET, SSY, PBK, PCNT */
    for (size_t i = 0; i < sizeof cbuf_flow / sizeof *cbuf_flow; i++) {
        code = translate(&cbuf_flow[i], 1, ST_FS, &info, &words, 0);
        check_refused(code, &info,
                      i == 0   ? "PRET"
                      : i == 1 ? "SSY"
                      : i == 2 ? "PBK"
                               : "PCNT",
                      "constant-buffer control target is refused");
        free(code);
    }
    {
        u64 program[64] = {0};
        const u32 passthrough = 1u << 24; /* geometry passthrough */
        set_sph(program, 0, passthrough);
        for (u32 i = 0; i < 3; i++) set_sph(program, 2 + i, geometry_sph[i]);
        program[10] = SCHED;
        program[11] = EXIT;
        memset(&info, 0, sizeof info);
        info.rt_int_mask = 3;
        code = spirv_of(program, sizeof program, ST_GS, &info, &words, 1, 1, 1, 1);
        check_translated(code, &info, "triangle passthrough translates");
        if (code) {
            check(count_opcode(code, words, 218, 0xffffffffu) == 3, "passthrough emits three vertices");
            check(count_opcode(code, words, 219, 0xffffffffu) == 1, "passthrough ends its primitive");
            save_fixture(passthrough_paths[0], code, words);
        }
        free(code);
        const u32 counts[] = {1, 2, 4, 6};
        for (u32 i = 0; i < 4; i++) {
            memset(&info, 0, sizeof info);
            info.rt_int_mask = counts[i];
            memset(info.gp_skip_mask, 0xff, sizeof info.gp_skip_mask);
            info.gp_skip_mask[0] &= ~(15u << 28); /* position */
            info.gp_skip_mask[1] &= ~5u;          /* only x and z of attribute 0 */
            code = spirv_of(program, sizeof program, ST_GS, &info, &words, 1, 1, 1, 1);
            check_translated(code, &info, "point, line and adjacency passthrough translate");
            if (code) {
                u32 expected = counts[i] == 4 ? 2 : counts[i] == 6 ? 3 : counts[i];
                check(count_opcode(code, words, 218, 0xffffffffu) == (int)expected,
                      "adjacency vertices are excluded from passthrough output");
                check(info.in_mask == 1 && info.gp_skip_mask[1] == ~5u,
                      "partial passthrough mask is retained and limits inputs");
                save_fixture(passthrough_paths[i + 1], code, words);
            }
            free(code);
        }
        program[11] = 0xeff07f800647ff00ull; /* AST Layer, R0: generated per primitive */
        program[12] = EXIT & ~(7ull << 16);  /* conditional exit */
        program[13] = EXIT;
        memset(&info, 0, sizeof info);
        info.rt_int_mask = 3;
        memset(info.gp_skip_mask, 0xff, sizeof info.gp_skip_mask);
        info.gp_skip_mask[0] &= ~(15u << 28);
        code = spirv_of(program, sizeof program, ST_GS, &info, &words, 1, 1, 1, 1);
        check_translated(code, &info, "passthrough preserves generated layer across conditional exits");
        if (code) {
            check(has_builtin(code, words, 9), "generated Layer is declared");
            check(count_opcode(code, words, 218, 0xffffffffu) == 6, "each exit emits a whole triangle");
            save_fixture(passthrough_paths[5], code, words);
        }
        free(code);
    }
}

static void pexit_flow(void) {
    u64 program[64] = {0};
    program[10] = SCHED;
    program[11] = 0xe23000000007000full | (16ull << 20); /* PEXIT to instruction 15 */
    program[12] = EXIT;
    program[13] = EXIT;
    program[14] = SCHED;
    program[15] = EXIT;
    u32 len = 0;
    hash_of(program, sizeof program, &len);
    check(len == 16 * 8, "PEXIT target extends the shader past an earlier EXIT");
    ShaderInfo info = {0};
    size_t words = 0;
    u32* code = spirv_of(program, sizeof program, ST_FS, &info, &words, 0, 1, 1, 1);
    check_translated(code, &info, "PEXIT branches to its target");
    if (code) check(count_opcode(code, words, 250, 0xffffffffu) > 0, "PEXIT EXIT tests the CRS token");
    if (code) save_fixture(pexit_path, code, words);
    free(code);
    program[12] = EXIT & ~(7ull << 16); /* EXIT under P0 */
    memset(&info, 0, sizeof info);
    code = spirv_of(program, sizeof program, ST_FS, &info, &words, 0, 1, 1, 1);
    check(!code && info.unsupported && !strcmp(info.first_unsupported, "conditional EXIT with PEXIT token"),
          "conditional EXIT with a PEXIT token is refused");
    free(code);
}

/* TXD: a 2D texture with a constant offset, TXD.B on a 2D array (the handle in Ra), and a 1D array whose varying
   signed nibble selects one of sixteen exact constant offsets. */
static void gradients(void) {
    const u64 offset_2d[] = {
        mov32i(6, 0x110000), /* offset (1, 1) */
        0xde00000000000000ull | 7ull << 51 | 1ull << 35 | 0x10ull << 36 | 0xfull << 31 | 2ull << 28 | 8ull << 20 |
            7ull << 16 | 4ull << 8, /* TXD R0, R4, R8, 2D, AOFFI */
    };
    static const u64 array_2d[] = {
        HANDLE_CBUF_R4, /* MOV R4, c[2][0x168] */
        0xde40000000000000ull | 7ull << 51 | 3ull << 31 | 3ull << 28 | 12ull << 20 | 7ull << 16 |
            4ull << 8, /* TXD.B R0, R4, R12 */
    };
    static const u64 dynamic_1d[] = {
        0xe003ff880ff7ff05ull, /* IPA R5, a[0x80]: a packed register only known at run time */
        0xde00000000000000ull | 7ull << 51 | 1ull << 35 | 0x10ull << 36 | 1ull << 31 | 1ull << 28 | 8ull << 20 |
            7ull << 16 | 4ull << 8, /* TXD R0, R4, R8, 1D array, AOFFI */
    };
    struct {
        const u64* instructions;
        u32 type, array, bindless;
    } cases[] = {{offset_2d, 1, 0, 0}, {array_2d, 1, 1, 1}, {dynamic_1d, 0, 1, 0}};
    for (int i = 0; i < 3; i++) {
        ShaderInfo info;
        size_t words = 0;
        u32* code = translate(cases[i].instructions, 2, ST_FS, &info, &words, 0);
        check_translated(code, &info, "TXD texture variant translates");
        check(code && info.ntex == 1 && info.tex[0].type == cases[i].type && info.tex[0].array == cases[i].array &&
                  info.tex[0].bindless == cases[i].bindless,
              "TXD samples the texture its type names");
        if (code) {
            check(count_opcode(code, words, 88, 0xffffffffu) == (i == 2 ? 16 : 1),
                  "TXD preserves the constant path and expands the complete varying1D offset domain");
            if (i == 2)
                check(count_opcode(code, words, 251, 0xffffffffu) == 1 && has_image_operands(code, words, 88, 12) &&
                          count_opcode(code, words, 103, 0xffffffffu) == 0 && count_opcode(code, words, 136, 0xffffffffu) == 0,
                      "varying TXD uses Grad with ConstOffset and no base-mip coordinate approximation");
            save_fixture(gradient_paths[i], code, words);
        }
        free(code);
    }
    const u64 derivative_order[] = {mov32i(8, 0x1111), mov32i(9, 0x2222), mov32i(10, 0x3333), mov32i(11, 0x4444),
                                    offset_2d[1]};
    ShaderInfo derivative_info;
    size_t derivative_words = 0;
    u32* derivative_code = translate(derivative_order, 5, ST_FS, &derivative_info, &derivative_words, 0);
    check_translated(derivative_code, &derivative_info, "TXD with distinct derivatives translates");
    if (derivative_code)
        check(seeded_gradient_component(derivative_code, derivative_words, 0x1111, 0, 0) &&
                  seeded_gradient_component(derivative_code, derivative_words, 0x3333, 0, 1) &&
                  seeded_gradient_component(derivative_code, derivative_words, 0x2222, 1, 0) &&
                  seeded_gradient_component(derivative_code, derivative_words, 0x4444, 1, 1),
              "TXD builds gradients from alternating X and Y registers");
    free(derivative_code);
    u64 cube_array[] = {
        array_2d[0],        mov32i(12, 0x1111), mov32i(13, 0x2222), mov32i(14, 0x3333),
        mov32i(15, 0x4444), mov32i(16, 0x5555), mov32i(17, 0x6666), (array_2d[1] & ~(7ull << 28)) | 7ull << 28};
    ShaderInfo info;
    size_t words = 0;
    u32* code = translate(cube_array, sizeof cube_array / sizeof *cube_array, ST_FS, &info, &words, 0);
    check_translated(code, &info, "TXD.B cube arrays translate");
    if (code) {
        check(info.ntex == 1 && info.tex[0].type == 3 && info.tex[0].array && info.tex[0].bindless &&
                  has_image_operands(code, words, 88, 4),
              "TXD.B retains the cube-array texture and gradient image operands");
        check(seeded_gradient_component(code, words, 0x1111, 0, 0) &&
                  seeded_gradient_component(code, words, 0x3333, 0, 1) &&
                  seeded_gradient_component(code, words, 0x5555, 0, 2) &&
                  seeded_gradient_component(code, words, 0x2222, 1, 0) &&
                  seeded_gradient_component(code, words, 0x4444, 1, 1) &&
                  seeded_gradient_component(code, words, 0x6666, 1, 2),
              "TXD.B cube gradients retain all three alternating derivative components");
        save_fixture(gradient_cube_path, code, words);
    }
    free(code);
}

/* Texture forms compared with Eden's: TEXS's depth compares with a level (5, 6, 9), TLDS's multisample fetch with
   half results, TLD's multisample fetch in the bound form and its offsets on a 3D texture, TLD4's offset on a 1D
   texture, and TEX's packed minimum LOD. TLD/TXD clamp forms remain refused without a proven operand contract. */
static void texture_forms(void) {
#define TEXS_W(fp32, encoding, a, b)                                                                                   \
    (0xd000000000000000ull | (u64)(fp32) << 59 | (u64)(encoding) << 53 | 0x10ull << 36 | 0xffull << 28 |               \
     (u64)(b) << 20 | 7ull << 16 | (u64)(a) << 8)
    static const u64 texs_ll_dc[] = {TEXS_W(1, 5, 2, 4)};
    static const u64 texs_lz_dc[] = {TEXS_W(1, 6, 2, 4)};
    static const u64 texs_array_dc[] = {TEXS_W(1, 9, 2, 4)};
    static const u64 tlds_ms_half[] = {TEXS_W(0, 6, 2, 4) | 1ull << 57}; /* TLDS: bits 58-57 01 */
    static const u64 tld_forms[] = {
        0xdc00000000000000ull | 7ull << 51 | 1ull << 50 | 0xfull << 31 | 2ull << 28 | 0x10ull << 36 | 4ull << 20 |
            7ull << 16 | 2ull << 8, /* TLD.MS R0, R2, R4: the sample in Rb, as the bound form has no handle */
        0xdc00000000000004ull | 7ull << 51 | 1ull << 35 | 0xfull << 31 | 4ull << 28 | 0x11ull << 36 | 6ull << 20 |
            7ull << 16 | 8ull << 8, /* TLD.AOFFI R4, R8, R6 on a 3D texture */
    };
    static const u64 tld4_1d[] = {0xc800000000000000ull | 7ull << 51 | 1ull << 54 | 0xfull << 31 | 0x10ull << 36 |
                                  4ull << 20 | 7ull << 16 | 2ull << 8}; /* TLD4.AOFFI R0, R2, R4 on a 1D texture */
#undef TEXS_W
    struct {
        const u64* instructions;
        int count;
        const char* what;
    } cases[] = {{texs_ll_dc, 1, "TEXS 2D.LL.DC translates"},
                 {texs_lz_dc, 1, "TEXS 2D.LZ.DC translates"},
                 {texs_array_dc, 1, "TEXS ARRAY_2D.LZ.DC translates"},
                 {tlds_ms_half, 1, "TLDS 2D.LZ.MS with half results translates"},
                 {tld_forms, 2, "TLD.MS and a 3D TLD.AOFFI translate"},
                 {tld4_1d, 1, "TLD4.AOFFI on a 1D texture translates"}};
    for (int i = 0; i < 6; i++) {
        ShaderInfo info;
        size_t words = 0;
        u32* code = translate(cases[i].instructions, cases[i].count, ST_FS, &info, &words, 0);
        check_translated(code, &info, cases[i].what);
        if (code) {
            if (i <= 2) check(has_image_operands(code, words, 90, 2), "TEXS depth compare samples at explicit LOD");
            if (i == 3 || i == 4)
                check(has_image_operands(code, words, 95, 64), "multisample fetch carries a sample operand");
            if (i == 4)
                check(has_image_operands(code, words, 95, 2) && count_opcode(code, words, 202, 0xffffffffu) >= 3,
                      "3D TLD offset moves all three coordinates before the LOD fetch");
            if (i == 5) check(has_image_operands(code, words, 96, 16), "1D TLD4 gather carries a dynamic offset");
        }
        if (code) save_fixture(texture_paths[i], code, words);
        free(code);
    }
    const u64 texs_operands[] = {mov32i(3, 0x1111), mov32i(4, 0x2222), mov32i(5, 0x3333), texs_ll_dc[0]};
    ShaderInfo texs_info;
    size_t texs_words = 0;
    u32* texs_code = translate(texs_operands, 4, ST_FS, &texs_info, &texs_words, 0);
    check_translated(texs_code, &texs_info, "TEXS.LL.DC with distinct coordinate and reference registers translates");
    if (texs_code)
        check(seeded_register_feeds(texs_code, texs_words, 0x1111, 80, 4) &&
                  seeded_register_feeds(texs_code, texs_words, 0x3333, 90, 5) &&
                  seeded_register_feeds(texs_code, texs_words, 0x2222, 90, 7),
              "TEXS.LL.DC takes y from Ra+1, level from Rb and dref from Rb+1");
    free(texs_code);
    const u64 lod_offset[] = {mov32i(6, 0x1234), mov32i(7, 0x5678), tld_forms[1] | (1ull << 55)};
    const u64 ms_offset[] = {mov32i(6, 0x1234), mov32i(7, 0x5678),
                             (tld_forms[0] & ~(0xffull << 20)) | (6ull << 20) | (1ull << 35)};
    ShaderInfo order_info;
    size_t order_words = 0;
    u32* order_code = translate(lod_offset, 3, ST_FS, &order_info, &order_words, 0);
    check_translated(order_code, &order_info, "TLD with level and offset translates");
    if (order_code)
        check(seeded_register_feeds(order_code, order_words, 0x1234, 95, 6) &&
                  seeded_register_feeds(order_code, order_words, 0x5678, 202, 3),
              "TLD reads Rb as level and Rb+1 as offset");
    free(order_code);
    order_code = translate(ms_offset, 3, ST_FS, &order_info, &order_words, 0);
    check_translated(order_code, &order_info, "TLD with offset and sample translates");
    if (order_code)
        check(seeded_register_feeds(order_code, order_words, 0x1234, 202, 3) &&
                  seeded_register_feeds(order_code, order_words, 0x5678, 95, 6),
              "TLD reads Rb as offset and Rb+1 as sample");
    free(order_code);
    {
        ShaderInfo info;
        size_t words = 0;
        u32* code = translate((const u64[]){0xc400000000000000ull | 0xfull << 31 | 2ull << 28 | 0x10ull << 36 |
                                            4ull << 20 | 7ull << 16 | 2ull << 8},
                              1, ST_FS, &info, &words, 0);
        check_translated(code, &info, "TEX with the packed minimum LOD translates");
        if (code)
            check(has_image_operands(code, words, 87, 128) && count_opcode(code, words, 17, 42) == 1,
                  "TEX minimum LOD carries MinLod and declares its capability");
        free(code);
    }
    const u64 clamp_forms[] = {
        0xdeb8000000070008ull | (6ull << 28) | (1ull << 40) | (4ull << 20), /* TEX.B LC */
        tld_forms[1] | (1ull << 54),                                        /* TLD LC */
        0xde00000000000000ull | (7ull << 51) | (1ull << 50) | (2ull << 28) | (8ull << 20) | (7ull << 16) |
            (4ull << 8), /* TXD LC */
    };
    const char* clamp_names[] = {"TEX.B", "TLD", "TXD"};
    for (size_t i = 0; i < sizeof clamp_forms / sizeof *clamp_forms; i++) {
        ShaderInfo info;
        size_t words = 0;
        const u64 instructions[] = {HANDLE_CBUF_R4, clamp_forms[i]};
        u32* code = translate(instructions, 2, ST_FS, &info, &words, 0);
        if (i == 0) {
            check_translated(code, &info, "TEX.B with the packed minimum LOD translates");
            if (code)
                check(info.ntex == 1 && info.tex[0].bindless && has_image_operands(code, words, 87, 128) &&
                          count_opcode(code, words, 17, 42) == 1,
                      "TEX.B minimum LOD retains its bindless handle and declares MinLod");
        } else {
            check_refused(code, &info, clamp_names[i], "unproven TLD/TXD LOD clamp form is refused");
        }
        free(code);
    }
}

/* Arithmetic forms compared with Eden's: XMAD's signed RC and immediate forms with X, CC, CBCC, PSL and MRG, LOP's
   and LOP32I's flags, FMUL32I.FMZ, the F64 and 64-bit integer conversions, VSETP's signed immediate, DADD's rounding,
   BFE's and LOP3's flags. SHL.CC, whose flags no source describes, is refused. */
static void arithmetic_forms(void) {
    static const u64 forms[] = {
        0x5143818400470200ull, /* XMAD.U16.S16.CC.X R0, R2, R3, c[1][0x10] */
        0x361201b800170200ull, /* XMAD.S16.CBCC.PSL.MRG R0, R2, 0x8001, R3 */
        0x5b00000000370201ull, /* XMAD.R R1, R2, R3, R6 */
        0x4e00000000370201ull, /* XMAD.CR R1, R2, c[0][0xc], R6 */
        0x5c47880000370201ull, /* LOP.AND.CC.X R1, R2, R3 */
        0x041000000ff70201ull, /* LOP32I.AND.CC R1, R2, 0xff */
        0x1e43f80000070201ull, /* FMUL32I.FMZ R1, R2, 1.0 */
        0x5ca8000000470e01ull, /* F2F.F32.F64 R1, R4 */
        0x5ca8000000270b06ull, /* F2F.F64.F32 R6, R2 */
        0x5cb0000000271b08ull, /* F2I.S64.F32 R8, R2 */
        0x5cb0000000470e01ull, /* F2I.U32.F64 R1, R4 */
        0x5cb8200000472f0aull, /* I2F.F64.S64 R10, -R4 */
        0x5cb8000000470e01ull, /* I2F.F32.U64 R1, R4 */
        0x50f30b8840070201ull, /* VSETP.LT.S8.B0 P0, P1, R2, 0x8400 */
        0x5c70008000470206ull, /* DADD.RM R6, R2, R4 */
        0x5c00800000370201ull, /* BFE.CC R1, R2, R3 */
        0x5be7824800370201ull, /* LOP3.CC.X R1, R2, R3, R4 */
    };
    ShaderInfo info;
    size_t words = 0;
    u32* code = translate(forms, (int)(sizeof forms / sizeof *forms), ST_FS, &info, &words, 0);
    check_translated(code, &info, "the arithmetic forms Eden translates translate");
    if (code) {
        check(count_opcode(code, words, 17, 11) == 1, "a 64-bit integer conversion declares Int64");
        check(has_execution_mode_width(code, words, 4461, 64), "F64 math preserves signed zero, infinities and NaNs");
        save_fixture(arithmetic_path, code, words);
    }
    free(code);
    code = translate(&forms[4], 1, ST_FS, &info, &words, 0); /* LOP.AND.CC.X */
    check_translated(code, &info, "LOP.CC.X translates alone");
    if (code)
        check(count_opcode(code, words, 167, 0xffffffffu) == 1,
              "LOP.CC.X combines the previous and current zero flags");
    free(code);
    code = translate(&forms[1], 1, ST_FS, &info, &words, 0);
    check_translated(code, &info, "XMAD.CBCC.PSL.MRG translates alone");
    if (code)
        check(count_shifts_by(code, words, 16) >= 3, "XMAD.CBCC adds shifted B alongside the PSL product and MRG half");
    free(code);
    const u64 dadd_pair[] = {mov32i(2, 0x101), mov32i(3, 0x202), mov32i(4, 0x303), mov32i(5, 0x404), forms[14]};
    code = translate(dadd_pair, 5, ST_FS, &info, &words, 0);
    check_translated(code, &info, "DADD with distinct low and high words translates");
    if (code)
        check(seeded_register_feeds(code, words, 0x101, 80, 3) && seeded_register_feeds(code, words, 0x202, 80, 4) &&
                  seeded_register_feeds(code, words, 0x303, 80, 3) && seeded_register_feeds(code, words, 0x404, 80, 4),
              "DADD reads each double's low word before its high word");
    free(code);
    const u64 f2f_f64 = 0x5ca8000000470e01ull;
    const u64 f2i_s64 = 0x5cb0000000271b08ull;
    const u64 conversions[] = {
        (f2f_f64 & ~(3ull << 8)) | (3ull << 8) | (1ull << 42) | (1ull << 39), /* F64.F64.FLOOR */
        (f2f_f64 & ~(3ull << 8)) | (1ull << 8),                               /* F16.F64 */
        (f2f_f64 & ~(3ull << 10)) | (1ull << 10) | (3ull << 8),               /* F64.F16 */
        f2i_s64,                                                              /* S64.F32 */
        (f2i_s64 & ~(1ull << 12)) | (3ull << 10),                             /* U64.F64 */
    };
    for (size_t i = 0; i < sizeof conversions / sizeof *conversions; i++) {
        code = translate(&conversions[i], 1, ST_FS, &info, &words, 0);
        check_translated(code, &info, "F64 conversion variant translates");
        if (code) {
            if (i == 0) check(has_glsl_instruction(code, words, 8), "F64.F64.FLOOR rounds the double");
            if (i == 1) check(has_glsl_instruction(code, words, 58), "F64 to F16 packs a half");
            if (i == 2) check(has_glsl_instruction(code, words, 62), "F16 to F64 unpacks a half");
            if (i == 3)
                check(count_opcode(code, words, 12, 0xffffffffu) > 0 &&
                          has_constant64(code, words, 0x7fffffffffffffffull),
                      "F32 to S64 clamps and selects the signed overflow result");
            if (i == 4)
                check(has_constant64(code, words, 0x43efffffffffffffull) && has_constant64(code, words, ~0ull),
                      "F64 to U64 clamps below 2^64 and selects the unsigned overflow result");
        }
        free(code);
    }
    code = translate((const u64[]){0x5c48800000370201ull}, 1, ST_FS, &info, &words, 0); /* SHL.CC R1, R2, R3 */
    check_refused(code, &info, "SHL", "SHL.CC is refused");
    free(code);
    const u64 xmad_csfu = (forms[0] & ~(3ull << 50)) | (3ull << 50);
    code = translate(&xmad_csfu, 1, ST_FS, &info, &words, 0);
    check_refused(code, &info, "XMAD", "XMAD.CSFU is refused");
    free(code);
    const u64 refused_flags[] = {forms[7] | (1ull << 47), forms[9] | (1ull << 47), forms[11] | (1ull << 47),
                                 0x5c28000000370201ull | (1ull << 47), 0x5c10000000370201ull | (1ull << 50)};
    const char* refused_names[] = {"F2F", "F2I", "I2F", "SHR", "IADD"};
    for (size_t i = 0; i < sizeof refused_flags / sizeof *refused_flags; i++) {
        code = translate(&refused_flags[i], 1, ST_FS, &info, &words, 0);
        check_refused(code, &info, refused_names[i], "unsupported arithmetic flag is refused");
        free(code);
    }
}

/* Opcodes Eden doesn't translate: the scalar video instructions with saturation, merges and secondary operations,
   PRMT's index, F4E and RC16 modes, FCHK, RRO's immediate form, MUFU.RCP64H, CS2R, CCTL and IDE, and the 32-bit
   compare and swaps on global and shared memory */
static void beyond_eden(void) {
    static const u64 arithmetic[] = {
        0x20c7036060370201ull, /* VADD.SAT.S32 R1.H1, R2, R3, R6 */
        0x5424031020370201ull, /* VABSDIFF.ACC R1, R2.B1, R3.B2, R6 */
        0x4073036fffb70201ull, /* VSET.LT.MAX R1, R2, -5, R6 */
        0x573e036060370201ull, /* VSHL.WRAP R1, R2, R3, R6 */
        0x5615036060370201ull, /* VSHR.S32 R1.B0, R2, R3, R6 */
        0x5bc0030000370201ull, /* PRMT R1, R2, R3, R6 */
        0x5bc1030000370201ull, /* PRMT.F4E R1, R2, R3, R6 */
        0x36c6030000270201ull, /* PRMT.RC16 R1, R2, 0x2, R6 */
        0x5c88000000370200ull, /* FCHK.DIVIDE P0, R2, R3 */
        0x3890003f80070001ull, /* RRO.SINCOS R1, 1.0 */
        0x5080000000670201ull, /* MUFU.RCP64H R1, R2 */
        0x5080000000770201ull, /* MUFU.RSQ64H R1, R2 */
        0x50c800000ff70004ull, /* CS2R R4, SRZ */
        0xef6000000007ff00ull, /* CCTL op 0 */
        0xe390000000070000ull, /* IDE.EN */
    };
    static const u64 global_cas[] = {
        0xef9500007307ff02ull, /* LDC.64 R2:R3, c[0][0x730]: the buffer's 64-bit address */
        0xeef0000000470200ull, /* ATOM.CAS R0, [R2], R4, R5 */
    };
    static const u64 shared_cas[] = {0xee4000000047ff00ull}; /* ATOMS.CAS R0, [RZ], R4, R5 */
    struct {
        const u64* instructions;
        int count, stage;
        const char* what;
    } cases[] = {{arithmetic, (int)(sizeof arithmetic / sizeof *arithmetic), ST_FS, "the opcodes Eden lacks translate"},
                 {global_cas, 2, ST_FS, "ATOM.CAS translates"},
                 {shared_cas, 1, ST_CS, "ATOMS.CAS translates"}};
    for (int i = 0; i < 3; i++) {
        ShaderInfo info;
        size_t words = 0;
        u32* code = translate(cases[i].instructions, cases[i].count, cases[i].stage, &info, &words, 1);
        check_translated(code, &info, cases[i].what);
        if (code && i > 0) check(count_opcode(code, words, 230, 0xffffffffu) == 1, "a compare and swap is atomic");
        if (code && i == 1)
            check(count_opcode(code, words, 5381, 0xffffffffu) > 0,
                  "fragment compare and swap skips helper invocations");
        if (code) save_fixture(beyond_paths[i], code, words);
        free(code);
    }
    const u64 cas_order[] = {global_cas[0], mov32i(4, 0x1234), mov32i(5, 0x5678), global_cas[1]};
    ShaderInfo cas_info;
    size_t cas_words = 0;
    u32* cas_code = translate(cas_order, 4, ST_FS, &cas_info, &cas_words, 1);
    check_translated(cas_code, &cas_info, "ATOM.CAS with distinct expected and desired words translates");
    if (cas_code)
        check(seeded_register_feeds(cas_code, cas_words, 0x5678, 230, 7) &&
                  seeded_register_feeds(cas_code, cas_words, 0x1234, 230, 8),
              "ATOM.CAS takes Rb+1 as desired and Rb as expected");
    free(cas_code);
    {
        const u64 rcp64h = arithmetic[10];
        ShaderInfo info;
        size_t words = 0;
        u32* code = translate(&rcp64h, 1, ST_FS, &info, &words, 0);
        check_translated(code, &info, "MUFU.RCP64H translates");
        if (code) check(has_zero_low_double_word(code, words), "RCP64H builds a double with a zero low word");
        free(code);
    }
    {
        const u64 vadd_sat = arithmetic[0];
        ShaderInfo info;
        size_t words = 0;
        u32* code = translate(&vadd_sat, 1, ST_FS, &info, &words, 0);
        check_translated(code, &info, "VADD.SAT translates");
        if (code)
            check(has_constant64(code, words, 0x7fffull) && has_constant64(code, words, 0xffffffffffff8000ull),
                  "signed VADD.SAT clamps to the 16-bit destination range");
        free(code);
    }
    for (u32 comparison = 1; comparison <= 4; comparison += 3) {
        const u64 vset = (arithmetic[2] & ~((7ull << 54) | (7ull << 51))) | (u64)comparison << 54 | 7ull << 51;
        ShaderInfo info;
        size_t words = 0;
        u32* code = translate(&vset, 1, ST_FS, &info, &words, 0);
        check_translated(code, &info, "VSET.LT or VSET.GT translates");
        if (code)
            check(vset_selects_comparison(code, words, comparison == 4),
                  "VSET.LT and VSET.GT select opposite signed comparisons");
        free(code);
    }
    {
        const u64 vshl_sat = (arithmetic[3] & ~(1ull << 49)) | 1ull << 55; /* unsigned VSHL.SAT, count clamps at 32 */
        ShaderInfo info;
        size_t words = 0;
        u32* code = translate(&vshl_sat, 1, ST_FS, &info, &words, 1);
        check_translated(code, &info, "unsigned VSHL.SAT translates");
        if (code) check(count_opcode(code, words, 176, 0xffffffffu) > 0, "VSHL.SAT clamps with an unsigned compare");
        free(code);
    }
    for (int form = 0; form < 2; form++) {
        const u64 prmt[] = {mov32i(6, 0x1234), form == 0 ? 0x53c0030000370201ull : 0x4bc0030000370201ull}; /* RC, CR */
        ShaderInfo info;
        size_t words = 0;
        u32* code = translate(prmt, 2, ST_FS, &info, &words, 1);
        check_translated(code, &info, form == 0 ? "PRMT.RC translates" : "PRMT.CR translates");
        if (code)
            check(prmt_selector_uses_seeded_register(code, words) == (form == 0),
                  "PRMT.RC takes its selector from Rc; PRMT.CR takes it from cbuf");
        free(code);
    }
    for (u32 mode = 2; mode <= 5; mode++) {
        u64 prmt = (arithmetic[5] & ~(7ull << 48)) | (u64)mode << 48;
        ShaderInfo info;
        size_t words = 0;
        u32* code = translate(&prmt, 1, ST_FS, &info, &words, 1);
        check_translated(code, &info, "PRMT B4E, RC8, ECL or ECR translates");
        if (code && mode == 2)
            check(count_opcode(code, words, 130, 0xffffffffu) >= 4, "PRMT.B4E walks bytes backward from the selector");
        free(code);
    }
    {
        const u64 f4e = (arithmetic[5] & ~(7ull << 48)) | 1ull << 48;
        ShaderInfo info;
        size_t words = 0;
        u32* code = translate(&f4e, 1, ST_FS, &info, &words, 1);
        check_translated(code, &info, "PRMT.F4E translates");
        if (code)
            check(count_opcode(code, words, 128, 0xffffffffu) >= 4,
                  "PRMT.F4E adds each output byte position to the selector");
        free(code);
    }
    const u64 invalid_global[] = {
        global_cas[1] | (1ull << 49),
        global_cas[1] | (1ull << 50),
        global_cas[1] | (1ull << 51),
    };
    for (size_t i = 0; i < sizeof invalid_global / sizeof *invalid_global; i++) {
        const u64 program[] = {global_cas[0], invalid_global[i]};
        ShaderInfo info;
        size_t words = 0;
        u32* code = translate(program, 2, ST_FS, &info, &words, 1);
        check_refused(code, &info, "ATOM", "unsupported global compare-and-swap flags are refused");
        free(code);
    }
    const u64 invalid_shared = (shared_cas[0] & ~(0xfull << 52)) | 5ull << 52;
    ShaderInfo info;
    size_t words = 0;
    u32* code = translate(&invalid_shared, 1, ST_CS, &info, &words, 1);
    check_refused(code, &info, "ATOMS", "unsupported shared compare-and-swap subop is refused");
    free(code);
}

/* AL2P keeps byte offsets, including bit 10, and adds the register before an attribute access. */
/* Transform feedback (ShaderInfo.xfb): a vertex program's AST.128 of R0-R3 to generic 0, of which buffer 0 captures y
   and z (from mid-vector, 8 bytes a vertex) and buffer 1 x. Each captured component gets an offset of its own: the
   module declares TransformFeedback and Xfb, and three captures (Pokemon Legends Z-A skins with them, issue #440) */
static void transform_feedback(void) {
    const u64 instructions[] = {mov32i(0, 0x3f800000), mov32i(1, 0x40000000), mov32i(2, 0x40400000),
                                mov32i(3, 0x40800000), 0xeff000000007ff00ull | (0x80ull << 20) | (3ull << 47)};
    ShaderInfo info;
    size_t slots, words = 0;
    u64* code = guest_program(instructions, 5, NULL, &slots);
    if (!code) return;
    memset(&info, 0, sizeof info);
    info.xfb.layout[0][0] = 33;
    info.xfb.layout[0][1] = 34;
    info.xfb.count[0] = 2;
    info.xfb.stride[0] = 8;
    info.xfb.layout[1][0] = 32;
    info.xfb.count[1] = 1;
    info.xfb.stride[1] = 4;
    u32* spv = spirv_of(code, slots * sizeof *code, ST_VS, &info, &words, 0, 1, 1, 1);
    free(code);
    check_translated(spv, &info, "a transform feedback capture translates");
    if (spv) {
        check(count_opcode(spv, words, 17, 53) == 1, "transform feedback declares its capability once");
        check(count_opcode(spv, words, 16, 11) == 1, "the entry point captures (Xfb)");
        check(count_opcode(spv, words, 71, 36) == 3 && count_opcode(spv, words, 71, 35) == 3,
              "each captured component has a buffer and an offset");
        save_fixture(xfb_path, spv, words);
    }
    free(spv);
}

/* how many OpDecorate give decoration (30 Location, 14 Flat) with this literal, or with none when literal is ~0 */
static int decorations(const u32* code, size_t words, u32 decoration, u32 literal) {
    int found = 0;
    for (size_t i = 5; i < words && code[i] >> 16;) {
        u32 width = code[i] >> 16;
        if ((code[i] & 0xffff) == 71 && width >= 3 && code[i + 2] == decoration &&
            (literal == 0xffffffffu || (width >= 4 && code[i + 3] == literal)))
            found++;
        i += width;
    }
    return found;
}
/* whether the module binds a resource at binding (OpDecorate Binding) */
static int has_binding(const u32* code, size_t words, u32 binding) {
    return decorations(code, words, 33, binding) > 0;
}
/* without the extension (MoltenVK) the vertex program stores the capture into storage buffers after its global
   buffers, with no TransformFeedback capability, and keeps the key for the renderer */
static void transform_feedback_stores(void) {
    const u64 instructions[] = {mov32i(0, 0x3f800000), mov32i(1, 0x40000000), mov32i(2, 0x40400000),
                                mov32i(3, 0x40800000), 0xeff000000007ff00ull | (0x80ull << 20) | (3ull << 47)};
    ShaderInfo info;
    size_t slots, words = 0;
    u64* code = guest_program(instructions, 5, NULL, &slots);
    if (!code) return;
    memset(&info, 0, sizeof info);
    info.xfb.layout[0][0] = 33;
    info.xfb.layout[0][1] = 34;
    info.xfb.count[0] = 2;
    info.xfb.stride[0] = 8;
    info.xfb.layout[1][0] = 32;
    info.xfb.count[1] = 1;
    info.xfb.stride[1] = 4;
    g_shader_xfb_stores = 1;
    u32* spv = spirv_of(code, slots * sizeof *code, ST_VS, &info, &words, 0, 1, 1, 1);
    g_shader_xfb_stores = 0;
    free(code);
    check_translated(spv, &info, "a transform feedback capture by stores translates");
    if (spv) {
        check(count_opcode(spv, words, 17, 53) == 0 && count_opcode(spv, words, 16, 11) == 0,
              "a capture by stores declares no TransformFeedback and no Xfb");
        check(count_opcode(spv, words, 71, 36) == 0, "a capture by stores decorates no XfbBuffer");
        check(has_binding(spv, words, SNX_VKB_XFB_PARAMS) && has_binding(spv, words, SNX_VKB_XFB) &&
                  has_binding(spv, words, SNX_VKB_XFB + 1) && !has_binding(spv, words, SNX_VKB_XFB + 2),
              "a capture by stores binds its parameters and the buffers it captures into");
        check(info.xfb.count[0] == 2 && info.xfb.count[1] == 1, "a capture by stores keeps its layout in its info");
        check(count_opcode(spv, words, 199, 0xffffffffu) >= 2,
              "a capture by stores tests the bound mask of each buffer it captures into (OpBitwiseAnd)");
        save_fixture(xfb_stores_path, spv, words);
    }
    free(spv);
}
/* a capture by stores of the fog (layout word 186, a[0x2e8]) stores generic 21's x (word 116), where the fog moved */
static void transform_feedback_stores_fog(void) {
    const u64 instructions[] = {mov32i(0, 0x3f800000), 0xeff07f800007ff00ull | 0x2e8ull << 20};
    size_t slots, words[2] = {0};
    u64* code = guest_program(instructions, 2, NULL, &slots);
    u32* spv[2] = {NULL, NULL};
    if (!code) return;
    g_shader_xfb_stores = 1;
    for (int i = 0; i < 2; i++) {
        ShaderInfo info;
        memset(&info, 0, sizeof info);
        info.xfb.layout[0][0] = i ? 116 : 186;
        info.xfb.count[0] = 1;
        info.xfb.stride[0] = 4;
        spv[i] = spirv_of(code, slots * sizeof *code, ST_VS, &info, &words[i], 0, 1, 1, 1);
        check_translated(spv[i], &info, "a capture by stores of the fog translates");
    }
    g_shader_xfb_stores = 0;
    check(spv[0] && spv[1] && words[0] == words[1] && !memcmp(spv[0], spv[1], words[0] * sizeof *spv[0]),
          "a capture by stores of the fog stores the component it moved to (x), as a capture of that generic does");
    free(spv[0]);
    free(spv[1]);
    free(code);
}

/* xfb_stores.h over a begin, two draws, a pause and a resume: buffer 2 has a bad stride, and the guest left 3 off */
static void transform_feedback_stores_ranges(void) {
    const u32 stride[4] = {16, 8, 6, 4}, words[4] = {4, 1, 2, 1}, size[4] = {112, 1000, 64, 0};
    u32 from[4] = {0};
    SnxXfbRange r;
    snx_xfb_range(3, 3, stride, words, size, from, &r);
    check(r.declared == 0xb && r.bound == 3 && r.limit == 3 && r.start[0] == 0 && r.start[1] == 0 &&
              r.used[0] == 48 && r.used[1] == 24 && r.end[0] == 48 && r.end[1] == 24,
          "a capture by stores from a begin takes the triangle, from each buffer's start");
    for (int b = 0; b < 4; b++) from[b] = r.end[b];
    snx_xfb_range(6, 3, stride, words, size, from, &r);
    check(r.bound == 3 && r.limit == 3 && r.start[0] == 48 && r.start[1] == 24 && r.end[0] == 96 && r.end[1] == 48,
          "a second draw resumes at the first's ends and takes the whole triangles that fit");
    const u32 paused[4] = {r.end[0], r.end[1], r.end[2], r.end[3]};
    snx_xfb_range(1, 1, stride, words, size, paused, &r);
    check(r.bound == 3 && r.limit == 1 && r.start[0] == 96 && r.start[1] == 48 && r.used[0] == 16 &&
              r.used[1] == 8 && r.end[0] == 112 && r.end[1] == 56,
          "a resume from the pause's ends captures the point after them");
    for (int b = 0; b < 4; b++) from[b] = r.end[b];
    snx_xfb_range(1, 1, stride, words, size, from, &r);
    check(r.declared == 0xb && r.bound == 0 && r.limit == 0, "a full buffer stops the capture of every buffer");
}

/* The fixed function's varyings take the generics 17-31, the same in every stage (legacy_varyings): a vertex
   program's front diffuse green from R43 (AST a[0x284], one game's), another's whole first texture coordinate
   (AST a[0x300] x4, another game's), a fragment program reading them and the fog with the header's color and texture
   coordinate interpolation, and an evaluation program passing a coordinate on. A program writing a legacy varying
   and the generic it moves to is refused, naming both; a store running from a color into the clip distances is
   refused as before; a captured texture coordinate is captured where it moved */
static void legacy_varyings_forms(void) {
    const u64 ast = 0xeff07f800007ff00ull, ald = 0xefd87f800007ff00ull, ipa = 0xe003ff800ff7ff00ull;
#define LEGACY_AST(addr, count, reg) (ast | (u64)(addr) << 20 | (u64)((count) - 1) << 47 | (reg))
#define LEGACY_IPA(addr, reg)        (ipa | (u64)((addr) / 4) << 30 | (reg))
    ShaderInfo info;
    size_t words = 0, slots;
    const u64 diffuse[] = {mov32i(43, 0x3f000000), LEGACY_AST(0x284, 1, 43)};
    u32* code = translate(diffuse, 2, ST_VS, &info, &words, 1);
    check_translated(code, &info, "a vertex program writing a legacy color translates");
    if (code) {
        check(info.out_mask == 1u << 17 && decorations(code, words, 30, 17) == 1,
              "the front diffuse color is written as generic 17");
        save_fixture(legacy_paths[0], code, words);
    }
    free(code);
    const u64 coordinate[] = {LEGACY_AST(0x300, 4, 4)};
    code = translate(coordinate, 1, ST_VS, &info, &words, 1);
    check_translated(code, &info, "a vertex program writing a texture coordinate translates");
    check(code && info.out_mask == 1u << 22 && decorations(code, words, 30, 22) == 1,
          "the first texture coordinate is written as generic 22");
    if (code) save_fixture(legacy_paths[1], code, words);
    free(code);
    /* the header's color map flat (1) for the diffuse color's four components, its first coordinate perspective (2) */
    const u64 reads[] = {LEGACY_IPA(0x284, 0), LEGACY_IPA(0x2e8, 1), LEGACY_IPA(0x308, 2)};
    u64* program = guest_program(reads, 3, NULL, &slots);
    if (program) {
        set_sph(program, 14, 0x55);
        set_sph(program, 15, 0xaa);
        memset(&info, 0, sizeof info);
        code = spirv_of(program, slots * sizeof *program, ST_FS, &info, &words, 1, 1, 1, 1);
        check_translated(code, &info, "a fragment program reading legacy varyings translates");
        if (code) {
            check(info.in_mask == (1u << 17 | 1u << 21 | 1u << 22) && decorations(code, words, 30, 17) == 1 &&
                      decorations(code, words, 30, 21) == 1 && decorations(code, words, 30, 22) == 1,
                  "the diffuse color, the fog and the first texture coordinate are read as generics 17, 21 and 22");
            check(decorations(code, words, 14, 0xffffffffu) == 1,
                  "only the color the header's map makes constant is a flat input");
            save_fixture(legacy_paths[2], code, words);
        }
        free(code);
        free(program);
    }
    const u64 passed[] = {ald | 0x300ull << 20, LEGACY_AST(0x300, 1, 0)};
    code = translate_stage(passed, 2, ST_TES, NULL, 0, &info, &words);
    check_translated(code, &info, "an evaluation program passing a texture coordinate on translates");
    check(code && info.in_mask == 1u << 22 && info.out_mask == 1u << 22,
          "an evaluation program reads and writes the coordinate as generic 22");
    if (code) save_fixture(legacy_paths[3], code, words);
    free(code);
    const u64 both[] = {LEGACY_AST(0x190, 1, 0), LEGACY_AST(0x280, 1, 1)};
    code = translate(both, 2, ST_VS, &info, &words, 1);
    check_refused(code, &info, "legacy varying 0x280 written as generic 17, which the program also writes",
                  "a program writing a legacy color and its generic is refused, naming both");
    free(code);
    const u64 across[] = {LEGACY_AST(0x2bc, 2, 0)};
    code = translate(across, 1, ST_VS, &info, &words, 1);
    check_refused(code, &info, "AST addr 700 count 2", "a store from a color into the clip distances stays refused");
    free(code);
    program = guest_program(coordinate, 1, NULL, &slots);
    if (program) {
        memset(&info, 0, sizeof info);
        info.xfb.layout[0][0] = 0x300 / 4 + 1; /* the first coordinate's y */
        info.xfb.count[0] = 1;
        info.xfb.stride[0] = 4;
        code = spirv_of(program, slots * sizeof *program, ST_VS, &info, &words, 0, 1, 1, 1);
        check_translated(code, &info, "a captured texture coordinate translates");
        check(code && count_opcode(code, words, 71, 36) == 1, "a captured texture coordinate is captured");
        free(code);
        free(program);
    }
#undef LEGACY_AST
#undef LEGACY_IPA
}

/* A rectangle texture's (ShaderInfo.tex_rect_mask, the key) sample takes texel coordinates, divided by the size at
   level 0 (one OpImageQuerySizeLod, an OpFDiv each for x and y, and for TXD each gradient's two too); a fragment
   program keeps a scaled target's size unscaled first (the push constant's scaled-slot bits). Without the key the
   program is the same as it was. TEX R8, R0, 2D, a texel of a bound slot; TXD R0, R4, R8, 2D; TLD4 R0, R4, 2D */
static void rect_textures(void) {
    const u64 tex =
        0xc000000000000000ull | 0x10ull << 36 | 7ull << 51 | 1ull << 31 | 2ull << 28 | 0xffull << 20 | 7ull << 16 | 8;
    const u64 txd = 0xde00000000000000ull | 7ull << 51 | 0x10ull << 36 | 0xfull << 31 | 2ull << 28 | 8ull << 20 |
                    7ull << 16 | 4ull << 8;
    const u64 tld4 = 0xc800000000000000ull | 7ull << 51 | 0x10ull << 36 | 0xfull << 31 | 2ull << 28 | 0xffull << 20 |
                     7ull << 16 | 4ull << 8;
    const u64* programs[3] = {&tex, &txd, &tld4};
    static const int divisions[3] = {2, 6, 2};
    static const char* names[3] = {"TEX", "TXD", "TLD4"};
    for (int k = 0; k < 3; k++) {
        ShaderInfo info;
        size_t words = 0, plain_words = 0;
        u32* plain = translate(programs[k], 1, ST_FS, &info, &plain_words, 1);
        check_translated(plain, &info, "a sample of a normalized texture translates");
        int plain_query = plain ? count_opcode(plain, plain_words, 103, 0xffffffffu) : -1;
        int plain_div = plain ? count_opcode(plain, plain_words, 136, 0xffffffffu) : -1;
        size_t slots;
        u64* code = guest_program(programs[k], 1, NULL, &slots);
        if (!code) continue;
        memset(&info, 0, sizeof info);
        info.tex_rect_mask = 1;
        u32* rect = spirv_of(code, slots * sizeof *code, ST_FS, &info, &words, 1, 1, 1, 1);
        free(code);
        check_translated(rect, &info, "a sample of a rectangle texture translates");
        if (rect && plain) {
            if (count_opcode(rect, words, 103, 0xffffffffu) != plain_query + 1 ||
                count_opcode(rect, words, 136, 0xffffffffu) != plain_div + divisions[k])
                fprintf(stderr, "%s of a rectangle texture: %d size queries, %d divisions; without the key %d and %d\n",
                        names[k], count_opcode(rect, words, 103, 0xffffffffu),
                        count_opcode(rect, words, 136, 0xffffffffu), plain_query, plain_div);
            check(count_opcode(rect, words, 103, 0xffffffffu) == plain_query + 1 &&
                      count_opcode(rect, words, 136, 0xffffffffu) == plain_div + divisions[k],
                  "a rectangle texture's coordinates (and gradients) are divided by its size");
            check(plain_query == 0, "a normalized texture's sample queries no size");
            save_fixture(rect_paths[k], rect, words);
        }
        free(plain);
        free(rect);
    }
}

/* One game's culling geometry program stores RZ or a MOV32I's 1 to the viewport mask (AST a[0x3a0]): no viewport or
   viewport 0 alone, no broadcast, so the program is the one without the store, word for word. A mask the translation
   can't know (R0, an attribute's), one of more viewports (3) and a mask of 1 beside a written viewport index are
   refused, each naming what it holds. A vertex program's zero mask is taken too */
static void viewport_mask(void) {
    const u64 mask_rz = 0xeff07f800007ff00ull | 0x3a0ull << 20 | 0xffull, mask_r0 = mask_rz & ~0xffull;
    const u64 with[] = {0xefd9ff800707ff00ull, 0xeff1ff800707ff00ull, mask_rz, 0xfbe000800ff70404ull,
                        0xfbe001000ff70404ull};
    const u64 without[] = {0xefd9ff800707ff00ull, 0xeff1ff800707ff00ull, 0x50b0000000070f00ull /* NOP */,
                           0xfbe000800ff70404ull, 0xfbe001000ff70404ull};
    const u32 geometry_sph[3] = {1u << 24, 7u << 24, 3};
    ShaderInfo info;
    size_t words = 0, plain_words = 0;
    u32* code = translate_stage(with, 5, ST_GS, geometry_sph, 3, &info, &words);
    check_translated(code, &info, "a geometry program storing a zero viewport mask translates");
    u32* plain = translate_stage(without, 5, ST_GS, geometry_sph, 3, &info, &plain_words);
    check(code && plain && words == plain_words && !memcmp(code, plain, words * sizeof *code),
          "a zero viewport mask's store emits nothing");
    free(code);
    free(plain);
    const u64 dynamic[] = {0xefd9ff800707ff00ull, 0xeff1ff800707ff00ull, mask_r0, 0xfbe000800ff70404ull,
                           0xfbe001000ff70404ull};
    code = translate_stage(dynamic, 5, ST_GS, geometry_sph, 3, &info, &words);
    check_refused(code, &info, "viewport mask R0 at 68: no constant", "a viewport mask no constant holds is refused");
    free(code);
    const u64 mask_r5 = (mask_rz & ~0xffull) | 5;
    for (u32 k = 0; k < 3; k++) {
        static const u32 values[3] = {1, 3, 1};
        const u64 constant[] = {mov32i(5, values[k]),
                                0xefd9ff800707ff00ull,
                                0xeff1ff800707ff00ull,
                                k == 2 ? 0xeff07f800007ff00ull | 0x68ull << 20 | 0xffull : 0x50b0000000070f00ull,
                                mask_r5,
                                0xfbe000800ff70404ull,
                                0xfbe001000ff70404ull};
        const u64 plain[] = {mov32i(5, values[k]),  0xefd9ff800707ff00ull, 0xeff1ff800707ff00ull, 0x50b0000000070f00ull,
                             0x50b0000000070f00ull, 0xfbe000800ff70404ull, 0xfbe001000ff70404ull};
        code = translate_stage(constant, 7, ST_GS, geometry_sph, 3, &info, &words);
        if (k == 0) {
            check_translated(code, &info, "a viewport mask of viewport 0 from a MOV32I translates");
            plain_words = 0;
            u32* expected = translate_stage(plain, 7, ST_GS, geometry_sph, 3, &info, &plain_words);
            check(code && expected && words == plain_words && !memcmp(code, expected, words * sizeof *code),
                  "a viewport mask of viewport 0 emits nothing");
            free(expected);
        } else
            check_refused(code, &info,
                          k == 1 ? "viewport mask 0x3 at 80: a broadcast" : "viewport mask 1 beside a viewport index",
                          "a broadcast, or a mask beside a viewport index, is refused");
        free(code);
    }
    const u64 vertex[] = {mask_rz};
    code = translate(vertex, 1, ST_VS, &info, &words, 1);
    check_translated(code, &info, "a vertex program storing a zero viewport mask translates");
    free(code);
}

static void attribute_pointer(void) {
    const u64 base = 0xefa070010607ff01ull; /* a game's: AL2P R1, RZ + 0x60, PT */
    const u64 instructions[] = {mov32i(4, 0x1234),
                                (base & ~((0xffull << 8) | (0x7ffull << 20))) | (4ull << 8) | (0x460ull << 20)};
    ShaderInfo info;
    size_t words = 0;
    u32* code = translate(instructions, 2, ST_VS, &info, &words, 0);
    check_translated(code, &info, "AL2P attribute pointer translates");
    check(code && seeded_register_feeds(code, words, 0x1234, 128, 3) && has_constant(code, words, 0x460),
          "AL2P adds its source and full eleven-bit byte offset");
    if (code) save_fixture(attribute_paths[3], code, words);
    free(code);
    code = translate(&base, 1, ST_VS, &info, &words, 0);
    check_translated(code, &info, "AL2P accepts the zero register");
    free(code);
    /* The range predicate (P0 here) isn't modeled: a program translates when nothing can read it, as the NVN
       compiler's never are (one game's fragment programs leave theirs, or overwrite them with a comparison before
       the alpha test reads it), and is refused when a later instruction may read it */
    const u64 predicate = base & ~(7ull << 44);
    code = translate(&predicate, 1, ST_VS, &info, &words, 0);
    check_translated(code, &info, "AL2P with a range predicate nothing reads translates");
    free(code);
    const u64 guarded_mov = (mov32i(2, 1) & ~(0xfull << 16)) | (0ull << 16); /* @P0 MOV32I R2, 1 */
    const u64 read_after[] = {predicate, guarded_mov};
    code = translate(read_after, 2, ST_VS, &info, &words, 0);
    check_refused(code, &info, "AL2P range predicate read", "AL2P range predicate that is read is refused");
    free(code);
    const u64 fsetp_p0 = 0x36b183be80070507ull; /* the alpha test's FSETP P0, PT, R5, imm */
    const u64 overwritten[] = {predicate, fsetp_p0, guarded_mov};
    code = translate(overwritten, 3, ST_VS, &info, &words, 0);
    check_translated(code, &info, "AL2P range predicate overwritten before its read translates");
    free(code);
    /* Paths follow the branches: another game's fragment programs branch between the AL2P and the comparison that
       overwrites its predicate, and a read is refused only when a path to it skips the comparison */
    const u64 branch_p1 = 0xe24000000001000full | (16ull << 20); /* @P1 BRA over the next instruction */
    const u64 joined[] = {predicate, branch_p1, mov32i(3, 2), fsetp_p0, guarded_mov};
    code = translate(joined, 5, ST_VS, &info, &words, 0);
    check_translated(code, &info, "AL2P range predicate overwritten where a branch joins translates");
    free(code);
    const u64 skipped[] = {predicate, branch_p1, fsetp_p0, guarded_mov};
    code = translate(skipped, 4, ST_VS, &info, &words, 0);
    check_refused(code, &info, "AL2P range predicate read",
                  "AL2P range predicate read past a branch over its overwrite is refused");
    free(code);
    /* The driver offers a direct CBE export path and ordinary attribute exports. With direct export disabled,
       its address registers are zero and its stores must not allocate or access a global-buffer descriptor. */
    const u64 export[] = {
        0xf0c8000001570008ull, 0xf0c8000001670005ull, 0xf0c8000001770006ull, 0x5c1000000067080aull,
        0x1c00000008070a0aull, 0x38c0050008070718ull, 0x5c10000000a71804ull, 0xeedc60000007040bull,
        0x5cc0000000681802ull, 0xeedc600000080201ull,
    };
    code = translate(export, sizeof export / sizeof *export, ST_VS, &info, &words, 0);
    check_translated(code, &info, "disabled direct CBE export translates");
    check(code && !info.ngbuf, "disabled CBE export does not bind guest global memory");
    if (code) save_fixture(attribute_paths[4], code, words);
    free(code);
    /* a conditional EXIT ends the block, where R0 and R1 are stored with the value of R2 */
    const u64 handles[] = {mov32i(2, 3), 0xefd0000100070200ull, 0xefd1000300070201ull, 0xE30000000001000Full};
    const u32 sph[3] = {4u << 24, 0, 0};
    code = translate_stage(handles, 4, ST_TCS, sph, 0, &info, &words);
    check_translated(code, &info, "ISBERD output-buffer handles translate");
    int seed_stores = 0;
    for (size_t i = 5; code && i < words && code[i] >> 16; i += code[i] >> 16)
        seed_stores +=
            (code[i] & 0xffff) == 62 && (code[i] >> 16) == 3 && is_constant_value(code, words, code[i + 2], 3);
    check(seed_stores == 3, "ISBERD preserves the vertex index");
    if (code) save_fixture(attribute_paths[5], code, words);
    free(code);
}

/* ALD with an index register, as a game's vertex programs read an attribute AL2P addressed: the inputs it can reach
   are the ones the header's map lists (generic 1 and 3 here), each of the two words read picks one of them by the
   address's vector, and a patch read through a register stays refused */
static void indexed_attribute_load(void) {
    const u64 al2p = (0xefa070010607ff01ull & ~(0xffull | (0xffull << 8) | (0x7ffull << 20))) | 5ull | (4ull << 8);
    const u64 ald = (0xefd9ff800707ff00ull & ~((0xffull << 8) | (0x3ffull << 20) | (3ull << 47))) | (5ull << 8) |
                    (1ull << 47); /* ALD R0, a[R5], 2 words */
    const u64 instructions[] = {mov32i(4, 0x90), al2p, ald, 0xeff000000007ff00ull | (0x80ull << 20) | (1ull << 47)};
    ShaderInfo info;
    size_t slots, words = 0;
    u64* code = guest_program(instructions, 4, NULL, &slots);
    if (!code) return;
    set_sph(code, 6, 0xf0f0); /* generic inputs 1 and 3, all four components */
    memset(&info, 0, sizeof info);
    u32* spv = spirv_of(code, slots * sizeof *code, ST_VS, &info, &words, 0, 1, 1, 1);
    check_translated(spv, &info, "ALD through an index register translates");
    check(spv && info.in_mask == ((1u << 1) | (1u << 3)), "an indexed ALD declares the inputs the header maps");
    check(spv && count_opcode(spv, words, 169, 0xffffffffu) >= 4, "each indexed word selects among the inputs");
    if (spv) save_fixture(attribute_paths[6], spv, words);
    free(spv);
    free(code);
    const u64 patch[] = {mov32i(4, 0x20), ald | (1ull << 31)};
    spv = translate(patch, 2, ST_TES, &info, &words, 0);
    check_refused(spv, &info, "ALD", "a patch read through an index register is refused");
    free(spv);
    /* a game's fragment programs: AL2P R8, R2 + 0x80 and IPA.IDX.MUL R0, a[R8], R7 into colour 0, the inputs the
       header gives a mode (0 and 1, perspective) the candidates */
    const u64 fragment_al2p =
        (0xefa070010607ff01ull & ~(0xffull | (0xffull << 8) | (0x7ffull << 20))) | 8ull | (2ull << 8) | (0x80ull << 20);
    const u64 fragment[] = {mov32i(2, 0x10), fragment_al2p, 0xe06000c800770800ull};
    code = guest_program(fragment, 3, NULL, &slots);
    if (!code) return;
    set_sph(code, 6, 0xaaaa);
    set_sph(code, 18, 0xf);
    memset(&info, 0, sizeof info);
    spv = spirv_of(code, slots * sizeof *code, ST_FS, &info, &words, 0, 1, 1, 1);
    check_translated(spv, &info, "IPA.IDX translates");
    check(spv && info.in_mask == 3u, "IPA.IDX declares the inputs the header gives a mode");
    check(spv && count_opcode(spv, words, 169, 0xffffffffu) >= 2, "IPA.IDX selects among the inputs");
    free(spv);
    free(code);
}

/* A fragment program writing a sample mask and depth after its colour; reading PrimitiveId, SR_THREAD_KILL and
   PIXLD's sample and coverage; and a compute program reading SR_TID packed and the clock's high word */
static void system_values(void) {
    u64 program[64] = {0};
    ShaderInfo info;
    size_t words = 0;
    const u32 sph_output[2] = {0xf, 3}; /* RGBA in R0-R3, then sample mask in R4 and depth in R5 */
    set_sph(program, 18, sph_output[0]);
    set_sph(program, 19, sph_output[1]);
    program[10] = SCHED;
    program[11] = mov32i(4, 0xff);
    program[12] = EXIT;
    memset(&info, 0, sizeof info);
    u32* code = spirv_of(program, sizeof program, ST_FS, &info, &words, 0, 1, 1, 1);
    check_translated(code, &info, "sample-mask and depth output translates");
    check(code && has_builtin(code, words, 20) == 1 && has_builtin(code, words, 22) == 1,
          "a fragment program writes its sample mask and depth");
    if (code)
        check(seeded_register_feeds(code, words, 0xff, 62, 2),
              "the sample-mask output reads the register after the colour outputs");
    if (code) save_fixture(attribute_paths[0], code, words);
    free(code);
    static const u64 fragment[] = {
        0xe003ff860ff7ff00ull, /* IPA R0, a[0x60]: PrimitiveId */
        0xf0c8000001370001ull, /* S2R R1, SR_THREAD_KILL */
        0xefe8e0028007ff02ull, /* PIXLD.MYINDEX R2 */
        0xefe8e0008007ff03ull, /* PIXLD.COVMASK R3 */
    };
    code = translate(fragment, 4, ST_FS, &info, &words, 0);
    check_translated(code, &info, "fragment system-value inputs translate");
    check(code && has_builtin(code, words, 7) == 1 && has_builtin(code, words, 18) == 1 &&
              has_builtin(code, words, 20) == 1 && count_opcode(code, words, 71, 14) >= 3 &&
              count_opcode(code, words, 5381, 0xffffffffu) == 1,
          "a fragment program reads PrimitiveId, its helper state, its sample and its coverage");
    if (code) save_fixture(attribute_paths[1], code, words);
    free(code);
    const u64 pixld_sample = 0xefe8e0028007ff02ull;
    const u64 pixld_coverage = 0xefe8e0008007ff03ull;
    code = translate(&pixld_sample, 1, ST_FS, &info, &words, 0);
    check_translated(code, &info, "PIXLD.MYINDEX translates");
    if (code)
        check(has_builtin(code, words, 18) && !has_builtin(code, words, 20) && info.uses_sample_id,
              "PIXLD.MYINDEX reads SampleId rather than SampleMask");
    free(code);
    code = translate(&pixld_coverage, 1, ST_FS, &info, &words, 0);
    check_translated(code, &info, "PIXLD.COVMASK translates");
    if (code)
        check(has_builtin(code, words, 20) && !has_builtin(code, words, 18) && !info.uses_sample_id,
              "PIXLD.COVMASK reads SampleMask rather than SampleId");
    free(code);
    static const u64 layer_primitive[] = {
        0xe003ff864ff7ff00ull, /* IPA R0, a[0x64]: Layer */
        0xe003ff860ff7ff01ull, /* IPA R1, a[0x60]: PrimitiveId */
    };
    code = translate(layer_primitive, 2, ST_FS, &info, &words, 0);
    check_translated(code, &info, "Layer and PrimitiveId inputs translate");
    check(code && has_builtin(code, words, 9) == 1 && has_builtin(code, words, 7) == 1 &&
              count_opcode(code, words, 17, 2) == 1,
          "Layer and PrimitiveId share one Geometry capability");
    free(code);
    g_shader_geometry = 0;
    code = translate(fragment, 4, ST_FS, &info, &words, 0);
    check_translated(code, &info, "PrimitiveId with tessellation translates");
    check(code && has_builtin(code, words, 7) == 1 && count_opcode(code, words, 17, 3) == 1,
          "PrimitiveId uses Tessellation when Geometry is unavailable");
    free(code);
    g_shader_tessellation = 0;
    code = translate(fragment, 4, ST_FS, &info, &words, 0);
    check_translated(code, &info, "PrimitiveId fallback translates");
    check(code && !info.unsupported && !has_builtin(code, words, 7),
          "PrimitiveId reads as zero when geometry and tessellation are unavailable");
    free(code);
    g_shader_geometry = 1;
    g_shader_tessellation = 1;
    static const u64 compute[] = {
        0xf0c8000002070000ull, /* S2R R0, SR_TID */
        0xf0c8000005170001ull, /* S2R R1, SR_CLOCKHI */
    };
    code = translate(compute, 2, ST_CS, &info, &words, 0);
    check_translated(code, &info, "a compute program reads SR_TID and SR_CLOCKHI");
    if (code) {
        check(count_opcode(code, words, 5056, 0xffffffffu) == 1, "SR_CLOCKHI reads the subgroup clock");
        check(has_insert_range(code, words, 16, 10) && has_insert_range(code, words, 26, 6),
              "SR_TID packs y into bits 16-25 and z into bits 26-31");
    }
    if (code) save_fixture(attribute_paths[2], code, words);
    free(code);
    const u64 pair_clock[] = {(0x50c800000ff70004ull & ~(0xffull << 20)) | (0x50ull << 20)};
    code = translate(pair_clock, 1, ST_CS, &info, &words, 0);
    check_translated(code, &info, "CS2R reads both clock words");
    if (code) check(count_opcode(code, words, 5056, 0xffffffffu) == 1, "CS2R samples the clock once");
    if (code) check(clock_pair_extracts_both_words(code, words), "CS2R writes both words of one clock sample");
    free(code);
    g_shader_clock_scopes = 0;
    code = translate(pair_clock, 1, ST_CS, &info, &words, 0);
    check_refused(code, &info, "shader clock unavailable", "CS2R refuses an unavailable shader clock");
    free(code);
    g_shader_clock_scopes = 3;
    code = translate(compute, 2, ST_FS, &info, &words, 0);
    check_refused(code, &info, "S2R", "SR_TID is unavailable outside compute");
    free(code);
}

/* Indexed depth comparisons retain the hardware compare operation for depth
   entries, while color entries use their own TSC function. Both paths must be
   valid in a single module; the emitted fixtures also run through spirv-val. */
static void audit_shader_regressions(void) {
    const u64 indexed[] = {
        0xe003ff870ff7ff00ull,
        0x5cb0118000070a00ull,
        0x3848000000470000ull,
        0xef94002000070004ull,
        0x5c9807800ff70000ull,
        0x5c9807800ff70001ull,
        0xdeb8000000070008ull | (2ull << 28) | (1ull << 37) | (4ull << 20) | (1ull << 50) | (1ull << 31),
    };
    ShaderInfo info;
    size_t words = 0;
    g_shader_texture_arrays = 1;
    u32* code = translate(indexed, 7, ST_FS, &info, &words, 1);
    check_translated(code, &info, "indexed shadow translates mixed depth/color table");
    if (code) {
        check(count_opcode(code, words, 88, 0xffffffffu) && count_opcode(code, words, 90, 0xffffffffu),
              "indexed shadow keeps both plain and hardware depth LOD samples");
        check(count_opcode(code, words, 180, 0xffffffffu) > 0,
              "indexed shadow also compares color entries in the shader");
        save_fixture(audit_paths[0], code, words);
    }
    free(code);
    u64 variant[7];
    memcpy(variant, indexed, sizeof variant);
    variant[6] |= 1ull << 36; /* Register offsets retain the selected array's own mip semantics. */
    code = translate(variant, 7, ST_FS, &info, &words, 1);
    check_translated(code, &info, "indexed shadow dynamic offset translates");
    if (code) {
        check(!count_opcode(code, words, 103, 0xffffffffu) &&
                  has_image_operands(code, words, 88, 10) && has_image_operands(code, words, 90, 10),
              "indexed shadow keeps exact Lod|ConstOffset for both descriptor arrays");
        save_fixture(audit_paths[4], code, words);
    }
    free(code);
    variant[6] = 0xdec0000000070008ull | (2ull << 28) | (1ull << 31) | (4ull << 20) | (1ull << 50);
    code = translate(variant, 7, ST_FS, &info, &words, 1);
    check_translated(code, &info, "indexed shadow gather translates");
    if (code) {
        check(count_opcode(code, words, 96, 0xffffffffu) && count_opcode(code, words, 97, 0xffffffffu),
              "indexed shadow gather keeps color and hardware depth paths");
        save_fixture(audit_paths[5], code, words);
    }
    free(code);
    /* One handle table read as a 2D texture, a cube and a 2D depth compare: one game's character programs index
       tables of more than one kind. Each kind has its own array on the shared bindings, so the views of each array's
       elements match its type. */
    const u64 mixed[] = {
        0xe003ff870ff7ff00ull,
        0x5cb0118000070a00ull,
        0x3848000000470000ull,
        0xef94002000070004ull,
        0x5c9807800ff70000ull,
        0x5c9807800ff70001ull,
        0x5c9807800ff70002ull,
        0xdeb8000000070008ull | (2ull << 28) | (4ull << 20) | (1ull << 31),
        0xdeb8000000070009ull | (6ull << 28) | (4ull << 20) | (1ull << 31),
        0xdeb800000007000aull | (2ull << 28) | (1ull << 37) | (4ull << 20) | (1ull << 50) | (1ull << 31),
    };
    code = translate(mixed, 10, ST_FS, &info, &words, 1);
    check_translated(code, &info, "indexed textures of three kinds translate");
    if (code) {
        int arrays = 0, depth_arrays = 0;
        for (size_t i = 5; i < words; i += code[i] >> 16) {
            if (!(code[i] >> 16)) break;
            if ((code[i] & 0xffff) != 71 || code[i + 2] != 33) continue; /* OpDecorate Binding */
            arrays += code[i + 3] == SNX_VKB_TEXARRAY;
            depth_arrays += code[i + 3] == SNX_VKB_TEXDEPTH;
        }
        check(info.ntex == 3 && arrays == 3 && depth_arrays == 1,
              "each indexed kind declares its own array, and the shadow kind its depth array");
        save_fixture(audit_paths[6], code, words);
    }
    free(code);
    g_shader_texture_arrays = 0;
    const u64 instance = 0xefd800000007ff00ull | (0x2f8ull << 20);
    /* without draw parameters the instance ID is InstanceIndex less the guest's base instance, pushed; with them
       InstanceIndex less BaseInstance plus what the draw's first instance adds to the guest's base (a 3D engine draw
       that continues an earlier one's instances), so an indirect draw's base instance can come from the GPU */
    g_shader_draw_parameters = 0;
    code = translate(&instance, 1, ST_VS, &info, &words, 1);
    check_translated(code, &info, "instance attribute translates with original guest base");
    if (code) {
        check(count_builtin(code, words, 43) == 1 && count_builtin(code, words, 4425) == 0,
              "instance without draw parameters uses InstanceIndex and not BaseInstance");
        check(count_opcode(code, words, 130, 0xffffffffu) == 1, "instance subtracts the original guest base");
        save_fixture(audit_paths[1], code, words);
    }
    free(code);
    g_shader_draw_parameters = 1;
    code = translate(&instance, 1, ST_VS, &info, &words, 1);
    check_translated(code, &info, "instance attribute translates with draw parameters");
    if (code) {
        check(count_builtin(code, words, 43) == 1 && count_builtin(code, words, 4425) == 1,
              "instance with draw parameters uses InstanceIndex and BaseInstance");
        check(count_opcode(code, words, 130, 0xffffffffu) == 2,
              "instance subtracts BaseInstance less the pushed offset");
    }
    free(code);
    /* ALD.64 R0, a[0x2f8]: the instance number into R0 and the vertex number into R1 in one read, as uam compiles a
       vertex program that places its point by gl_VertexID and gl_InstanceID. It wasn't translated, and its draws
       were skipped */
    const u64 instance_vertex = 0xefd8ff802f87ff00ull;
    code = translate(&instance_vertex, 1, ST_VS, &info, &words, 1);
    check_translated(code, &info, "the instance and vertex numbers read as a pair translate");
    if (code)
        check(count_builtin(code, words, 43) == 1 && count_builtin(code, words, 42) == 1,
              "the pair reads InstanceIndex and VertexIndex");
    free(code);
    const u64 tlds = 0xD244012030570A02ull;
    for (int sign = 0; sign < 2; sign++) {
        size_t slots;
        u64* guest = guest_program(&tlds, 1, NULL, &slots);
        memset(&info, 0, sizeof info);
        info.tex_int_mask = 1;
        info.tex_sint_mask = sign;
        code = spirv_of(guest, slots * sizeof *guest, ST_FS, &info, &words, 1, 1, 1, 1);
        check_translated(code, &info, "integer TLDS half output translates");
        if (code) {
            check(count_opcode(code, words, sign ? 111 : 112, 0xffffffffu) > 0,
                  "integer TLDS converts its actual integer component type to float");
            save_fixture(audit_paths[2 + sign], code, words);
        }
        free(code);
        free(guest);
    }
}

/* Fire Emblem Three Houses fragment shaders sample fp16 surfaces with TLDS.
   The f32 forms translate; the fp16 form must translate the same way, with the
   components converted and packed into halves. */
static void tlds_fp16(void) {
    static const u64 words[] = {0xD244012030570A02ull, 0xD252010040570A18ull, 0xD252014000570A1Aull,
                                0xD25200E000B70A17ull, 0xD252014010B70A06ull};
    int saved = g_shader_half_rounding;
    for (u32 backend = 0; backend < 3; backend++) {
        g_shader_half_rounding = backend == 2 ? 3 : (int)backend;
        for (size_t i = 0; i < sizeof words / sizeof *words; i++) {
            ShaderInfo info;
            size_t size = 0;
            u32* code = translate(&words[i], 1, ST_FS, &info, &size, 1);
            if (!code || info.unsupported)
                fprintf(stderr, "tlds fp16 backend%u case%zu: %s\n", backend, i, info.first_unsupported);
            check_translated(code, &info, "TLDS fp16 translates with native or software rounding");
            if (code) {
                check(count_opcode(code, size, 95, 0xffffffffu) == 1,
                      "TLDS fp16 retains its texel fetch before packing");
                check(count_opcode(code, size, 17, 9) == !!backend,
                      "TLDS fp16 requires Float16 only on the controlled native path");
                if (backend) {
                    check(count_opcode(code, size, 115, 0xffffffffu) == 2 &&
                              count_opcode(code, size, 16, 4462) == (backend == 2 ? 2 : 1) &&
                              !has_glsl_instruction(code, size, 58),
                          "TLDS packs both pairs with native conversion and explicit RTE modes");
                } else {
                    check(count_opcode(code, size, 115, 0xffffffffu) == 0 && count_opcode(code, size, 16, 4462) == 0 &&
                              has_glsl_instruction(code, size, 58) && count_opcode(code, size, 194, 0xffffffffu) >= 4 &&
                              count_opcode(code, size, 197, 0xffffffffu) >= 2,
                          "TLDS software packing rounds both pairs with integer guard and sticky bits");
                }
            }
            free(code);
        }
    }
    g_shader_half_rounding = saved;
}

/* OpTypeImage declarations of that dimensionality (0 1D, 1 2D, 2 3D, 3 cube) */
static int count_image_dim(const u32* code, size_t words, u32 dim) {
    int found = 0;
    for (size_t i = 5; i < words;) {
        u32 width = code[i] >> 16, op = code[i] & 0xffff;
        if (!width || i + width > words) return -1;
        if (op == 25 && width > 3 && code[i + 3] == dim) found++;
        i += width;
    }
    return found;
}
/* A compute program of one game queries a volume's size with a TXQ through a bound handle and fetches its texels with
   a TLD.B through a bindless one: nothing in the program joins the two, so the query's texture was a 2D one, which
   the renderer binds a 1x1 null view to (a 3D texture has no 2D view), and every invocation but the first left on
   the size check. The translation marks the slot a TXQ alone names (tex_query_mask), and the renderer keys the
   program on the header's dimension (tex_query_hint): a 3D query then, answering the depth too */
static void query_only_volume(void) {
    static const u64 insns[] = {HANDLE_CBUF_R4, 0x5c9807800ff70008ull /* MOV R8, RZ */,
                                0xdf48048380470808ull /* TXQ R8, R8, 0x48 */,
                                0xdd380007c0470004ull /* TLD.B R4, R0, R4, 3D */};
    size_t slots, words = 0;
    u64* guest = guest_program(insns, 4, NULL, &slots);
    ShaderInfo info = {0};
    if (!guest) {
        check(0, "query_only_volume: out of memory");
        return;
    }
    info.cs_local[0] = info.cs_local[1] = info.cs_local[2] = 1;
    u32* code = spirv_of(guest, slots * sizeof *guest, ST_CS, &info, &words, 1, 1, 1, 1);
    check_translated(code, &info, "a bound TXQ beside a bindless TLD.B translates");
    if (code) {
        check(info.tex_query_mask == 1, "the texture a TXQ alone names is slot 0 of tex_query_mask");
        check(count_image_dim(code, words, 1) == 1 && count_image_dim(code, words, 2) == 1 && info.ntex == 2 &&
                  info.tex[0].type == 1 && info.tex[0].cbuf_off == 0x48 && info.tex[1].type == 2,
              "without a hint the query's texture is 2D and the fetch's 3D");
    }
    free(code);
    memset(&info, 0, sizeof info);
    info.cs_local[0] = info.cs_local[1] = info.cs_local[2] = 1;
    info.tex_query_hint[0] = 1 + 2; /* the header: a 3D texture */
    code = spirv_of(guest, slots * sizeof *guest, ST_CS, &info, &words, 1, 1, 1, 1);
    check_translated(code, &info, "a bound TXQ keyed as a volume translates");
    if (code) {
        check(info.tex_query_mask == 1, "the keyed query's texture is still slot 0 of tex_query_mask");
        /* one OpTypeImage serves both 3D textures, the query's and the fetch's */
        check(count_image_dim(code, words, 1) == 0 && count_image_dim(code, words, 2) == 1 && info.ntex == 2 &&
                  info.tex[0].type == 2 && !info.tex[0].array && info.tex[0].cbuf_off == 0x48,
              "keyed on a 3D header the query's texture is 3D");
    }
    free(code);
    free(guest);
}

/* the query hints of a graphics program's key (words 14 to 17 of vk_shader_get's mask) hold every slot's hint and
   come back as they went */
static void query_hint_words(void) {
    u8 hint[SH_MAXTEX], back[SH_MAXTEX], want[SH_MAXTEX];
    u32 words[SH_QUERY_WORDS];
    int zero = 1;
    for (int i = 0; i < SH_MAXTEX; i++) {
        hint[i] = (u8)(i % 9);                /* 0 to 8, the hints there are */
        want[i] = hint[i] == 1 ? 0 : hint[i]; /* a 1D header's query is the 2D slot's */
    }
    shader_query_hint_pack(hint, words);
    shader_query_hint_unpack(words, back);
    check(!memcmp(want, back, sizeof want), "a graphics key's query hint words carry every slot's hint back");
    memset(hint, 0, sizeof hint);
    shader_query_hint_pack(hint, words);
    for (int i = 0; i < SH_QUERY_WORDS; i++) zero &= words[i] == 0;
    check(zero, "no hint packs as zero words");
}

/* the same for a vertex and a fragment program: the slot a TXQ alone names is marked, 2D without a hint and the
   header's dimension with one (the graphics key carries it, pipeline.c sh_key_info) */
static void query_only_volume_stages(void) {
    static const u64 insns[] = {
        0x0100000000071005ull /* MOV32I R5, 0 */, 0xdf48010200470500ull /* TXQ.DIMENSION R0, R5, 0x10, depth only */,
        0x1c0ffffffff00009ull /* IADD32I R9, R0, -1 */, 0x5cb8000000970a09ull /* I2F.F32.U32 R9, R9 */};
    const int stages[] = {ST_VS, ST_FS};
    for (int k = 0; k < 2; k++) {
        size_t slots, words = 0;
        u64* guest = guest_program(insns, 4, NULL, &slots);
        ShaderInfo info = {0};
        if (!guest) {
            check(0, "query_only_volume_stages: out of memory");
            return;
        }
        u32* code = spirv_of(guest, slots * sizeof *guest, stages[k], &info, &words, 1, 1, 1, 1);
        check_translated(code, &info, "a TXQ alone translates in a vertex or fragment program");
        if (code) {
            fprintf(stderr, "stage %d no hint: ntex %d type %d query_mask %llx 2D %d 3D %d\n", stages[k], info.ntex,
                    info.ntex ? info.tex[0].type : -1, (unsigned long long)info.tex_query_mask,
                    count_image_dim(code, words, 1), count_image_dim(code, words, 2));
            check(info.tex_query_mask == 1 && info.ntex == 1 && info.tex[0].type == 1 &&
                      count_image_dim(code, words, 1) == 1 && count_image_dim(code, words, 2) == 0,
                  "without a hint a vertex or fragment program's lone query is 2D");
        }
        free(code);
        memset(&info, 0, sizeof info);
        info.tex_query_hint[0] = 1 + 2;
        code = spirv_of(guest, slots * sizeof *guest, stages[k], &info, &words, 1, 1, 1, 1);
        check_translated(code, &info, "a TXQ alone keyed as a volume translates in a vertex or fragment program");
        if (code) {
            fprintf(stderr, "stage %d hint 3D: ntex %d type %d query_mask %llx 2D %d 3D %d\n", stages[k], info.ntex,
                    info.ntex ? info.tex[0].type : -1, (unsigned long long)info.tex_query_mask,
                    count_image_dim(code, words, 1), count_image_dim(code, words, 2));
            check(info.tex_query_mask == 1 && info.ntex == 1 && info.tex[0].type == 2 &&
                      count_image_dim(code, words, 1) == 0 && count_image_dim(code, words, 2) == 1,
                  "keyed on a 3D header a vertex or fragment program's lone query is 3D");
        }
        free(code);
        free(guest);
    }
}

/* A fragment program at a resolution scale (SNX_VKB_RS: 1 / the scale twice, then the scale). TLD.LZ at texel p
   fetches p where the push constants do not mark the slot as bound to a scaled target. Where they do, and mark the
   target as one of the size drawn to, the pixel of texel p that lies where the fragment lies in its guest pixel
   (issues #730, #1579): at 2x texel (5, 3) is (10, 6) for the fragment at (6, 4) and (11, 7) for the one at (7, 5),
   where the centre of the scaled texel gave (11, 7) to both; in a lower-left window, whose y counts up, the rows are
   7 and 6; at 1.5x texel 4 is pixel 6 alone, where the fragment's place would give 7; at 0.5x texel (5, 3) is
   (2, 1). A scaled target of another size (a table) gives every fragment the centre. TLDS and a surface load (SULD)
   fetch the same pixel */
static void scaled_texel_fetch(void) {
    static const struct {
        float scale, fx, fy; /* the fragment's FragCoord */
        /* the texel fetched, its slot's bit, whether its target has the size drawn to, the window's origin */
        u32 px, py, scaled, own, lower_left;
        u32 x, y; /* the coordinates fetched */
    } cases[] = {
        {2.0f, 7.5f, 5.5f, 5, 3, 1, 1, 0, 11, 7}, {2.0f, 6.5f, 4.5f, 5, 3, 1, 1, 0, 10, 6},
        {2.0f, 7.5f, 5.5f, 5, 3, 0, 0, 0, 5, 3},  {2.0f, 7.5f, 5.5f, 5, 3, 1, 1, 1, 11, 6},
        {2.0f, 6.5f, 4.5f, 5, 3, 1, 1, 1, 10, 7}, {2.0f, 6.5f, 4.5f, 5, 3, 0, 1, 1, 5, 3},
        {1.5f, 5.5f, 3.5f, 4, 3, 1, 1, 0, 6, 5},  {0.5f, 2.5f, 1.5f, 5, 3, 1, 1, 0, 2, 1},
        {2.0f, 6.5f, 4.5f, 5, 3, 1, 0, 0, 11, 7}, {2.0f, 6.5f, 4.5f, 5, 3, 1, 0, 1, 11, 7},
        {1.5f, 4.5f, 3.5f, 5, 3, 1, 0, 0, 8, 5},  {3.0f, 0.5f, 0.5f, 5, 3, 1, 0, 0, 16, 10},
    };
    static const u64 tld_2d[] = {0xdc00000000000000ull | 7ull << 51 | 0xfull << 31 | 2ull << 28 | 0x10ull << 36 |
                                 0xffull << 20 | 7ull << 16 | 2ull << 8}; /* TLD.LZ R0, R2, RZ, 2D */
    /* TXQ.B: the handle in R4, the level in R5 */
    static const u64 txq_2d[] = {HANDLE_CBUF_R4, 0x5c9807800ff70005ull, 0xdf50000180470400ull};
    ShaderInfo info;
    size_t words = 0;
    u32* code = NULL;
    u32* constants = calloc(4096 * 4, sizeof *constants); /* the constant buffers SULD's handle comes from: zeros */
    if (!constants) {
        check(0, "scaled_texel_fetch: out of memory");
        return;
    }
    for (u32 k = 0; k < 3 * sizeof cases / sizeof *cases; k++) {
        u32 at = k / 3, form = k % 3;
        const char* name = form == 0 ? "TLD" : form == 1 ? "TLDS" : "SULD.D.32";
        /* x and y in R2 and R3, then the TLD, or TLDS.LZ R0, R2, R3, 2D (encoding 2: x in Ra, y in Rb), or the handle in
           R4 and SULD.D.32 R8, R2, a raw load of a 2D surface */
        const u64 fetch[] = {mov32i(2, cases[at].px), mov32i(3, cases[at].py),
                             form == 0   ? tld_2d[0]
                             : form == 1 ? 0xd200000000000000ull | 2ull << 53 | 0x10ull << 36 | 0xffull << 28 |
                                               3ull << 20 | 7ull << 16 | 2ull << 8
                                         : HANDLE_CBUF_R4,
                             0xeb00000000070208ull | 3ull << 33 | 4ull << 39 | 1ull << 52 | 4ull << 20};
        code = translate(fetch, form == 2 ? 4 : 3, ST_FS, &info, &words, 0);
        check_translated(code, &info, "a fragment fetch of a 2D texture translates");
        if (!code) continue;
        float s = cases[at].scale;
        u32 scale[4], bits;
        const float values[3] = {1.0f / s, 1.0f / s, s};
        memcpy(scale, values, sizeof values);
        scale[3] = 0;
        SpvEvalEnv env = {0};
        memcpy(&bits, &cases[at].fx, 4);
        env.builtin[15][0] = bits; /* FragCoord */
        memcpy(&bits, &cases[at].fy, 4);
        env.builtin[15][1] = bits;
        for (u32 bank = 0; bank < 18; bank++) {
            env.ubo[SNX_VKB_CB + bank] = constants;
            env.ubo_words[SNX_VKB_CB + bank] = 4096 * 4;
        }
        env.ubo[SNX_VKB_RS] = scale;
        env.ubo_words[SNX_VKB_RS] = 4;
        env.push[4] = cases[at].scaled; /* the uvec4 after the fragment stage's vec4: slot 0's bit, the clip's height */
        env.spec[SH_SPEC_LOWER_LEFT] = cases[at].lower_left;
        env.spec_given = cases[at].lower_left ? 1u << SH_SPEC_LOWER_LEFT : 0; /* upper left is the default */
        env.push[7] = 8;
        env.push[12] = cases[at].own; /* the uvec4 at byte 96: slot 0's target has the size drawn to */
        const char* why = spv_eval(code, words, &env);
        if (why) fprintf(stderr, "scaled fetch: the evaluator stops at %s\n", why);
        int fetched = !why && env.fetched[SNX_VKB_TEX][0] == cases[at].x && env.fetched[SNX_VKB_TEX][1] == cases[at].y;
        if (!fetched)
            fprintf(stderr,
                    "scaled fetch: at %gx the fragment at (%g, %g) of a%s window fetches (%s) texel (%u, %u) of a "
                    "texture %s a scaled target at (%u, %u), not (%u, %u)\n",
                    (double)s, (double)cases[at].fx, (double)cases[at].fy,
                    cases[at].lower_left ? " lower-left" : "n upper-left", name, cases[at].px, cases[at].py,
                    !cases[at].scaled ? "that is not"
                    : cases[at].own   ? "bound to, of the size drawn to,"
                                      : "bound to, of another size than the one drawn to,",
                    env.fetched[SNX_VKB_TEX][0], env.fetched[SNX_VKB_TEX][1], cases[at].x, cases[at].y);
        check(fetched,
              "a fragment fetch of a scaled target reads the pixel where the fragment lies in its guest pixel");
        free(code);
    }
    free(constants);
    code = translate(txq_2d, 3, ST_FS, &info, &words, 0);
    check_translated(code, &info, "fragment TXQ translates");
    if (code)
        check(count_opcode(code, words, 112, 0xffffffffu) == 2 && count_opcode(code, words, 109, 0xffffffffu) == 2,
              "fragment TXQ answers width and height over the scale its texture's bit selects");
    free(code);
    code = translate(tld_2d, 1, ST_VS, &info, &words, 0);
    check_translated(code, &info, "vertex TLD 2D translates");
    if (code) check(count_opcode(code, words, 111, 0xffffffffu) == 0, "a vertex TLD fetches at the guest's texel");
    free(code);
}

/* Fire Emblem Three Houses fragment shaders sample array textures with TXD,
   explicit gradients in Rb. It must sample with the Grad operands and write
   the masked components to consecutive registers. */
static void txd_grad(void) {
    static const u64 words[] = {0xDE3800A3B0071010ull};
    for (size_t i = 0; i < sizeof words / sizeof *words; i++) {
        ShaderInfo info;
        size_t size = 0;
        u32* code = translate(&words[i], 1, ST_FS, &info, &size, 1);
        if (!code || info.unsupported) fprintf(stderr, "txd %zu: %s\n", i, info.first_unsupported);
        check(code && !info.unsupported, "TXD translates");
        if (code) check(count_opcode(code, size, 88, 0xffffffffu) >= 1, "TXD samples with explicit LOD");
        free(code);
    }
}

/* a rejected instruction is logged with its word */
static void unsupported_names_word(void) {
    ShaderInfo info;
    size_t words = 0;
    u32* code = translate((const u64[]){0xFFFFFFFFFFFFFFFFull}, 1, ST_FS, &info, &words, 0);
    check(!code && !strcmp(info.first_unsupported, "unknown opcode ffffffffffffffff"), "an unknown opcode is named");
    free(code);
    /* SULD.D.64 on a buffer image: its 32-bit form is translated */
    code = translate((const u64[]){0xEB10030200580502ull}, 1, ST_CS, &info, &words, 0);
    check(!code && !strcmp(info.first_unsupported, "SULD eb10030200580502"), "a SULD form not done yet is named");
    free(code);
}

/* a depth-compare sample (TEXS, shadow) of a color texture: the image is declared as color and sampled without a
   reference, and the shader compares its first component (ShaderInfo.tex_dc_color_mask); a depth texture keeps the
   compare sample */
static void dc_color_compare(void) {
    const u64 texs = 0xD800000000000000ull | (4ull << 53) | (1ull << 36) | (255ull << 28) | (1ull << 20) | 3;
    u64 code[64] = {0};
    code[10] = SCHED;
    code[11] = texs;
    code[12] = EXIT;
    for (int color = 0; color < 2; color++) {
        ShaderInfo info = {0};
        info.tex_dc_color_mask = (u32)color;
        info.tex_dc_color_funcs = 4; /* GREATER */
        size_t words = 0;
        u32* spv = spirv_of(code, sizeof code, ST_FS, &info, &words, 1, 1, 1, 1);
        check(spv && info.ntex == 1 && info.tex[0].shadow, "a TEXS shadow sample translates");
        if (!spv) continue;
        /* OpImageSampleDrefImplicitLod 89, OpImageSampleImplicitLod 87, OpSelect 169 */
        int dref = count_opcode(spv, words, 89, 0xffffffffu), plain = count_opcode(spv, words, 87, 0xffffffffu),
            select = count_opcode(spv, words, 169, 0xffffffffu);
        check(color ? !dref && plain == 1 && select >= 1 : dref == 1 && !plain,
              color ? "a color texture is sampled plainly and compared in the shader" : "a depth texture keeps Dref");
        check(!color || info.tex_dc_color_mask == 1, "the translated info keeps the depth-compare key");
        if (color) save_fixture(dc_color_path, spv, words);
        free(spv);
    }
}

/* A fragment input the stage before rasterization doesn't write (ShaderInfo.in_unfed_mask) isn't declared, so
   Vulkan's interface matches, and IPA reads it as 0, 0, 0, 1 */
static void unfed_fragment_input(void) {
    u64 code[64] = {0};
    set_sph(code, 18, 15);  /* one RGBA output */
    set_sph(code, 7, 0xff); /* byte 0x18 + 4: generic input 4, all components, perspective */
    code[10] = SCHED;
    code[11] = 0xE000000000070000ull | (0xC0ull << 28) | 0xFF00; /* IPA R0, a[0xc0]: input 4's x */
    code[12] = EXIT;
    int locations[2] = {0, 0};
    for (int unfed = 0; unfed < 2; unfed++) {
        ShaderInfo info = {0};
        size_t words = 0;
        info.in_unfed_mask = unfed ? 1u << 4 : 0;
        u32* spv = spirv_of(code, sizeof code, ST_FS, &info, &words, 1, 1, 1, 1);
        check(spv && !info.unsupported && (info.in_mask >> 4 & 1), "IPA of a generic input translates");
        if (!spv) continue;
        locations[unfed] = count_opcode(spv, words, 71, 30); /* OpDecorate ... Location */
        check(!unfed || info.in_unfed_mask == 1u << 4, "the translated info keeps the unfed-input key");
        free(spv);
    }
    check(locations[1] == locations[0] - 1, "an unfed input isn't declared, and the others still are");
}

/* FSWZADD R0, R1, R2 with this swizzle: each quad lane's mode in two bits, the upper left lane's the top two */
static u64 fswzadd(u32 swizzle) { return 0x50F8000000070000ull | (u64)swizzle << 28 | 2ull << 20 | 1ull << 8; }
static u32* fswzadd_spirv(u64 instruction, int stage, int can_subgroup, ShaderInfo* info, size_t* words) {
    size_t slots;
    u64* code = guest_program(&instruction, 1, NULL, &slots);
    if (!code) return NULL;
    memset(info, 0, sizeof *info);
    u32* result = spirv_of(code, slots * sizeof *code, stage, info, words, 0, 1, 1, can_subgroup);
    free(code);
    return result;
}

/* FSWZADD is lane-local: the lanes of a quad pick their mode from the subgroup's lane, with no derivative, in any
   stage. One mode for every lane needs no subgroup, and the flags (CC) aren't written */
static void fswzadd_forms(void) {
    ShaderInfo info;
    size_t words = 0;
    u32* code = fswzadd_spirv(fswzadd(0x99), ST_FS, 1, &info, &words); /* dFdx after SHFL.BFLY 1 */
    check_translated(code, &info, "FSWZADD with a swizzle per lane translates");
    if (code) {
        check(count_opcode(code, words, 207, 0xffffffffu) == 0 && count_opcode(code, words, 208, 0xffffffffu) == 0,
              "FSWZADD reads no derivative");
        check(count_opcode(code, words, 169, 0xffffffffu) == 2, "FSWZADD 0x99 selects the negated a and b per lane");
        save_fixture(fswzadd_path, code, words);
    }
    free(code);
    code = fswzadd_spirv(fswzadd(0xcc), ST_VS, 1, &info, &words); /* nouveau's TXD: a + b on the right, b left */
    check_translated(code, &info, "FSWZADD translates outside fragment programs");
    if (code) check(count_opcode(code, words, 169, 0xffffffffu) == 1, "FSWZADD 0xcc selects b on the left lanes");
    free(code);
    code = fswzadd_spirv(fswzadd(0xcc), ST_FS, 0, &info, &words);
    check_refused(code, &info, "subgroup", "FSWZADD with lanes that differ needs the subgroup's lane");
    free(code);
    int adds[4];
    for (u32 mode = 0; mode < 4; mode++) {
        code = fswzadd_spirv(fswzadd(mode * 0x55), ST_FS, 0, &info, &words);
        check_translated(code, &info, "FSWZADD with one mode for every lane translates without subgroups");
        adds[mode] = code ? count_opcode(code, words, 129, 0xffffffffu) : -1;
        if (code)
            check(count_opcode(code, words, 169, 0xffffffffu) == 0, "one FSWZADD mode for all lanes selects nothing");
        free(code);
    }
    check(adds[0] == adds[3] + 1 && adds[1] == adds[0] && adds[2] == adds[0], "FSWZADD mode 3 moves b, the others add");
    code = fswzadd_spirv(fswzadd(0) | 1ull << 47, ST_FS, 1, &info, &words);
    check_refused(code, &info, "FSWZADD", "FSWZADD.CC is refused");
    free(code);
}

/* SHFL R0, R1 with this mode (0 IDX, 1 UP, 3 BFLY) and mask, its lane an immediate or RZ */
static u64 shfl(u32 mode, int immediate, u32 index, u32 mask) {
    return 0xEF10000000070000ull | (u64)mask << 34 | 1ull << 29 | (u64)(immediate != 0) << 28 | (u64)mode << 30 |
           (u64)(immediate ? index : 255) << 20 | 1ull << 8;
}
static u32* shfl_spirv(u64 instruction, int stage, ShaderInfo* info, size_t* words) {
    size_t slots;
    u64* code = guest_program(&instruction, 1, NULL, &slots);
    if (!code) return NULL;
    memset(info, 0, sizeof *info);
    if (stage == ST_CS) info->cs_local[0] = 32, info->cs_local[1] = info->cs_local[2] = 1;
    u32* result = spirv_of(code, slots * sizeof *code, stage, info, words, 0, 1, 1, 1);
    free(code);
    return result;
}
/* the constant operand (lane or direction) of the module's one instruction of this opcode, ~0u without exactly one */
static u32 quad_operand(const u32* code, size_t words, u32 opcode) {
    u32 id = 0, found = ~0u;
    int count = 0;
    for (size_t i = 5; i < words && code[i] >> 16; i += code[i] >> 16)
        if ((code[i] & 0xffff) == opcode && code[i] >> 16 == 6) id = code[i + 5], count++;
    for (size_t i = 5; count == 1 && i < words && code[i] >> 16; i += code[i] >> 16)
        if ((code[i] & 0xffff) == 43 && code[i] >> 16 == 4 && code[i + 2] == id) found = code[i + 3]; /* OpConstant */
    return found;
}

/* ShaderInfo.tex_own_pixel_mask: a slot every read of which fetches the fragment's own pixel, TLDS 2D.LZ or TLD.LZ at
   F2I.TRUNC of IPA's a[0x70] and a[0x74], is one a draw may sample in place while rendering to it. A fetch one row
   off, a filtered TEXS, or both kinds of read of one slot leave it out: those read other pixels, which the draw
   itself may have written already */
#define OWN_TEXS(encoding, a, b)                                                                                       \
    (0xd000000000000000ull | 1ull << 59 | (u64)(encoding) << 53 | 0x10ull << 36 | 0xffull << 28 | (u64)(b) << 20 |     \
     7ull << 16 | (u64)(a) << 8)
static void feedback_own_pixel(void) {
    const u64 ipa_x = 0xe003ff870ff7ff00ull;                /* IPA R0, a[0x70] */
    const u64 ipa_y = 0xe003ff874ff7ff01ull;                /* IPA R1, a[0x74] */
    const u64 f2i_x = F2I, f2i_y = F2I | 1ull << 20 | 1ull; /* F2I.TRUNC R0, R0 and R1, R1 */
    const u64 tlds = OWN_TEXS(2, 0, 1) | 1ull << 57;        /* TLDS 2D.LZ R0, R0, R1 */
    const u64 tld = 0xdc00000000000004ull | 7ull << 51 | 0xfull << 31 | 2ull << 28 | 0x10ull << 36 | 0xffull << 20 |
                    7ull << 16;                  /* TLD.LZ R4, R0 on a 2D texture */
    const u64 texs = OWN_TEXS(2, 0, 1);          /* TEXS 2D.LZ R0, R0, R1 */
    const u64 row_below = 0x1c00000000170101ull; /* IADD32I R1, R1, 1 */
    const u64 own_tlds[] = {ipa_x, ipa_y, f2i_x, f2i_y, tlds};
    const u64 own_tld[] = {ipa_x, ipa_y, f2i_x, f2i_y, tld};
    const u64 shifted[] = {ipa_x, ipa_y, f2i_x, f2i_y, row_below, tlds};
    const u64 sampled[] = {ipa_x, ipa_y, texs};
    const u64 both[] = {ipa_x, ipa_y, f2i_x, f2i_y, tlds, texs};
    struct {
        const u64* instructions;
        int count;
        u64 own;
        const char* what;
    } cases[] = {{own_tlds, 5, 1, "TLDS at gl_FragCoord's integer x and y reads the fragment's own pixel"},
                 {own_tld, 5, 1, "TLD at gl_FragCoord's integer x and y reads the fragment's own pixel"},
                 {shifted, 6, 0, "TLDS a row below gl_FragCoord reads another pixel"},
                 {sampled, 3, 0, "TEXS at gl_FragCoord reads through the sampler, not the own pixel"},
                 {both, 6, 0, "a slot read by TLDS at its own pixel and by TEXS reads another pixel"}};
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        ShaderInfo info;
        size_t words = 0;
        u32* code = translate(cases[i].instructions, cases[i].count, ST_FS, &info, &words, 0);
        check_translated(code, &info, cases[i].what);
        if (code) {
            if (info.ntex != 1 || info.tex_own_pixel_mask != cases[i].own)
                fprintf(stderr, "  %d slots, own pixel mask %#llx, expected %#llx\n", info.ntex,
                        (unsigned long long)info.tex_own_pixel_mask, (unsigned long long)cases[i].own);
            check(info.ntex == 1 && info.tex_own_pixel_mask == cases[i].own, cases[i].what);
        }
        free(code);
    }
}

/* ShaderInfo.tex_screen_mask: a plain 2D sample at a perspective generic's (x / w, y / w), each an IPA multiplied by
   1 / gl_FragCoord.w as the console's compiler writes them, reads the fragment's own pixel's centre wherever the vertex
   program wrote that generic as its position mapped onto [0, 1]; screen_in names the generic. Read undivided, with y
   over another generic's w, with a level bias, or from a screen-linear input it reads other pixels */
#define SCREEN_IPA(d, addr)          (0xe000000000070000ull | (u64)(addr) << 28 | 0xff00ull | (u64)(d))
#define SCREEN_IPA_MUL(d, addr, mul) (SCREEN_IPA(d, addr) | 1ull << 54 | (u64)(mul) << 20)
#define SCREEN_RCP(d, a)             (0x5080000000470000ull | (u64)(a) << 8 | (u64)(d))
#define SCREEN_FMUL(d, a, b)         (0x5c68000000070000ull | (u64)(b) << 20 | (u64)(a) << 8 | (u64)(d))
#define SCREEN_TEX(d, a)                                                                                               \
    (0xc000000000000000ull | 0x10ull << 36 | 0xfull << 31 | 2ull << 28 | 0xffull << 20 | 7ull << 16 | (u64)(a) << 8 |  \
     (u64)(d))
static void screen_uv_case(const u64* instructions, int count, u32 imap7, u64 screen, const char* what) {
    size_t slots, words = 0;
    u64* code = guest_program(instructions, count, NULL, &slots);
    if (!code) return;
    set_sph(code, 7, imap7); /* byte 0x18 + 5 on: generic 5's (and 6's) components, 2 bits each */
    ShaderInfo info;
    memset(&info, 0, sizeof info);
    u32* spv = spirv_of(code, slots * sizeof *code, ST_FS, &info, &words, 0, 1, 1, 1);
    free(code);
    check_translated(spv, &info, what);
    int ok = spv && info.tex_screen_mask == screen && !info.tex_own_pixel_mask && (!screen || info.screen_in == 5);
    if (spv && !ok)
        fprintf(stderr, "  screen mask %#llx (generic %u), own pixel %#llx, expected screen %#llx\n",
                (unsigned long long)info.tex_screen_mask, info.screen_in, (unsigned long long)info.tex_own_pixel_mask,
                (unsigned long long)screen);
    check(ok, what);
    free(spv);
}
static void feedback_screen_uv(void) {
    /* generic 5 (a[0xd0]) divided by its w (a[0xdc]), the multiplier 1 / gl_FragCoord.w in R0, then the sample */
    const u64 ipa_w = SCREEN_IPA(0, 0x7c), rcp_w = SCREEN_RCP(0, 0), x = SCREEN_IPA_MUL(1, 0xd0, 0),
              y = SCREEN_IPA_MUL(2, 0xd4, 0), w = SCREEN_IPA_MUL(3, 0xdc, 0), rcp = SCREEN_RCP(3, 3),
              ux = SCREEN_FMUL(4, 1, 3), uy = SCREEN_FMUL(5, 2, 3), tex = SCREEN_TEX(8, 4);
    const u64 screen[] = {ipa_w, rcp_w, x, y, w, rcp, ux, uy, tex};
    const u64 screen_texs[] = {ipa_w, rcp_w, x, y, w, rcp, ux, uy, OWN_TEXS(1, 4, 5)};
    const u64 passed[] = {SCREEN_IPA(1, 0xd0), SCREEN_IPA(2, 0xd4), SCREEN_IPA(3, 0xdc), rcp, ux, uy, tex};
    const u64 undivided[] = {ipa_w, rcp_w, x, y, w, rcp, ux, uy, SCREEN_TEX(8, 1)};
    const u64 other_y[] = {ipa_w, rcp_w, x, SCREEN_IPA_MUL(2, 0xe4, 0), w, rcp, ux, uy, tex};
    const u64 two_generics[] = {ipa_w,
                                rcp_w,
                                x,
                                SCREEN_IPA_MUL(2, 0xe4, 0),
                                w,
                                rcp,
                                SCREEN_IPA_MUL(6, 0xec, 0),
                                SCREEN_RCP(6, 6),
                                ux,
                                SCREEN_FMUL(5, 2, 6),
                                tex};
    const u64 saturated_w[] = {
        SCREEN_IPA(1, 0xd0), SCREEN_IPA(2, 0xd4), SCREEN_IPA(3, 0xdc) | 1ull << 51, rcp, ux, uy, tex};
    const u64 mixed[] = {ipa_w, rcp_w, x, y, SCREEN_IPA(3, 0xdc), rcp, ux, uy, tex};
    const u64 bias[] = {ipa_w, rcp_w, x, y, w, rcp, ux, uy, tex | 2ull << 55};
    const u64 both[] = {ipa_w, rcp_w, x, y, w, rcp, ux, uy, tex, SCREEN_TEX(12, 1)};
    screen_uv_case(screen, 9, 0xaa00, 1, "TEX at a generic's x / w and y / w samples the own pixel's centre");
    screen_uv_case(screen_texs, 9, 0xaa00, 1, "TEXS 2D at a generic's x / w and y / w samples the own pixel's centre");
    screen_uv_case(passed, 7, 0xaa00, 1, "x, y and w read without the multiplier divide alike");
    screen_uv_case(undivided, 9, 0xaa00, 0, "TEX at a generic's x and y, not divided by its w, is no sample at x / w");
    screen_uv_case(other_y, 9, 0xaaaa00, 0, "TEX whose y is another generic's over this one's w reads elsewhere");
    screen_uv_case(two_generics, 11, 0xaaaa00, 0, "TEX at one generic's x / w and another's y / w reads elsewhere");
    screen_uv_case(saturated_w, 7, 0xaa00, 0, "a w read saturated is not the position's w");
    screen_uv_case(mixed, 9, 0xaa00, 0, "x and y multiplied by 1 / gl_FragCoord.w over a w read without it");
    screen_uv_case(bias, 9, 0xaa00, 0, "TEX.LB at the own pixel's centre may read another level");
    screen_uv_case(screen, 9, 0xff00, 0, "a screen-linear generic's x / w and y / w is not the position's");
    screen_uv_case(both, 10, 0xaa00, 0, "a slot also sampled at a generic's x and y reads elsewhere");
}

/* ShaderInfo.tex_unread_mask: a slot only TXQ or TMML names is asked its size, its levels or a level of detail, and no
   texel of it is read, in any stage: a draw that renders to its texture needs no pixels in the copy it binds. Each
   instruction that reads texels (TEXS, TLDS, TLD, TEX, TXD, TLD4, a 2D SULD) takes the slot of a TXQ of its handle
   out, and leaves a TXQ of another handle in */
#define UNREAD_TEXS(off) (OWN_TEXS(2, 0, 1) ^ (0x10ull ^ (u64)(off)) << 36) /* TEXS 2D.LZ R0, R0, R1 */
static void texture_unread_slots(void) {
    const u64 txq = 0xdf48010200470500ull; /* TXQ.DIMENSION R0, R5 of the handle at 0x40: the depth */
    const u64 tmml = 0xdf58000000000000ull | 3ull << 31 | 2ull << 28 | 0x10ull << 36 | 4ull << 20 | 7ull << 16 |
                     8ull << 8; /* TMML R0, R8 on a 2D texture */
    const u64 texs = UNREAD_TEXS(0x10), tlds = UNREAD_TEXS(0x10) | 1ull << 57;
    const u64 tld = 0xdc00000000000004ull | 7ull << 51 | 0xfull << 31 | 2ull << 28 | 0x10ull << 36 | 0xffull << 20 |
                    7ull << 16; /* TLD.LZ R4, R0 on a 2D texture */
    const u64 tex = SCREEN_TEX(0, 0);
    const u64 txd = 0xde00000000000000ull | 7ull << 51 | 0x10ull << 36 | 0xfull << 31 | 2ull << 28 | 8ull << 20 |
                    7ull << 16 | 4ull << 8; /* TXD R0, R4, R8 on a 2D texture */
    const u64 tld4 = 0xc800000000000000ull | 7ull << 51 | 0xfull << 31 | 0x10ull << 36 | 4ull << 20 | 7ull << 16 |
                     2ull << 8; /* TLD4 R0, R2 on a 1D texture */
    const u64 with_texs[] = {txq, texs}, with_tlds[] = {txq, tlds}, with_tld[] = {txq, tld}, with_tex[] = {txq, tex};
    const u64 suld = 0xeb00000000000000ull | 1ull << 51 | 0x10ull << 36 | 3ull << 33 | 0xfull << 20 | 7ull << 16 |
                     8ull << 8; /* SULD.P R0..3, R8 of a 2D surface: a texel fetch in a fragment program */
    const u64 with_txd[] = {txq, txd}, with_tld4[] = {txq, tld4}, with_suld[] = {txq, suld};
    const u64 asked_other[] = {txq, UNREAD_TEXS(0x11)};
    struct {
        const u64* instructions;
        int count, stage, slots;
        u64 unread;
        const char* what;
    } cases[] = {{&txq, 1, ST_FS, 1, 1, "a fragment program's slot only TXQ names has no texel read"},
                 {&txq, 1, ST_VS, 1, 1, "a vertex program's slot only TXQ names has no texel read"},
                 {&tmml, 1, ST_FS, 1, 1, "a slot only TMML names has no texel read"},
                 {with_texs, 2, ST_FS, 1, 0, "a slot TXQ asks and TEXS samples has its texels read"},
                 {with_tlds, 2, ST_FS, 1, 0, "a slot TXQ asks and TLDS fetches has its texels read"},
                 {with_tld, 2, ST_FS, 1, 0, "a slot TXQ asks and TLD fetches has its texels read"},
                 {with_tex, 2, ST_FS, 1, 0, "a slot TXQ asks and TEX samples has its texels read"},
                 {with_txd, 2, ST_FS, 1, 0, "a slot TXQ asks and TXD samples has its texels read"},
                 {with_tld4, 2, ST_FS, 1, 0, "a slot TXQ asks and TLD4 gathers has its texels read"},
                 {with_suld, 2, ST_FS, 1, 0, "a slot TXQ asks and a 2D SULD loads has its texels read"},
                 {asked_other, 2, ST_FS, 2, 1, "a TXQ's slot stays unread beside another handle's TEXS"}};
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        ShaderInfo info;
        size_t words = 0;
        u32* code = translate(cases[i].instructions, cases[i].count, cases[i].stage, &info, &words, 0);
        check_translated(code, &info, cases[i].what);
        if (code) {
            if (info.ntex != cases[i].slots || info.tex_unread_mask != cases[i].unread)
                fprintf(stderr, "  %d slots, unread mask %#llx, expected %d and %#llx\n", info.ntex,
                        (unsigned long long)info.tex_unread_mask, cases[i].slots, (unsigned long long)cases[i].unread);
            check(info.ntex == cases[i].slots && info.tex_unread_mask == cases[i].unread, cases[i].what);
        }
        free(code);
    }
}
#undef UNREAD_TEXS

/* ShaderInfo.screen_ndc: the program maps a generic's x / w and y / w from clip space onto [0, 1] itself, x / w / 2 +
   1/2 and +-y / w / 2 + 1/2, each an FFMA by an immediate +-1/2 with 1/2 in a register: the own pixel's centre wherever
   the vertex program wrote that generic as its position itself. Another factor or addend, a saturation, x mirrored,
   or a slot also sampled at the unmapped x / w and y / w reads elsewhere */
#define SCREEN_FFMA_HALF(d, a, c, neg)                                                                                 \
    (0x3280000000070000ull | (u64)(neg) << 56 | (u64)(c) << 39 | 0x3f000ull << 20 | (u64)(a) << 8 | (u64)(d))
static void screen_ndc_case(const u64* instructions, int count, u64 screen, u32 ndc, const char* what) {
    size_t slots, words = 0;
    u64* code = guest_program(instructions, count, NULL, &slots);
    if (!code) return;
    set_sph(code, 7, 0xaa00);
    ShaderInfo info;
    memset(&info, 0, sizeof info);
    u32* spv = spirv_of(code, slots * sizeof *code, ST_FS, &info, &words, 0, 1, 1, 1);
    free(code);
    check_translated(spv, &info, what);
    int ok = spv && info.tex_screen_mask == screen && !info.tex_own_pixel_mask &&
             (!screen || (info.screen_in == 5 && info.screen_ndc == ndc));
    if (spv && !ok)
        fprintf(stderr, "  screen mask %#llx (generic %u, mapping %u), own pixel %#llx, expected %#llx (mapping %u)\n",
                (unsigned long long)info.tex_screen_mask, info.screen_in, info.screen_ndc,
                (unsigned long long)info.tex_own_pixel_mask, (unsigned long long)screen, ndc);
    check(ok, what);
    free(spv);
}
static void feedback_screen_ndc(void) {
    const u64 ipa_w = SCREEN_IPA(0, 0x7c), rcp_w = SCREEN_RCP(0, 0), x = SCREEN_IPA_MUL(1, 0xd0, 0),
              y = SCREEN_IPA_MUL(2, 0xd4, 0), w = SCREEN_IPA_MUL(3, 0xdc, 0), rcp = SCREEN_RCP(3, 3),
              ux = SCREEN_FMUL(4, 1, 3), uy = SCREEN_FMUL(5, 2, 3), half = mov32i(6, 0x3f000000u),
              quarter = mov32i(6, 0x3e800000u), tex = SCREEN_TEX(8, 4);
    const u64 mx = SCREEN_FFMA_HALF(4, 4, 6, 0), my_down = SCREEN_FFMA_HALF(5, 5, 6, 1),
              my_up = SCREEN_FFMA_HALF(5, 5, 6, 0);
    const u64 down[] = {ipa_w, rcp_w, x, y, w, rcp, ux, uy, half, mx, my_down, tex};
    const u64 up[] = {ipa_w, rcp_w, x, y, w, rcp, ux, uy, half, mx, my_up, tex};
    const u64 mirrored[] = {ipa_w, rcp_w, x, y, w, rcp, ux, uy, half, SCREEN_FFMA_HALF(4, 4, 6, 1), my_down, tex};
    const u64 addend[] = {ipa_w, rcp_w, x, y, w, rcp, ux, uy, quarter, mx, my_down, tex};
    const u64 mx_quarter = (mx & ~(0x7ffffull << 20)) | 0x3e800ull << 20; /* x / w times 1/4 */
    const u64 factor[] = {ipa_w, rcp_w, x, y, w, rcp, ux, uy, half, mx_quarter, my_down, tex};
    const u64 saturated[] = {ipa_w, rcp_w, x, y, w, rcp, ux, uy, half, mx | 1ull << 50, my_down, tex};
    const u64 mixed[] = {ipa_w, rcp_w, x, y, w, rcp, ux, uy, SCREEN_TEX(12, 4), half, mx, my_down, tex};
    screen_ndc_case(down, 12, 1, 2, "TEX at x / w / 2 + 1/2 and -y / w / 2 + 1/2 samples the own pixel's centre");
    screen_ndc_case(up, 12, 1, 1, "TEX at x / w / 2 + 1/2 and y / w / 2 + 1/2 samples the own pixel's centre");
    screen_ndc_case(mirrored, 12, 0, 0, "TEX at -x / w / 2 + 1/2 reads elsewhere");
    screen_ndc_case(addend, 12, 0, 0, "an addend of 1/4 reads elsewhere");
    screen_ndc_case(factor, 12, 0, 0, "a factor of 1/4 reads elsewhere");
    screen_ndc_case(saturated, 12, 0, 0, "a saturated mapping reads elsewhere");
    screen_ndc_case(mixed, 13, 0, 0, "a slot sampled at the mapped and the unmapped x / w and y / w reads elsewhere");
}

/* ShaderInfo.screen_out_mask: the vertex program writes generic 5 as its position mapped onto [0, 1],
   ((w + x) / 2, (w - y) / 2, -, w), the y's sign in screen_out_yneg_mask, each word stored once with no guard, from
   x, y, z and w loaded one register each. Half of x - w, w from another register, a quarter, a guarded or second
   store, or an EXIT that may come before the stores leave the generic out */
#define SCREEN_FADD(d, a, b, negb)                                                                                     \
    (0x5c58000000070000ull | (u64)(negb) << 45 | (u64)(b) << 20 | (u64)(a) << 8 | (u64)(d))
#define SCREEN_HALF(d, a)   (0x3868000000070000ull | 0x3f000ull << 20 | (u64)(a) << 8 | (u64)(d))
#define SCREEN_HALF32(d, a) (0x1e00000000070000ull | 0x3f000000ull << 20 | (u64)(a) << 8 | (u64)(d))
#define SCREEN_AST(addr, reg, count)                                                                                   \
    (0xeff0000000070000ull | (u64)((count) - 1) << 47 | (u64)(addr) << 20 | 0xff00ull | (u64)(reg))
/* ALD Rd, a[0x80 + 4 d]: one word of the vertex's generic input 0 */
#define SCREEN_ALD(d) ((0xefd9ff8000070000ull & ~(3ull << 47)) | (0x80ull + 4 * (d)) << 20 | 0xff00ull | (u64)(d))
static void vertex_screen_out(void) {
    const u64 guarded_exit = EXIT & ~(0xfull << 16);
    struct {
        u64 instructions[12];
        int count;
        u32 out, yneg;
        const char* what;
    } cases[] = {
        {{SCREEN_ALD(0), SCREEN_ALD(1), SCREEN_ALD(2), SCREEN_ALD(3), SCREEN_FADD(4, 3, 0, 0), SCREEN_FADD(5, 3, 1, 1),
          SCREEN_HALF(4, 4), SCREEN_HALF32(5, 5), SCREEN_AST(0x70, 0, 4), SCREEN_AST(0xd0, 4, 2),
          SCREEN_AST(0xdc, 3, 1)},
         11,
         1u << 5,
         1u << 5,
         "(w + x) / 2, (w - y) / 2 and w: the position mapped onto [0, 1], y down"},
        {{SCREEN_ALD(0), SCREEN_ALD(1), SCREEN_ALD(2), SCREEN_ALD(3), SCREEN_FADD(4, 0, 3, 0), SCREEN_FADD(5, 1, 3, 0),
          SCREEN_HALF(4, 4), SCREEN_HALF(5, 5), SCREEN_AST(0x70, 0, 4), SCREEN_AST(0xd0, 4, 2), SCREEN_AST(0xdc, 3, 1)},
         11,
         1u << 5,
         0,
         "(x + w) / 2, (y + w) / 2 and w: the position mapped onto [0, 1], y up"},
        {{SCREEN_ALD(0), SCREEN_ALD(1), SCREEN_ALD(2), SCREEN_ALD(3), SCREEN_FADD(4, 0, 3, 1), SCREEN_FADD(5, 3, 1, 1),
          SCREEN_HALF(4, 4), SCREEN_HALF(5, 5), SCREEN_AST(0x70, 0, 4), SCREEN_AST(0xd0, 4, 2), SCREEN_AST(0xdc, 3, 1)},
         11,
         0,
         0,
         "(x - w) / 2 is not the position's x mapped onto [0, 1]"},
        {{SCREEN_ALD(0), SCREEN_ALD(1), SCREEN_ALD(2), SCREEN_ALD(3), SCREEN_FADD(4, 3, 0, 0), SCREEN_FADD(5, 3, 1, 1),
          SCREEN_HALF(4, 4), SCREEN_HALF(5, 5), SCREEN_AST(0x70, 0, 4), SCREEN_AST(0xd0, 4, 2), SCREEN_AST(0xdc, 2, 1)},
         11,
         0,
         0,
         "a w that is the position's z, not its w"},
        {{SCREEN_ALD(0), SCREEN_ALD(1), SCREEN_ALD(2), SCREEN_ALD(3), SCREEN_FADD(4, 3, 0, 0), SCREEN_FADD(5, 3, 1, 1),
          SCREEN_HALF(4, 4), 0x1e00000000070000ull | 0x3e800000ull << 20 | 5ull << 8 | 5ull, SCREEN_AST(0x70, 0, 4),
          SCREEN_AST(0xd0, 4, 2), SCREEN_AST(0xdc, 3, 1)},
         11,
         0,
         0,
         "a quarter of w - y is not half"},
        {{SCREEN_ALD(0), SCREEN_ALD(1), SCREEN_ALD(2), SCREEN_ALD(3), SCREEN_FADD(4, 3, 0, 0), SCREEN_FADD(5, 3, 1, 1),
          SCREEN_HALF(4, 4), SCREEN_HALF(5, 5), SCREEN_AST(0x70, 0, 4), SCREEN_AST(0xd0, 4, 2) & ~(0xfull << 16),
          SCREEN_AST(0xdc, 3, 1)},
         11,
         0,
         0,
         "a store under @P0 may not run"},
        {{SCREEN_ALD(0), SCREEN_ALD(1), SCREEN_ALD(2), SCREEN_ALD(3), SCREEN_FADD(4, 3, 0, 0), SCREEN_FADD(5, 3, 1, 1),
          SCREEN_HALF(4, 4), SCREEN_HALF(5, 5), SCREEN_AST(0x70, 0, 4), SCREEN_AST(0xd0, 4, 2), SCREEN_AST(0xdc, 3, 1),
          SCREEN_AST(0xd0, 0, 1)},
         12,
         0,
         0,
         "a second store of x leaves the generic as that one wrote it"},
        {{SCREEN_ALD(0), SCREEN_ALD(1), SCREEN_ALD(2), SCREEN_ALD(3), SCREEN_AST(0xd0, 0, 1), SCREEN_FADD(4, 3, 0, 0),
          SCREEN_FADD(5, 3, 1, 1), SCREEN_HALF(4, 4), SCREEN_HALF(5, 5), SCREEN_AST(0x70, 0, 4), SCREEN_AST(0xd0, 4, 2),
          SCREEN_AST(0xdc, 3, 1)},
         12,
         1u << 5,
         1u << 5,
         "a store of x before the last one is overwritten by it"},
        {{SCREEN_ALD(0), SCREEN_ALD(1), SCREEN_ALD(2), SCREEN_ALD(3), guarded_exit, SCREEN_FADD(4, 3, 0, 0),
          SCREEN_FADD(5, 3, 1, 1), SCREEN_HALF(4, 4), SCREEN_HALF(5, 5), SCREEN_AST(0x70, 0, 4), SCREEN_AST(0xd0, 4, 2),
          SCREEN_AST(0xdc, 3, 1)},
         12,
         0,
         0,
         "an @P0 EXIT before the stores may leave them unwritten"},
    };
    for (size_t k = 0; k < sizeof cases / sizeof *cases; k++) {
        ShaderInfo info;
        size_t words = 0;
        u32* spv = translate(cases[k].instructions, cases[k].count, ST_VS, &info, &words, 0);
        check_translated(spv, &info, cases[k].what);
        if (spv && (info.screen_out_mask != cases[k].out || info.screen_out_yneg_mask != cases[k].yneg))
            fprintf(stderr, "  screen out %#x (y down %#x), expected %#x (%#x)\n", info.screen_out_mask,
                    info.screen_out_yneg_mask, cases[k].out, cases[k].yneg);
        check(spv && info.screen_out_mask == cases[k].out && info.screen_out_yneg_mask == cases[k].yneg, cases[k].what);
        free(spv);
    }
}

/* ShaderInfo.pos_out_mask: the vertex program writes generic 5's x, y and w from the registers its position's x, y
   and w come from, whatever its z. A w or an x from another register, a store under a predicate or a later store of x
   leave the generic out */
static void vertex_position_out(void) {
    struct {
        u64 instructions[9];
        int count;
        u32 out;
        const char* what;
    } cases[] = {
        {{SCREEN_ALD(0), SCREEN_ALD(1), SCREEN_ALD(2), SCREEN_ALD(3), SCREEN_AST(0x70, 0, 4), SCREEN_AST(0xd0, 0, 2),
          SCREEN_AST(0xdc, 3, 1)},
         7,
         1u << 5,
         "x, y and w stored from the position's registers"},
        {{SCREEN_ALD(0), SCREEN_ALD(1), SCREEN_ALD(2), SCREEN_ALD(3), SCREEN_AST(0x70, 0, 4), SCREEN_AST(0xd0, 0, 4)},
         6,
         1u << 5,
         "the four words stored as the position's"},
        {{SCREEN_ALD(0), SCREEN_ALD(1), SCREEN_ALD(2), SCREEN_ALD(3), SCREEN_AST(0x70, 0, 4), SCREEN_AST(0xd0, 0, 2),
          SCREEN_AST(0xdc, 2, 1)},
         7,
         0,
         "a w that is the position's z, not its w"},
        {{SCREEN_ALD(0), SCREEN_ALD(1), SCREEN_ALD(2), SCREEN_ALD(3), SCREEN_AST(0x70, 0, 4), SCREEN_AST(0xd0, 1, 1),
          SCREEN_AST(0xd4, 1, 1), SCREEN_AST(0xdc, 3, 1)},
         8,
         0,
         "an x that is the position's y"},
        {{SCREEN_ALD(0), SCREEN_ALD(1), SCREEN_ALD(2), SCREEN_ALD(3), SCREEN_AST(0x70, 0, 4),
          SCREEN_AST(0xd0, 0, 2) & ~(0xfull << 16), SCREEN_AST(0xdc, 3, 1)},
         7,
         0,
         "a store under @P0 may not run"},
        {{SCREEN_ALD(0), SCREEN_ALD(1), SCREEN_ALD(2), SCREEN_ALD(3), SCREEN_AST(0x70, 0, 4), SCREEN_AST(0xd0, 0, 2),
          SCREEN_AST(0xdc, 3, 1), SCREEN_AST(0xd0, 2, 1)},
         8,
         0,
         "a later store of x leaves the generic as that one wrote it"},
    };
    for (size_t k = 0; k < sizeof cases / sizeof *cases; k++) {
        ShaderInfo info;
        size_t words = 0;
        u32* spv = translate(cases[k].instructions, cases[k].count, ST_VS, &info, &words, 0);
        check_translated(spv, &info, cases[k].what);
        if (spv && info.pos_out_mask != cases[k].out)
            fprintf(stderr, "  position out %#x, expected %#x\n", info.pos_out_mask, cases[k].out);
        check(spv && info.pos_out_mask == cases[k].out, cases[k].what);
        free(spv);
    }
}

/* The position pass (ShaderInfo.varying, SH_VARYING_POSITION): a perspective generic the key names reads its x and y
   over w as the fragment's own place, (gl_FragCoord - the viewport's origin) / its size * 2 - 1 with the viewport the
   third vector of the push constants, at byte 80, and its w over w as 1; its z, a screen-linear input and a program
   without the key read the input. smooth_in_mask names the perspective generics that plain IPAs read, none where an
   indexed IPA reads any. A division (OpFDiv, 136) is the pass's alone in these programs */
static void position_pass_case(const u64* instructions, int count, u32 imap7, u32 varying, int divisions, u32 smooth,
                               const char* what) {
    size_t slots, words = 0;
    u64* code = guest_program(instructions, count, NULL, &slots);
    if (!code) return;
    set_sph(code, 7, imap7); /* byte 0x18 + 5 on: generic 5's (and 6's) components, 2 bits each */
    ShaderInfo info = {0};
    info.varying = varying;
    u32* spv = spirv_of(code, slots * sizeof *code, ST_FS, &info, &words, 0, 1, 1, 1);
    free(code);
    check_translated(spv, &info, what);
    if (spv) {
        int found = count_opcode(spv, words, 136, 0xffffffffu), viewport = count_opcode(spv, words, 72, 2);
        if (found != divisions || viewport != (divisions != 0) || info.smooth_in_mask != smooth)
            fprintf(stderr, "  %d divisions (expected %d), %d third push constant vectors, smooth inputs %#x (%#x)\n",
                    found, divisions, viewport, info.smooth_in_mask, smooth);
        check(found == divisions && viewport == (divisions != 0) && info.smooth_in_mask == smooth, what);
    }
    free(spv);
}
static void fragment_position_pass(void) {
    const u32 generic5 = shader_varying_positions(1u << 5);
    const u64 xyw[] = {SCREEN_IPA(1, 0xd0), SCREEN_IPA(2, 0xd4), SCREEN_IPA(3, 0xdc)};
    const u64 z[] = {SCREEN_IPA(1, 0xd8)};
    const u64 indexed[] = {SCREEN_IPA(1, 0xd0), 0xe000000000070000ull | 1ull << 38 | 1ull << 8 | 2}; /* a[R1] */
    check(shader_varying_position_mask(shader_varying_positions(0x80000021u)) == 0x80000021u &&
              shader_varying_position_mask(shader_varying_positions(0x8000002fu)) == 7u && !shader_varying_positions(0),
          "the position pass's key holds the three lowest generics");
    position_pass_case(xyw, 3, 0xaa00, generic5, 2, 1u << 5, "x, y and w of a position generic from gl_FragCoord");
    position_pass_case(xyw, 3, 0xaa00, 0, 0, 1u << 5, "without the key the inputs are read");
    position_pass_case(xyw, 3, 0xaa00, shader_varying_positions(1u << 6), 0, 1u << 5, "another generic's key");
    position_pass_case(z, 1, 0xaa00, generic5, 0, 1u << 5, "a position generic's z is the input's");
    position_pass_case(xyw, 3, 0xff00, generic5, 0, 0, "a screen-linear generic is not rebuilt");
    position_pass_case(indexed, 2, 0xaa00, 0, 0, 0, "an indexed IPA leaves no generic to a varying pass");
}

/* The per-vertex pass (SH_VARYING_BARYCENTRIC): each perspective generic a plain IPA reads, and the position in the
   key's generic, are the three vertices' values (PerVertexKHR, 5285, with the FragmentBarycentricKHR capability,
   5284), weighed by the fragment's place; a screen-linear input stays the device's. The vertex program's key names
   the generic its position goes to as well, which no store of its own may be. With a geometry program's key
   (SH_VARYING_GEOMETRY) the same values are flat inputs (14): the generic's own, two copies of it and three positions */
static void per_vertex_case(u32 imap7, u32 varying, int inputs, int flats, const char* what) {
    const u64 xyw[] = {SCREEN_IPA(1, 0xd0), SCREEN_IPA(2, 0xd4), SCREEN_IPA(3, 0xe8)}; /* generics 5 and 6 */
    size_t slots, words = 0;
    u64* code = guest_program(xyw, 3, NULL, &slots);
    if (!code) return;
    set_sph(code, 7, imap7);
    ShaderInfo info = {0};
    info.varying = varying;
    u32* spv = spirv_of(code, slots * sizeof *code, ST_FS, &info, &words, 0, 1, 1, 1);
    free(code);
    check_translated(spv, &info, what);
    if (spv) {
        int found = count_opcode(spv, words, 71, 5285), capability = count_opcode(spv, words, 17, 5284);
        int flat = count_opcode(spv, words, 71, 14); /* Flat */
        if (found != inputs || capability != (inputs != 0) || flat != flats)
            fprintf(stderr, "  %d per-vertex inputs (expected %d), %d capabilities, %d flat inputs (expected %d)\n",
                    found, inputs, capability, flat, flats);
        check(found == inputs && capability == (inputs != 0) && flat == flats, what);
    }
    free(spv);
}
static void per_vertex_pass(void) {
    const u32 key = shader_varying_per_vertex(SH_VARYING_BARYCENTRIC, 7, 0); /* the position in generic 7 */
    const u32 flat = shader_varying_per_vertex(SH_VARYING_GEOMETRY, 7, 0);
    check(SH_VARYING_SLOT(key) == 7 && !shader_varying_position_mask(key) &&
              shader_varying_position_mask(shader_varying_per_vertex(SH_VARYING_BARYCENTRIC, 31, 0x80000021u)) == 0x21u,
          "the per-vertex pass's key holds the position's generic and the two lowest position inputs");
    check(shader_varying_copy(0x60, 7, 5, 0) == 10 && shader_varying_copy(0x60, 7, 6, 1) == 13 &&
              shader_varying_copy(0x60, 7, 32, 0) == 14,
          "a geometry program's copies follow the position's three generics, two a generic");
    per_vertex_case(0xaaaa00, key, 3, 0, "two perspective generics and the position per vertex");
    per_vertex_case(0xffaa00, key, 2, 0, "a screen-linear generic is the device's");
    per_vertex_case(0xaaaa00, shader_varying_per_vertex(SH_VARYING_BARYCENTRIC, 7, 1u << 5), 2, 0,
                    "a position input stays the position pass's");
    per_vertex_case(0xaaaa00, 0, 0, 0, "without the key the inputs are the device's");
    /* a geometry program's: each generic flat with two flat copies, and the three positions */
    per_vertex_case(0xaaaa00, flat, 0, 9, "two perspective generics and the position as flat inputs");
    per_vertex_case(0xffaa00, flat, 0, 6, "a screen-linear generic beside one taken as flat inputs");
    struct {
        u64 instructions[7];
        int count;
        u32 slot, out;
        const char* refused;
        const char* what;
    } cases[] = {
        {{SCREEN_ALD(0), SCREEN_ALD(1), SCREEN_ALD(2), SCREEN_ALD(3), SCREEN_AST(0x70, 0, 4), SCREEN_AST(0xd0, 0, 4)},
         6,
         7,
         1u << 5 | 1u << 7,
         NULL,
         "the position goes to the key's generic too"},
        {{SCREEN_ALD(0), SCREEN_ALD(1), SCREEN_ALD(2), SCREEN_ALD(3), SCREEN_AST(0x70, 0, 4), SCREEN_AST(0xd0, 0, 4)},
         6,
         0,
         1u << 5,
         NULL,
         "without the key the program's own outputs"},
        {{SCREEN_ALD(0), SCREEN_ALD(1), SCREEN_ALD(2), SCREEN_ALD(3), SCREEN_AST(0x70, 0, 4), SCREEN_AST(0xd0, 0, 4)},
         6,
         5,
         0,
         "the position's copy onto a generic the program writes",
         "a generic the program writes can't take the position"},
    };
    for (size_t k = 0; k < sizeof cases / sizeof *cases; k++) {
        size_t slots, words = 0;
        u64* code = guest_program(cases[k].instructions, cases[k].count, NULL, &slots);
        if (!code) continue;
        ShaderInfo info = {0};
        info.position_copy = cases[k].slot ? cases[k].slot + 1 : 0;
        u32* spv = spirv_of(code, slots * sizeof *code, ST_VS, &info, &words, 0, 1, 1, 1);
        free(code);
        if (cases[k].refused)
            check_refused(spv, &info, cases[k].refused, cases[k].what);
        else {
            check_translated(spv, &info, cases[k].what);
            if (spv && info.out_mask != cases[k].out)
                fprintf(stderr, "  outputs %#x, expected %#x\n", info.out_mask, cases[k].out);
            check(spv && info.out_mask == cases[k].out, cases[k].what);
        }
        free(spv);
    }
}

/* ShaderInfo.tex_uv_mask: a plain 2D sample at a generic's x and y as the vertex program wrote them, read by IPA in
   pass mode or multiplied by 1 / gl_FragCoord.w as the console's compiler reads a perspective input, reads the
   fragment's own pixel's centre wherever the vertex program wrote its position from that generic; uv_in names the
   generic. A generic constant over the primitive, another multiplier, a coordinate computed further, x and y of two
   generics or swapped, a level bias, or a slot also sampled at x / w and y / w read other pixels */
static void plain_uv_case(const u64* instructions, int count, u32 imap7, u64 uv, const char* what) {
    size_t slots, words = 0;
    u64* code = guest_program(instructions, count, NULL, &slots);
    if (!code) return;
    set_sph(code, 7, imap7); /* byte 0x18 + 5 on: generic 5's (and 6's) components, 2 bits each */
    ShaderInfo info = {0};
    u32* spv = spirv_of(code, slots * sizeof *code, ST_FS, &info, &words, 0, 1, 1, 1);
    free(code);
    check_translated(spv, &info, what);
    int ok =
        spv && info.tex_uv_mask == uv && !info.tex_own_pixel_mask && !info.tex_screen_mask && (!uv || info.uv_in == 5);
    if (spv && !ok)
        fprintf(stderr, "  uv mask %#llx (generic %u), screen %#llx, own pixel %#llx, expected uv %#llx\n",
                (unsigned long long)info.tex_uv_mask, info.uv_in, (unsigned long long)info.tex_screen_mask,
                (unsigned long long)info.tex_own_pixel_mask, (unsigned long long)uv);
    check(ok, what);
    free(spv);
}
static void feedback_plain_uv(void) {
    const u64 ipa_w = SCREEN_IPA(0, 0x7c), rcp_w = SCREEN_RCP(0, 0), x = SCREEN_IPA_MUL(1, 0xd0, 0),
              y = SCREEN_IPA_MUL(2, 0xd4, 0), tex = SCREEN_TEX(8, 1);
    const u64 passed[] = {SCREEN_IPA(1, 0xd0), SCREEN_IPA(2, 0xd4), tex};
    const u64 perspective[] = {ipa_w, rcp_w, x, y, tex};
    const u64 texs[] = {ipa_w, rcp_w, x, y, OWN_TEXS(1, 1, 2)};
    const u64 other_multiplier[] = {SCREEN_IPA(3, 0xdc), SCREEN_RCP(3, 3), SCREEN_IPA_MUL(1, 0xd0, 3),
                                    SCREEN_IPA_MUL(2, 0xd4, 3), tex};
    const u64 scaled[] = {SCREEN_IPA(1, 0xd0), SCREEN_IPA(2, 0xd4), SCREEN_FMUL(1, 1, 2), tex};
    const u64 other_y[] = {SCREEN_IPA(1, 0xd0), SCREEN_IPA(2, 0xe4), tex};
    const u64 swapped[] = {SCREEN_IPA(1, 0xd4), SCREEN_IPA(2, 0xd0), tex};
    const u64 saturated[] = {SCREEN_IPA(1, 0xd0) | 1ull << 51, SCREEN_IPA(2, 0xd4), tex};
    const u64 bias[] = {SCREEN_IPA(1, 0xd0), SCREEN_IPA(2, 0xd4), tex | 2ull << 55};
    /* the same slot at the generic's x / w and y / w too, as feedback_screen_uv samples it */
    const u64 both[] = {ipa_w,
                        rcp_w,
                        x,
                        y,
                        SCREEN_IPA_MUL(3, 0xdc, 0),
                        SCREEN_RCP(3, 3),
                        SCREEN_FMUL(4, 1, 3),
                        SCREEN_FMUL(5, 2, 3),
                        SCREEN_TEX(8, 4),
                        SCREEN_TEX(12, 1)};
    plain_uv_case(passed, 3, 0xaa00, 1, "TEX at a generic's x and y read in pass mode samples where the generic says");
    plain_uv_case(perspective, 5, 0xaa00, 1, "TEX at a generic's x and y multiplied by 1 / gl_FragCoord.w does too");
    plain_uv_case(texs, 5, 0xaa00, 1, "TEXS 2D at a generic's x and y does too");
    plain_uv_case(passed, 3, 0xff00, 1, "a screen-linear generic's x and y do too");
    plain_uv_case(passed, 3, 0x5500, 0, "a generic constant over the primitive is no place of a fragment");
    plain_uv_case(other_multiplier, 5, 0xaa00, 0, "x and y multiplied by another reciprocal are not the generic's");
    plain_uv_case(scaled, 4, 0xaa00, 0, "a coordinate computed from the generic's reads elsewhere");
    plain_uv_case(other_y, 3, 0xaaaa00, 0, "TEX at one generic's x and another's y reads elsewhere");
    plain_uv_case(swapped, 3, 0xaa00, 0, "TEX at a generic's y and x, swapped, reads elsewhere");
    plain_uv_case(saturated, 3, 0xaa00, 0, "an x read saturated is not the generic's x");
    plain_uv_case(bias, 3, 0xaa00, 0, "TEX.LB at a generic's x and y may read another level");
    plain_uv_case(both, 10, 0xaa00, 0, "a slot sampled at x and y and at x / w and y / w needs both mappings");
}

/* ShaderInfo.uv_out_mask: the vertex program writes its position from generic 0's x and y, pos.x = x * 2 - 1 and
   pos.y = y * -2 + 1 (the generic's bit in uv_out_yneg_mask) or y * 2 - 1, with the position's w never stored or the
   constant 1: the generic's (x, y) at a fragment is then that fragment's place in [0, 1] of the viewport. Another
   scale or offset, a w from an attribute (a perspective divide), the generic's x and y swapped or stored from other
   values, or a saturated product leave the generic out. The program may drop a vertex
   first: under a predicate made from the instance's number alone it writes a position outside the clip volume and
   leaves, by a SYNC under that predicate to the stores. A predicate made from a vertex's attribute, a dropped
   position inside the clip volume, a write or the SYNC under another guard, or a SYNC to before the stores may put
   vertices of both kinds, or neither, in one triangle */
#define UV_ALD(d, addr) ((0xefd9ff8000070000ull & ~(3ull << 47)) | (u64)(addr) << 20 | 0xff00ull | (u64)(d))
#define UV_FFMA(d, a, c, scale, negc)                                                                                  \
    (0x32a0000000070000ull | (u64)((scale) >> 12 & 0x7ffff) << 20 | (u64)((scale) >> 31) << 56 | (u64)(c) << 39 |      \
     (u64)(negc) << 49 | (u64)(a) << 8 | (u64)(d))
static void vertex_uv_out(void) {
    const u32 two = 0x40000000u, minus_two = 0xc0000000u, one = 0x3f800000u;
    const u64 u = UV_ALD(0, 0x80), v = UV_ALD(1, 0x84), r6 = mov32i(6, one);
    const u64 x = UV_FFMA(4, 0, 6, two, 1), y_down = UV_FFMA(5, 1, 6, minus_two, 0), y_up = UV_FFMA(5, 1, 6, two, 1);
    const u64 pos = SCREEN_AST(0x70, 4, 2), generic = SCREEN_AST(0x80, 0, 2);
    /* SSY to the stores (word 29), the instance's number, @P0 the dropped vertex's outputs and SYNC, then the
       others' */
    const u64 ssy = 0xe290000000000000ull | 0x88ull << 20, instance = UV_ALD(0, 0x2f8),
              isetp = 0x366b038000170007ull, /* ISETP P0, R0, 1 */
        sync_p0 = 0xf0f800000000000full, sync = 0xf0f800000007000full;
    const u64 far_x = 0x010c61c40000f004ull, far_y = 0x010c61c40000f005ull;   /* @P0 MOV32I R4 and R5, -10000.0 */
    const u64 near_x = 0x0103f0000000f004ull;                                 /* @P0 MOV32I R4, 0.5 */
    const u64 zero_u = 0x5c9807800ff00008ull, zero_v = 0x5c9807800ff00009ull; /* @P0 MOV R8 and R9, RZ */
    const u64 u8_ = UV_ALD(8, 0x80), v9 = UV_ALD(9, 0x84), x8 = UV_FFMA(4, 8, 6, two, 1),
              y9 = UV_FFMA(5, 9, 6, minus_two, 0), generic8 = SCREEN_AST(0x80, 8, 2);
    struct {
        u64 instructions[17];
        int count;
        u32 out, yneg;
        const char* what;
    } cases[] = {
        {{u, v, r6, x, y_down, pos, generic},
         7,
         1,
         1,
         "x * 2 - 1 and y * -2 + 1: the generic mapped onto the viewport, y down"},
        {{u, v, r6, x, y_up, pos, generic},
         7,
         1,
         0,
         "x * 2 - 1 and y * 2 - 1: the generic mapped onto the viewport, y up"},
        {{u, v, r6, x, y_down, pos, generic, SCREEN_AST(0x7c, 6, 1)}, 8, 1, 1, "a w stored as the constant 1"},
        {{u, v, r6, x, y_down, pos, generic, UV_ALD(3, 0x8c), SCREEN_AST(0x7c, 3, 1)},
         9,
         0,
         0,
         "a w from an attribute divides the position and not the generic"},
        {{u, v, r6, UV_FFMA(4, 0, 6, two, 0), y_down, pos, generic}, 7, 0, 0, "x * 2 + 1 is an offset of a viewport"},
        {{u, v, r6, UV_FFMA(4, 0, 6, one, 1), y_down, pos, generic}, 7, 0, 0, "x * 1 - 1 is half the viewport's scale"},
        {{u, v, r6, UV_FFMA(4, 1, 6, two, 1), y_down, pos, generic}, 7, 0, 0, "a position's x from the generic's y"},
        {{u, v, r6, x, y_down, pos, SCREEN_AST(0x80, 1, 2)},
         7,
         0,
         0,
         "a generic stored from other values than the position's"},
        {{u, v, r6, x | 1ull << 50, y_down, pos, generic}, 7, 0, 0, "a saturated product is not x * 2 - 1"},
        {{ssy, instance, isetp, far_x, far_y, zero_u, zero_v, sync_p0, u8_, v9, r6, x8, y9, sync, pos, generic8},
         16,
         1,
         1,
         "a vertex dropped by the instance's number, outside the clip volume, leaves the others' mapping"},
        {{ssy, UV_ALD(0, 0x90), isetp, far_x, far_y, zero_u, zero_v, sync_p0, u8_, v9, r6, x8, y9, sync, pos, generic8},
         16,
         0,
         0,
         "a vertex dropped by an attribute of its own may share a triangle with one that isn't"},
        {{ssy, instance, isetp, near_x, far_y, zero_u, zero_v, sync_p0, u8_, v9, r6, x8, y9, sync, pos, generic8},
         16,
         0,
         0,
         "a dropped vertex inside the clip volume is drawn"},
        {{ssy, instance, isetp, far_x ^ 8ull << 16, far_y, zero_u, zero_v, sync_p0, u8_, v9, r6, x8, y9, sync, pos,
          generic8},
         16,
         0,
         0,
         "a dropped vertex's x under another guard than the SYNC's"},
        {{ssy, instance, isetp, far_x, far_y ^ 8ull << 16, zero_u, zero_v, sync_p0, u8_, v9, r6, x8, y9, sync, pos,
          generic8},
         16,
         0,
         0,
         "a dropped vertex's y under another guard than its x"},
        {{ssy, instance, isetp, far_x, far_y, zero_u, zero_v, sync_p0 ^ 8ull << 16, u8_, v9, r6, x8, y9, sync, pos,
          generic8},
         16,
         0,
         0,
         "a SYNC under the predicate's other side takes away the vertices that wrote nothing"},
        {{0xe290000000000000ull | 0x68ull << 20, instance, isetp, far_x, far_y, zero_u, zero_v, sync_p0, u8_, v9, r6,
          x8, y9, sync, pos, generic8},
         16,
         0,
         0,
         "a SYNC that lands among the other vertices' instructions (word 25) runs some of them"},
        /* @P1 BRA (word 12) to the dropped vertex's third write (word 19); the stores are at word 31 */
        {{0xe290000000000000ull | 0x98ull << 20, 0xe24000000001000full | 48ull << 20, instance, isetp, far_x, far_y,
          zero_u, zero_v, sync_p0, u8_, v9, r6, x8, y9, sync, pos, generic8},
         17,
         0,
         0,
         "a branch to among the dropped vertex's writes leaves its position unwritten"},
    };
    for (size_t k = 0; k < sizeof cases / sizeof *cases; k++) {
        ShaderInfo info;
        size_t words = 0;
        u32* spv = translate(cases[k].instructions, cases[k].count, ST_VS, &info, &words, 0);
        check_translated(spv, &info, cases[k].what);
        if (spv && (info.uv_out_mask != cases[k].out || info.uv_out_yneg_mask != cases[k].yneg))
            fprintf(stderr, "  uv out %#x, y negated %#x, expected %#x and %#x\n", info.uv_out_mask,
                    info.uv_out_yneg_mask, cases[k].out, cases[k].yneg);
        check(spv && info.uv_out_mask == cases[k].out && info.uv_out_yneg_mask == cases[k].yneg, cases[k].what);
        free(spv);
    }
}
#undef UV_ALD
#undef UV_FFMA
#undef SCREEN_IPA
#undef SCREEN_IPA_MUL
#undef SCREEN_RCP
#undef SCREEN_FMUL
#undef SCREEN_TEX
#undef SCREEN_FADD
#undef SCREEN_HALF
#undef SCREEN_HALF32
#undef SCREEN_AST
#undef SCREEN_ALD
#undef OWN_TEXS

/* A fragment program's SHFL within its quad (mask 0x1c03) from a lane known when translated is a quad operation:
   the quad's helper lanes, which a triangle's edge leaves out, take part in it, as the console's lanes do in SHFL. One
   game's edge pass read its upper left lane so and drew blue lines along every edge. Other SHFL stay shuffles */
/* A SHFL writes the one predicate its bits 48-50 name, none for PT: a handle loaded under @P0 reaches a surface store
   under @P0 past a SHFL to PT, as a fragment program of a game has them, and not past one that writes P0 */
static void shfl_predicate(void) {
    const u64 shfl_pt = 0xef17007cf0370207ull; /* SHFL PT, R7, R2, ... */
    for (u32 writes_p0 = 0; writes_p0 < 2; writes_p0++) {
        const u64 instructions[] = {
            0x4c98078804a0000aull, /* @P0 MOV R10, c[0x2][0x128] */
            writes_p0 ? shfl_pt & ~(7ull << 48) : shfl_pt,
            0xeb20050600f00804ull, /* @P0 SUST.P R4, [R8], R10 */
        };
        ShaderInfo info;
        size_t words = 0;
        u32* code = translate(instructions, 3, ST_FS, &info, &words, 1);
        if (writes_p0)
            check_refused(code, &info, "untracked surface handle", "a SHFL writing P0 leaves the handle's guard open");
        else
            check_translated(code, &info, "a handle under @P0 reaches its surface store past a SHFL to PT");
        free(code);
    }
}

static void shfl_quad_forms(void) {
    static const struct {
        u32 mode, index, opcode, operand;
        int immediate;
        const char* what;
    } quad[] = {
        {0, 0, 365, 0, 1, "SHFL.IDX 0 in a quad broadcasts lane 0"},
        {0, 3, 365, 3, 1, "SHFL.IDX 3 in a quad broadcasts lane 3"},
        {0, 6, 365, 2, 1, "SHFL.IDX 6 in a quad broadcasts lane 6 & 3"},
        {0, 0, 365, 0, 0, "SHFL.IDX RZ in a quad broadcasts lane 0"},
        {3, 1, 366, 0, 1, "SHFL.BFLY 1 in a quad swaps horizontally"},
        {3, 2, 366, 1, 1, "SHFL.BFLY 2 in a quad swaps vertically"},
        {3, 3, 366, 2, 1, "SHFL.BFLY 3 in a quad swaps diagonally"},
    };
    ShaderInfo info;
    size_t words = 0;
    for (size_t k = 0; k < sizeof quad / sizeof *quad; k++) {
        u32* code = shfl_spirv(shfl(quad[k].mode, quad[k].immediate, quad[k].index, 0x1c03), ST_FS, &info, &words);
        check_translated(code, &info, quad[k].what);
        if (code) {
            check(quad_operand(code, words, quad[k].opcode) == quad[k].operand, quad[k].what);
            check(count_opcode(code, words, 345, 0xffffffffu) == 0, "a quad's SHFL leaves no subgroup shuffle");
            check(count_opcode(code, words, 17, 68) == 1, "a quad's SHFL declares GroupNonUniformQuad");
        }
        free(code);
    }
    const struct {
        u64 instruction;
        int stage;
        const char* what;
    } plain[] = {
        {shfl(3, 1, 4, 0x1c03), ST_FS, "SHFL.BFLY 4 leaves the quad: a shuffle"},
        {shfl(1, 1, 1, 0x1c03), ST_FS, "SHFL.UP in a quad: a shuffle"},
        {shfl(0, 1, 0, 0x1f), ST_FS, "SHFL.IDX over the whole warp: a shuffle"},
        {shfl(0, 1, 0, 0x1c03), ST_CS, "SHFL.IDX in a compute program's quad: a shuffle"},
    };
    for (size_t k = 0; k < sizeof plain / sizeof *plain; k++) {
        u32* code = shfl_spirv(plain[k].instruction, plain[k].stage, &info, &words);
        check_translated(code, &info, plain[k].what);
        if (code)
            check(count_opcode(code, words, 345, 0xffffffffu) == 1 &&
                      count_opcode(code, words, 365, 0xffffffffu) == 0 &&
                      count_opcode(code, words, 366, 0xffffffffu) == 0 && count_opcode(code, words, 17, 68) == 0,
                  plain[k].what);
        free(code);
    }
    g_shader_quad = 0;
    u32* code = shfl_spirv(shfl(0, 1, 0, 0x1c03), ST_FS, &info, &words);
    check_translated(code, &info, "SHFL in a quad translates on a device without quad operations");
    if (code)
        check(count_opcode(code, words, 345, 0xffffffffu) == 1 && count_opcode(code, words, 17, 68) == 0,
              "SHFL in a quad is a shuffle on a device without quad operations");
    free(code);
    g_shader_quad = 1;
}

/* VOTE R<d>, P<out>, [!]P<in> in this mode (0 ALL, 1 ANY, 2 EQ) */
static u64 vote(u32 mode, u32 d, u32 out, u32 in, int negate) {
    return 0x50D8000000070000ull | (u64)mode << 48 | (u64)out << 45 | (u64)(negate != 0) << 42 | (u64)in << 39 | d;
}
static u32* lone_lane_spirv(const u64* instructions, int count, int stage, int can_subgroup, ShaderInfo* info,
                            size_t* words) {
    const u32 geometry_sph[3] = {1u << 24, 7u << 24, 3}, none[3] = {0};
    size_t slots;
    u64* code = guest_program(instructions, count, stage == ST_GS ? geometry_sph : none, &slots);
    if (!code) return NULL;
    memset(info, 0, sizeof *info);
    info->rt_int_mask = stage == ST_GS ? 3 : 0;
    if (stage == ST_TCS) info->clip_enable = SNX_SH_NOT_LAST;
    u32* result = spirv_of(code, slots * sizeof *code, stage, info, words, 1, 1, 1, can_subgroup);
    free(code);
    return result;
}
/* the module uses no subgroup: no GroupNonUniform capability, instruction or SubgroupLocalInvocationId */
static int no_subgroup(const u32* code, size_t words) {
    int found = has_builtin(code, words, 41);
    for (u32 capability = 61; capability <= 68; capability++) found += count_opcode(code, words, 17, capability);
    for (u32 opcode = 333; opcode <= 366; opcode++) found += count_opcode(code, words, opcode, 0xffffffffu);
    return found == 0;
}

/* A vertex, evaluation or geometry program that votes or shuffles, on a device whose subgroups leave its stage out (an
   Adreno 830 lists fragment and compute), runs each invocation as lane 0 alone, as the console runs a batch of one
   vertex or primitive. The forms are those of a game's vertex programs, whose draws were skipped on that phone: a
   warp-aggregated atomic (the ballot of the live lanes, whose highest adds their count) and a block branched around
   when the whole warp agrees, plus SHFL and the other votes. Control and fragment programs still need the subgroup */
static void lone_lane_forms(void) {
    const u64 aggregated[] = {
        vote(0, 2, 7, 7, 0),   /* VOTE.ALL R2, PT, PT: the live lanes */
        0xF0C8000000070001ull, /* S2R R1, SR_LANEID */
        0x5C30000000270000ull, /* FLO.U32 R0, R2: the highest live lane */
        0x5C08000000270003ull, /* POPC R3, R2: how many */
        0x5B64038000170007ull, /* ISETP.EQ.U32.AND P0, PT, R0, R1, PT */
        0x4C98078004870000ull, /* MOV R0, c[0x0][0x120] */
        0x4C98078004970001ull, /* MOV R1, c[0x0][0x124] */
        0xEBF9000000000003ull, /* @P0 RED.E.ADD [R0], R3 */
    };
    const u64 uniform[] = {
        0x5B6503800FF70407ull, /* ISETP.EQ.U32.AND P0, PT, R4, RZ, PT */
        vote(0, 255, 1, 0, 0), /* VOTE.ALL RZ, P1, P0 */
        0x0103F8000001F00Aull, /* @P1 MOV32I R10, 1.0 */
        0xE24000000181000Full, /* @P1 BRA past the next two and their schedule word */
        0x010400000008F00Aull, /* @!P0 MOV32I R10, 2.0 */
        0x0103F8000000F00Aull, /* @P0 MOV32I R10, 1.0 */
        0xEFF07F800807FF0Aull, /* AST a[0x80], R10 */
    };
    const u64 others[] = {vote(1, 5, 2, 0, 1), vote(2, 6, 3, 0, 0), shfl(3, 1, 1, 0x1f), shfl(0, 0, 0, 0x1f)};
    static const struct {
        int stage;
        const char* what;
    } stages[] = {{ST_VS, "a vertex"}, {ST_TES, "an evaluation"}, {ST_GS, "a geometry"}};
    const struct {
        const u64* instructions;
        int count;
        const char* what;
    } forms[] = {{aggregated, 8, "a warp-aggregated atomic"},
                 {uniform, 7, "a branch on VOTE.ALL"},
                 {others, 4, "VOTE.ANY, VOTE.EQ and SHFL"}};
    ShaderInfo info;
    size_t words = 0;
    char what[160];
    int fixture = 0;
    for (size_t s = 0; s < sizeof stages / sizeof *stages; s++)
        for (size_t f = 0; f < sizeof forms / sizeof *forms; f++) {
            u32* code = lone_lane_spirv(forms[f].instructions, forms[f].count, stages[s].stage, 0, &info, &words);
            snprintf(what, sizeof what, "%s in %s program translates without subgroups in its stage", forms[f].what,
                     stages[s].what);
            check_translated(code, &info, what);
            if (code) {
                snprintf(what, sizeof what, "%s in %s program runs as lane 0 alone: no subgroup in its module",
                         forms[f].what, stages[s].what);
                check(no_subgroup(code, words), what);
                check(!info.primitive_split_safe, "a program that votes is never split into primitives");
                if (fixture < 9) save_fixture(lone_lane_paths[fixture++], code, words);
            }
            free(code);
            code = lone_lane_spirv(forms[f].instructions, forms[f].count, stages[s].stage, 1, &info, &words);
            snprintf(what, sizeof what, "%s in %s program uses the subgroup where its stage has it", forms[f].what,
                     stages[s].what);
            check_translated(code, &info, what);
            if (code) check(!no_subgroup(code, words) && count_opcode(code, words, 17, 61) == 1, what);
            free(code);
        }
    /* a control program's invocations are the lanes of one warp, together: a lane alone isn't one of its states */
    for (size_t f = 0; f < sizeof forms / sizeof *forms; f++) {
        u32* code = lone_lane_spirv(forms[f].instructions, forms[f].count, ST_TCS, 0, &info, &words);
        snprintf(what, sizeof what, "%s in a control program still needs the subgroup", forms[f].what);
        check_refused(code, &info, "subgroup ballot/shuffle unavailable", what);
        free(code);
    }
    u32* code = lone_lane_spirv(others, 4, ST_FS, 0, &info, &words);
    check_refused(code, &info, "subgroup ballot/shuffle unavailable", "SHFL in a fragment program needs the subgroup");
    free(code);
    /* FSWZADD's lanes are a quad's: the lone lane doesn't stand for them */
    const u64 swizzled[] = {vote(0, 2, 7, 7, 0), fswzadd(0xcc)};
    code = lone_lane_spirv(swizzled, 2, ST_VS, 0, &info, &words);
    check_refused(code, &info, "subgroup ballot/shuffle unavailable",
                  "FSWZADD with lanes that differ keeps the subgroup");
    free(code);
}

/* Real TCS builtins and nonzero geometry streams use distinct typed interfaces. */
static void rendering_stage_features(const char* directory) {
    int point_size_available = g_shader_stage_point_size;
    g_shader_stage_point_size = 1;
    const u32 control_header[3] = {3u << 24, 0, 0};
    const u64 control[] = {0x01000003f807f000ull, 0xeff000000007ff00ull | (0x6cull << 20),
                           0xeff000000007ff00ull | (0x2c0ull << 20), 0xe30000000007000full};
    ShaderInfo info;
    size_t words;
    u32* code = translate_stage(control, 4, ST_TCS, control_header, 0, &info, &words);
    check_translated(code, &info, "TCS point size and clip-distance outputs translate");
    check(code && info.writes_point_size && info.uses_clip_mask == 1, "TCS builtin output metadata is retained");
    if (code && directory) save_fixture(fixture_path(directory, "tcs_builtin_outputs"), code, words);
    free(code);
    const u64 evaluation[] = {0xefd800000007ff00ull | (0xffull << 39) | (0x6cull << 20),
                              0xefd800000007ff01ull | (0xffull << 39) | (0x2c0ull << 20), 0xe30000000007000full};
    const u32 empty_header[3] = {0};
    code = translate_stage(evaluation, 3, ST_TES, empty_header, 1, &info, &words);
    check_translated(code, &info, "TES per-vertex point size and clip-distance inputs translate");
    check(code && info.reads_point_size && info.clip_input_mask == 1, "TES builtin input metadata is retained");
    if (code && directory) save_fixture(fixture_path(directory, "tes_builtin_inputs"), code, words);
    free(code);
    g_shader_stage_point_size = point_size_available;
}

/* whether the module has a 32-bit float constant of those bits */
static int has_float_constant(const u32* code, size_t words, u32 bits) {
    u32 f32 = 0;
    for (size_t i = 5; i < words;) {
        u32 width = code[i] >> 16;
        if (!width || i + width > words) return 0;
        if ((code[i] & 0xffff) == 22 && width == 3 && code[i + 2] == 32) f32 = code[i + 1];
        if ((code[i] & 0xffff) == 43 && width == 4 && f32 && code[i + 1] == f32 && code[i + 3] == bits) return 1;
        i += width;
    }
    return 0;
}
/* the word of instruction i as guest_program lays a program out: a scheduling word before every third */
static u32 flow_word(u32 i) { return 11 + i + i / 3; }
/* BRA from instruction from to instruction to, on predicate (7: always) */
static u64 flow_bra(u32 from, u32 to, u32 predicate) {
    u32 bytes = (flow_word(to) - (flow_word(from) + 1)) * 8;
    return 0xe24000000000000full | (u64)(bytes & 0xffffff) << 20 | (u64)predicate << 16;
}
/* a fragment program on a device with fragment subgroups or without, with MaximallyReconvergesKHR or without, and
   with an advanced blend equation or none */
static u32* flow_lod_spirv(const u64* instructions, int count, int can_subgroup, int reconverges, u64 blend,
                           ShaderInfo* info, size_t* words) {
    size_t slots;
    u64* code = guest_program(instructions, count, NULL, &slots);
    if (!code) return NULL;
    memset(info, 0, sizeof *info);
    info->advanced_blend = blend;
    if (blend) set_sph(code, 18, 15); /* the equation blends render target 0's RGBA */
    int device = g_shader_maximal_reconvergence;
    g_shader_maximal_reconvergence = reconverges;
    u32* result = spirv_of(code, slots * sizeof *code, ST_FS, info, words, 1, 1, 1, can_subgroup);
    g_shader_maximal_reconvergence = device;
    free(code);
    return result;
}
/* A fragment program's loop ends for a helper lane after SH_HELPER_LOOP_ROUNDS back-edges, and runs as written for the
   other lanes: a loop with no way out but EXIT kills the helper, a loop with a BRK lets it on past the loop, and the
   dispatch loop of a program with calls ends for it. Run on the CPU (support/spv_eval.c) as a helper and as a lane
   that is not one; a vertex program and a fragment program without loops don't ask whether a lane is a helper */
static void helper_loop_exit(void) {
    enum { IS_HELPER = 5381 };
    /* @P1 BRA to the EXIT at 4, then a loop of @P0 EXIT and a BRA back to it: P0 and P1 are never set, and the
       loop's only way out is its EXIT */
    const u64 endless[] = {flow_bra(0, 4, 1), mov32i(0, 1), 0xe30000000000000full, flow_bra(3, 2, 7)};
    const u64 pbk = 0xe2a000000007000full | (u64)((flow_word(3) - flow_word(1)) * 8) << 20;  /* PBK to 3 */
    const u64 breaks[] = {pbk, 0xe34000000000000full, flow_bra(2, 1, 7), mov32i(0, 0x1234)}; /* @P0 BRK */
    const u64 cal = 0xe260000000070040ull | (u64)((flow_word(2) - flow_word(1)) * 8) << 20;  /* CAL 2 */
    const u64 called[] = {cal, EXIT, flow_bra(2, 2, 7), 0xe32000000007000full};              /* RET */
    const struct {
        const u64* instructions;
        int count, killed;
        u32 out;
        const char* what;
    } cases[] = {
        {endless, 4, 1, 0, "a helper in a loop no lane leaves but by EXIT ends there"},
        {breaks, 4, 0, 0x1234, "a helper in a loop with a BRK goes on past the loop"},
        {called, 4, 0, 0, "a helper in the dispatch loop of a program with calls leaves it"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        ShaderInfo info = {0};
        size_t words = 0, slots;
        u64* code = guest_program(cases[i].instructions, cases[i].count, NULL, &slots);
        if (!code) return;
        set_sph(code, 18, 0xf);
        info.rt_int_mask = 1;
        u32* spv = spirv_of(code, slots * sizeof *code, ST_FS, &info, &words, 1, 1, 1, 1);
        free(code);
        check_translated(spv, &info, "a fragment program with a loop translates");
        if (!spv) continue;
        for (int helper = 1; helper >= 0; helper--) {
            SpvEvalEnv env = {0};
            float one[4] = {1.0f, 1.0f, 1.0f, 0.0f};
            u32 scale[4];
            memcpy(scale, one, sizeof one);
            env.ubo[SNX_VKB_RS] = scale;
            env.ubo_words[SNX_VKB_RS] = 4;
            env.helper = helper;
            env.steps_max = 32u * 65536u; /* room for a helper's 65536 back-edges (SH_HELPER_LOOP_ROUNDS) */
            const char* why = spv_eval(spv, words, &env);
            int ended = !why && env.killed == cases[i].killed && (!cases[i].out || env.out[0][0] == cases[i].out);
            if (helper ? !ended : !why)
                fprintf(stderr, "helper loop case %zu as %s: %s, killed %d, R0 %08x\n", i,
                        helper ? "a helper" : "a lane that is not one", why ? why : "ended", env.killed, env.out[0][0]);
            check(helper ? ended : why && !strcmp(why, "too many steps"),
                  helper ? cases[i].what : "a lane that is not a helper runs the loop as written");
        }
        free(spv);
    }
    const u64 straight[] = {mov32i(0, 1)};
    for (int stage = 0; stage < 2; stage++) {
        ShaderInfo info;
        size_t words = 0;
        u32* spv = translate(stage ? straight : endless, stage ? 1 : 4, stage ? ST_FS : ST_VS, &info, &words, 1);
        check_translated(spv, &info,
                         stage ? "a fragment program without loops translates" : "a vertex loop translates");
        if (spv)
            check(count_opcode(spv, words, IS_HELPER, 0xffffffffu) == 0,
                  stage ? "a fragment program without loops doesn't count back-edges"
                        : "a vertex program's loop doesn't count back-edges");
        free(spv);
    }
}
/* A fragment program with flow and a sample that takes its level from derivatives, on a device whose subgroups leave
   the fragment stage out (Turnip on an Adreno 610 lists compute only, llvmpipe's have 8 lanes): the program's own
   branches and loops as structured constructs, with no subgroup operation and no dispatch loop, and
   MaximallyReconvergesKHR where the device has it. Such a program was refused whole. One the structure can't take (a
   call, an indirect jump, a control stack kept at run time, an advanced blend equation, flow shader_structure gives
   no structure for) is still refused there, with the reason, and still translated with fragment subgroups */
static void fragment_flow_derivatives(void) {
    /* TEX R0, R4, R5: a 2D texture by the handle at c[2][0x128], the level its derivatives give */
    const u64 tex = 0xc038000000000000ull | 74ull << 36 | 7ull << 51 | 1ull << 31 | 2ull << 28 | 7ull << 16 | 4ull << 8;
    const u64 nop = 0x50B0000000070F00ull;
    const u64 branch[] = {flow_bra(0, 2, 0), nop, tex};                    /* @P0 BRA over a NOP, then the sample */
    const u64 loop[] = {nop, flow_bra(1, 0, 0), tex};                      /* @P0 BRA back, then the sample */
    const u64 explicit_lod[] = {flow_bra(0, 2, 0), nop, tex | 1ull << 55}; /* TEX.LZ: no derivative */
    const struct {
        const u64* instructions;
        const char* what;
    } structured[] = {{branch, "a sample after a branch"}, {loop, "a sample after a loop"}};
    ShaderInfo info;
    size_t words = 0;
    char what[160];
    int switches;
    for (size_t f = 0; f < sizeof structured / sizeof *structured; f++)
        for (int reconverges = 0; reconverges < 2; reconverges++) {
            u32* code = flow_lod_spirv(structured[f].instructions, 3, 0, reconverges, 0, &info, &words);
            snprintf(what, sizeof what, "%s translates without fragment subgroups", structured[f].what);
            check_translated(code, &info, what);
            if (code) {
                snprintf(what, sizeof what, "%s is structured flow without fragment subgroups: no dispatch loop",
                         structured[f].what);
                check(switch_cases(code, words, &switches) == 0 && switches == 0 &&
                          count_opcode(code, words, 246, 0xffffffffu) >= 1,
                      what);
                snprintf(what, sizeof what, "%s uses no subgroup and keeps its implicit level", structured[f].what);
                check(no_subgroup(code, words) && count_opcode(code, words, 87, 0xffffffffu) == 1, what);
                snprintf(what, sizeof what, "%s declares MaximallyReconvergesKHR only where the device has it",
                         structured[f].what);
                check(count_opcode(code, words, 16, 6023) == reconverges, what);
            }
            free(code);
        }
    /* with fragment subgroups the program is what it was: the dispatch loop on the subgroup's lowest pc, or
       structured flow where the device reconverges */
    u32* code = flow_lod_spirv(branch, 3, 1, 0, 0, &info, &words);
    check_translated(code, &info, "a sample after a branch translates with fragment subgroups");
    if (code)
        check(switch_selects_current_pc(code, words) && !no_subgroup(code, words) &&
                  count_opcode(code, words, 16, 6023) == 0,
              "with fragment subgroups and no MaximallyReconvergesKHR the lanes wait in the dispatch loop");
    free(code);
    code = flow_lod_spirv(branch, 3, 1, 1, 0, &info, &words);
    check_translated(code, &info, "a sample after a branch translates on a device that reconverges");
    if (code)
        check(switch_cases(code, words, &switches) == 0 && switches == 0 && count_opcode(code, words, 16, 6023) == 1,
              "with fragment subgroups and MaximallyReconvergesKHR the program is structured");
    free(code);
    /* flow without a derivative never needed the subgroup */
    code = flow_lod_spirv(explicit_lod, 3, 0, 0, 0, &info, &words);
    check_translated(code, &info, "a sample of level 0 after a branch translates without fragment subgroups");
    free(code);
    /* what the structure can't take */
    const u64 calls[] = {0xe260000000070040ull | (u64)((flow_word(4) - flow_word(1)) * 8) << 20, /* CAL 4 */
                         tex, EXIT, nop, 0xe32000000007000full};                                 /* RET */
    const u64 jump[] = {flow_bra(0, 2, 0), nop, tex, 0xe250000000070d0full}; /* a BRX with no table to read */
    const u64 stack[] = {0xe23000000007000full | (u64)((flow_word(2) - flow_word(1)) * 8) << 20, /* PEXIT 2 */
                         nop, tex};
    /* a loop entered at two places: @P0 BRA 2 into its second block, which branches back to its first */
    const u64 irreducible[] = {flow_bra(0, 2, 0), nop, flow_bra(2, 1, 1), tex};
    const struct {
        const u64* instructions;
        int count;
        u64 blend;
        const char *with, *what;
    } refused[] = {{calls, 5, 0, "calls", "a call"},
                   {jump, 4, 0, "indirect jumps", "an indirect jump"},
                   {stack, 3, 0, "a run-time control stack", "a control stack kept at run time"},
                   {branch, 3, 5ull << 37, "advanced blend", "an advanced blend equation"},
                   {irreducible, 4, 0, "flow without a structure", "irreducible flow"}};
    for (size_t f = 0; f < sizeof refused / sizeof *refused; f++) {
        char reason[96];
        snprintf(reason, sizeof reason, "derivatives after flow, without fragment subgroups, in a program with %s",
                 refused[f].with);
        for (int reconverges = 0; reconverges < 2; reconverges++) {
            code = flow_lod_spirv(refused[f].instructions, refused[f].count, 0, reconverges, refused[f].blend, &info,
                                  &words);
            snprintf(what, sizeof what, "a sample after flow with %s is refused without fragment subgroups, by name",
                     refused[f].what);
            check_refused(code, &info, reason, what);
            check(strlen(info.first_unsupported) == strlen(reason), "the refusal's reason is whole");
            free(code);
        }
        code = flow_lod_spirv(refused[f].instructions, refused[f].count, 1, 0, refused[f].blend, &info, &words);
        snprintf(what, sizeof what, "a sample after flow with %s translates with fragment subgroups", refused[f].what);
        check_translated(code, &info, what);
        if (code) {
            snprintf(what, sizeof what, "a sample after flow with %s waits in the dispatch loop", refused[f].what);
            check(switch_cases(code, words, &switches) > 0 && !no_subgroup(code, words), what);
        }
        free(code);
    }
}

/* An Input variable is loaded once in a block and each of its components taken once, however many instructions read
   it (translate.h's input_seen); a later block, which the first need not dominate, loads it again */
static void inputs_loaded_once_a_block(void) {
#define INPUT_IPA(d, addr) (0xe000000000070000ull | (u64)(addr) << 28 | 0xff00ull | (u64)(d))
#define INPUT_ALD(d, addr) ((0xefd9ff8000070000ull & ~(3ull << 47)) | (u64)(addr) << 20 | 0xff00ull | (u64)(d))
    enum { EXTRACT = 81, FRAG_COORD = 15, LANE = 41 };
    const u64 branch = 0xe24000000000000full | (16ull << 20); /* @P0 BRA over the next instruction */
    const u64 al2p =
        (0xefa070010607ff01ull & ~(0xffull | (0xffull << 8) | (0x7ffull << 20))) | 8ull | (2ull << 8) | (0x80ull << 20);
    const u64 indexed = 0xe06000c800770800ull; /* IPA.IDX.MUL R0, a[R8], R7 */
    const struct {
        u64 instructions[5];
        int count;
        int generic, coord, extracts; /* the loads of the generic inputs and of FragCoord, and the components taken */
        const char* what;
    } fragment[] = {
        {{0}, 0, 0, 0, 0, "a fragment program of an EXIT alone loads no input"},
        {{INPUT_IPA(0, 0xc0), INPUT_IPA(1, 0xc4), INPUT_IPA(2, 0xc0)},
         3,
         1,
         1,
         3,
         "a perspective input's x, y and x again: one load of it and one of FragCoord, and x, y and w taken once"},
        {{INPUT_IPA(0, 0x70), INPUT_IPA(1, 0x74), INPUT_IPA(2, 0x7c), INPUT_IPA(3, 0x70)},
         4,
         0,
         1,
         3,
         "the fragment's x, y, 1/w and x again: one load of FragCoord and three components"},
        {{INPUT_IPA(0, 0xc0), INPUT_IPA(1, 0x7c)},
         2,
         1,
         1,
         2,
         "a perspective input and a[0x7c] share FragCoord's load and its w"},
        {{INPUT_IPA(0, 0xc0), branch, INPUT_IPA(1, 0xc0), INPUT_IPA(2, 0xc0)},
         4,
         3,
         3,
         6,
         "an input read in three blocks is loaded in each, with FragCoord, and its x and w taken in each"},
        {{mov32i(2, 0x10), al2p, indexed, indexed},
         4,
         -1,
         1,
         -1,
         "two indexed IPAs in a block share FragCoord's load for their perspective inputs"},
    };
    int base_extracts = 0;
    for (size_t i = 0; i < sizeof fragment / sizeof *fragment; i++) {
        ShaderInfo info;
        size_t words = 0, slots;
        int again = 0;
        u64* code = guest_program(fragment[i].instructions, fragment[i].count, NULL, &slots);
        if (!code) return;
        set_sph(code, 6, 0xaaaa); /* generic inputs 0 and 1, perspective: the indexed IPAs' candidates */
        set_sph(code, 7, 0xaa);   /* generic input 4, perspective */
        set_sph(code, 18, 0xf);
        memset(&info, 0, sizeof info);
        u32* spv = spirv_of(code, slots * sizeof *code, ST_FS, &info, &words, 0, 1, 1, 1);
        free(code);
        check_translated(spv, &info, "a fragment program reading its inputs translates");
        if (!spv) continue;
        int generic = input_loads(spv, words, -1, &again), coord = input_loads(spv, words, FRAG_COORD, &again);
        int extracts = count_opcode(spv, words, EXTRACT, 0xffffffffu) - base_extracts;
        if (!i) base_extracts = extracts;
        if ((fragment[i].generic >= 0 && generic != fragment[i].generic) || coord != fragment[i].coord ||
            (i && fragment[i].extracts >= 0 && extracts != fragment[i].extracts))
            fprintf(stderr,
                    "fragment inputs %zu: %d loads of generic inputs, %d of FragCoord, %d components, not %d, %d, %d\n",
                    i, generic, coord, extracts, fragment[i].generic, fragment[i].coord, fragment[i].extracts);
        check((fragment[i].generic < 0 || generic == fragment[i].generic) && coord == fragment[i].coord &&
                  (!i || fragment[i].extracts < 0 || extracts == fragment[i].extracts),
              fragment[i].what);
        free(spv);
    }
    /* a vertex program: attribute 0's x, y and x again */
    ShaderInfo info;
    size_t words = 0;
    int again = 0;
    const u64 attribute[] = {INPUT_ALD(0, 0x80), INPUT_ALD(1, 0x84), INPUT_ALD(2, 0x80)};
    u32* spv = translate(NULL, 0, ST_VS, &info, &words, 0);
    int vertex_extracts = spv ? count_opcode(spv, words, EXTRACT, 0xffffffffu) : 0;
    free(spv);
    spv = translate(attribute, 3, ST_VS, &info, &words, 0);
    check_translated(spv, &info, "a vertex program reading an attribute translates");
    check(spv && input_loads(spv, words, -1, &again) == 1 &&
              count_opcode(spv, words, EXTRACT, 0xffffffffu) == vertex_extracts + 2,
          "an attribute's x, y and x again: one load of it, and x and y taken once");
    free(spv);
    /* two FSWZADD whose lanes differ read the subgroup's lane once */
    const u64 swizzled[] = {fswzadd(0x99), fswzadd(0x99)};
    size_t slots;
    u64* code = guest_program(swizzled, 2, NULL, &slots);
    if (!code) return;
    memset(&info, 0, sizeof info);
    spv = spirv_of(code, slots * sizeof *code, ST_FS, &info, &words, 0, 1, 1, 1);
    free(code);
    check_translated(spv, &info, "two FSWZADD translate");
    check(spv && input_loads(spv, words, LANE, &again) == 1, "two FSWZADD in a block load the subgroup's lane once");
    free(spv);
#undef INPUT_IPA
#undef INPUT_ALD
}

/* The alpha test's function is the pipeline's, a specialization constant (shader.h's SH_SPEC_ALPHA): a fragment
   program that writes a colour declares it and reads the reference alone from the push constants, and with each
   function keeps or discards the fragment as the 3D engine's test does; with none given the test is off. A program
   that writes no colour has no test and declares nothing */
static void alpha_function_specialized(void) {
    enum { SPEC_CONSTANT = 50, DECORATE = 71, SPEC_ID = 1 };
    const float alphas[3] = {0.25f, 0.5f, 0.75f}, reference = 0.5f;
    for (u32 at = 0; at < 3; at++) {
        u32 alpha_bits, reference_bits;
        memcpy(&alpha_bits, &alphas[at], 4);
        memcpy(&reference_bits, &reference, 4);
        const u64 program[] = {mov32i(3, alpha_bits)}; /* R3, colour 0's alpha */
        for (int colour = 0; colour < 2; colour++) {
            ShaderInfo info = {0};
            size_t words = 0, slots;
            u64* code = guest_program(program, 1, NULL, &slots);
            if (!code) return;
            set_sph(code, 18, colour ? 0xf : 0);
            u32* spv = spirv_of(code, slots * sizeof *code, ST_FS, &info, &words, 0, 1, 1, 1);
            free(code);
            check_translated(spv, &info, "a fragment program setting its alpha translates");
            if (!spv) continue;
            int constants = count_opcode(spv, words, SPEC_CONSTANT, 0xffffffffu), named = 0;
            for (size_t i = 5; i < words; i += spv[i] >> 16) /* OpDecorate id SpecId SH_SPEC_ALPHA */
                if ((spv[i] & 0xffff) == DECORATE && spv[i] >> 16 == 4 && spv[i + 2] == SPEC_ID &&
                    spv[i + 3] == SH_SPEC_ALPHA)
                    named++;
            check(constants == colour && named == colour && info.spec == (colour ? 1u << SH_SPEC_ALPHA : 0u),
                  "a program that writes a colour declares the alpha function, and one that writes none nothing");
            for (u32 function = 0; function <= 8 && colour; function++) { /* 8: none given */
                SpvEvalEnv env = {0};
                env.push[0] = env.push[1] = 0x7fc00000u; /* what the test's switch and function were: not read */
                env.push[2] = reference_bits;
                env.spec[SH_SPEC_ALPHA] = function;
                env.spec_given = function < 8 ? 1u << SH_SPEC_ALPHA : 0;
                const char* why = spv_eval(spv, words, &env);
                float a = alphas[at];
                int passes = function == 1   ? (a < reference)
                             : function == 2 ? (a == reference)
                             : function == 3 ? (a <= reference)
                             : function == 4 ? (a > reference)
                             : function == 5 ? (a != reference)
                             : function == 6 ? (a >= reference)
                                             : function >= 7;
                if (why || env.killed != !passes)
                    fprintf(stderr, "alpha test: function %u of alpha %g against %g: %s, killed %d, not %d\n", function,
                            (double)a, (double)reference, why ? why : "ran", env.killed, !passes);
                check(!why && env.killed == !passes,
                      "each alpha function keeps the fragments the 3D engine's test keeps");
            }
            free(spv);
        }
    }
}

/* The window's origin and the point sprite's flip are the pipeline's too (SH_SPEC_LOWER_LEFT, SH_SPEC_SPRITE_FLIP):
   only a program that reads the fragment's y or the sprite's T declares one, and each reads as the state says. The
   surface clip's height stays a push constant */
static void window_origin_specialized(void) {
    const struct {
        u64 read;       /* into R1, colour 0's green */
        u32 spec;       /* the constant it declares beside the alpha function, SH_SPEC_COUNT for none */
        float given[2]; /* FragCoord's or PointCoord's component, and what R1 holds with the constant 0 */
        float turned;   /* and with the constant 1 */
        const char* what;
    } cases[] = {
        {0xe003ff874ff7ff01ull,
         SH_SPEC_LOWER_LEFT,
         {1.5f, 1.5f},
         6.5f,
         "the fragment's y counts up from the clip's 8 rows in a lower-left window"},
        {0xe003ff870ff7ff01ull, SH_SPEC_COUNT, {1.5f, 1.5f}, 1.5f, "the fragment's x declares no constant"},
        {0xe003ffae4ff7ff01ull,
         SH_SPEC_SPRITE_FLIP,
         {0.25f, 0.25f},
         0.75f,
         "the sprite's T runs the other way with the flip"},
        {0xe003ffae0ff7ff01ull, SH_SPEC_COUNT, {0.25f, 0.25f}, 0.25f, "the sprite's S declares no constant"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        ShaderInfo info = {0};
        size_t words = 0, slots;
        u64* code = guest_program(&cases[i].read, 1, NULL, &slots);
        if (!code) return;
        set_sph(code, 18, 0xf);
        u32* spv = spirv_of(code, slots * sizeof *code, ST_FS, &info, &words, 0, 1, 1, 1);
        free(code);
        check_translated(spv, &info, "a fragment program reading its place translates");
        if (!spv) continue;
        u32 want = 1u << SH_SPEC_ALPHA | (cases[i].spec < SH_SPEC_COUNT ? 1u << cases[i].spec : 0u);
        if (info.spec != want) fprintf(stderr, "origin case %zu declares constants %x, not %x\n", i, info.spec, want);
        check(info.spec == want && count_opcode(spv, words, 50, 0xffffffffu) == (cases[i].spec < SH_SPEC_COUNT ? 2 : 1),
              cases[i].what);
        for (u32 on = 0; on < 3; on++) { /* 2: none given */
            SpvEvalEnv env = {0};
            float one[4] = {1.0f, 1.0f, 1.0f, 0.0f}, got;
            u32 scale[4];
            memcpy(scale, one, sizeof one);
            env.ubo[SNX_VKB_RS] = scale;
            env.ubo_words[SNX_VKB_RS] = 4;
            for (u32 k = 0; k < 2; k++) { /* FragCoord (15) and PointCoord (16), x and y alike */
                memcpy(&env.builtin[15][k], &cases[i].given[0], 4);
                memcpy(&env.builtin[16][k], &cases[i].given[0], 4);
            }
            env.push[7] = 8; /* the surface clip's height */
            if (cases[i].spec < SH_SPEC_COUNT && on < 2) {
                env.spec[cases[i].spec] = on;
                env.spec_given = 1u << cases[i].spec;
            }
            const char* why = spv_eval(spv, words, &env);
            memcpy(&got, &env.out[0][1], 4);
            float expected = on == 1 ? cases[i].turned : cases[i].given[1];
            if (why || got != expected)
                fprintf(stderr, "origin case %zu with the constant %u: %s, read %g, not %g\n", i, on, why ? why : "ran",
                        (double)got, (double)expected);
            check(!why && got == expected, cases[i].what);
        }
        free(spv);
    }
}

/* What the translator knows as it translates is settled there, not left for the driver's compiler: PT, the
   predicate most instructions name, is the constant true; a constant a register holds is a constant of the type it
   is read as; and a float a register holds is read back as that float, not through its bits */
static void settled_when_translated(void) {
    enum { SELECT = 169, BITCAST = 124, F_MIN = 37, F_MAX = 40, F_ABS = 4 };
    const u64 pt = 7ull << 39, negated = 1ull << 42, a1_b2 = 1ull << 8 | 2ull << 20;
    ShaderInfo info;
    size_t words = 0;
    u32* code = translate(NULL, 0, ST_FS, &info, &words, 0);
    check_translated(code, &info, "a program of an EXIT alone translates");
    int selects = code ? count_opcode(code, words, SELECT, 0xffffffffu) : 0;
    int bitcasts = code ? count_opcode(code, words, BITCAST, 0xffffffffu) : 0;
    check(code && !has_glsl_instruction(code, words, F_MIN) && !has_glsl_instruction(code, words, F_MAX) &&
              !has_glsl_instruction(code, words, F_ABS),
          "a fragment program's epilogue has no minimum, maximum or absolute value of its own");
    free(code);
    /* FSETP P1, P2 = R1 < R2 combined with PT, !PT and P0: by AND, OR and XOR (bits 45-46) */
    const u64 fsetp = 0x5BB0000000070000ull | 1ull << 48 | a1_b2 | 1ull << 3 | 2;
    const struct {
        u64 predicate;
        int made; /* the ANDs, ORs or XORs written, one for each of the two predicates */
    } combined[] = {{pt, 0}, {pt | negated, 0}, {0, 2}};
    static const u32 combine[3] = {167, 166, 165}; /* OpLogicalAnd, OpLogicalOr, OpLogicalNotEqual */
    for (u32 by = 0; by < 3; by++) {
        code = translate(NULL, 0, ST_FS, &info, &words, 0);
        int epilogue = code ? count_opcode(code, words, combine[by], 0xffffffffu) : 0;
        free(code);
        for (int i = 0; i < 3; i++) {
            const u64 program[] = {fsetp | combined[i].predicate | (u64)by << 45};
            code = translate(program, 1, ST_FS, &info, &words, 0);
            check_translated(code, &info, "FSETP translates");
            int made = code ? count_opcode(code, words, combine[by], 0xffffffffu) - epilogue : -1;
            if (made != combined[i].made)
                fprintf(stderr, "FSETP combined by %u with predicate form %d writes %d of opcode %u, not %d\n", by, i,
                        made, combine[by], combined[i].made);
            check(
                made == combined[i].made,
                "a comparison combined with PT or !PT is settled, and one combined with a predicate is written twice");
            free(code);
        }
    }
    /* PSETP P1, P2 = PT and P0, !PT and P0, each combined with PT: P0 and false, with no NOT of PT written */
    code = translate(NULL, 0, ST_FS, &info, &words, 0);
    int nots = code ? count_opcode(code, words, 168, 0xffffffffu) : 0;
    free(code);
    const u64 psetp[] = {0x5090000000070000ull | 7ull << 12 | pt | 1ull << 3 | 2};
    code = translate(psetp, 1, ST_FS, &info, &words, 0);
    check_translated(code, &info, "PSETP translates");
    check(code && count_opcode(code, words, 168, 0xffffffffu) == nots,
          "PSETP's second predicate takes !PT as the constant false, with no OpLogicalNot");
    free(code);
    /* FMNMX R4 = R1, R2 picked by PT (the minimum), !PT (the maximum) and P0 (either), then DMNMX, IMNMX and SEL */
    const struct {
        u64 word;
        int minimum, maximum, selects;
    } picked[] = {{0x5C60000000070000ull | pt, 1, 0, 2}, /* the two operands' NaN tests */
                  {0x5C60000000070000ull | pt | negated, 0, 1, 2},
                  {0x5C60000000070000ull, 1, 1, 3},
                  {0x5C50000000070000ull | pt, 1, 0, 2}, /* DMNMX: as FMNMX, of doubles */
                  {0x5C50000000070000ull | pt | negated, 0, 1, 2},
                  {0x5C50000000070000ull, 1, 1, 5},
                  {0x5C20000000070000ull | pt, 0, 0, 1}, /* IMNMX: the minimum's select */
                  {0x5C20000000070000ull | pt | negated, 0, 0, 1},
                  {0x5C20000000070000ull, 0, 0, 3},
                  {0x5CA0000000070000ull | pt, 0, 0, 0}, /* SEL: R1 itself */
                  {0x5CA0000000070000ull, 0, 0, 1}};
    for (int i = 0; i < 11; i++) {
        const u64 program[] = {picked[i].word | a1_b2 | 4};
        code = translate(program, 1, ST_FS, &info, &words, 0);
        check_translated(code, &info, "FMNMX, DMNMX, IMNMX and SEL translate");
        if (code && count_opcode(code, words, SELECT, 0xffffffffu) != selects + picked[i].selects)
            fprintf(stderr, "pick %d: %d OpSelect, not the epilogue's %d and its %d\n", i,
                    count_opcode(code, words, SELECT, 0xffffffffu), selects, picked[i].selects);
        check(code && has_glsl_instruction(code, words, F_MIN) == picked[i].minimum &&
                  has_glsl_instruction(code, words, F_MAX) == picked[i].maximum &&
                  count_opcode(code, words, SELECT, 0xffffffffu) == selects + picked[i].selects,
              "PT or !PT picks one side as the program is translated: the other side and the select are not made");
        free(code);
    }
    /* R4 = R1 * R2, R5 = R4 (MOV), R6 = R5 * R5: R1 and R2 are read as floats, and nothing else is cast */
    const u64 copied[] = {0x5C68000000070000ull | a1_b2 | 4, 0x5C98078000070000ull | 4ull << 20 | 5,
                          0x5C68000000070000ull | 5ull << 8 | 5ull << 20 | 6};
    code = translate(copied, 3, ST_FS, &info, &words, 0);
    check_translated(code, &info, "a float product copied and multiplied translates");
    if (code && count_opcode(code, words, BITCAST, 0xffffffffu) != bitcasts + 2)
        fprintf(stderr, "a copied float: %d OpBitcast, not the epilogue's %d and 2\n",
                count_opcode(code, words, BITCAST, 0xffffffffu), bitcasts);
    check(code && count_opcode(code, words, BITCAST, 0xffffffffu) == bitcasts + 2,
          "a register copied from a float is read as that float, with no bitcast of its bits");
    free(code);
    /* R4 = 0.5's bits (MOV32I), R5 = R1 * R4: the factor is the float constant */
    const u64 immediate[] = {mov32i(4, 0x3f000000u), 0x5C68000000070000ull | 1ull << 8 | 4ull << 20 | 5};
    code = translate(immediate, 2, ST_FS, &info, &words, 0);
    check_translated(code, &info, "a float product with a register's constant translates");
    check(code && count_opcode(code, words, BITCAST, 0xffffffffu) == bitcasts + 1 &&
              has_float_constant(code, words, 0x3f000000u),
          "a register's constant read as a float is a float constant of its bits, not their bitcast");
    free(code);
    /* R4 = -1.0's bits, R5 = R1 + |R4| (FADD's bit 49): the constant's absolute value, 1.0, with no FAbs */
    const u64 absolute[] = {mov32i(4, 0xbf800000u), 0x5C58000000070000ull | 1ull << 49 | 1ull << 8 | 4ull << 20 | 5};
    code = translate(absolute, 2, ST_FS, &info, &words, 0);
    check_translated(code, &info, "a float sum with a constant's absolute value translates");
    check(code && !has_glsl_instruction(code, words, F_ABS) && has_float_constant(code, words, 0x3f800000u),
          "a constant's absolute value is the constant 1.0 for -1.0, with no FAbs");
    free(code);
    /* Each program's bitcasts beyond the epilogue's. R5 = R1's bits (MOV), R4 = R1 * R5: those bits are cast once.
       R4 = R1 * R2, R5 = PT ? R4 : R2 (SEL, R4's bits themselves), R6 = R5 * R5: R1, R2 and R4's bits for the SEL,
       which R5 gives back as R4's float. R4 = floor(R1) (F2F), R5 = R4 * R4: R1 alone. R4 = c1[2]'s bits (MOV),
       R5 = R1 * c1[2], R6 = R4 * R4: R1 and the constant buffer word, cast once. R4 = R1 * RZ: R1, RZ being the
       constant 0.0. R5 = R4 < 0 ? R1 : 1.0 (FCMP with an immediate): R4, the immediate's bits being a constant.
       R4 = floor(c1[2]) (F2F), R5 = R1 * c1[2]: R1 and the word, cast once for both */
    const u64 fmul = 0x5C68000000070000ull, mov = 0x5C98078000070000ull, word = 1ull << 34 | 2ull << 20;
    const u64 twice[] = {mov | 1ull << 20 | 5, fmul | 1ull << 8 | 5ull << 20 | 4};
    const u64 back[] = {fmul | a1_b2 | 4, 0x5CA0000000070000ull | pt | 4ull << 8 | 2ull << 20 | 5,
                        fmul | 5ull << 8 | 5ull << 20 | 6};
    const u64 floored[] = {0x5CA8000000070000ull | 1ull << 42 | 1ull << 39 | 2ull << 10 | 2ull << 8 | 1ull << 20 | 4,
                           fmul | 4ull << 8 | 4ull << 20 | 5};
    const u64 buffer[] = {0x4C98078000070000ull | word | 4, 0x4C68000000070000ull | word | 1ull << 8 | 5,
                          fmul | 4ull << 8 | 4ull << 20 | 6};
    const u64 zero[] = {fmul | 1ull << 8 | 255ull << 20 | 4};
    const u64 compared[] = {0x36A0000000070000ull | 4ull << 39 | 1ull << 48 | 1ull << 8 | 0x3f800ull << 20 | 5};
    const u64 floored_word[] = {0x4CA8000000070000ull | 1ull << 42 | 1ull << 39 | 2ull << 10 | 2ull << 8 | word | 4,
                                0x4C68000000070000ull | word | 1ull << 8 | 5};
    const struct {
        const u64* program;
        int count, casts;
        const char* what;
    } cast[] = {{twice, 2, 1, "a register's bits copied to another are cast to a float once for both"},
                {back, 3, 3, "bits made from a float and passed on are read back as that float"},
                {floored, 2, 1, "a float conversion's result is kept as the float, its bits not made"},
                {buffer, 3, 2, "a constant buffer word is cast to a float once for a register that holds it too"},
                {zero, 1, 1, "RZ read as a float is the constant 0.0"},
                {compared, 1, 1, "a float immediate taken as bits is the constant of its bits"},
                {floored_word, 2, 2, "a constant buffer word a conversion reads is cast to a float once"}};
    for (int i = 0; i < 7; i++) {
        code = translate(cast[i].program, cast[i].count, ST_FS, &info, &words, 0);
        check_translated(code, &info, cast[i].what);
        int made = code ? count_opcode(code, words, BITCAST, 0xffffffffu) - bitcasts : -1;
        if (made != cast[i].casts)
            fprintf(stderr, "cast program %d writes %d OpBitcast beyond the epilogue's %d, not %d\n", i, made, bitcasts,
                    cast[i].casts);
        check(made == cast[i].casts, cast[i].what);
        free(code);
    }
    /* R4 = R1 * R2, R5 = R4 (MOV), then an EXIT under P0, which ends the block: R4 and R5 are stored from one
       bitcast of the product, after R1's and R2's; the program's two exits each have the epilogue */
    const u64 stored[] = {fmul | a1_b2 | 4, mov | 4ull << 20 | 5, 0xE30000000000000Full};
    code = translate(stored, 3, ST_FS, &info, &words, 0);
    check_translated(code, &info, "a copied float stored at a block's end translates");
    int stored_casts = code ? count_opcode(code, words, BITCAST, 0xffffffffu) - 2 * bitcasts : -1;
    if (stored_casts != 3)
        fprintf(stderr, "two registers of one float stored: %d OpBitcast beyond the epilogues' %d, not 3\n",
                stored_casts, 2 * bitcasts);
    check(stored_casts == 3, "two registers that hold one float are stored from one bitcast of it");
    free(code);
    /* Reads that spirv_of's check judges, each of a float the instruction before wrote: a product as FCMP's third
       operand, as HADD2's two F32 operands, as TEXS.LL.DC's depth reference (R5), as TLD4's y (R5) and TLD4S.DC's
       depth reference (R6), and the depth comparison's result (TEXS.LL.DC writes R0, TEX.DC R8) multiplied */
    const u64 texs =
        0xD000000000070000ull | 1ull << 59 | 5ull << 53 | 0x10ull << 36 | 0xffull << 28 | 4ull << 20 | 2ull << 8;
    const u64 tex =
        0xC000000000070000ull | 0x10ull << 36 | 7ull << 51 | 1ull << 50 | 1ull << 31 | 2ull << 28 | 0xffull << 20 | 8;
    const u64 tld4 = 0xC800000000070000ull | 7ull << 51 | 0x10ull << 36 | 0xfull << 31 | 2ull << 28 | 4ull << 8;
    const u64 judged[][2] = {
        {fmul | a1_b2 | 4, 0x5BA0000000070000ull | 4ull << 39 | 1ull << 48 | a1_b2 | 5},
        {fmul | a1_b2 | 4, 0x5D10000000070000ull | 1ull << 47 | 1ull << 28 | 4ull << 8 | 4ull << 20 | 5},
        {fmul | a1_b2 | 5, texs},
        {fmul | a1_b2 | 5, tld4 | 0xffull << 20},
        {fmul | a1_b2 | 6, 0xDF00000000070000ull | 0x10ull << 36 | 1ull << 50 | 6ull << 20 | 2ull << 8},
        {texs, fmul | 6},
        {tex, fmul | 8ull << 8 | 8ull << 20 | 6}};
    for (int i = 0; i < 7; i++) {
        code = translate(judged[i], 2, ST_FS, &info, &words, 0);
        check_translated(code, &info,
                         "a float read by FCMP, HADD2, TEXS or TLD4, or after a depth comparison, translates");
        free(code);
    }
    /* and the product as a color target's alpha (R3 of four components: the program header's word 18 is 15) */
    u64 colored[16] = {0};
    colored[9] = 15;
    colored[10] = SCHED;
    colored[11] = fmul | a1_b2 | 3;
    colored[12] = EXIT;
    ShaderInfo colored_info = {0};
    code = spirv_of(colored, sizeof colored, ST_FS, &colored_info, &words, 0, 1, 1, 1);
    check_translated(code, &colored_info, "a float product written as a color's alpha translates");
    free(code);
}

int main(int argc, char** argv) {
    g_shader_clock_scopes = 3; /* Exercise both optional Vulkan clock scopes in offline SPIR-V fixtures. */
    if (argc != 1 && argc != 2) {
        fprintf(stderr, "usage: %s [fixture directory] (got %d arguments)\n", argv[0], argc - 1);
        return 2;
    }
    if (argc == 2) {
        storage_paths[0] = fixture_path(argv[1], "storage_write");
        storage_paths[1] = fixture_path(argv[1], "storage_atomic");
        storage_paths[2] = fixture_path(argv[1], "storage_write_array");
        storage_paths[3] = fixture_path(argv[1], "storage_atomic_array");
        for (int i = 0; i < 9; i++) {
            char name[32];
            snprintf(name, sizeof name, "storage_signed_%d", i);
            storage_signed_paths[i] = fixture_path(argv[1], name);
        }
        for (int i = 0; i < 4; i++) {
            char name[32];
            snprintf(name, sizeof name, "storage_volume_atomic_%d", i);
            storage_volume_atomic_paths[i] = fixture_path(argv[1], name);
        }
        for (int i = 0; i < 8; i++) {
            char name[32];
            snprintf(name, sizeof name, "storage_layer_load_%d", i);
            storage_layer_load_paths[i] = fixture_path(argv[1], name);
        }
        static const char* nro_names[] = {"nro_raw", "nro_ms", "nro_ms_array", "nro_exchange"};
        for (int i = 0; i < 4; i++) nro_paths[i] = fixture_path(argv[1], nro_names[i]);
        char name[32];
        for (int i = 0; i < 17; i++) {
            snprintf(name, sizeof name, "issue126_%d", i);
            issue126_paths[i] = fixture_path(argv[1], name);
        }
        for (int i = 0; i < 3; i++) {
            snprintf(name, sizeof name, "stage_%d", i);
            stage_paths[i] = fixture_path(argv[1], name);
        }
        for (int i = 0; i < 2; i++) {
            snprintf(name, sizeof name, "query_%d", i);
            query_paths[i] = fixture_path(argv[1], name);
        }
        for (int i = 0; i < 3; i++) {
            snprintf(name, sizeof name, "opcode_%d", i);
            opcode_paths[i] = fixture_path(argv[1], name);
        }
        for (int i = 0; i < 3; i++) {
            snprintf(name, sizeof name, "gradient_%d", i);
            gradient_paths[i] = fixture_path(argv[1], name);
        }
        gradient_cube_path = fixture_path(argv[1], "gradient_cube_array");
        for (int i = 0; i < 10; i++) {
            snprintf(name, sizeof name, "draw_buffer_%d", i);
            draw_buffer_paths[i] = fixture_path(argv[1], name);
        }
        for (int i = 0; i < 6; i++) {
            snprintf(name, sizeof name, "texture_%d", i);
            texture_paths[i] = fixture_path(argv[1], name);
        }
        arithmetic_path = fixture_path(argv[1], "arithmetic");
        for (int i = 0; i < 3; i++) {
            snprintf(name, sizeof name, "beyond_%d", i);
            beyond_paths[i] = fixture_path(argv[1], name);
        }
        for (int i = 0; i < 7; i++) {
            snprintf(name, sizeof name, "attribute_%d", i);
            attribute_paths[i] = fixture_path(argv[1], name);
        }
        pexit_path = fixture_path(argv[1], "pexit");
        global_dynamic_path = fixture_path(argv[1], "global_dynamic");
        xfb_path = fixture_path(argv[1], "xfb");
        xfb_stores_path = fixture_path(argv[1], "xfb_stores");
        for (int i = 0; i < 6; i++) {
            char name[32];
            snprintf(name, sizeof name, "passthrough_%d", i);
            passthrough_paths[i] = fixture_path(argv[1], name);
        }
        dc_color_path = fixture_path(argv[1], "dc_color");
        for (int i = 0; i < 7; i++) {
            snprintf(name, sizeof name, "audit_%d", i);
            audit_paths[i] = fixture_path(argv[1], name);
        }
        for (int i = 0; i < 2; i++) {
            snprintf(name, sizeof name, "structured_%d", i);
            structured_paths[i] = fixture_path(argv[1], name);
        }
        fswzadd_path = fixture_path(argv[1], "fswzadd");
        saturate_path = fixture_path(argv[1], "saturate");
        cbuf_storage_path = fixture_path(argv[1], "cbuf_storage");
        for (int i = 0; i < 7; i++) {
            snprintf(name, sizeof name, "surface_op_%d", i);
            surface_op_paths[i] = fixture_path(argv[1], name);
        }
        for (int i = 0; i < 4; i++) {
            snprintf(name, sizeof name, "legacy_%d", i);
            legacy_paths[i] = fixture_path(argv[1], name);
        }
        for (int i = 0; i < 3; i++) {
            snprintf(name, sizeof name, "rect_%d", i);
            rect_paths[i] = fixture_path(argv[1], name);
        }
        for (int i = 0; i < 3; i++) {
            snprintf(name, sizeof name, "window_%d", i);
            window_paths[i] = fixture_path(argv[1], name);
        }
        for (int i = 0; i < 9; i++) {
            snprintf(name, sizeof name, "lone_lane_%d", i);
            lone_lane_paths[i] = fixture_path(argv[1], name);
        }
        if (g_fixtures.failed) {
            fprintf(stderr, "failed to allocate fixture paths\n");
            return 2;
        }
    }
    audit_shader_regressions();
    dc_color_compare();
    unfed_fragment_input();
    brx_length();
    subroutine_calls();
    invalid_ldc_bank();
    cbuf_integer_loads();
    unsupported_names_word();
    f2i_modifiers();
    i2f_by_halves();
    flag_transfers();
    global_buffers();
    compute_write_log();
    float_controls();
    precise_math();
    f2f_half_rounding();
    saturate_nan();
    hfma2_immediate();
    tlds_fp16();
    scaled_texel_fetch();
    query_only_volume();
    query_hint_words();
    query_only_volume_stages();
    txd_grad();
    fragment_storage_images();
    buffer_formatted_loads();
    immediate_sampler_handle();
    constant_buffers_past_the_limit();
    guarded_handle_merges();
    volume_surface_atomics();
    surface_atomic_operations();
    draw_buffer_surfaces();
    layered_surface_loads();
    transform_feedback();
    transform_feedback_stores();
    transform_feedback_stores_fog();
    transform_feedback_stores_ranges();
    nro_shader_modes();
    issue126_shader_modes();
    between_stages();
    rendering_stage_features(argc == 2 ? argv[1] : NULL);
    layer_capabilities();
    viewport_capabilities();
    window_positions();
    fragment_queries();
    eden_forms();
    pexit_flow();
    gradients();
    texture_forms();
    arithmetic_forms();
    beyond_eden();
    system_values();
    attribute_pointer();
    legacy_varyings_forms();
    rect_textures();
    viewport_mask();
    indexed_attribute_load();
    structured_flow();
    fswzadd_forms();
    shfl_quad_forms();
    shfl_predicate();
    lone_lane_forms();
    fragment_flow_derivatives();
    helper_loop_exit();
    feedback_own_pixel();
    feedback_screen_uv();
    texture_unread_slots();
    feedback_screen_ndc();
    vertex_screen_out();
    vertex_position_out();
    feedback_plain_uv();
    vertex_uv_out();
    fragment_position_pass();
    per_vertex_pass();
    settled_when_translated();
    alpha_function_specialized();
    window_origin_specialized();
    inputs_loaded_once_a_block();
    check(!g_fixtures.failed, "shader fixture paths allocated");
    for (size_t i = 0; i < g_fixtures.count; i++) free(g_fixtures.paths[i]);
    free(g_fixtures.paths);
    return failures ? 1 : 0;
}
