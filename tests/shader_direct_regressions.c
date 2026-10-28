/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "nx/gpu/shader/shader.h"
#include "common/utilities/endian.h"
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
static const char* audit_paths[6];
static const char* issue126_paths[17];
static const char* stage_paths[3];
static const char* query_paths[2];
static const char* opcode_paths[3];
static const char* gradient_paths[3];
static const char* texture_paths[6];
static const char* arithmetic_path;
static const char* beyond_paths[3];
static const char* attribute_paths[7];
static const char* pexit_path;
static const char* global_dynamic_path;
static const char* xfb_path;
static const char* passthrough_paths[6];
static const char* structured_paths[2];
static const char* fswzadd_path;
static char* fixture_paths[93];
static size_t fixture_count;

static const char* fixture_path(const char* directory, const char* name) {
    size_t dir_len = strlen(directory), name_len = strlen(name);
    if (fixture_count == sizeof fixture_paths / sizeof *fixture_paths || dir_len > SIZE_MAX - name_len - 6) return NULL;
    char* path = malloc(dir_len + name_len + 6);
    if (!path) return NULL;
    snprintf(path, dir_len + name_len + 6, "%s/%s.spv", directory, name);
    fixture_paths[fixture_count++] = path;
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
static u32* spirv_of(const u64* code, size_t bytes, int stage, ShaderInfo* info, size_t* words, int flush,
                     int can_preserve, int can_preserve_f64, int can_subgroup) {
    u8* guest = guest_bytes(code, bytes);
    u32* result = guest ? shader_to_spirv(guest, (u32)bytes, stage, info, words, flush, can_preserve,
                                          can_preserve_f64, can_subgroup)
                        : NULL;
    free(guest);
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
    u32 at = opcode == 95 ? 5 : 6;
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

static int emit_only_on_zero_stream(const u32* code, size_t words) {
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
        if ((code[i] & 0xffff) == 250 && width == 4 && code[i + 3] == emit_label && code[i + 2] != emit_label) return 1;
        i += width;
    }
    return 0;
}

static int is_constant_value(const u32* code, size_t words, u32 id, u32 value);

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

static int seeded_gradient_component(const u32* code, size_t words, u32 seed, u32 gradient, u32 component) {
    u32 values[64];
    size_t nvalues = seeded_values(code, words, seed, values, 64);
    for (size_t v = 0; v < nvalues; v++) {
        u32 loaded = values[v];
        for (size_t j = 5; j < words;) {
            u32 span = code[j] >> 16;
            if (!span || j + span > words) return 0;
            if ((code[j] & 0xffff) == 124 && span == 4 && code[j + 3] == loaded) {
                u32 value = code[j + 2];
                for (size_t k = 5; k < words;) {
                    u32 construct_span = code[k] >> 16;
                    if (!construct_span || k + construct_span > words) return 0;
                    if ((code[k] & 0xffff) == 80 && construct_span > 3 + component &&
                        code[k + 3 + component] == value) {
                        u32 vector = code[k + 2];
                        for (size_t m = 5; m < words;) {
                            u32 sample_span = code[m] >> 16;
                            if (!sample_span || m + sample_span > words) return 0;
                            if ((code[m] & 0xffff) == 88 && sample_span > 6 + gradient && code[m + 5] == 4 &&
                                code[m + 6 + gradient] == vector)
                                return 1;
                            m += sample_span;
                        }
                    }
                    k += construct_span;
                }
            }
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

static u64 mov32i(u32 reg, u32 value) { return 0x0100000000000000ull | ((u64)value << 20) | 0x71000ull | reg; }

/* R2P and P2R transfer the condition flags too (bit 40): the flags are variables of their own (z, s, c, o), read
   and written by their VAR_FLAG + p index, not by their variable id. A program that doesn't also use registers at
   those ids left R2P loading id 0, invalid SPIR-V that radv crashed on while making the pipeline. */
static void flag_transfers(void) {
    const u64 program[] = {
        mov32i(5, 15),                                                   /* r5 = 15: the flags' bits */
        mov32i(6, 15),                                                   /* r6 = 15: the mask */
        R2P_R | (1ull << 40) | (5ull << 8) | (6ull << 20),               /* R2P: z,s,c,o = r5 & r6 */
        P2R_R | (1ull << 40) | 7 | (5ull << 8) | (6ull << 20),           /* P2R: r7 = r5 with the flags */
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
                check(info.fp32_flush == (i == 0 || i == 1) && info.fp32_preserve == (i != 0),
                      "FP32 modes recorded");
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
            code = translate_with_rt_masks(instructions, 2, signed_stages[stage], &alternate, &words, 1,
                                           0, 1u << 8);
            check_translated(code, &alternate, "signed surface store translates");
            if (code) {
                check(alternate.nimg == 1 && alternate.img[0].kind == 3 &&
                          has_storage_image_format(code, words, 0),
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

static void volume_surface_atomics(void) {
    const u64 operations[] = {0xea40020a00870009ull, 0xea48020a20870009ull,
                               0xeb50020a00070008ull, 0xeb50020a01170008ull};
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
    const u64 refused[] = {operations[0] | 1ull << 49, operations[0] | 3ull << 29,
                           operations[0] | 2ull << 51, operations[2] | 1ull << 23};
    for (u32 i = 0; i < 4; i++) {
        const u64 instructions[] = {0x4c98078804a70004ull, refused[i]};
        ShaderInfo info;
        size_t words = 0;
        u32* code = translate(instructions, 2, ST_CS, &info, &words, 1);
        check_refused(code, &info, i == 3 ? "SURED" : "SUATOM",
                      "unimplemented 3D atomic widths, clamp and wrap modes remain rejected");
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
                    check(info.ntex == 1 && info.tex[0].type == (dim == 5 ? 2 : 1) &&
                              info.tex[0].array == (dim == 4) && (info.tex[0].flags & 1) == raw,
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
    /* a 2D texture offset from a register the translator can't resolve: the coordinates move instead */
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
                 {dynamic_3d_offset, 2, ST_FS, 0, 103},
                 {split_cbuf_handle, 3, ST_FS, 0, 88},
                 {handle_past_writers, 5, ST_FS, 0, 88},
                 {buffer_surfaces, 4, ST_CS, 0, 98},
                 {bindless_query, 3, ST_FS, 0, 103},
                 {clip_flow, 2, ST_VS, 1, 65},
                 {dynamic_2d_offset, 2, ST_FS, 0, 103},
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
        if (code)
            check(count_opcode(code, words, 195, 0xffffffffu) == 0, "a 2D texel fetch keeps its coordinates");
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
    check(!code && info.unsupported && !strcmp(info.first_unsupported, "untracked texture handle"),
          "a handle overwritten by a texture result is not tracked");
    free(code);
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
    const u64 joined[] = {choose, 0x4c98078800070006ull, pair_or, merge,
                          0x4c98078800070006ull, pair_or, tex_b};
    code = translate(joined, 7, ST_FS, &info, &words, 1);
    check_translated(code, &info, "equal split handles merge across a forward branch");
    check(code && info.ntex == 1 && info.tex[0].bl2 && info.tex[0].bl_cbuf == 2 &&
              info.tex[0].cbuf_off == 0 && info.tex[0].bl2_cbuf == 2 && info.tex[0].bl2_off == 0x100,
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
        check(code && info.ntex == 1 && info.tex[0].bindless && info.tex[0].bl_cbuf == 2 &&
                  info.tex[0].cbuf_off == 0,
              "the surviving lanes retain the original handle source");
        free(code);
        const u64 store = 0xeb20000a00f70c10ull | (6ull << 39);
        const u64 store_after_exit[] = {0x4c98078800070006ull, conditional_exits[i], store, FMUL_KEEP | 5, back};
        code = translate(store_after_exit, 5, ST_VS, &info, &words, 1);
        check_translated(code, &info, "a vertex image-store handle survives a conditional exit before a loop");
        check(code && info.nimg == 1 && info.img[0].bindless && info.img[0].bl_cbuf == 2 &&
                  info.img[0].cbuf_off == 0 && info.img[0].type == 5,
              "the vertex loop retains its 3D storage image descriptor");
        free(code);
        const u64 overwritten[] = {0x4c98078800070006ull, conditional_exits[i], store, FMUL_KEEP | 6, back};
        code = translate(overwritten, 5, ST_VS, &info, &words, 1);
        check_refused(code, &info, "untracked surface handle",
                      "a conditional exit does not hide a handle overwritten on the loop back edge");
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
    const u64 renamed_guard[] = {load_p0, save_p0, fsetp_p0, (restore_p0 & ~63ull) | 15,
                                 pred_tex | (1ull << 16)};
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
        const u64 invocation[] = {0xf0c8000001d70000ull, /* S2R R0, SR_INVOCATION_INFO */
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

static void viewport_capabilities(void)
{
	const u64 write = 0xeff07f800687ff00ull; /* AST a[0x68], R0 */
	const u32 geometry_sph[3] = {1u << 24, 7u << 24, 3};
	const u32 none[3] = {0};
	int stages[3] = {ST_VS, ST_TES, ST_GS};

	for (int i = 0; i < 3; i++) {
		ShaderInfo info = {0};
		size_t words = 0;
		u32* code = translate_stage(&write, 1, stages[i], i == 2 ? geometry_sph : none, i == 2 ? 3 : 0,
					    &info, &words);
		check_translated(code, &info, "viewport index stores translate in vertex, evaluation and geometry stages");
		if (code) {
			check(info.writes_viewport, "viewport writes request indexed dynamic state");
			check(has_builtin(code, words, 10), "viewport index has its own scalar output");
			check(!has_builtin(code, words, 0), "viewport index is not a position component");
			check(count_opcode(code, words, 17, i == 2 ? 57 : 70) == 1, "viewport output declares the stage capability");
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
        check(count_opcode(code, words, 218, 0xffffffffu) == 1, "only the stream that may be 0 emits a vertex");
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
    const u64 cbuf_flow[] = {0xe26000000007002full, 0xe27000000007002full, 0xe29000000007002full, 0xe2a000000007002full,
                             0xe2b000000007002full}; /* CAL, PRET, SSY, PBK, PCNT */
    for (size_t i = 0; i < sizeof cbuf_flow / sizeof *cbuf_flow; i++) {
        code = translate(&cbuf_flow[i], 1, ST_FS, &info, &words, 0);
        check_refused(code, &info,
                      i < 2    ? i ? "PRET" : "CAL"
                      : i == 2 ? "SSY"
                      : i == 3 ? "PBK"
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
            info.gp_skip_mask[1] &= ~5u; /* only x and z of attribute 0 */
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
        program[12] = EXIT & ~(7ull << 16); /* conditional exit */
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

/* TXD: a 2D texture with a constant offset, TXD.B on a 2D array (the handle in Ra), and a 1D array whose offset
   isn't known, which moves the coordinates */
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
            check(count_opcode(code, words, 88, 0xffffffffu) == 1, "TXD is an explicit-LOD sample with gradients");
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
    u64 invalid_cube[] = {array_2d[0], (array_2d[1] & ~(7ull << 28)) | 7ull << 28};
    ShaderInfo info;
    size_t words = 0;
    u32* code = translate(invalid_cube, 2, ST_FS, &info, &words, 0);
    check_refused(code, &info, "TXD", "TXD.B refuses cube arrays");
    free(code);
}

/* Texture forms compared with Eden's: TEXS's depth compares with a level (5, 6, 9), TLDS's multisample fetch with
   half results, TLD's multisample fetch in the bound form and its offsets on a 3D texture, TLD4's offset on a 1D
   texture, and TEX's LOD clamp, which is refused */
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
        check_refused(code, &info, "TEX", "TEX with the LOD clamp is refused");
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
        u32* code = translate(&clamp_forms[i], 1, ST_FS, &info, &words, 0);
        check_refused(code, &info, clamp_names[i], "LOD clamp form is refused");
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
        seed_stores += (code[i] & 0xffff) == 62 && (code[i] >> 16) == 3 && is_constant_value(code, words, code[i + 2], 3);
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
        check(has_builtin(code, words, 18) && !has_builtin(code, words, 20),
              "PIXLD.MYINDEX reads SampleId rather than SampleMask");
    free(code);
    code = translate(&pixld_coverage, 1, ST_FS, &info, &words, 0);
    check_translated(code, &info, "PIXLD.COVMASK translates");
    if (code)
        check(has_builtin(code, words, 20) && !has_builtin(code, words, 18),
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
        0xe003ff870ff7ff00ull, 0x5cb0118000070a00ull, 0x3848000000470000ull,
        0xef94002000070004ull, 0x5c9807800ff70000ull, 0x5c9807800ff70001ull,
        0xdeb8000000070008ull | (2ull << 28) | (1ull << 37) | (4ull << 20) |
            (1ull << 50) | (1ull << 31),
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
    variant[6] |= 1ull << 36; /* register offset: dimensions must come from the selected array */
    code = translate(variant, 7, ST_FS, &info, &words, 1);
    check_translated(code, &info, "indexed shadow dynamic offset translates");
    if (code) {
        check(count_opcode(code, words, 103, 0xffffffffu) == 2,
              "indexed shadow queries dimensions of both descriptor arrays");
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
        check(count_opcode(code, words, 130, 0xffffffffu) == 1,
              "instance subtracts the original guest base");
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
    for (size_t i = 0; i < sizeof words / sizeof *words; i++) {
        ShaderInfo info;
        size_t size = 0;
        u32* code = translate(&words[i], 1, ST_FS, &info, &size, 1);
        if (!code || info.unsupported) fprintf(stderr, "tlds fp16 %zu: %s\n", i, info.first_unsupported);
        check(code && !info.unsupported, "TLDS fp16 translates");
        if (code) check(count_opcode(code, size, 115, 0xffffffffu) >= 1, "TLDS fp16 converts to halves");
        free(code);
    }
}

/* fragment TLD and TXQ of a scaled target take the scale its texture's bit selects (issue #730) */
static void scaled_texel_fetch(void) {
    static const u64 tld_2d[] = {0xdc00000000000000ull | 7ull << 51 | 0xfull << 31 | 2ull << 28 | 0x10ull << 36 |
                                 0xffull << 20 | 7ull << 16 | 2ull << 8}; /* TLD.LZ R0, R2, RZ, 2D */
    /* TXQ.B: the handle in R4, the level in R5 */
    static const u64 txq_2d[] = {HANDLE_CBUF_R4, 0x5c9807800ff70005ull, 0xdf50000180470400ull};
    ShaderInfo info;
    size_t words = 0;
    u32* code = translate(tld_2d, 1, ST_FS, &info, &words, 0);
    check_translated(code, &info, "fragment TLD 2D translates");
    if (code)
        check(count_opcode(code, words, 111, 0xffffffffu) == 2 && count_opcode(code, words, 110, 0xffffffffu) == 2 &&
                  count_opcode(code, words, 169, 0xffffffffu) == 1,
              "fragment TLD 2D takes x and y at the scale its texture's bit selects");
    free(code);
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
        if (code) check(count_opcode(code, words, 169, 0xffffffffu) == 0, "one FSWZADD mode for all lanes selects nothing");
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
        for (int i = 0; i < 6; i++) {
            char name[32];
            snprintf(name, sizeof name, "passthrough_%d", i);
            passthrough_paths[i] = fixture_path(argv[1], name);
        }
        dc_color_path = fixture_path(argv[1], "dc_color");
        for (int i = 0; i < 6; i++) {
            snprintf(name, sizeof name, "audit_%d", i);
            audit_paths[i] = fixture_path(argv[1], name);
        }
        for (int i = 0; i < 2; i++) {
            snprintf(name, sizeof name, "structured_%d", i);
            structured_paths[i] = fixture_path(argv[1], name);
        }
        fswzadd_path = fixture_path(argv[1], "fswzadd");
        if (fixture_count != 93) {
            fprintf(stderr, "failed to allocate fixture paths\n");
            return 2;
        }
    }
    audit_shader_regressions();
    dc_color_compare();
    unfed_fragment_input();
    brx_length();
    invalid_ldc_bank();
    cbuf_integer_loads();
    unsupported_names_word();
    f2i_modifiers();
    flag_transfers();
    global_buffers();
    float_controls();
    precise_math();
    f2f_half_rounding();
    hfma2_immediate();
    tlds_fp16();
    scaled_texel_fetch();
    txd_grad();
    fragment_storage_images();
    volume_surface_atomics();
    layered_surface_loads();
    transform_feedback();
    nro_shader_modes();
    issue126_shader_modes();
    between_stages();
    layer_capabilities();
    viewport_capabilities();
    fragment_queries();
    eden_forms();
    pexit_flow();
    gradients();
    texture_forms();
    arithmetic_forms();
    beyond_eden();
    system_values();
    attribute_pointer();
    indexed_attribute_load();
    structured_flow();
    fswzadd_forms();
    shfl_quad_forms();
    shfl_predicate();
    for (size_t i = 0; i < fixture_count; i++) free(fixture_paths[i]);
    return failures ? 1 : 0;
}
