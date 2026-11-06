#version 300 es
// GLSL ES 3.00: loops of every kind, break and continue, nested ternaries, short circuits with side effects,
// out and inout parameters, arrays as values, struct equality
precision mediump float;
out vec4 color;
uniform int limit;
uniform float values[8];
struct Pair { int a; float b; };

int counter;
bool bump() { counter++; return counter > 2; }

void accumulate(inout float sum, out int steps, in float add) {
    steps = 0;
    for (float x = 0.0; x < add; x += 1.0) {
        if (x > 4.0) continue;
        sum += x;
        steps++;
    }
}

float pick(int i) {
    float table[4] = float[4](1.0, 2.0, 4.0, 8.0);
    return table[i & 3];
}

void main() {
    counter = 0;
    float sum = 0.0;
    int steps;
    accumulate(sum, steps, values[1]);
    int i = 0;
    while (i < limit) {
        i++;
        if (i == 3) break;
    }
    do {
        i--;
    } while (i > 0 && bump());
    bool any_big = (sum > 10.0) || bump();
    Pair p = Pair(steps, sum);
    Pair q = Pair(steps, sum);
    float k = p == q ? (any_big ? 1.0 : 0.5) : (i > 1 ? 0.25 : 0.125);
    float w = pick(limit) + (limit > 2 && bump() ? 1.0 : 0.0);
    vec2 v = vec2(1.0, 2.0);
    v.yx = v;
    v[1] += 3.0;
    mat2 m = mat2(v, v.yx);
    m[1][0] = 7.0;
    m *= 2.0;
    color = vec4(sum, k, w, float(counter)) + vec4(m[0], m[1]) * 0.0;
}
