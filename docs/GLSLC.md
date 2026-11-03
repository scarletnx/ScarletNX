<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# glslc: GLSL ES into SPIR-V

## Index

* [Using it](#using-it)
* [How it works](#how-it-works)
* [What it compiles](#what-it-compiles)
* [OpenGL ES on Vulkan](#opengl-es-on-vulkan)
* [Not compiled yet](#not-compiled-yet)
* [Where it differs from glslangValidator](#where-it-differs-from-glslangvalidator)
* [Tests](#tests)

`src/glslc` compiles the OpenGL ES shading language into SPIR-V, for the backends that run OpenGL ES programs on
Vulkan. It takes GLSL ES 1.00, 3.00, 3.10 and 3.20, what an OpenGL ES 3.2 GPU of the Mali-G52's class takes, for the
vertex, fragment and compute stages, and writes a SPIR-V 1.3 module for Vulkan 1.1. It is C99 and the C library only,
and it uses no other compiler: no glslang, no SPIRV-Cross.

No target of the build compiles a shader with it. ScarletNX's own shaders are Vulkan GLSL and go through the host's
glslangValidator (`cmake/glsl.cmake`); they need not compile with glslc.

## Using it

```c
#include "glslc/glslc.h"

static uint32_t words[SNX_GLSLC_MAX_WORDS];
static GlslcResult result;

/* once for each thread that compiles: glslc allocates nothing, the workspace is all the memory it uses */
GlslcWorkspace workspace;
size_t bytes = snx_glslc_workspace_size();
if (snx_glslc_workspace_init(&workspace, malloc(bytes), bytes, &result) != 0)
    fprintf(stderr, "%s\n", result.error);

/* then for each shader; bind, a binding map, is optional */
GlslcOptions options = {SNX_GLSLC_FRAGMENT, bind, bind_user};
if (snx_glslc_compile(&workspace, &options, source, size, words, SNX_GLSLC_MAX_WORDS, &result) != 0)
    fprintf(stderr, "%u:%u: %s\n", result.line, result.column, result.error);
```

- The workspace is the caller's memory, 16-byte aligned and `snx_glslc_workspace_size()` bytes (3.6 MiB): one per
  thread, reused by every compilation on it, one at a time. Each compilation carves its arrays from it anew, so
  nothing of one reaches the next. A workspace too small or misaligned is refused with its size and the size needed.
- `result.words` is the module's size. With a buffer too small (or `NULL`) the call returns -1 and still says how
  many words the module needs.
- An error has its line and column, with `#line` applied. A warning (an extension glslc does not have, a name with
  `__`) is counted, and the first is kept in `result.warning` with its place.
- `result.reflect` lists the interface the program binds by name: inputs and outputs with their locations, the loose
  uniforms with their offsets in the default uniform block, samplers, images and blocks with their set and binding.
  Each name is a view of the source, so the source must stay while the caller reads them.

The library links as `snx_glslc`. `tests/glslc/glslc_cli.c` is a small command line around it.

## How it works

The source stays where the caller keeps it. A token, an identifier or a member name is a view of it, a 16-bit position
and length, so a source is 65535 bytes at most. Three passes write arrays of fixed records that refer to each other
by 16-bit index, never by pointer, all carved from the caller's workspace by each compilation:

1. `lexer.c`: the preprocessor and the tokens. Macros expand on a stack of tokens: a use's arguments are expanded,
   put into the body, and the result scanned again with the macro turned off, each step on top of the last.
2. `parser.c`: typed syntax nodes, by recursive descent. GLSL declares every name before its use, so the same pass
   resolves names, checks types and folds the scalar constant expressions that array sizes, case labels and layout
   values need.
3. `emit.c`: SPIR-V, section by section, put together at the end.

`builtins.c` holds the tables: the built-in types, the words each version keeps (checked against glslangValidator
word by word), the built-in variables and constants, and the built-in functions with the SPIR-V each becomes.

Every array has an upper bound. A shader that needs more is refused, and the error names the bound and what the
shader needed (`the shader has 32769 tokens after preprocessing, more than the 32768 glslc takes`):

| Bound | Value | What it counts |
| --- | --- | --- |
| `SNX_GLSLC_MAX_SOURCE` | 65535 | bytes of source |
| `SNX_GLSLC_MAX_TOKENS` | 32768 | tokens after preprocessing |
| `SNX_GLSLC_MAX_NODES` | 32768 | syntax nodes; no construct makes more than it has tokens |
| `SNX_GLSLC_MAX_MACROS` | 512 | macros, the predefined ones included |
| `SNX_GLSLC_MAX_MACRO_TOKENS` | 8192 | tokens of every macro's parameters and body |
| `SNX_GLSLC_MAX_EXPANSION` | 4096 | tokens one macro use expands to |
| `SNX_GLSLC_MAX_MACRO_DEPTH` | 32 | macros expanding inside each other |
| `SNX_GLSLC_MAX_IF_DEPTH` | 64 | `#if` nested |
| `SNX_GLSLC_MAX_DEPTH` | 96 | expressions and statements nested |
| `SNX_GLSLC_MAX_SCOPES` | 64 | scopes nested |
| `SNX_GLSLC_MAX_TYPES` | 2048 | types, the built-in ones included |
| `SNX_GLSLC_MAX_SYMBOLS` | 4096 | names declared |
| `SNX_GLSLC_MAX_WORDS` | 262144 | words of SPIR-V |

`glslc.h` lists the others. The nesting bounds also bound the C stack each pass takes.

## What it compiles

- The preprocessor: `#define` with and without parameters, `#undef`, `#if`, `#ifdef`, `#ifndef`, `#elif`, `#else`,
  `#endif` with `defined`, `#error`, `#pragma` (`STDGL invariant(all)` makes every output invariant), `#extension`,
  `#line`, `#version`, and `__LINE__`, `__FILE__`, `__VERSION__`, `GL_ES`.
- Declarations: scalars, vectors, matrices, structs, arrays (of arrays from 3.10), the qualifiers and their layouts
  (`location`, `binding`, `set`, `std140`, `std430`, `row_major`, `local_size_*`, `early_fragment_tests`, the image
  formats), precision and the default precision rules, uniform blocks, buffer blocks with a runtime-sized last
  member, `shared`, functions with `in`, `out`, `inout` and `const` parameters.
- Every statement and operator, the constructors with their conversions, swizzles, `length()`.
- The built-in functions: angles and trigonometry, exponentials, the common ones, geometry, matrices, vector
  relations, integers (3.10), packing, derivatives, the texture functions of every sampler (shadow, cube, arrays,
  3D, multisample, buffer), `textureGather`, the image functions with their atomics, the atomics on buffers and
  shared memory, the barriers. 1.00's `texture2D` family as well.
- The built-in variables of the three stages, the sample ones of 3.20, and the implementation constants, set to
  OpenGL ES's minimums.
- Extensions: `GL_OES_standard_derivatives`, `GL_EXT_shader_texture_lod`, `GL_EXT_frag_depth`,
  `GL_EXT_draw_buffers`, `GL_OES_texture_3D` and `GL_EXT_shadow_samplers` for 1.00; `GL_OES_shader_image_atomic`,
  `GL_EXT_texture_buffer`, `GL_OES_texture_buffer`, `GL_EXT_texture_cube_map_array`,
  `GL_OES_texture_cube_map_array` and `GL_OES_texture_storage_multisample_2d_array` for 3.10; and
  `GL_OES_sample_variables`. Another one is refused with `require` and gives a warning with `enable` or `warn`.

## OpenGL ES on Vulkan

The module is what a Vulkan pipeline takes; the OpenGL ES side of it is the backend's, through `result.reflect`.

- **Locations.** An `in` or `out` without a location takes the first free one, in declaration order, after the
  given ones; a matrix takes one per column, an array one per element. `gl_FragColor` and `gl_FragData` are output
  location 0. OpenGL ES matches a vertex output to a fragment input by name: the backend matches the two lists.
- **Bindings.** OpenGL ES numbers texture units, image units, uniform blocks and buffer blocks apart, and Vulkan has
  one binding space per set, so the backend places them: `GlslcOptions.bind`, a function, with `bind_user` its first
  argument. glslc calls it for each sampler, image and block in declaration order, with its kind, name, array size,
  whether it is a texel buffer (`samplerBuffer`, `imageBuffer`), a block's size, and the set and binding the source
  gave (-1 for none), and takes the set and binding it writes. A nonzero answer refuses the shader, and two resources
  at one place, or a set or binding past 65535, are errors. Without a map (`bind` NULL), a resource keeps the set and
  binding it gives, and one without a binding takes the first free binding of set 0, in declaration order.
- **Loose uniforms** are the members of one block, `gl_DefaultUniformBlock`, laid out as std140. It takes the first
  free binding of set 0 once every other resource is placed, whether by the source, by glslc or by a binding map;
  the map is not asked about it. Its entry in `result.reflect` has an empty name and that binding, and each
  uniform's entry its offset in it.
- **Blocks** are std140 for uniform blocks and std430 for buffer blocks unless they say otherwise; `shared` and
  `packed` are laid out as std140. A buffer block is a StorageBuffer variable, as SPIR-V 1.3 has them. Their offsets
  and strides are glslangValidator's, which the tests check. A bool in a block is a uint there, as Vulkan has no bool
  in a block.
- **Built-ins.** `gl_VertexID` and `gl_InstanceID` are Vulkan's VertexIndex and InstanceIndex: the backend draws with
  a first instance of 0, and a vertex index counts from the first vertex as OpenGL ES's does. Fragment shaders have
  Vulkan's upper-left origin, so `gl_FragCoord` is Vulkan's: a backend that draws with a negative viewport height sees
  OpenGL ES's. A texture sampled in a vertex or compute shader without a level reads level 0.
- **Precision.** `lowp` and `mediump` variables, `in` parameters and loose uniforms are RelaxedPrecision; the results of
  operations and the members of structs and blocks are not.

## Not compiled yet

Each of these is refused with an error that names it, or is a name the shader cannot use:

- the geometry and tessellation stages, core in 3.20, and in and out blocks;
- atomic counters (`atomic_uint`), which have no Vulkan form;
- samplers and images inside structs; images as function parameters;
- `interpolateAtCentroid`, `interpolateAtSample`, `interpolateAtOffset`; `textureGatherOffsets`;
  `gl_DepthRange`; `gl_MaxComputeWorkGroupCount` and `gl_MaxComputeWorkGroupSize`;
- the preprocessor's `#` and `##`, which GLSL ES does not have either, and a line continuation inside a name or
  number;
- an array size from a constant expression glslc does not fold, such as a built-in function's result;
- `samplerExternalOES`, framebuffer fetch, and the extensions not listed above.

## Where it differs from glslangValidator

`tests/glslc` runs both on the same shaders. glslc refuses at once what glslang only finds when it links (a function
called and never defined, recursion, `gl_FragColor` with `gl_FragData`), and refuses a compute shader without a local
size, as GLSL ES does, where glslang takes 1. It takes vertical tab and form feed as white space, as GLSL ES says, and
`layout(set)` in 3.10, as glslang does only when it compiles for Vulkan. glslang has no `GL_EXT_draw_buffers`.

## Tests

`tests/glslc`, all through `ctest -R glslc`:

- `glslc_lex`, `glslc_parse`, `glslc_emit`: each pass on its own and the API, with every bound;
- `glslc_errors`: the cases of `errors.glsl`, each with its message and place, refused by glslangValidator too;
- `glslc_keywords`: every word of the keyword table in each version, against glslangValidator;
- `glslc_shaders`: every shader of `shaders/` valid for Vulkan 1.1 by spirv-val, taken by glslangValidator, with
  the same block layouts, capabilities and interface decorations;
- `glslc_semantics`: `semantics/values.comp` folded by spirv-opt from both compilers' modules into the same constants;
- `glslc_fuzz`: 3000 mutants of the shaders in one workspace, compiled or refused with a message, never a crash
  (with `SNX_TEST_SANITIZERS`, under ASan and UBSan), and every 16th again over the workspace filled with another
  pattern, with the same result;
- `glslc_sources`: no allocation in `src/glslc`.

Every shader in `tests/glslc` is synthetic.
