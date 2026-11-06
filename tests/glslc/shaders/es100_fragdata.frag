// glslang: rejects (it has no GL_EXT_draw_buffers)
// GLSL ES 1.00: gl_FragData with GL_EXT_draw_buffers, derivatives with GL_OES_standard_derivatives
#extension GL_EXT_draw_buffers : require
#extension GL_OES_standard_derivatives : enable
precision highp float;
varying vec2 v_uv;
uniform sampler2D u_tex;
#ifdef GL_OES_standard_derivatives
float edge(vec2 p) { return length(fwidth(p)); }
#else
float edge(vec2 p) { return 0.0; }
#endif
void main() {
    vec4 t = texture2D(u_tex, v_uv, -0.5);
    gl_FragData[0] = t;
    gl_FragData[1] = vec4(dFdx(v_uv.x), dFdy(v_uv.y), edge(v_uv), 1.0);
}
