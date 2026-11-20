#version 120
// desktop GLSL 1.20: no default precision needed, a precision statement taken and of no effect, the f suffix
// glslang: rejects (precision is a reserved word of desktop GLSL 1.20, which a console's GL driver takes)
precision mediump float;
uniform sampler2D u_texture;
uniform float u_fade;
varying vec2 v_uv;
varying vec4 v_color;

void main() {
    vec4 c = texture2D(u_texture, v_uv) * v_color;
    gl_FragColor = vec4(c.rgb * (1.0f - u_fade), c.a);
}
