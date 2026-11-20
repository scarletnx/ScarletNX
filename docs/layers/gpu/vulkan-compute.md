<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# The GPU's compute path

## Index

* [The rule](#the-rule)
* [Why not a vendor's SDK](#why-not-a-vendors-sdk)
* [What a CUDA feature is here](#what-a-cuda-feature-is-here)
* [Where each one is used](#where-each-one-is-used)
* [How a compute path is chosen](#how-a-compute-path-is-chosen)
* [Measuring one](#measuring-one)

## The rule

Every data-parallel pass of the runtime is a Vulkan compute shader, or SPIR-V that our translators write. No pass is
written against CUDA, ROCm or HIP, OpenCL, SYCL, Metal Shading Language or an HLSL compiler. The build has no vendor
SDK in it: `cmake/dependencies.cmake` pins the Khronos headers, volk, glslang and SPIRV-Tools, and `cmake/glsl.cmake`
compiles GLSL to SPIR-V at build time.

Two consequences follow, and both are the point:

- **One path, every host.** The same shader runs on Linux and Windows on any Vulkan 1.3 driver, on macOS and iOS
  through MoltenVK, and on Android 9 and later through Turnip, PanVK or RADV-Xclipse
  ([the Android app](../../hosts/android-app.md)). A vendor kernel language would need one path for each vendor and
  would leave the others on a fallback.
- **One place for a driver bug.** A driver that gets an operation wrong is named once, in
  `src/common/gpu/vk/extension_bans.txt`, and every host reads that table.

The GPU work also stays on the GPU: a pass that the device cannot do in a shader is done differently, not read back to
the CPU ([GPU-owned guest memory](ownership.md)).

## Why not a vendor's SDK

A compute pass could be written in CUDA and be a little faster on one vendor's hardware. It would then need a second
copy for AMD's or Intel's hardware and a third for Apple's, each with its own dispatch, its own allocations, its own
fences and its own build tool. The switchable parts of the renderer already show what that costs in a smaller form: the
ban table carries a row per driver, and each row had to be found by running a device.

Vulkan covers what these passes need. The features that a CUDA programmer would reach for have a Khronos equivalent,
and each one is a promise that the runtime asks the device for and falls back from when it is not there.

## What a CUDA feature is here

| CUDA | Vulkan compute |
| --- | --- |
| a kernel, a grid, a block, `__syncthreads()` | a compute shader, `vkCmdDispatch`, workgroup storage and `OpControlBarrier` |
| `__shfl_sync`, `__ballot_sync`, `__any_sync`, `__reduce_*_sync` | subgroup operations: `GL_KHR_shader_subgroup_shuffle`, `_ballot`, `_vote`, `_arithmetic`, `_quad` |
| a warp of exactly 32 lanes | `VkPhysicalDeviceSubgroupSizeControlFeatures`, a pipeline's `requiredSubgroupSize`, `REQUIRE_FULL_SUBGROUPS` |
| `wmma`, `mma.sync`, tensor cores | `VK_KHR_cooperative_matrix`: `OpTypeCooperativeMatrixKHR`, `OpCooperativeMatrixLoadKHR`, `OpCooperativeMatrixMulAddKHR` |
| `-D` and templated kernels | specialization constants, and the `DEFINES` of `snx_glsl_spirv` at build time |
| cuBLAS, cuDNN | nothing: the matrix work here is the fixed fragment of a guest instruction, not a library GEMM |
| CUDA graphs | command buffers and the renderer's frame ring |

## Where each one is used

- **Shaders of our own** (`src/nx/gpu/`): the ASTC decode and the BC7 encode (`texture/astc.comp`, `texture/bc7.comp`,
  `texture/astc_decode.glsl`, `texture/bc7_encode.glsl`), the block linear swizzle and the format conversions
  (`texture/blocklinear.glsl`, `texture/texconv.glsl`), the pixel conversions (`texture/pixel_convert.glsl`) and the
  VIC composition (`video/vic_compose.glsl`). Each is compiled for Vulkan 1.1 or 1.3 by `snx_glsl_spirv`, and each has
  a C++ reference that the tests run on the CPU ([the ASTC layer](astc.md), [block linear](blocklinear.md)).
- **Cooperative matrices** for a guest program's HMMA: `hmma_coop` in `src/nx/gpu/shader/sm86_translate.c` emits
  `OpCooperativeMatrixMulAddKHR` on f16 A and B into an f32 accumulator, which is both faster than imitating the
  instruction with packed f16 and closer to the console, whose tensor core keeps the sum in f32. The device side is
  the query in `src/nx/gpu/vk/device.c`, and the gate is the `vk_khr_cooperative_matrix` setting, auto by default.
- **Subgroups of 32** for a compute program that reads another lane: `cs_reads_lanes` in `src/nx/gpu/vk/compute.c`
  asks for a subgroup of 32 and full subgroups for such a program, and `snx_vk_compute_pipeline_subgroups` in
  `src/common/gpu/vk/vk_common.c` makes the pipeline with them.
- **Shared memory** where invocations share data: the ASTC decode stages a tile of decoded blocks (`texture/astc.comp`).
  Where every input has one reader, a shader loads it directly instead: a tile costs shared memory, which is what
  bounds the workgroups a device keeps in flight ([the ASTC layer](astc.md)).
- **One shader, builds by define**: the encoded texture's format, the footprint of a block and the fast path of an
  encoder are `DEFINES` of one source, never a copied file.

## How a compute path is chosen

The device says what it has. `src/nx/gpu/vk/device.c` asks for each extension and feature, applies the player's
settings, then the ban table, and fills `g_spirv_translate`, which the translators read. A translator that needs a
feature the device lacks takes its fallback and the log says which way it went, for example
`vulkan: HMMA runs on the device's cooperative matrices, ...` or `vulkan: an SM86 compute program's HMMA: ...`.

A fallback is not a second implementation of the pass. It is the same pass over the same data, with the work that the
missing feature would do written out: a shuffle instead of a quad broadcast, packed f16 adds instead of a matrix
multiply, a shared word instead of a subgroup reduction.

## Measuring one

Time the forms of one pass in one process, with the same input, and check the output as well as the time. The HMMA
paths have a bench of their own:

```sh
SNX_HMMA_BENCH=5 build/src/nx/snx-test-shader-sm86-gpu
```

It prints a median for each form and the largest relative difference of any form's output from the first one's, so a
faster form that computes something else cannot pass for a win. A shader of ours is measured against its previous
version and against its CPU reference, as
[the shader skill](../../../.claude/skills/snx-gpu-shader/SKILL.md) describes.

A measurement is of one device. Say which device, its driver and the version, because the ranking is not the same on
another: a path that wins on a desktop GPU can lose on a phone GPU, which is what the ban table is for. The numbers
behind this page, and the paths that are still measured per device, are in
[#2676](https://github.com/scarletnx/ScarletNX/issues/2676).
