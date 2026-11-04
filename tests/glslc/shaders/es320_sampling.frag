#version 320 es
// GLSL ES 3.20: sample shading, texture buffers, cube map arrays, multisample arrays, fma, precise, image atomics
precision highp float;
precision highp samplerBuffer;
precision highp samplerCubeArray;
precision highp samplerCubeArrayShadow;
precision highp sampler2DMSArray;
precision highp iimage2D;
layout(binding = 0) uniform samplerBuffer table;
layout(binding = 1) uniform samplerCubeArray skies;
layout(binding = 2) uniform samplerCubeArrayShadow sky_shadows;
layout(binding = 3) uniform sampler2DMSArray layers;
layout(binding = 4, r32i) uniform iimage2D histogram;
layout(location = 0) sample in vec3 dir;
layout(location = 0) out vec4 color;
void main() {
    vec4 t = texelFetch(table, gl_SampleID);
    vec4 s = texture(skies, vec4(dir, 1.0)) + textureLod(skies, vec4(dir, 0.0), 1.0);
    float sh = texture(sky_shadows, vec4(dir, 2.0), 0.5);
    vec4 m = texelFetch(layers, ivec3(gl_FragCoord.xy, 1), gl_SampleID);
    precise float f = fma(t.x, s.y, m.z);
    int old = imageAtomicMax(histogram, ivec2(gl_FragCoord.xy), int(f));
    ivec3 cube_size = textureSize(skies, 0);
    int buffer_size = textureSize(table);
    color = vec4(f + float(old + cube_size.z + buffer_size), sh, gl_SamplePosition) * float(gl_SampleMaskIn[0] & 1);
    gl_SampleMask[0] = 0x7fffffff;
    gl_FragDepth = gl_FragCoord.z;
}
