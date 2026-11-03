// GLSL ES 1.00: a default precision, texture2D and textureCube, gl_FragColor, discard
precision mediump float;
varying vec3 v_color;
varying vec2 v_uv;
uniform sampler2D u_albedo;
uniform samplerCube u_environment;
uniform lowp float u_alpha_cut;
uniform highp vec4 u_fog;

void main() {
    lowp vec4 albedo = texture2D(u_albedo, v_uv);
    if (albedo.a < u_alpha_cut) discard;
    vec3 env = textureCube(u_environment, vec3(v_uv * 2.0 - 1.0, 1.0)).rgb;
    float fog = clamp(gl_FragCoord.z * u_fog.w, 0.0, 1.0);
    vec3 c = mix(albedo.rgb * v_color + env * 0.1, u_fog.rgb, fog);
    gl_FragColor = vec4(c, albedo.a);
}
