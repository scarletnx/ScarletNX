#version 300 es
// GLSL ES 3.00: every operator on scalars, vectors and matrices, the constructors and their conversions, and the
// built-in functions of each family
precision highp float;
precision highp int;
uniform vec4 u;
uniform ivec4 iu;
uniform uvec4 uu;
uniform mat3 m3;
uniform mat2x3 m23;
uniform bool flag;
out vec4 color;
const float folded = sin(1.0) + 2.0;
const mat2 rotation = mat2(0.0, 1.0, -1.0, 0.0);
const vec3 axes[2] = vec3[2](vec3(1.0, 0.0, 0.0), vec3(0.0, 1.0, 0.0));
float g = folded * 2.0;
struct Pair { vec2 a; ivec2 b; };

void main() {
    /* arithmetic, with a scalar beside a vector or matrix */
    vec4 v = u * 2.0 + 1.0 - u / vec4(3.0) * -u;
    ivec4 i = iu * 3 + (iu / 2) % 5 - ivec4(1) * iu;
    uvec4 w = uu * 3u + (uu / 2u) % uvec4(5u) - 1u;
    i = (i << 2) >> ivec4(1) & 0xff | (i ^ ~iu);
    w = (w << 1u) >> 2u & uvec4(0xffu) | (w ^ ~uu);
    i <<= 1; w >>= uvec4(1u); i %= 3; w &= 7u; v /= 2.0; v -= u;
    /* matrices */
    mat3 m = m3 * m3 + m3 - 2.0 * m3 / 3.0;
    m = -m;
    m++;
    m *= m3;
    mat3 c = matrixCompMult(m, m3) + outerProduct(u.xyz, u.wzy) + transpose(m) + inverse(m3);
    vec3 r = m * u.xyz + u.xyz * m + m23 * u.xy + vec3(determinant(m), 0.0, 0.0);
    mat3x2 t = transpose(m23);
    mat2 p = t * m23 * rotation;
    vec3 q = u.xy * t;
    /* constructors and conversions */
    vec3 from_matrix = vec3(m3);
    mat2 from_vector = mat2(u);
    mat3 mixed = mat3(u.xy, iu.x, u.xyz, vec3(2.0));
    mat4 grown = mat4(m3);
    mat2 shrunk = mat2(m3);
    ivec3 from_floats = ivec3(u.xyz) + ivec3(bvec3(true, false, flag));
    uvec2 from_ints = uvec2(iu.xy) + uvec2(u.zw) + uvec2(flag);
    bvec4 truth = equal(bvec4(u), bvec4(iu));
    bool same = bvec4(u) == bvec4(iu) && m3 == m3 && axes == axes;
    float scalar = float(uu.x) + float(iu.y) + float(flag) + float(u) + float(m3);
    int first = int(u.z) + int(uu.w) + int(true);
    bool b = bool(iu.x) && bool(u.y) || bool(uu.z) ^^ flag;
    Pair pair = Pair(u.xy, iu.zw);
    Pair other = pair;
    other.b.y++;
    /* relational and vector built-ins */
    bvec4 lt = lessThan(u, v);
    bvec3 ge = greaterThanEqual(iu.xyz, i.xyz);
    bvec2 ne = notEqual(uu.xy, w.xy);
    bool all_of = all(lt) || any(ge) || any(not(ne)) || all(equal(truth, bvec4(b)));
    /* the families */
    vec4 f = abs(v) + sign(v) + floor(v) + ceil(v) + fract(v) + trunc(v) + round(v) + roundEven(v);
    f += mod(v, 2.0) + mod(v, u) + min(v, 1.0) + max(v, u) + clamp(v, 0.0, 1.0) + clamp(v, u, u + 1.0);
    f += mix(v, u, 0.5) + mix(v, u, u) + mix(v, u, lt) + step(0.5, v) + step(u, v) + smoothstep(0.0, 1.0, v);
    f += sqrt(abs(v)) + inversesqrt(abs(v) + 1.0) + pow(abs(v), u) + exp(v) + exp2(v) + log(abs(v) + 1.0);
    f += log2(abs(v) + 1.0) + radians(v) + degrees(v) + sin(v) + cos(v) + tan(v) + asin(u) + acos(u) + atan(u);
    f += atan(u, v) + sinh(v) + cosh(v) + tanh(v) + asinh(v) + acosh(abs(v) + 1.0) + atanh(u * 0.5);
    float lengths = length(v) + distance(v, u) + dot(v, u) + dot(v.x, u.x);
    vec3 geometric = cross(v.xyz, u.xyz) + normalize(v.xyz) + faceforward(v.xyz, u.xyz, r) + reflect(v.xyz, u.xyz);
    geometric += refract(normalize(v.xyz), normalize(u.xyz), 0.75);
    ivec4 ints = abs(iu) + sign(iu) + min(iu, 3) + max(iu, iu) + clamp(iu, -1, 1);
    uvec4 uints = min(uu, 3u) + max(uu, uu) + clamp(uu, uvec4(1u), uvec4(9u));
    vec4 bits = intBitsToFloat(floatBitsToInt(v)) + uintBitsToFloat(floatBitsToUint(u));
    bvec4 odd = isnan(v);
    odd = odd.x || true ? isinf(u) : odd;
    vec4 whole;
    vec4 fraction = modf(v, whole);
    vec2 unpacked = unpackSnorm2x16(packSnorm2x16(u.xy)) + unpackUnorm2x16(packUnorm2x16(u.zw)) +
                    unpackHalf2x16(packHalf2x16(v.xy));
    color = v + f + bits + fraction + whole + vec4(r, scalar) + vec4(c[1], float(first)) + vec4(geometric, lengths) +
            vec4(ints + ivec4(uints) + i + ivec4(w)) + vec4(p[1], q.xy) + vec4(from_matrix, 1.0) + vec4(from_vector) +
            vec4(mixed[2], 0.0) + grown[3] + vec4(shrunk) + vec4(from_floats, int(from_ints.x)) +
            vec4(all_of || b || odd.x || same, pair == other, other.b) + vec4(unpacked, g, axes[1].y) * folded;
}
