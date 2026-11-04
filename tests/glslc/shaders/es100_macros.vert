// the preprocessor: object and function macros, nesting, #if arithmetic, defined, __LINE__ and __VERSION__
#define SCALE 2.0
#define SQUARE(x) ((x) * (x))
#define MAD(a, b, c) ((a) * (b) + (c))
#define TWICE(x) MAD(x, SCALE, 0.0)
#define USE_SKIN (1 + 1 == 2)
#if USE_SKIN && !defined(NO_SKIN) && __VERSION__ == 100 && (8 >> 2) == 2 && (7 % 4) == 3
#define BONES 4
#else
#define BONES 1
#endif
#undef SCALE
#define SCALE 3.0
attribute vec4 a_pos;
attribute vec4 a_weights;
uniform mat4 u_bones[BONES];
varying float v_line;
void main() {
    mat4 skin = u_bones[0] * a_weights.x;
    for (int i = 1; i < BONES; ++i) skin += u_bones[i] * a_weights[i];
    vec4 p = skin * a_pos;
    p.xy = vec2(SQUARE(p.x), TWICE(p.y));
    v_line = float(__LINE__);
    gl_Position = p * SCALE;
}
