// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// Shaders glslc refuses, each after a line "--- NAME.STAGE LINE:COLUMN: MESSAGE", the error it gives, and
// glslangValidator refuses each one too, but where a line "--- glslang accepts: WHY" says it does not, which
// tests/glslc/glslc_test.py checks as well. The cases are synthetic.

--- implicit_conversion.frag 4:32: cannot initialize float b with int (GLSL ES converts no value)
#version 300 es
precision mediump float;
out vec4 c;
void main() { int a = 1; float b = a; c = vec4(b); }
--- vector_sizes.frag 4:29: no operator + takes vec4 and vec3 (GLSL ES converts neither)
#version 300 es
precision mediump float;
out vec4 c;
void main() { c = vec4(1.0) + vec3(1.0); }
--- swizzle_twice.frag 4:20: this assignment cannot write to a swizzle that names a component twice
#version 300 es
precision mediump float;
out vec4 c;
void main() { c.xx = vec2(1.0); }
--- index_out_of_range.frag 4:38: index 2 is past the end of float[2]
#version 300 es
precision mediump float;
out vec4 c;
void main() { float a[2]; c = vec4(a[2]); }
--- negative_index.frag 4:46: index -1 of vec4 is negative
#version 300 es
precision mediump float;
out vec4 c;
void main() { vec4 v = vec4(1.0); c = vec4(v[-1]); }
--- last_label_alone.frag 4:51: the last case or default label of a switch has no statement after it
#version 300 es
precision mediump float;
out vec4 c;
void main() { switch (1) { case 1: c = vec4(1.0); case 2: } }
--- no_return.frag 4:7: h returns float and has no return statement
#version 300 es
precision mediump float;
out vec4 c;
float h() { }
void main() { c = vec4(1.0); }
--- uniform_initializer.frag 4:15: u cannot be initialized: it is a uniform
#version 300 es
precision mediump float;
out vec4 c;
uniform float u = 1.0;
void main() { c = vec4(u); }
--- undeclared.frag 4:15: 'undeclared' is not declared
#version 300 es
precision mediump float;
out vec4 c;
void main() { undeclared = 1.0; }
--- missing_semicolon.frag 4:29: expected ';' after an expression, found '}'
#version 300 es
precision mediump float;
out vec4 c;
void main() { c = vec4(1.0) }
--- builtin_argument.frag 4:25: no overload of sin takes (int)
#version 300 es
precision mediump float;
out vec4 c;
void main() { float x = sin(1); c = vec4(x); }
--- const_without_value.frag 4:27: the constant k needs an initializer
#version 300 es
precision mediump float;
out vec4 c;
void main() { const float k; c = vec4(k); }
--- write_uniform.frag 5:17: this assignment cannot write to a uniform
#version 300 es
precision mediump float;
out vec4 c;
uniform float u;
void main() { u = 1.0; c = vec4(u); }
--- too_few_components.frag 4:19: vec4 needs 4 components, and its arguments have 2
#version 300 es
precision mediump float;
out vec4 c;
void main() { c = vec4(1.0, 2.0); }
--- too_many_arguments.frag 4:19: vec4 has 4 components, and its arguments give 5: the last is left over
#version 300 es
precision mediump float;
out vec4 c;
void main() { c = vec4(1.0, 2.0, 3.0, 4.0, 5.0); }
--- ternary_types.frag 4:28: the two sides of ?: are int and float, not one type
#version 300 es
precision mediump float;
out vec4 c;
void main() { int i = true ? 1 : 2.0; c = vec4(float(i)); }
--- break_outside.frag 4:15: break is only inside a loop or a switch
#version 300 es
precision mediump float;
out vec4 c;
void main() { break; }
--- discard_in_vertex.vert 2:15: discard is only in fragment shaders
#version 300 es
void main() { discard; }
--- out_argument_value.frag 5:17: an out argument cannot write to a value that is not a variable
#version 300 es
precision mediump float;
out vec4 c;
void f(out float x) { x = 1.0; }
void main() { f(1.0); c = vec4(1.0); }
--- struct_member_type.frag 5:23: member x of S is float, not int
#version 300 es
precision mediump float;
out vec4 c;
struct S { float x; };
void main() { S s = S(1); c = vec4(s.x); }
--- swizzle_past_end.frag 4:46: 'z' in .xyz is past the 2 components of vec2
#version 300 es
precision mediump float;
out vec4 c;
void main() { vec2 v = vec2(1.0); c = vec4(v.xyz, 1.0); }
--- modulo_of_floats.frag 4:29: no operator % takes vec4 and vec4 (GLSL ES converts neither)
#version 300 es
precision mediump float;
out vec4 c;
void main() { c = vec4(1.0) % vec4(2.0); }
--- shift_by_float.frag 4:25: no operator << takes int and float (GLSL ES converts neither)
#version 300 es
precision mediump float;
out vec4 c;
void main() { int x = 1 << 2.0; c = vec4(float(x)); }
--- void_returns_value.frag 4:15: this function returns void, and return gives a value
#version 300 es
precision mediump float;
out vec4 c;
void main() { return 1.0; }
--- redeclared.frag 4:26: 'x' is already declared in this scope
#version 300 es
precision mediump float;
out vec4 c;
void main() { int x; int x; }
--- main_twice.frag 5:6: main is defined twice
#version 300 es
precision mediump float;
out vec4 c;
void main() { c = vec4(1.0); }
void main() { }
--- never_defined.frag 5:15: foo is called, and never defined
--- glslang accepts: it reports a missing body when it links
#version 300 es
precision mediump float;
out vec4 c;
void foo();
void main() { foo(); c = vec4(1.0); }
--- recursion.frag 5:23: f is called from inside itself: GLSL ES has no recursion
--- glslang accepts: it reports recursion when it links
#version 300 es
precision mediump float;
out vec4 c;
int f(int x);
int g(int x) { return f(x); }
int f(int x) { return g(x); }
void main() { c = vec4(float(f(1))); }
--- no_float_precision.frag 2:10: float has no precision here: give it lowp, mediump or highp, or set a default with 'precision mediump float;'
#version 300 es
out vec4 c;
void main() { float x = 1.0; c = vec4(x); }
--- no_sampler_precision.frag 4:19: sampler3D has no precision here: give it lowp, mediump or highp, or set a default with 'precision mediump sampler3D;'
#version 300 es
precision mediump float;
out vec4 c;
uniform sampler3D s;
void main() { c = texture(s, vec3(0.0)); }
--- texture_coordinates.frag 5:19: no overload of texture takes (sampler2D, vec3)
#version 300 es
precision mediump float;
out vec4 c;
uniform sampler2D s;
void main() { c = texture(s, vec3(1.0)); }
--- texture2d_in_300.frag 5:19: the built-in function texture2D is not in GLSL ES 3.00
#version 300 es
precision mediump float;
out vec4 c;
uniform sampler2D s;
void main() { c = texture2D(s, vec2(1.0)); }
--- fragcolor_and_fragdata.frag 2:41: a fragment shader writes gl_FragColor or gl_FragData, not both
--- glslang accepts: it reports both outputs when it links
precision mediump float;
void main() { gl_FragColor = vec4(1.0); gl_FragData[0] = vec4(1.0); }
--- modulo_in_100.frag 2:48: operator % needs GLSL ES 3.00 (1.00 reserves it)
precision mediump float;
void main() { int x = 5 - 5 / 2 * 2; int y = x % 2; gl_FragColor = vec4(float(y)); }
--- in_in_100.frag 2:1: in at global scope needs GLSL ES 3.00 (1.00 has attribute and varying)
precision mediump float;
in vec4 x;
void main() { gl_FragColor = x; }
--- bool_input.frag 4:9: in b is bool: in variables are numbers, vectors or matrices
#version 300 es
precision mediump float;
out vec4 c;
in bool b;
void main() { c = vec4(1.0); }
--- integer_input_not_flat.frag 4:8: i is int: an integer fragment input must be flat
#version 300 es
precision mediump float;
out vec4 c;
in int i;
void main() { c = vec4(float(i)); }
--- integer_output_not_flat.vert 2:9: i is int: an integer vertex output must be flat
#version 300 es
out int i;
void main() { i = 1; gl_Position = vec4(1.0); }
--- fragcolor_in_300.frag 4:30: gl_FragColor is not in GLSL ES 3.00
#version 300 es
precision mediump float;
out vec4 c;
void main() { c = vec4(1.0); gl_FragColor = c; }
--- case_twice.frag 5:36: case 1 is given twice
#version 300 es
precision mediump float;
out vec4 c;
uniform int u;
void main() { switch (u) { case 1: case 1: break; } c = vec4(1.0); }
--- matrix_times_vector.frag 4:46: no operator * takes mat3 and vec4 (GLSL ES converts neither)
#version 300 es
precision mediump float;
out vec4 c;
void main() { mat3 m = mat3(1.0); vec3 v = m * vec4(1.0); c = vec4(v, 1.0); }
--- sampler_dynamic_index.frag 6:28: sampler2D[2] is indexed by a constant expression only in GLSL ES 3.00
#version 300 es
precision mediump float;
out vec4 c;
uniform sampler2D s[2];
uniform int i;
void main() { c = texture(s[i], vec2(0.0)); }
--- array_size_mismatch.frag 4:19: cannot initialize int[3] a with int[2] (GLSL ES converts no value)
#version 300 es
precision mediump float;
out vec4 c;
void main() { int a[3] = int[](1, 2); c = vec4(1.0); }
--- swizzle_mixed_sets.frag 4:25: 'xg' is not a swizzle: its letters come from one of xyzw, rgba and stpq
#version 300 es
precision mediump float;
out vec4 c;
void main() { vec4 v; v.xg = vec2(1.0); c = v; }
--- defined_twice.frag 5:7: f is defined twice
#version 300 es
precision mediump float;
out vec4 c;
float f(float x) { return x; }
float f(float y) { return y; }
void main() { c = vec4(1.0); }
--- return_type_differs.frag 5:5: f is declared again with another return type
#version 300 es
precision mediump float;
out vec4 c;
float f(float x);
int f(float x) { return 1; }
void main() { c = vec4(1.0); }
--- gl_name.frag 4:21: 'gl_x': names starting with gl_ are GLSL's own
#version 300 es
precision mediump float;
out vec4 c;
void main() { float gl_x = 1.0; c = vec4(gl_x); }
--- no_local_size.comp 3:1: a compute shader declares its local size: layout(local_size_x = 64) in;
--- glslang accepts: it takes a local size of 1 when none is given
#version 310 es
void main() {}
--- image_without_format.comp 3:43: the image img needs a format: layout(rgba8) and the like
#version 310 es
layout(local_size_x = 4) in;
layout(binding = 0) uniform highp image2D img;
void main() {}
--- image_read_write_rgba8.comp 3:50: the image img (format rgba8) must be readonly or writeonly: only r32f, r32i and r32ui are both
#version 310 es
layout(local_size_x = 4) in;
layout(binding = 0, rgba8) uniform highp image2D img;
void main() {}
--- atomic_on_local.comp 3:36: atomicAdd works on a buffer or shared variable
#version 310 es
layout(local_size_x = 4) in;
void main() { int l = 0; atomicAdd(l, 1); }
--- literal_past_32_bits.frag 4:23: 4294967296 does not fit in 32 bits
#version 300 es
precision mediump float;
out vec4 c;
void main() { int i = 4294967296; c = vec4(float(i)); }
--- for_body_redeclares.frag 4:49: 'i' is already declared in this scope
#version 300 es
precision mediump float;
out vec4 c;
void main() { for (int i = 0; i < 4; i++) { int i = 2; } c = vec4(1.0); }
--- float_plus_int.frag 4:39: no operator += takes float and int
#version 300 es
precision mediump float;
out vec4 c;
void main() { vec4 v = vec4(1.0); v.x += 1; c = v; }
--- write_uniform_block.frag 5:19: this assignment cannot write to a uniform
#version 300 es
precision mediump float;
out vec4 c;
uniform U { vec4 a; } u;
void main() { u.a = vec4(1.0); c = u.a; }
--- global_semicolon.frag 4:1: a ';' alone is not a declaration in GLSL ES
#version 300 es
precision mediump float;
out vec4 c;
;
void main() { c = vec4(1.0); }
--- location_on_vertex_output.vert 2:1: layout(location) on a vertex output needs GLSL ES 3.10
#version 300 es
layout(location = 1) out vec4 v;
void main() { v = vec4(1.0); gl_Position = v; }
--- stringizing.vert 2:14: the # operator (stringizing) is not in GLSL ES
#version 300 es
#define S(a) #a
void main() { gl_Position = vec4(1.0); }
--- token_pasting.vert 2:19: token pasting (##) is not in GLSL ES
#version 300 es
#define P(a, b) a ## b
void main() { gl_Position = vec4(1.0); }
--- undefined_in_if.vert 2:5: 'FOO' is not a defined macro: a #if in GLSL ES takes defined macros and numbers only
#version 300 es
#if FOO > 1
#endif
void main() { gl_Position = vec4(1.0); }
--- reserved_word.frag 4:21: expected a variable name, found 'goto', a word GLSL ES 3.00 reserves
#version 300 es
precision mediump float;
out vec4 c;
void main() { float goto = 1.0; c = vec4(goto); }
--- attribute_in_300.vert 2:1: 'attribute' is a word GLSL ES 3.00 reserves
#version 300 es
attribute vec4 a;
void main() { gl_Position = a; }
--- switch_in_100.frag 3:15: 'switch' is a word GLSL ES 1.00 reserves
precision mediump float;
uniform int u;
void main() { switch (u) { default: break; } gl_FragColor = vec4(1.0); }
--- uint_in_100.frag 2:15: 'uint' is not declared
precision mediump float;
void main() { uint u = 1; gl_FragColor = vec4(1.0); }
--- suffix_f_in_100.frag 2:35: '1.0f': the suffix f needs GLSL ES 3.00 or later
precision mediump float;
void main() { gl_FragColor = vec4(1.0f); }
--- builtin_redefined.frag 4:7: sin is a built-in function, which a GLSL ES shader cannot redefine or overload
#version 300 es
precision mediump float;
out vec4 c;
float sin(float x) { return x; }
void main() { c = vec4(sin(1.0)); }
--- embedded_struct.frag 4:12: a struct cannot be defined inside a struct or block
#version 300 es
precision mediump float;
out vec4 c;
struct S { struct T { float y; } t; };
void main() { c = vec4(1.0); }
--- assign_to_input.frag 5:17: this assignment cannot write to an input
#version 300 es
precision mediump float;
out vec4 c;
in vec4 v;
void main() { v = vec4(1.0); c = v; }
--- const_parameter_written.frag 4:28: this assignment cannot write to a const parameter
#version 300 es
precision mediump float;
out vec4 c;
float f(const float x) { x = 1.0; return x; }
void main() { c = vec4(f(1.0)); }
--- nonconstant_global.frag 5:13: the initializer of g must be a constant expression
#version 300 es
precision mediump float;
out vec4 c;
uniform float u;
float g = u * 2.0;
void main() { c = vec4(g); }
--- vertex_input_array.vert 2:9: a vertex input cannot be an array
#version 300 es
in vec4 a[2];
void main() { gl_Position = a[0]; }
--- derivative_in_vertex.vert 3:34: the built-in function dFdx is only in fragment shaders
#version 300 es
in vec4 a;
void main() { gl_Position = vec4(dFdx(a.x)); }
--- bias_in_vertex.vert 3:29: texture takes a bias only in a fragment shader
#version 300 es
uniform highp sampler2D s;
void main() { gl_Position = texture(s, vec2(0.0), 1.0); }
--- offset_not_constant.frag 6:47: this argument of textureOffset must be a constant expression
#version 300 es
precision mediump float;
out vec4 c;
uniform sampler2D s;
uniform ivec2 o;
void main() { c = textureOffset(s, vec2(0.0), o); }
--- condition_not_bool.frag 4:19: an if's condition is a bool, not int
#version 300 es
precision mediump float;
out vec4 c;
void main() { if (1) c = vec4(1.0); }
--- call_a_variable.frag 4:39: 'f' is a variable, not a function
#version 300 es
precision mediump float;
out vec4 c;
void main() { float f = 1.0; c = vec4(f(2.0)); }
--- shared_in_fragment.frag 3:1: shared variables are only in compute shaders
#version 310 es
precision mediump float;
shared float s;
layout(location = 0) out vec4 c;
void main() { c = vec4(1.0); }
--- readonly_buffer_written.comp 4:20: this assignment cannot write to a readonly block member
#version 310 es
layout(local_size_x = 4) in;
layout(std430, binding = 0) readonly buffer B { float v[]; };
void main() { v[0] = 1.0; }
--- unsized_not_last.comp 3:46: only a buffer block's last member may be an array with no size
#version 310 es
layout(local_size_x = 4) in;
layout(std430, binding = 0) buffer B { float v[]; float w; };
void main() { w = v[0]; }
--- compute_in_300.comp 1:1: a compute shader needs #version 310 es or later, and this one is GLSL ES 300 es
#version 300 es
void main() {}
--- array_assigned_in_100.frag 2:41: an array cannot be assigned in GLSL ES 1.00
precision mediump float;
void main() { float a[2]; float b[2]; a = b; gl_FragColor = vec4(a[0]); }
--- name_with_two_underscores_in_100.frag 2:7: 'a__b': GLSL ES 1.00 reserves names with two underscores in a row
precision mediump float;
float a__b;
void main() { gl_FragColor = vec4(a__b); }
--- macro_with_two_underscores_in_100.vert 1:9: 'A__B': GLSL ES 1.00 reserves names with two underscores in a row
#define A__B 1
void main() { gl_Position = vec4(1.0); }
--- texel_offset_out_of_range.frag 5:47: the offset -9 is outside gl_MinProgramTexelOffset (-8) to gl_MaxProgramTexelOffset (7)
#version 300 es
precision mediump float;
out vec4 c;
uniform sampler2D s;
void main() { c = textureOffset(s, vec2(0.0), ivec2(1, -9)); }
