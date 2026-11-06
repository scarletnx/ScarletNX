#version 300 es
// GLSL ES 3.00: textures read in a vertex shader (no derivatives there: the base level, or the level given), loops
// with a declared condition and a comma, a function that returns early, inout swizzles and arrays as arguments
precision highp float;
uniform sampler2D height;
uniform highp sampler2DShadow depth;
uniform highp sampler2DArray layers;
uniform highp usampler2D ids;
layout(location = 0) in vec3 position;
layout(location = 1) in vec2 uv;
out vec2 v_uv;
flat out uint v_id;

float sample_height(vec2 at) {
    if (at.x < 0.0) return 0.0;
    return texture(height, at).r + textureLod(height, at, 1.0).r + textureGrad(height, at, vec2(0.01), vec2(0.0)).r;
}

void nudge(inout vec2 p, in float by, out int steps) {
    steps = 0;
    for (int i = 0, j = 3; i < j; i++, j--) {
        p += vec2(by);
        steps++;
    }
}

float total(float values[3]) { return values[0] + values[1] + values[2]; }

void main() {
    vec4 p = vec4(position, 1.0);
    p.y += sample_height(uv);
    p.x += texture(depth, vec3(uv, 0.5)) + textureLod(depth, vec3(uv, 0.5), 0.0);
    p.z += texture(layers, vec3(uv, 2.0)).g + texelFetch(layers, ivec3(1, 2, 3), 0).b;
    int steps;
    nudge(p.zx, 0.5, steps);
    float values[3] = float[3](p.x, p.y, float(steps));
    p.w = total(values);
    int k = 0;
    while (bool more = k < 3) {
        k++;
        if (k == 2) continue;
        p.x += 1.0;
    }
    do {
        k--;
        if (k == 1) continue;
    } while (k > 0);
    v_uv = uv;
    v_id = texture(ids, uv).x + uint(textureSize(height, 0).x);
    gl_Position = p;
}
