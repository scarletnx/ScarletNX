/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The mode and coverage parameters are fixed for a translated variant. All divisions select a nonzero denominator
   before evaluation, including color dodge/burn and HSL endpoints. No branch diverges inside an interlock. */
#include "nx/gpu/shader/advanced_blend_spirv.h"
#include "nx/gpu/shader/capture_tiles_spirv.h"
#include "nx/gpu/nvn/nvn.h"

typedef struct {
    SpvEmitter* e;
    u32 f, b, zero, one;
} BlendEmit;
static u32 constant(BlendEmit* c, float x) { return spv_emit_constant_f32(c->e, c->f, x); }
static u32 add(BlendEmit* c, u32 a, u32 b) { return spv_emit_binary(c->e, SPV_OP_F_ADD, c->f, a, b); }
static u32 subtract(BlendEmit* c, u32 a, u32 b) { return SPV(c->e, SPV_OP_F_SUB, c->f, a, b); }
static u32 multiply(BlendEmit* c, u32 a, u32 b) { return spv_emit_binary(c->e, SPV_OP_F_MUL, c->f, a, b); }
static u32 minimum(BlendEmit* c, u32 a, u32 b) { return SPV_EXT(c->e, c->f, SPV_GLSL_F_MIN, a, b); }
static u32 maximum(BlendEmit* c, u32 a, u32 b) { return SPV_EXT(c->e, c->f, SPV_GLSL_F_MAX, a, b); }
static u32 less(BlendEmit* c, u32 a, u32 b) { return SPV(c->e, SPV_OP_F_ORD_LESS_THAN, c->b, a, b); }
static u32 equal(BlendEmit* c, u32 a, u32 b) { return SPV(c->e, SPV_OP_F_ORD_EQUAL, c->b, a, b); }
static u32 choose(BlendEmit* c, u32 predicate, u32 yes, u32 no) {
    return spv_emit_ternary(c->e, SPV_OP_SELECT, c->f, predicate, yes, no);
}
static u32 divide(BlendEmit* c, u32 a, u32 b) {
    return spv_emit_binary(c->e, SPV_OP_F_DIV, c->f, a, choose(c, equal(c, b, c->zero), c->one, b));
}
/* OpFDiv permits reciprocal error. Preserve the exact fully covered color endpoint before equations compare it to
   one; otherwise VIVIDLIGHT can choose zero instead of one when a source color equals its nonzero alpha. */
static u32 unpremultiply(BlendEmit* c, u32 color, u32 alpha) {
    u32 endpoint = choose(c, equal(c, alpha, c->zero), c->zero, c->one);
    return choose(c, equal(c, color, alpha), endpoint, divide(c, color, alpha));
}
static u32 complement(BlendEmit* c, u32 a) { return subtract(c, c->one, a); }
static u32 clamp(BlendEmit* c, u32 a) { return minimum(c, c->one, maximum(c, c->zero, a)); }
u32 shader_blend_srgb(SpvEmitter* e, u32 f32, u32 boolean, u32 vec4, u32 color, int encode) {
    BlendEmit context = {e, f32, boolean, spv_emit_constant_f32(e, f32, 0), spv_emit_constant_f32(e, f32, 1)};
    BlendEmit* c = &context;
    u32 result[4];
    for (u32 i = 0; i < 3; i++) {
        u32 x = clamp(c, spv_emit_vec4_extract(e, f32, color, i));
        u32 low = encode ? multiply(c, constant(c, 12.92f), x) : divide(c, x, constant(c, 12.92f));
        u32 high = encode
                       ? subtract(c, multiply(c, constant(c, 1.055f),
                                              SPV_EXT(e, f32, SPV_GLSL_POW, x, constant(c, 1.0f / 2.4f))),
                                  constant(c, 0.055f))
                       : SPV_EXT(e, f32, SPV_GLSL_POW,
                                 divide(c, add(c, x, constant(c, 0.055f)), constant(c, 1.055f)), constant(c, 2.4f));
        result[i] = choose(c, less(c, constant(c, encode ? 0.0031308f : 0.04045f), x), high, low);
    }
    result[3] = spv_emit_vec4_extract(e, f32, color, 3);
    return spv_emit_vec4_construct(e, vec4, result);
}
static u32 dodge(BlendEmit* c, u32 s, u32 d) {
    return choose(c, less(c, c->zero, d),
                  choose(c, less(c, s, c->one), minimum(c, c->one, divide(c, d, complement(c, s))), c->one), c->zero);
}
static u32 burn(BlendEmit* c, u32 s, u32 d) {
    return choose(
        c, less(c, d, c->one),
        choose(c, less(c, c->zero, s), complement(c, minimum(c, c->one, divide(c, complement(c, d), s))), c->zero),
        c->one);
}
static u32 artistic(BlendEmit* c, u32 mode, u32 s, u32 d) {
    u32 two = constant(c, 2), half = constant(c, 0.5f);
    switch (mode) {
        case NVN_BLEND_ADVANCED_MODE_MULTIPLY: return multiply(c, s, d);
        case NVN_BLEND_ADVANCED_MODE_SCREEN: return subtract(c, add(c, s, d), multiply(c, s, d));
        case NVN_BLEND_ADVANCED_MODE_OVERLAY:
            return choose(c, less(c, half, d),
                          complement(c, multiply(c, two, multiply(c, complement(c, s), complement(c, d)))),
                          multiply(c, two, multiply(c, s, d)));
        case NVN_BLEND_ADVANCED_MODE_DARKEN: return minimum(c, s, d);
        case NVN_BLEND_ADVANCED_MODE_LIGHTEN: return maximum(c, s, d);
        case NVN_BLEND_ADVANCED_MODE_COLORDODGE: return dodge(c, s, d);
        case NVN_BLEND_ADVANCED_MODE_COLORBURN: return burn(c, s, d);
        case NVN_BLEND_ADVANCED_MODE_HARDLIGHT:
            return choose(c, less(c, half, s),
                          complement(c, multiply(c, two, multiply(c, complement(c, s), complement(c, d)))),
                          multiply(c, two, multiply(c, s, d)));
        case NVN_BLEND_ADVANCED_MODE_SOFTLIGHT: {
            u32 curved = multiply(
                c, d,
                add(c, multiply(c, d, subtract(c, multiply(c, constant(c, 16), d), constant(c, 12))), constant(c, 3)));
            u32 root = subtract(c, SPV_EXT(c->e, c->f, SPV_GLSL_SQRT, maximum(c, d, c->zero)), d);
            u32 upper = add(c, d,
                            multiply(c, subtract(c, multiply(c, two, s), c->one),
                                     choose(c, less(c, constant(c, 0.25f), d), root, curved)));
            u32 lower =
                subtract(c, d, multiply(c, complement(c, multiply(c, two, s)), multiply(c, d, complement(c, d))));
            return choose(c, less(c, half, s), upper, lower);
        }
        case NVN_BLEND_ADVANCED_MODE_DIFFERENCE: {
            u32 delta = subtract(c, d, s);
            return maximum(c, delta, spv_emit_unary(c->e, SPV_OP_F_NEGATE, c->f, delta));
        }
        case NVN_BLEND_ADVANCED_MODE_EXCLUSION: return subtract(c, add(c, s, d), multiply(c, two, multiply(c, s, d)));
        case NVN_BLEND_ADVANCED_MODE_INVERT: return complement(c, d);
        case NVN_BLEND_ADVANCED_MODE_INVERT_RGB: return multiply(c, s, complement(c, d));
        case NVN_BLEND_ADVANCED_MODE_LINEARDODGE: return minimum(c, c->one, add(c, s, d));
        case NVN_BLEND_ADVANCED_MODE_LINEARBURN: return maximum(c, c->zero, subtract(c, add(c, s, d), c->one));
        case NVN_BLEND_ADVANCED_MODE_VIVIDLIGHT:
            /* Vivid light's source endpoints take precedence over destination dodge/burn endpoints. */
            return choose(c, less(c, c->zero, s),
                          choose(c, less(c, s, c->one),
                                 choose(c, less(c, s, half), burn(c, multiply(c, two, s), d),
                                        dodge(c, subtract(c, multiply(c, two, s), c->one), d)),
                                 c->one),
                          c->zero);
        case NVN_BLEND_ADVANCED_MODE_LINEARLIGHT: return clamp(c, subtract(c, add(c, multiply(c, two, s), d), c->one));
        case NVN_BLEND_ADVANCED_MODE_PINLIGHT:
            return choose(c, less(c, s, half), minimum(c, d, multiply(c, two, s)),
                          maximum(c, d, subtract(c, multiply(c, two, s), c->one)));
        case NVN_BLEND_ADVANCED_MODE_HARDMIX: return choose(c, less(c, add(c, s, d), c->one), c->zero, c->one);
        default: return c->zero;
    }
}
static u32 lowest(BlendEmit* c, const u32 v[3]) { return minimum(c, minimum(c, v[0], v[1]), v[2]); }
static u32 highest(BlendEmit* c, const u32 v[3]) { return maximum(c, maximum(c, v[0], v[1]), v[2]); }
static u32 saturation(BlendEmit* c, const u32 v[3]) { return subtract(c, highest(c, v), lowest(c, v)); }
static u32 luminosity(BlendEmit* c, const u32 v[3]) {
    return add(c, add(c, multiply(c, v[0], constant(c, 0.30f)), multiply(c, v[1], constant(c, 0.59f))),
               multiply(c, v[2], constant(c, 0.11f)));
}
static void hsl(BlendEmit* c, u32 mode, const u32 s[3], const u32 d[3], u32 out[3]) {
    int source_hue = mode == NVN_BLEND_ADVANCED_MODE_HSL_HUE || mode == NVN_BLEND_ADVANCED_MODE_HSL_COLOR;
    const u32* hue = source_hue ? s : d;
    for (u32 i = 0; i < 3; i++) out[i] = hue[i];
    if (mode == NVN_BLEND_ADVANCED_MODE_HSL_HUE || mode == NVN_BLEND_ADVANCED_MODE_HSL_SATURATION) {
        u32 base = saturation(c, hue), sat = saturation(c, source_hue ? d : s), low = lowest(c, hue);
        for (u32 i = 0; i < 3; i++) out[i] = divide(c, multiply(c, subtract(c, hue[i], low), sat), base);
    }
    u32 delta = subtract(c, luminosity(c, mode == NVN_BLEND_ADVANCED_MODE_HSL_LUMINOSITY ? s : d), luminosity(c, out));
    for (u32 i = 0; i < 3; i++) out[i] = add(c, out[i], delta);
    u32 lum = luminosity(c, out), low = lowest(c, out), high = highest(c, out);
    for (u32 i = 0; i < 3; i++) {
        u32 clipped = add(c, lum, divide(c, multiply(c, subtract(c, out[i], lum), lum), subtract(c, lum, low)));
        out[i] = choose(c, less(c, low, c->zero), clipped, out[i]);
        clipped =
            add(c, lum, divide(c, multiply(c, subtract(c, out[i], lum), complement(c, lum)), subtract(c, high, lum)));
        out[i] = choose(c, less(c, c->one, high), clipped, out[i]);
    }
}

u32 shader_advanced_blend_spirv(SpvEmitter* e, u32 f32, u32 boolean, u32 vec4, u32 source, u32 destination, u32 mode,
                                int premultiplied, u32 overlap) {
    if (mode < NVN_BLEND_ADVANCED_MODE_ZERO || mode > NVN_BLEND_ADVANCED_MODE_HSL_LUMINOSITY || overlap > 2) return 0;
    BlendEmit context = {e, f32, boolean, spv_emit_constant_f32(e, f32, 0), spv_emit_constant_f32(e, f32, 1)};
    BlendEmit* c = &context;
    u32 a = spv_emit_vec4_extract(e, f32, source, 3), b = spv_emit_vec4_extract(e, f32, destination, 3);
    u32 s[4], d[4], result[4];
    for (u32 i = 0; i < 4; i++) {
        s[i] = spv_emit_vec4_extract(e, f32, source, i);
        d[i] = spv_emit_vec4_extract(e, f32, destination, i);
        if (i < 3 && !premultiplied) s[i] = multiply(c, s[i], a);
    }
    if ((mode >= NVN_BLEND_ADVANCED_MODE_PLUS && mode <= NVN_BLEND_ADVANCED_MODE_PLUS_DARKER) ||
        mode == NVN_BLEND_ADVANCED_MODE_MINUS || mode == NVN_BLEND_ADVANCED_MODE_MINUS_CLAMPED ||
        mode == NVN_BLEND_ADVANCED_MODE_CONTRAST || mode == NVN_BLEND_ADVANCED_MODE_INVERT_OVG ||
        (mode >= NVN_BLEND_ADVANCED_MODE_RED && mode <= NVN_BLEND_ADVANCED_MODE_BLUE)) {
        u32 alpha_sum = minimum(c, c->one, add(c, a, b)), half = constant(c, 0.5f), two = constant(c, 2);
        for (u32 i = 0; i < 4; i++) {
            u32 sum = add(c, s[i], d[i]);
            switch (mode) {
                case NVN_BLEND_ADVANCED_MODE_PLUS: result[i] = sum; break;
                case NVN_BLEND_ADVANCED_MODE_PLUS_CLAMPED: result[i] = minimum(c, c->one, sum); break;
                case NVN_BLEND_ADVANCED_MODE_PLUS_CLAMPED_ALPHA: result[i] = minimum(c, alpha_sum, sum); break;
                case NVN_BLEND_ADVANCED_MODE_PLUS_DARKER:
                    result[i] = i == 3 ? alpha_sum
                                       : maximum(c, c->zero, add(c, subtract(c, subtract(c, alpha_sum, a), b), sum));
                    break;
                case NVN_BLEND_ADVANCED_MODE_MINUS: result[i] = subtract(c, d[i], s[i]); break;
                case NVN_BLEND_ADVANCED_MODE_MINUS_CLAMPED:
                    result[i] = maximum(c, c->zero, subtract(c, d[i], s[i]));
                    break;
                case NVN_BLEND_ADVANCED_MODE_CONTRAST:
                    result[i] = i == 3 ? b
                                       : add(c, multiply(c, b, half),
                                             multiply(c, two,
                                                      multiply(c, subtract(c, d[i], multiply(c, b, half)),
                                                               subtract(c, s[i], multiply(c, a, half)))));
                    break;
                case NVN_BLEND_ADVANCED_MODE_INVERT_OVG:
                    result[i] = i == 3
                                    ? subtract(c, add(c, a, b), multiply(c, a, b))
                                    : add(c, multiply(c, a, complement(c, d[i])), multiply(c, complement(c, a), d[i]));
                    break;
                default: result[i] = i == mode - NVN_BLEND_ADVANCED_MODE_RED ? s[i] : d[i]; break;
            }
        }
        return spv_emit_vec4_construct(e, vec4, result);
    }
    u32 intersection = overlap == 0   ? multiply(c, a, b)
                       : overlap == 1 ? maximum(c, c->zero, subtract(c, add(c, a, b), c->one))
                                      : minimum(c, a, b);
    u32 source_only = subtract(c, a, intersection), destination_only = subtract(c, b, intersection);
    u32 sb[3], db[3], hs[3] = {0};
    for (u32 i = 0; i < 3; i++) {
        sb[i] = premultiplied ? unpremultiply(c, s[i], a) : spv_emit_vec4_extract(e, f32, source, i);
        db[i] = unpremultiply(c, d[i], b);
    }
    if (mode >= NVN_BLEND_ADVANCED_MODE_HSL_HUE) hsl(c, mode, sb, db, hs);
    int x = 1, y = 1, z = 1, selector = 0;
    switch (mode) {
        case NVN_BLEND_ADVANCED_MODE_ZERO: x = y = z = 0; break;
        case NVN_BLEND_ADVANCED_MODE_SRC:
            z = 0;
            selector = 1;
            break;
        case NVN_BLEND_ADVANCED_MODE_DST:
            y = 0;
            selector = 2;
            break;
        case NVN_BLEND_ADVANCED_MODE_SRC_OVER: selector = 1; break;
        case NVN_BLEND_ADVANCED_MODE_DST_OVER: selector = 2; break;
        case NVN_BLEND_ADVANCED_MODE_SRC_IN:
            y = z = 0;
            selector = 1;
            break;
        case NVN_BLEND_ADVANCED_MODE_DST_IN:
            y = z = 0;
            selector = 2;
            break;
        case NVN_BLEND_ADVANCED_MODE_SRC_OUT: x = z = 0; break;
        case NVN_BLEND_ADVANCED_MODE_DST_OUT: x = y = 0; break;
        case NVN_BLEND_ADVANCED_MODE_SRC_ATOP:
            y = 0;
            selector = 1;
            break;
        case NVN_BLEND_ADVANCED_MODE_DST_ATOP:
            z = 0;
            selector = 2;
            break;
        case NVN_BLEND_ADVANCED_MODE_XOR: x = 0; break;
        case NVN_BLEND_ADVANCED_MODE_INVERT:
        case NVN_BLEND_ADVANCED_MODE_INVERT_RGB: y = 0; break;
        default: break;
    }
    for (u32 i = 0; i < 3; i++) {
        u32 f;
        if (mode == NVN_BLEND_ADVANCED_MODE_HARDMIX) {
            /* Compare s/a+d/b with one before approximate division: the boundary is discontinuous. When either
               alpha is zero the intersection is zero, so this term contributes nothing. */
            u32 sum = add(c, multiply(c, s[i], b), multiply(c, d[i], a));
            f = choose(c, less(c, sum, multiply(c, a, b)), c->zero, c->one);
        } else
            f = selector == 1                             ? sb[i]
                : selector == 2                           ? db[i]
                : mode >= NVN_BLEND_ADVANCED_MODE_HSL_HUE ? hs[i]
                                                          : artistic(c, mode, sb[i], db[i]);
        result[i] = multiply(c, f, intersection);
        if (y) result[i] = add(c, result[i], multiply(c, sb[i], source_only));
        if (z) result[i] = add(c, result[i], multiply(c, db[i], destination_only));
    }
    result[3] = x ? intersection : c->zero;
    if (y) result[3] = add(c, result[3], source_only);
    if (z) result[3] = add(c, result[3], destination_only);
    return spv_emit_vec4_construct(e, vec4, result);
}

/* A capture pass writes both attachments only after the original shader's discard and native depth/stencil
   tests. A separate validity attachment preserves source alpha0 and every source RGB bit. It is cleared for
   each primitive; a whole-draw capture would lose earlier overlapping fragments. */
static void blend_resolve_sample(SpvEmitter* e, u32 f, u32 b, u32 vec4, const u32 images[3],
                                  u32 coordinate, u32 sample, u64 packed, u32 flags) {
    u32 valid = sample ? SPV(e, SPV_OP_IMAGE_READ, vec4, images[1], coordinate, 64, sample)
                       : SPV(e, SPV_OP_IMAGE_READ, vec4, images[1], coordinate);
    u32 valid_red = spv_emit_vec4_extract(e, f, valid, 0);
    u32 covered = spv_emit_unary(e, SPV_OP_LOGICAL_NOT, b,
                                 SPV(e, SPV_OP_F_ORD_EQUAL, b, valid_red, spv_emit_constant_f32(e, f, 0)));
    u32 write = spv_emit_id(e), done = spv_emit_id(e);
    spv_emit_selection_merge(e, done);
    spv_emit_branch_conditional(e, covered, write, done);
    spv_emit_label(e, write);
    u32 source = sample ? SPV(e, SPV_OP_IMAGE_READ, vec4, images[0], coordinate, 64, sample)
                        : SPV(e, SPV_OP_IMAGE_READ, vec4, images[0], coordinate);
    u32 destination = sample ? SPV(e, SPV_OP_IMAGE_READ, vec4, images[2], coordinate, 64, sample)
                             : SPV(e, SPV_OP_IMAGE_READ, vec4, images[2], coordinate);
    u32 linear = flags & SH_BLEND_SRGB ? shader_blend_srgb(e, f, b, vec4, destination, 0) : destination;
    u32 result = shader_advanced_blend_spirv(e, f, b, vec4, source, linear, (u32)(packed >> 37) & 63,
                                             (int)(packed >> 46 & 1), (u32)(packed >> 44) & 3);
    if (flags & SH_BLEND_SRGB) result = shader_blend_srgb(e, f, b, vec4, result, 1);
    if ((flags & SH_BLEND_MASK) != SH_BLEND_MASK) {
        u32 component[4];
        for (u32 i = 0; i < 4; i++)
            component[i] = spv_emit_vec4_extract(e, f, flags & (1u << i) ? result : destination, i);
        result = spv_emit_vec4_construct(e, vec4, component);
    }
    if (sample) {
        u32 operands[] = {images[2], coordinate, result, 64, sample};
        spv_emit_void_op(e, 99, operands, 5);
    } else {
        u32 operands[] = {images[2], coordinate, result};
        spv_emit_void_op(e, 99, operands, 3);
    }
    spv_emit_branch(e, done);
    spv_emit_label(e, done);
}

u32* shader_blend_resolve_spirv(u64 packed, u32 flags, size_t* words) {
    u32 mode = (u32)(packed >> 37) & 63, overlap = (u32)(packed >> 44) & 3;
    u32 samples = flags & SH_BLEND_MULTISAMPLE ? flags >> 8 & 63 : 1;
    *words = 0;
    if (mode < NVN_BLEND_ADVANCED_MODE_ZERO || mode > NVN_BLEND_ADVANCED_MODE_HSL_LUMINOSITY || overlap > 2 ||
        !samples || samples > 32 || (samples & (samples - 1)))
        return NULL;
    SpvEmitter e;
    spv_emit_init(&e, 8192);
    spv_emit_shader_capability(&e);
    spv_emit_capability(&e, 50); /* ImageQuery */
    spv_emit_capability(&e, 55); /* StorageImageReadWithoutFormat */
    spv_emit_capability(&e, 56); /* StorageImageWriteWithoutFormat */
    if (flags & SH_BLEND_MULTISAMPLE) spv_emit_capability(&e, 27); /* StorageImageMultisample */
    if ((flags & (SH_BLEND_MULTISAMPLE | SH_BLEND_ARRAY)) == (SH_BLEND_MULTISAMPLE | SH_BLEND_ARRAY))
        spv_emit_capability(&e, 48); /* ImageMSArray */
    u32 f = spv_emit_type_f32(&e), b = spv_emit_type_bool(&e), u = spv_emit_type_u32(&e);
    u32 vec4 = spv_emit_type_vec4(&e, f), uv3 = spv_emit_type_vec3(&e, u);
    int array = !!(flags & SH_BLEND_ARRAY), multisample = !!(flags & SH_BLEND_MULTISAMPLE);
    u32 coord_type = array ? uv3 : spv_emit_type_vec2(&e, u);
    u32 image_type = spv_emit_type_image(&e, f, SPV_DIM_2D, 0, array, multisample, 2, 0);
    u32 image_variable[3], images[3];
    for (u32 i = 0; i < 3; i++) {
        image_variable[i] = spv_emit_variable(&e, 0, image_type);
        u32 set = 0;
        spv_emit_decorate(&e, image_variable[i], 34, &set, 1);
        spv_emit_decorate(&e, image_variable[i], 33, &i, 1);
        spv_emit_decorate(&e, image_variable[i], i == 2 ? 23 : 24, NULL, 0); /* Coherent or NonWritable */
    }
    u32 invocation = spv_emit_variable(&e, 1, uv3), builtin = flags & SH_CAPTURE_TILED ? 26 : 28;
    spv_emit_decorate(&e, invocation, 11, &builtin, 1);
    u32 fn = spv_emit_id(&e), interfaces[6] = {invocation, image_variable[0], image_variable[1], image_variable[2]};
    u32 interface_count = 4, tile_buffer = 0, local = 0;
    if (flags & SH_CAPTURE_TILED) {
        tile_buffer = shader_capture_tiles_declare(&e, u, 0, 3);
        local = spv_emit_variable(&e, 1, uv3);
        builtin = 27; /* LocalInvocationId */
        spv_emit_decorate(&e, local, 11, &builtin, 1);
        interfaces[interface_count++] = tile_buffer;
        interfaces[interface_count++] = local;
    }
    u32 v = spv_emit_type_void(&e), ft = spv_emit_type_function_void(&e, v);
    spv_emit_compute_entry_str(&e, fn, "main", interfaces, interface_count);
    spv_emit_local_size(&e, fn, 8, 8, 1);
    spv_emit_function_void(&e, v, fn, ft);
    spv_emit_label(&e, spv_emit_id(&e));
    for (u32 i = 0; i < 3; i++) images[i] = spv_emit_load(&e, image_type, image_variable[i]);
    u32 xyz = spv_emit_load(&e, uv3, invocation);
    if (flags & SH_CAPTURE_TILED)
        xyz = shader_capture_tile_coordinates(&e, u, tile_buffer, xyz,
                                               spv_emit_load(&e, uv3, local), !!(flags & SH_CAPTURE_ADAPTIVE));
    u32 size = SPV(&e, 104, coord_type, images[2]); /* OpImageQuerySize, no Lod for storage images */
    u32 inside = 0, coordinates[3];
    for (u32 i = 0; i < (array ? 3u : 2u); i++) {
        coordinates[i] = spv_emit_vec4_extract(&e, u, xyz, i);
        u32 limit = spv_emit_vec4_extract(&e, u, size, i);
        u32 fits = SPV(&e, SPV_OP_U_LESS_THAN, b, coordinates[i], limit);
        inside = i ? SPV(&e, SPV_OP_LOGICAL_AND, b, inside, fits) : fits;
    }
    u32 coordinate = array ? xyz : SPV(&e, SPV_OP_COMPOSITE_CONSTRUCT, coord_type, coordinates[0], coordinates[1]);
    u32 apply = spv_emit_id(&e), done = spv_emit_id(&e);
    spv_emit_selection_merge(&e, done);
    spv_emit_branch_conditional(&e, inside, apply, done);
    spv_emit_label(&e, apply);
    for (u32 sample = 0; sample < samples; sample++)
        blend_resolve_sample(&e, f, b, vec4, images, coordinate,
                              multisample ? spv_emit_constant_u32(&e, u, sample) : 0, packed, flags);
    spv_emit_branch(&e, done);
    spv_emit_label(&e, done);
    spv_emit_return(&e);
    spv_emit_function_end(&e);
    u32* code = spv_emit_finish(&e, words);
    spv_emit_destroy(&e);
    return code;
}
