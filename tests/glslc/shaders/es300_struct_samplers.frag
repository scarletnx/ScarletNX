#version 300 es
// GLSL ES 3.00: structs of samplers alone and with numbers, arrays of samplers inside a struct, a struct parameter
// passed on to another function, and an array of structs passed whole
precision highp float;
precision mediump sampler2DShadow;
precision mediump sampler3D;
struct Shadow { sampler2DShadow map; sampler3D noise; };
struct Terrain { vec4 scale; sampler2D splat[4]; mat2 rotate; };
struct Decal { sampler2D image; vec2 at; };
uniform Shadow shadow;
uniform Terrain terrain;
uniform Decal decals[3];
in vec3 v_pos;
out vec4 color;

float lit(Shadow s, vec3 p) { return texture(s.map, p) * texture(s.noise, p).r; }
float deeper(Shadow s, vec3 p) { return lit(s, p * 0.5); }
vec4 decal(Decal d, vec2 uv) { return texture(d.image, uv - d.at); }
vec4 all_decals(Decal ds[3], vec2 uv) { return decal(ds[0], uv) + decal(ds[1], uv) + decal(ds[2], uv); }

void main() {
    vec2 uv = terrain.rotate * v_pos.xy;
    vec4 c = texture(terrain.splat[0], uv) * terrain.scale.x + texture(terrain.splat[3], uv) * terrain.scale.w;
    c += textureLod(terrain.splat[1], uv, 2.0) + texelFetch(terrain.splat[2], ivec2(uv), 0);
    color = c * deeper(shadow, v_pos) + all_decals(decals, uv) + vec4(textureSize(decals[2].image, 0), 0.0, 1.0);
}
