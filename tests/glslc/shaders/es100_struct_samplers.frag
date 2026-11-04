// GLSL ES 1.00: samplers inside a uniform struct and an array of them, read with texture2D and passed to a function
precision mediump float;
struct Surface { sampler2D color; lowp float gain; samplerCube sky; };
uniform Surface u_surface;
uniform Surface u_layers[2];
varying vec2 v_uv;

vec4 lit(Surface s, vec2 uv) { return texture2D(s.color, uv) * s.gain + textureCube(s.sky, vec3(uv, 1.0)); }

void main() {
    vec4 c = lit(u_surface, v_uv);
    for (int i = 0; i < 2; i++) c += texture2D(u_layers[0].color, v_uv * float(i + 1)) * u_layers[i].gain;
    gl_FragColor = c + lit(u_layers[1], v_uv.yx);
}
