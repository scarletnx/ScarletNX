// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// The PICA200's texture environment as one fragment shader that reads its stages from push constants: what a draw
// uses while the shader generated for its exact state (fsgen.c) is still being built, so that the draw is not
// skipped. It follows fsgen.c's stages, rounding and depth, which follow Azahar's GLSL generator (GPL-2.0-or-later);
// what it leaves out (lighting, fog, procedural textures, shadows, cube and shadow textures on unit 0) it
// approximates, as a draw that is replaced within a few frames.
#version 450

layout(location = 0) in vec4 primary_color;
layout(location = 1) in vec2 texcoord0;
layout(location = 2) in vec2 texcoord1;
layout(location = 3) in vec2 texcoord2;
layout(location = 4) in float texcoord0_w;
layout(location = 0) out vec4 color;

// fs_data, std140, at SnxCtrFsUniforms' offsets (fsgen.h)
layout(std140, set = 0, binding = 2) uniform FsData {
    layout(offset = 4) int alphatest_ref;
    layout(offset = 8) float depth_scale;
    layout(offset = 12) float depth_offset;
    layout(offset = 24) int scissor_x1;
    layout(offset = 28) int scissor_y1;
    layout(offset = 32) int scissor_x2;
    layout(offset = 36) int scissor_y2;
    layout(offset = 1136) vec4 const_color[6];
    layout(offset = 1232) vec4 tev_combiner_buffer_color;
    layout(offset = 1248) vec4 tex_lod_bias;
    layout(offset = 1264) vec4 tex_border_color[3];
};
layout(set = 1, binding = 0) uniform sampler2D tex0;
layout(set = 1, binding = 1) uniform sampler2D tex1;
layout(set = 1, binding = 2) uniform sampler2D tex2;

// the stages of SnxCtrFsConfig (the registers' words), and its other switches in flags and borders:
// flags: alpha test function (bits 0-2), texture 0 type (3-5), texture 2 uses coordinate 1 (6), combiner buffer input
// (8-15), W-buffering (16), scissor mode (17-18), lighting on (19); borders: s clamp-to-border per unit (0-2), t (3-5)
layout(push_constant) uniform Config {
    uint sources[6], modifiers[6], ops[6], scales[6];
    uint flags, borders;
};

const uint TEX_PROJECTION = 3u, TEX_DISABLED = 5u;

vec4 byteround(vec4 x) { return round(x * 255.0) / 255.0; }
vec3 byteround(vec3 x) { return round(x * 255.0) / 255.0; }
float byteround(float x) { return round(x * 255.0) / 255.0; }
uint field(uint word, uint shift, uint bits) { return (word >> shift) & ((1u << bits) - 1u); }
float multiplier(uint scale) { return scale < 3u ? float(1u << scale) : 1.0; }

vec4 sample_unit(sampler2D t, vec2 uv, uint unit, bool border_s, bool border_t) {
    vec2 scaled = uv * vec2(textureSize(t, 0));
    vec2 d = max(abs(dFdx(scaled)), abs(dFdy(scaled)));
    vec4 sampled = textureLod(t, uv, log2(max(d.x, d.y)) + tex_lod_bias[unit]);
    bool outside = (border_s && (uv.x < 0.0 || uv.x > 1.0)) || (border_t && (uv.y < 0.0 || uv.y > 1.0));
    return outside ? tex_border_color[unit] : sampled;
}

vec4 rounded_primary, primary_fragment, secondary_fragment, units[4], combiner_buffer, combiner_output;

vec4 source(uint src, uint index) {
    switch (src) {
        case 0u: return rounded_primary;
        case 1u: return primary_fragment;
        case 2u: return secondary_fragment;
        case 3u: case 4u: case 5u: case 6u: return units[src - 3u];
        case 13u: return combiner_buffer;
        case 14u: return const_color[index];
        case 15u: return combiner_output;
        default: return vec4(0.0);
    }
}
// AppendColorModifier: the operand's channels (rgb, or one in all three, or nothing), then one minus it
vec3 color_modifier(uint modifier, uint src, uint index) {
    if (index == 0u && src == 15u) src = field(sources[0], 8u, 4u); // stage 0 reads source 3 for previous
    vec4 v = source(src, index);
    vec3 x;
    switch (modifier & 15u) {
        case 0u: case 1u: x = v.rgb; break;
        case 2u: case 3u: x = vec3(v.a); break;
        case 4u: case 5u: x = vec3(v.r); break;
        case 8u: case 9u: x = vec3(v.g); break;
        case 12u: case 13u: x = vec3(v.b); break;
        default: return vec3(0.0);
    }
    return (modifier & 1u) != 0u ? vec3(1.0) - x : x;
}
// AppendAlphaModifier
float alpha_modifier(uint modifier, uint src, uint index) {
    if (index == 0u && src == 15u) src = field(sources[0], 24u, 4u);
    vec4 v = source(src, index);
    float x = (modifier >> 1 & 3u) == 0u ? v.a : (modifier >> 1 & 3u) == 1u ? v.r : (modifier >> 1 & 3u) == 2u ? v.g : v.b;
    return (modifier & 1u) != 0u ? 1.0 - x : x;
}
vec3 combine(uint op, vec3 r1, vec3 r2, vec3 r3) {
    vec3 x;
    switch (op) {
        case 0u: x = r1; break;
        case 1u: x = r1 * r2; break;
        case 2u: x = r1 + r2; break;
        case 3u: x = r1 + r2 - 0.5; break;
        case 4u: x = mix(r2, r1, r3); break;
        case 5u: x = r1 - r2; break;
        case 6u: case 7u: x = vec3(dot(r1 - 0.5, r2 - 0.5) * 4.0); break;
        case 8u: x = fma(r1, r2, r3); break;
        case 9u: x = min(r1 + r2, 1.0) * r3; break;
        default: x = vec3(0.0); break;
    }
    return clamp(x, 0.0, 1.0);
}
float combine(uint op, float r1, float r2, float r3) {
    float x;
    switch (op) {
        case 0u: x = r1; break;
        case 1u: x = r1 * r2; break;
        case 2u: x = r1 + r2; break;
        case 3u: x = r1 + r2 - 0.5; break;
        case 4u: x = mix(r2, r1, r3); break;
        case 5u: x = r1 - r2; break;
        case 8u: x = fma(r1, r2, r3); break;
        case 9u: x = min(r1 + r2, 1.0) * r3; break;
        default: x = 0.0; break;
    }
    return clamp(x, 0.0, 1.0);
}

void main() {
    uint alpha_func = field(flags, 0u, 3u), type0 = field(flags, 3u, 3u);
    if (alpha_func == 0u) discard; // never passes
    // the units are sampled where every invocation runs, for their derivatives
    vec2 uv2 = field(flags, 6u, 1u) != 0u ? texcoord1 : texcoord2;
    units[0] = type0 == TEX_DISABLED ? vec4(0.0)
             : type0 == TEX_PROJECTION ? textureProj(tex0, vec3(texcoord0, texcoord0_w))
             : sample_unit(tex0, texcoord0, 0u, (borders & 1u) != 0u, (borders & 8u) != 0u);
    units[1] = sample_unit(tex1, texcoord1, 1u, (borders & 2u) != 0u, (borders & 16u) != 0u);
    units[2] = sample_unit(tex2, uv2, 2u, (borders & 4u) != 0u, (borders & 32u) != 0u);
    units[3] = vec4(0.0); // the procedural texture is not drawn here
    uint scissor = field(flags, 17u, 2u);
    if (scissor != 0u) {
        bool inside = gl_FragCoord.x >= float(scissor_x1) && gl_FragCoord.y >= float(scissor_y1) &&
                      gl_FragCoord.x < float(scissor_x2) && gl_FragCoord.y < float(scissor_y2);
        if (scissor == 3u ? !inside : inside) discard;
    }
    float depth = -gl_FragCoord.z * depth_scale + depth_offset;
    if (field(flags, 16u, 1u) == 0u) depth /= gl_FragCoord.w;
    rounded_primary = byteround(primary_color);
    // no lighting here: a lit surface shows its vertex colour
    primary_fragment = field(flags, 19u, 1u) != 0u ? clamp(primary_color, 0.0, 1.0) : vec4(0.0);
    secondary_fragment = vec4(0.0);
    uint buffer_input = field(flags, 8u, 8u);
    combiner_buffer = vec4(0.0);
    vec4 next_buffer = tev_combiner_buffer_color;
    combiner_output = vec4(0.0);
    for (uint i = 0u; i < 6u; ++i) {
        uint s = sources[i], m = modifiers[i], o = ops[i], k = scales[i];
        bool pass = field(o, 0u, 4u) == 0u && field(o, 16u, 4u) == 0u && field(s, 0u, 4u) == 15u &&
                    field(s, 16u, 4u) == 15u && field(m, 0u, 4u) == 0u && field(m, 12u, 3u) == 0u &&
                    multiplier(field(k, 0u, 2u)) == 1.0 && multiplier(field(k, 16u, 2u)) == 1.0;
        if (!pass) {
            uint color_op = field(o, 0u, 4u);
            vec3 r1 = color_modifier(field(m, 0u, 4u), field(s, 0u, 4u), i);
            vec3 r2 = color_modifier(field(m, 4u, 4u), field(s, 4u, 4u), i);
            vec3 r3 = color_modifier(field(m, 8u, 4u), field(s, 8u, 4u), i);
            vec3 color_out = byteround(combine(color_op, r1, r2, r3));
            float alpha_out;
            if (color_op == 7u)
                alpha_out = color_out.r; // Dot3_RGBA puts the result in alpha too
            else {
                float a1 = alpha_modifier(field(m, 12u, 3u), field(s, 16u, 4u), i);
                float a2 = alpha_modifier(field(m, 16u, 3u), field(s, 20u, 4u), i);
                float a3 = alpha_modifier(field(m, 20u, 3u), field(s, 24u, 4u), i);
                alpha_out = byteround(combine(field(o, 16u, 4u), a1, a2, a3));
            }
            combiner_output = vec4(clamp(color_out * multiplier(field(k, 0u, 2u)), 0.0, 1.0),
                                   clamp(alpha_out * multiplier(field(k, 16u, 2u)), 0.0, 1.0));
        }
        combiner_buffer = next_buffer;
        if (i < 4u && (buffer_input & (1u << i)) != 0u) next_buffer.rgb = combiner_output.rgb;
        if (i < 4u && ((buffer_input >> 4) & (1u << i)) != 0u) next_buffer.a = combiner_output.a;
    }
    if (alpha_func >= 2u) {
        int a = int(combiner_output.a * 255.0);
        bool pass;
        switch (alpha_func) {
            case 2u: pass = a == alphatest_ref; break;
            case 3u: pass = a != alphatest_ref; break;
            case 4u: pass = a < alphatest_ref; break;
            case 5u: pass = a <= alphatest_ref; break;
            case 6u: pass = a > alphatest_ref; break;
            default: pass = a >= alphatest_ref; break;
        }
        if (!pass) discard;
    }
    gl_FragDepth = depth;
    color = byteround(combiner_output);
}
