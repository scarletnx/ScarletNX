// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// The PICA200's fragment pipeline as one fragment shader that reads its state from push constants: what a draw uses
// while the shader generated for its exact state (fsgen.c) is still being made with async_pipelines, so that the draw
// is drawn rather than skipped (vk/render.c). It follows fsgen.c's lighting, stages, rounding, texture sampling, alpha
// test, fog, scissor and depth, which follow Azahar's GLSL generator (GPL-2.0-or-later, Citra Emulator Project /
// Azahar Emulator Project). It draws only what it draws as fsgen.c's shader would: no gas, procedural texture or
// shadow rendering, and on unit 0 a 2D or projection texture or none (stand_in_draws in render.c). Its interface is
// fsgen.h's, with the state in push constants (StandIn in render.c).
#version 450

layout(location = 0) in vec4 primary_color;
layout(location = 1) in vec2 texcoord0;
layout(location = 2) in vec2 texcoord1;
layout(location = 3) in vec2 texcoord2;
layout(location = 4) in float texcoord0_w;
layout(location = 5) in vec4 normquat;
layout(location = 6) in vec3 view;
layout(location = 0) out vec4 color;

// fs_data, std140, at SnxCtrFsUniforms' offsets (fsgen.h; render.c checks them)
struct LightSrc {
    vec3 specular_0;
    vec3 specular_1;
    vec3 diffuse;
    vec3 ambient;
    vec3 position;
    vec3 spot_direction;
    float dist_atten_bias;
    float dist_atten_scale;
};
layout(std140, set = 0, binding = 2) uniform FsData {
    layout(offset = 4) int alphatest_ref;
    layout(offset = 8) float depth_scale;
    layout(offset = 12) float depth_offset;
    layout(offset = 24) int scissor_x1;
    layout(offset = 28) int scissor_y1;
    layout(offset = 32) int scissor_x2;
    layout(offset = 36) int scissor_y2;
    layout(offset = 40) int fog_lut_offset;
    layout(offset = 80) ivec4 lighting_lut_offset[6];
    layout(offset = 176) vec3 fog_color;
    layout(offset = 224) vec3 lighting_global_ambient;
    layout(offset = 240) LightSrc light_src[8];
    layout(offset = 1136) vec4 const_color[6];
    layout(offset = 1232) vec4 tev_combiner_buffer_color;
    layout(offset = 1248) vec4 tex_lod_bias;
    layout(offset = 1264) vec4 tex_border_color[3];
};
layout(set = 1, binding = 0) uniform sampler2D tex0[6]; // the image six times for a 2D or projection texture
layout(set = 1, binding = 1) uniform sampler2D tex1;
layout(set = 1, binding = 2) uniform sampler2D tex2;
layout(set = 0, binding = 3) uniform samplerBuffer lut_lf; // the lighting LUTs, then fog's

// SnxCtrFsConfig's stages as the registers' words, and its other switches: flags holds the alpha test function (bits
// 0-2), the texture 0 type (3-5), texture 2 on coordinate 1 (6), the combiner buffer input (8-15), Z-buffering (16),
// the scissor mode (17-18), fog (19) and its flip (20); borders ClampToBorder in s for units 0-2 (bits 0-2) and in t
// (3-5). Lighting's: each slot's light in lights, ten bits a slot, three slots a word (its number, then directional,
// two-sided diffuse, distance and spot attenuation, the geometric factors 0 and 1, and shadow); each LUT's in luts, a
// byte each in d0, d1, sp, fr, rb, rg, rr's order (enabled, absolute input, the input in bits 2-4, the scale in 5-7);
// and the rest in lighting (enabled, the slots' count in bits 1-4, the bump mode 5-6 and selector 7-8, renormalized 9,
// clamped highlights 10, the configuration 11-14, Fresnel into the primary 15 and secondary 16 alpha, shadow 17, on
// the primary 18 and secondary 19 colours, inverted 20, on alpha 21, and its selector 22-23)
layout(push_constant) uniform StandIn {
    uint sources[6], modifiers[6], ops[6], scales[6];
    uint flags, borders;
    uint lights[3], luts[2], lighting;
};

const uint TEX_PROJECTION = 3u, TEX_DISABLED = 5u;

vec4 byteround(vec4 x) { return round(x * 255.0) * (1.0 / 255.0); }
vec3 byteround(vec3 x) { return round(x * 255.0) * (1.0 / 255.0); }
float byteround(float x) { return round(x * 255.0) * (1.0 / 255.0); }
uint field(uint word, uint shift, uint bits) { return (word >> shift) & ((1u << bits) - 1u); }
float multiplier(uint scale) { return scale < 3u ? float(1u << scale) : 1.0; }

// fsgen.c's sample_lod: textureLod at getLod of the texel coordinates plus the unit's bias
float lod_of(vec2 texels) {
    vec2 d = max(abs(dFdx(texels)), abs(dFdy(texels)));
    return log2(max(d.x, d.y));
}
// the unit's border colour where ClampToBorder puts the coordinate outside, else its sample
vec4 bordered(vec4 sampled, vec2 uv, uint unit) {
    bool s = field(borders, unit, 1u) != 0u && (uv.x < 0.0 || uv.x > 1.0);
    bool t = field(borders, 3u + unit, 1u) != 0u && (uv.y < 0.0 || uv.y > 1.0);
    return s || t ? tex_border_color[unit] : sampled;
}

vec4 rounded_primary, primary_fragment, secondary_fragment, units[4], combiner_buffer, combiner_output;

vec4 source(uint src, uint index) {
    switch (src) {
        case 0u: return rounded_primary;
        case 1u: return primary_fragment;
        case 2u: return secondary_fragment;
        case 3u: case 4u: case 5u: case 6u: return units[src - 3u]; // unit 3, the procedural texture, is off here
        case 13u: return combiner_buffer;
        case 14u: return const_color[index];
        case 15u: return combiner_output;
        default: return vec4(0.0);
    }
}

// ---- lighting, fsgen.c's lighting() ----
uint light_field(uint slot, uint shift, uint bits) { return field(lights[slot / 3u], (slot % 3u) * 10u + shift, bits); }
uint lut_field(uint which, uint shift, uint bits) { return field(luts[which >> 2], (which & 3u) * 8u + shift, bits); }
const uint L_D0 = 0u, L_D1 = 1u, L_SP = 2u, L_FR = 3u, L_RB = 4u, L_RG = 5u, L_RR = 6u;
const uint S_D0 = 0u, S_D1 = 1u, S_FR = 3u, S_RB = 4u, S_RG = 5u, S_RR = 6u, S_SP = 8u, S_DA = 16u;
float lut_scale(uint s) {
    const float scales[8] = float[8](1.0, 2.0, 4.0, 8.0, 0.0, 0.0, 0.25, 0.5);
    return scales[s & 7u];
}
bool sampler_supported(uint config, uint lut_id) {
    switch (lut_id) {
        case 0u: return config != 1u;                                  // D0
        case 1u: return config != 0u && config != 1u && config != 5u;  // D1
        case 8u: return config != 2u && config != 3u;                  // SP
        case 3u: return config != 0u && config != 2u && config != 4u;  // FR
        case 6u: return config != 3u;                                  // RR
        default: return config == 4u || config == 5u || config == 8u; // RG, RB
    }
}
// LookupLightingLUT, Unsigned and Signed
float lighting_lut(uint lut, int index, float delta) {
    vec4 entry = texelFetch(lut_lf, lighting_lut_offset[lut >> 2][lut & 3u] + index);
    return entry.r + entry.g * delta;
}
float lut_unsigned(uint lut, float pos) {
    float p = pos * 256.0;
    int index = int(clamp(floor(p), 0.0, 255.0));
    return lighting_lut(lut, index, p - float(index));
}
float lut_signed(uint lut, float pos) {
    float p = pos * 128.0;
    int index = int(clamp(floor(p), -128.0, 127.0));
    float delta = p - float(index);
    return lighting_lut(lut, index < 0 ? index + 256 : index, delta);
}
vec3 quaternion_rotate(vec4 q, vec3 v) { return v + 2.0 * cross(q.xyz, cross(q.xyz, v) + v * q.w); }
vec3 v_normal, v_tangent, v_view, v_light, v_spot, v_half;
// get_lut_value: Azahar indexes the two-sided switch by the light's number, not its slot
float lut_value(uint lut_id, uint light_num, uint input_type, bool absolute) {
    float index;
    switch (input_type) {
        case 0u: index = dot(v_normal, normalize(v_half)); break;
        case 1u: index = dot(normalize(v_view), normalize(v_half)); break;
        case 2u: index = dot(v_normal, normalize(v_view)); break;
        case 3u: index = dot(v_light, v_normal); break;
        case 4u: index = dot(v_light, v_spot); break;
        case 5u:
            if (field(lighting, 11u, 4u) == 8u) {
                vec3 h = normalize(v_half);
                index = dot(h - v_normal * dot(v_normal, h), v_tangent);
            } else
                index = 0.0;
            break;
        default: index = 0.0; break;
    }
    if (!absolute) return lut_signed(lut_id, index);
    index = light_field(light_num, 4u, 1u) != 0u ? abs(index) : max(index, 0.0);
    return lut_unsigned(lut_id, index);
}
float scaled_lut(uint which, uint lut_id, uint light_num) {
    return lut_scale(lut_field(which, 5u, 3u)) *
           lut_value(lut_id, light_num, lut_field(which, 2u, 3u), lut_field(which, 1u, 1u) != 0u);
}
bool lut_used(uint which, uint lut_id) {
    return lut_field(which, 0u, 1u) != 0u && sampler_supported(field(lighting, 11u, 4u), lut_id);
}
float lut_or(uint which, uint lut_id, uint light_num, float otherwise) {
    return lut_used(which, lut_id) ? scaled_lut(which, lut_id, light_num) : otherwise;
}
void light_fragment() {
    uint config = field(lighting, 11u, 4u), src_num = field(lighting, 1u, 4u);
    bool primary_alpha = field(lighting, 15u, 1u) != 0u, secondary_alpha = field(lighting, 16u, 1u) != 0u;
    vec4 diffuse_sum = vec4(0.0, 0.0, 0.0, 1.0), specular_sum = diffuse_sum;
    float clamp_highlights = 1.0, geo_factor = 1.0;
    vec3 surface_normal = vec3(0.0, 0.0, 1.0), surface_tangent = vec3(1.0, 0.0, 0.0);
    uint bump = field(lighting, 5u, 2u);
    vec3 bumped = units[field(lighting, 7u, 2u)].rgb * 2.0 - 1.0;
    if (bump == 1u) { // a normal map; renorm recomputes z for precision
        surface_normal = bumped;
        if (field(lighting, 9u, 1u) != 0u)
            surface_normal.z = sqrt(max(1.0 - (surface_normal.x * surface_normal.x + surface_normal.y * surface_normal.y), 0.0));
    } else if (bump == 2u) // a tangent map
        surface_tangent = bumped;
    vec4 q = normalize(normquat);
    v_normal = quaternion_rotate(q, surface_normal);
    v_tangent = quaternion_rotate(q, surface_tangent);
    v_view = view;
    vec4 shadow = vec4(1.0);
    if (field(lighting, 17u, 1u) != 0u) {
        shadow = units[field(lighting, 22u, 2u)];
        if (field(lighting, 20u, 1u) != 0u) shadow = vec4(1.0) - shadow;
    }
    for (uint li = 0u; li < src_num; ++li) {
        uint num = light_field(li, 0u, 3u);
        bool last = li + 1u == src_num, shadowed = light_field(li, 9u, 1u) != 0u;
        bool geo0 = light_field(li, 7u, 1u) != 0u, geo1 = light_field(li, 8u, 1u) != 0u;
        vec3 light_vector = light_src[num].position;
        if (light_field(li, 3u, 1u) == 0u) light_vector += v_view; // not directional
        float light_distance = length(light_vector);
        v_light = normalize(light_vector);
        v_spot = light_src[num].spot_direction;
        v_half = normalize(v_view) + v_light;
        float dot_product = dot(v_light, v_normal);
        dot_product = light_field(li, 4u, 1u) != 0u ? abs(dot_product) : max(dot_product, 0.0);
        if (field(lighting, 10u, 1u) != 0u) clamp_highlights = sign(dot_product);
        if (geo0 || geo1) {
            geo_factor = dot(v_half, v_half);
            geo_factor = geo_factor == 0.0 ? 0.0 : min(dot_product / geo_factor, 1.0);
        }
        // spot and distance attenuation
        float spot_atten = light_field(li, 6u, 1u) != 0u && sampler_supported(config, S_SP)
                               ? scaled_lut(L_SP, S_SP + num, num) : 1.0;
        float dist_atten = 1.0;
        if (light_field(li, 5u, 1u) != 0u)
            dist_atten = lut_unsigned(S_DA + num, clamp(light_src[num].dist_atten_scale * light_distance +
                                                          light_src[num].dist_atten_bias, 0.0, 1.0));
        vec3 specular_0 = light_src[num].specular_0 * lut_or(L_D0, S_D0, num, 1.0);
        if (geo0) specular_0 *= geo_factor;
        // reflection: red, then green and blue or red's
        float refl_r = lut_or(L_RR, S_RR, num, 1.0);
        vec3 refl_value = vec3(refl_r, lut_or(L_RG, S_RG, num, refl_r), lut_or(L_RB, S_RB, num, refl_r));
        vec3 specular_1 = refl_value * lut_or(L_D1, S_D1, num, 1.0) * light_src[num].specular_1;
        if (geo1) specular_1 *= geo_factor;
        // Fresnel: only the last slot applies it
        if (last && lut_used(L_FR, S_FR) && (primary_alpha || secondary_alpha)) {
            float fresnel = scaled_lut(L_FR, S_FR, num);
            if (primary_alpha) diffuse_sum.a = fresnel;
            if (secondary_alpha) specular_sum.a = fresnel;
        }
        vec3 diffuse = light_src[num].diffuse * dot_product;
        if (field(lighting, 18u, 1u) != 0u && shadowed) diffuse *= shadow.rgb;
        diffuse += light_src[num].ambient;
        diffuse = diffuse * dist_atten * spot_atten;
        diffuse_sum.rgb += diffuse;
        vec3 specular = (specular_0 + specular_1) * clamp_highlights;
        specular = specular * dist_atten * spot_atten;
        if (field(lighting, 19u, 1u) != 0u && shadowed) specular *= shadow.rgb;
        specular_sum.rgb += specular;
    }
    if (field(lighting, 21u, 1u) != 0u) {
        if (primary_alpha) diffuse_sum.a *= shadow.a;
        if (secondary_alpha) specular_sum.a *= shadow.a;
    }
    diffuse_sum.rgb += lighting_global_ambient;
    primary_fragment = clamp(diffuse_sum, 0.0, 1.0);
    secondary_fragment = clamp(specular_sum, 0.0, 1.0);
}
// AppendColorModifier: the operand's rgb, or one of its components in all three, or nothing; then one minus it
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
    uint which = (modifier >> 1) & 3u;
    float x = which == 0u ? v.a : which == 1u ? v.r : which == 2u ? v.g : v.b;
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
        default: x = 0.0; break; // 6 and 7 (Dot3) have no alpha form
    }
    return clamp(x, 0.0, 1.0);
}

void main() {
    uint alpha_func = field(flags, 0u, 3u), type0 = field(flags, 3u, 3u);
    if (alpha_func == 0u) discard; // never passes
    rounded_primary = byteround(primary_color);
    uint scissor = field(flags, 17u, 2u);
    if (scissor != 0u) {
        bool inside = gl_FragCoord.x >= float(scissor_x1) && gl_FragCoord.y >= float(scissor_y1) &&
                      gl_FragCoord.x < float(scissor_x2) && gl_FragCoord.y < float(scissor_y2);
        if (scissor == 3u ? !inside : inside) discard;
    }
    float depth = -gl_FragCoord.z * depth_scale + depth_offset;
    if (field(flags, 16u, 1u) == 0u) depth /= gl_FragCoord.w; // W-buffering
    // the units are sampled before any discard of the stages, where every invocation runs, for their derivatives
    vec2 uv2 = field(flags, 6u, 1u) != 0u ? texcoord1 : texcoord2;
    vec4 sampled0 = type0 == TEX_PROJECTION ? textureProj(tex0[0], vec3(texcoord0, texcoord0_w))
                  : textureLod(tex0[0], texcoord0, lod_of(texcoord0 * vec2(textureSize(tex0[0], 0))) + tex_lod_bias[0]);
    units[0] = type0 == TEX_DISABLED ? vec4(0.0) : bordered(sampled0, texcoord0, 0u);
    units[1] = bordered(textureLod(tex1, texcoord1, lod_of(texcoord1 * vec2(textureSize(tex1, 0))) + tex_lod_bias[1]),
                        texcoord1, 1u);
    units[2] = bordered(textureLod(tex2, uv2, lod_of(uv2 * vec2(textureSize(tex2, 0))) + tex_lod_bias[2]), uv2, 2u);
    units[3] = vec4(0.0);
    primary_fragment = secondary_fragment = vec4(0.0);
    if ((lighting & 1u) != 0u) light_fragment();
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
    if (field(flags, 19u, 1u) != 0u) { // fog
        float fog_index = (field(flags, 20u, 1u) != 0u ? 1.0 - depth : depth) * 128.0;
        float fog_i = clamp(floor(fog_index), 0.0, 127.0);
        vec4 entry = texelFetch(lut_lf, int(fog_i) + fog_lut_offset);
        float fog_factor = clamp(entry.r + entry.g * (fog_index - fog_i), 0.0, 1.0);
        combiner_output.rgb = mix(fog_color, combiner_output.rgb, fog_factor);
    }
    gl_FragDepth = depth;
    color = byteround(combiner_output);
}
