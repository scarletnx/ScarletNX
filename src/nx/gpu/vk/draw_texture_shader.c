/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "nx/gpu/vk/draw_texture_shader.h"
#include "nx/gpu/shader/advanced_blend_spirv.h"
#include "common/utilities/spirv_emit.h"
#include "common/utilities/bits.h"
#include "common/utilities/endian.h"
#include <stdlib.h>
#include <string.h>

static u64 draw_texture_hash(int stage, const u32 mask[26]) {
    return snx_fnv1a(snx_fnv_step(SNX_FNV_BASIS, (u32)stage), mask, 26 * sizeof *mask);
}
static VkSh* register_shader(int stage, const u32 mask[26], u32* code, size_t words, const ShaderInfo* info) {
    if (!code || !words) { free(code); return NULL; }
    u64 hash = draw_texture_hash(stage, mask);
    VkSh* shader = vk_shader_generated_find(hash, stage, mask);
    if (shader) { free(code); return shader; }
    shader = calloc(1, sizeof *shader);
    if (!shader) { free(code); return NULL; }
    shader->hash = hash;
    shader->stage = stage;
    shader->si = *info;
    memcpy(shader->mask, mask, sizeof shader->mask);
    VkShaderModuleCreateInfo create = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                       .codeSize = words * sizeof *code, .pCode = code};
    if (!VK_OK(vkCreateShaderModule(g_vk.dev, &create, NULL, &shader->mod))) {
        free(shader); free(code); return NULL;
    }
    vk_shader_generated_record(shader, code, words, NULL, 0);
    free(code);
    return shader;
}
VkSh* vk_draw_texture_vertex(void) {
    /* A fixed identity key also finds a module loaded from this build's pipeline file. */
    u32 mask[26] = {0x44545653, 1};
    u64 hash = draw_texture_hash(ST_VS, mask);
    VkSh* cached = vk_shader_generated_find(hash, ST_VS, mask);
    if (cached) return cached;
    SpvEmitter e;
    spv_emit_init(&e, 512);
    spv_emit_shader_capability(&e);
    u32 f32 = spv_emit_type_f32(&e), u32_type = spv_emit_type_u32(&e);
    u32 vec4 = spv_emit_type_vector(&e, f32, 4), pointer;
    u32 boolean = spv_emit_type_bool(&e);
    u32 push = spv_emit_push_vec4(&e, vec4, f32, 80, vec4, &pointer);
    u32 vertex = spv_emit_builtin_input_u32(&e, u32_type, 42);
    u32 position = spv_emit_position_output(&e, vec4), uv = spv_emit_output_vec4(&e, vec4, 0);
    u32 interfaces[] = {push, vertex, position, uv};
    u32 void_type = spv_emit_type_void(&e), fn_type = spv_emit_type_function_void(&e, void_type);
    u32 fn = spv_emit_id(&e);
    spv_emit_entry_str(&e, ST_VS, fn, "main", interfaces, 4);
    spv_emit_function_void(&e, void_type, fn, fn_type);
    spv_emit_label(&e, spv_emit_id(&e));
    u32 index = spv_emit_load(&e, u32_type, vertex);
    u32 one = spv_emit_constant_u32(&e, u32_type, 1);
    u32 component[2] = {spv_emit_bitwise_and_u32(&e, u32_type, index, one),
                        spv_emit_shift_right_u32(&e, u32_type, index, one)};
    u32 output[2][4];
    for (u32 member = 0; member < 2; member++) {
        for (u32 axis = 0; axis < 2; axis++) {
            u32 low = spv_emit_push_component_f32(&e, pointer, push,
                spv_emit_constant_u32(&e, u32_type, member),
                spv_emit_constant_u32(&e, u32_type, axis), f32);
            u32 high = spv_emit_push_component_f32(&e, pointer, push,
                spv_emit_constant_u32(&e, u32_type, member),
                spv_emit_constant_u32(&e, u32_type, axis + 2), f32);
            u32 select_high = spv_emit_i_equal(&e, boolean, component[axis], one);
            output[member][axis] = spv_emit_select(&e, f32, select_high, high, low);
        }
        output[member][2] = spv_emit_constant_f32(&e, f32, 0);
        output[member][3] = spv_emit_constant_f32(&e, f32, 1);
    }
    spv_emit_store(&e, position, spv_emit_vec4_construct(&e, vec4, output[0]));
    spv_emit_store(&e, uv, spv_emit_vec4_construct(&e, vec4, output[1]));
    spv_emit_return(&e); spv_emit_function_end(&e);
    size_t words;
    u32* code = spv_emit_finish(&e, &words);
    spv_emit_destroy(&e);
    ShaderInfo info = {.out_mask = 1};
    return register_shader(ST_VS, mask, code, words, &info);
}
static int no_constants(u32 bank, u32 offset, u32* value) {
    (void)bank; (void)offset; *value = 0; return 0;
}
VkSh* vk_draw_texture_fragment(u32 texture_int, u32 texture_signed, u32 target_int, u32 target_signed,
                                u64 advanced_blend, u32 advanced_mask) {
    u32 mask[26] = {texture_int, 0, texture_signed, 0, target_int, target_signed};
    mask[6] = 0x44544653;
    mask[21] = (u32)advanced_blend; mask[22] = (u32)(advanced_blend >> 32); mask[23] = advanced_mask;
    VkSh* cached = vk_shader_generated_find(draw_texture_hash(ST_FS, mask), ST_FS, mask);
    if (cached) return cached;
    /* The program only samples one normalized 2D texture. The normal translator provides alpha tests and ordered
       blending, so DrawTexture has the same per-fragment operations as a guest draw. */
    u8 code[160] = {0};
    snx_st_le64(code + 24, 0xff); /* screen-linear coordinates */
    snx_st_le64(code + 72, 15); /* RGBA output */
    const u64 operations[] = {
        0xe000000000070000ull | 0x20ull << 30,
        0xe000000000070000ull | 0x21ull << 30 | 1,
        0xc000000000000000ull | 1ull << 55 | 7ull << 51 | 15ull << 31 | 2ull << 28 |
            0xffull << 20 | 7ull << 16,
        0xe30000000007000full};
    u32 at = 10;
    for (u32 i = 0; i < 4; i++) {
        if (!(i % 3)) snx_st_le64(code + 8 * at++, 0x001f8000fc0007e0ull);
        snx_st_le64(code + 8 * at++, operations[i]);
    }
    ShaderInfo info = {.tex_int_mask = texture_int, .tex_sint_mask = texture_signed,
                        .rt_int_mask = target_int, .rt_sint_mask = target_signed,
                        .advanced_blend = advanced_blend, .advanced_blend_mask = advanced_mask,
                        .coverage_to_color = (advanced_mask & SH_COVERAGE_TO_COLOR)
                            ? 1 + ((advanced_mask >> SH_COVERAGE_OUTPUT_SHIFT) & 7) : 0};
    info.coverage_early_tests = info.coverage_to_color && (advanced_mask & SH_BLEND_EARLY_TESTS);
    VkCbReads reads = {0};
    size_t words = 0;
    u32* spirv = vk_translate_spirv(code, sizeof code, ST_FS, &info, &words, no_constants, &reads);
    free(reads.r);
    return register_shader(ST_FS, mask, spirv, words, &info);
}
