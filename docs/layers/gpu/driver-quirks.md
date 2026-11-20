<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# Driver quirks

What the GPU drivers that ScarletNX runs on get wrong, or do in a way that the renderer has to work around: what
breaks, where that is known from, what the renderer does about it and where, and what keeps it from coming back.

## Index

* [Where a workaround lives](#where-a-workaround-lives)
* [In the code](#in-the-code)
  * [AMD](#amd)
  * [NVIDIA](#nvidia)
  * [Intel](#intel)
  * [Adreno](#adreno)
  * [PanVK](#panvk)
  * [MoltenVK](#moltenvk)
  * [lavapipe](#lavapipe)
  * [Every driver but NVIDIA's](#every-driver-but-nvidias)
* [Mesh shaders](#mesh-shaders)
* [The ban table](#the-ban-table)
* [Known elsewhere, not hit here](#known-elsewhere-not-hit-here)

## Where a workaround lives

Two halves of one thing:

- **The ban table**, `src/common/gpu/vk/extension_bans.txt` ([Driver bans](../../dev/development.md#driver-bans)): a
  line per driver and versions, with the extension or feature, what breaks, the source and the case that tests it
  again. An `off` line takes an extension out of the device's list, or turns a feature off where the renderer has a
  way without it; a `warn` line is only said in the log: the renderer has no way around it, or the ban is not proven
  for the way the renderer uses the feature. Every line that holds is said at the start, with the device, the driver,
  its version and the source. `vk_ext_<name>=on` goes over every line on an extension, and `vulkan_ban_ignore=KEY` over
  one key, to test a driver again. [The ban table](#the-ban-table) below shows every line, written from the table by
  `scripts/check-docs.py --bans`; `docs_check` fails while the two differ, so a line is changed in the table alone.
  `tests/vk_extension_bans.c` loads the table the build carries and checks it (`vk_pipeline_parts`, `vk_vertex_input`
  and `vulkan_headless` load it too), and `snx-test-vk-bans --devices` lists the lines that hold for this machine's
  drivers, `--match qualcomm 512.800.48 android` for a driver given by hand.
- **The code**: what the renderer does on a driver for which no line of the table can stand, a `driverID` or a vendor
  checked where the work is done, or a rule of `tests/architecture.py` that keeps something out of the renderer.
  [In the code](#in-the-code) lists each, by hand, with the function, the commit that brought it and the test that
  keeps it, or none.

An extension whose driver lists it without one of the commands it requires needs neither: once the device is made,
each command of each extension it is made with is asked of the driver, and one that gives none takes its extension
out as an `off` line would, the device made again without it ([Driver bans](../../dev/development.md#driver-bans)). A
ban line on such a driver, as NVIDIA's on `VK_EXT_depth_clamp_control`, still says what broke and where.

A source is the case where it broke (a game, a test suite, a measure, an issue), a driver bug, or another project's
commit or file at a commit (the table's head names the hashes they were read at). Eden's driver bug list is Eden's
`docs/DriverBugs.md` at 9cf3a04af3; "Eden's driver bug list:N" is the line of its row.

## In the code

### AMD

| What breaks | Source | What the renderer does, where | Kept by |
|---|---|---|---|
| A demote or kill inside a loop lets AMD's Windows compiler drop the quad's helpers while they hold the loop open: the loop never ends, until the GPU resets | ec9c208c6 | a fragment program's flow discards after its loop, never inside (`translate.h`, `discarded`) | none |
| No `D24_UNORM_S8_UINT` on every GPU | ba8aec5f3 | D24S8 is made as `D32_SFLOAT_S8_UINT`, which holds the same values (`format.c`) | none |
| RADV hung the GPU fetching an attribute at an offset that is not a multiple of its component's size (a stride of 69 for 4-byte floats); Vulkan asks for that alignment | 283766472 | the stream is copied at a stride rounded to 4 (`render.c`, `guest_stride`, restride) | vulkan_headless (`vulkan_vertex_restride.inc`) |
| On RADV a point whose center is on a pixel's edge covers the pixel before it, where the console takes the pixel after | df3393042 | points move by a step of the subpixel grid up the guest's window axes (`render.c`, `point_nudge`) | none |
| On RADV a subgroup shuffle reads nothing from a helper lane the triangle's edge left out, where the console's SHFL reads it: a line along the edge | 3963d78b8 | a SHFL within a quad is a quad operation (`forms.c`, `shader_shfl_quad`) | none |
| amdgpu looks up every user-pointer buffer's pages at each submission, and RADV lists every buffer the device holds: 380 submissions a present cost 70% of a core | 5e226ae99 | on amdgpu (RADV, AMDVLK, AMD's Linux driver) vertices and constant buffers are copied, not imported (`uma.c`, `vk_uma_imports_dear`; `uma_copies` overrides) | vulkan_headless |
| NIR (RADV, lavapipe) takes a double rounded to a float and back for no conversion, which hid every step of a directed rounding | 16784830f | the round trip is `NoContraction` (`sm86_translate.c`, `to_float_and_back`) | none |
| RADV blits no `D32_SFLOAT_S8_UINT` | 3db868fe1 | a one to one depth blit is a copy (`surface.c`) | none |

### NVIDIA

| What breaks | Source | What the renderer does, where | Kept by |
|---|---|---|---|
| The compiler deadlocked with pipelines made on several threads at once (580.178.04, FreeBSD, GT 1030) | 0aae8f941 | pipelines are made one at a time on NVIDIA (`vk_common.h`, `snx_vk_compile_serial`) | none |
| The memory budget of the BAR window counts the whole system's use (615.78): read as this process's, each frame's ring was freed and made again | 738f08448 | the room is the budget alone where use and budget add up to the heap (`cmd.c`, `bar_room`) | none |
| A 64 MiB ring in the BAR window mapped with no room in the kernel and reported success; CPU writes then ran at 18 MB/s (615.78, open module) | ad34ae723 | a ring in the window is 32 MiB at most, the rest goes to host memory (`cmd.c`, `BAR_RING_MAX`) | none |
| A GPU that stopped (Xid 109) signals nothing and reports no device lost | dc9a66b32 | GPU waits go in slices of a second, and a semaphore that has not moved for `VK_GPU_STUCK_NS` ends the run with the hang logged (`cmd.c`) | none |
| An allocation past 256 MiB fails where `maxMemoryAllocationSize` is larger (FreeBSD); a GT 1030 states 4 GiB and gives 240 MiB at once | 2ffa49851, 7f08e9143 | no allocation is bigger than `g_vk.alloc_cap`, which comes down at the first failure (`memory.c`, `vk_alloc_cap_lower`) | none |
| `shaderDenormFlushToZeroFloat32` is false, yet the GPU flushes the console's FTZ operations itself (615, RTX 2080); the flush per instruction made pipelines compile twice as long | e0bf932c1 | a probe measures it, and a device that flushes is translated without the flush (`ftz_probe.c`, `SH_FTZ_NATIVE`) | shader_direct_regressions |
| The compiler (621.24) made code that faulted the GPU of a pair of complementary guarded writes, and crashed on variants of it | 09f2f9732 | such a pair is one select (`sm86_translate.c`) | shader_sm86 |
| dma-buf mmap rejects a nonzero offset (Linux) | 27afddb0c | a full mapping at offset 0, moved with mremap (`plat_posix.c`) | none |
| The driver pins an import's pages | 606022c5e | the imports over pages that change are dropped before the change (`uma.c`, `pages_replaced`) | none |

### Intel

| What breaks | Source | What the renderer does, where | Kept by |
|---|---|---|---|
| The Windows driver (igdumdim64) sets an unhandled-exception filter after the runtime's, which hid the reports of a fault in the game's code | fc4774857 | the fault is reported by a vectored handler, first in the chain (`plat_win32.c`) | none |
| Mesa's driver keeps two memory types over the same system memory, so the cached one costs the GPU nothing | 82069fa2c | the rings the CPU fills use cached memory there by default (`cmd.c`, `stage_cached`; `staging_memory` overrides) | none |

### Adreno

| What breaks | Source | What the renderer does, where | Kept by |
|---|---|---|---|
| An Adreno 6xx has five hardware descriptor sets, and Turnip keeps one for dynamic buffer offsets: four bound sets, where the renderer's layout takes one for each of five stages, and every tessellation and geometry draw was skipped | 338b38978 | `tu_dont_reserve_descriptor_set=true` before any driver opens (`gpu_driver_load.c`, `turnip_sets`) | `tests/gpu_driver_load.c` |
| With that option, a dynamic buffer reads zeros and faults the GPU at address 0 in every layout, and the limits still say 16 (Mesa 273feeb6, by design) | 7976953b2 (#1940) | no descriptor of `src/nx` is a dynamic buffer: the UI gives its offset as a push constant (`ui.c`, `ui.vert`) | `tests/architecture.py`, `check_dynamic_descriptors` |
| Under DenormFlushToZero, Qualcomm's driver flushes the bits of an integer loaded as a float that look like a denormal, and a jump table of small offsets reads 0 | 83f59d7e8 | constant buffer words load as integers (`emit_memory.c`) | none |
| Qualcomm's driver makes a descriptor pool's memory whole when the pool is: 98 MiB a frame's pool, 470 MiB for 1024 sets that could all hold texture arrays, 18 to 32 ms to make one | 51d7060be, 47446c32e | pools are sized for the sets that hold arrays apart, and a frame slot keeps the pools its last 512 uses took (`pipeline.c`, `vk_pool_sizes`; `cmd.c`, `vk_desc_pools_keep`) | none |
| The memory budget is a share of the phone's memory, blind to what the game and other processes hold: Android's low memory killer ended a game with the caches never trimmed (Adreno 830) | ffb7def43 | the caches grow by what the system can still give above a reserve (`uma_budget.h`) | vk_uma_budget |
| Android's kernel (kgsl) ends a GPU job that runs past 2 s, and its fence never signals: an Adreno 610 took 7.1 s for a 2048x2048 level of the ETC2 build | 7bc183f09 | BCn is encoded again in submissions of 16384 blocks at most (`texconv.c`, `TC_ENCODE_SUBMIT_BLOCKS`) | astc_vulkan |

### PanVK

| What breaks | Source | What the renderer does, where | Kept by |
|---|---|---|---|
| The driver reads an import's pages on the CPU from the thread that holds the renderer's lock, up to a page a trap holds (#1875) | 9b874f118 | no window of guest memory that holds a trapped page is imported (`uma.c`, `imp_can`) | none |

### MoltenVK

| What breaks | Source | What the renderer does, where | Kept by |
|---|---|---|---|
| Each timestamp gets a Metal encoder of its own that waits for the work before it: a scene ran 10 times slower, and the timestamps are unreliable on Apple GPUs | MoltenVK issues 1585 and 2378; 1e2067249 | `vk_gpu_profile` runs as 0 on a portability driver (`gpuprof_setting.h`) | vk_gpuprof_setting |

### lavapipe

| What breaks | Source | What the renderer does, where | Kept by |
|---|---|---|---|
| The push constants of every stage a draw binds go to the linear rasterizer as one buffer, their ranges added up (a shader object's, or a pipeline library's layout each), and `lp_fs_linear_run` copies that buffer into a stack array of 256 bytes' worth: two stages of 224 bytes smashed the stack (Mesa 26.2.6) | #2461 | constant buffers are bound one by one, without windows of the staging ring, on lavapipe (`cb_windows.h`); `vk_cb_windows=1` takes them there with `LP_PERF=no_rast_linear` | vk_cb_windows, vulkan_headless_cb_windows |

### Every driver but NVIDIA's

| What breaks | Source | What the renderer does, where | Kept by |
|---|---|---|---|
| A GPU that clips a huge primitive (AMD, Intel, Arm) interpolates the clipped vertices' inputs with less precision than the position, where the console interpolates the vertices' own values: a generic holding the position misses the fragment's pixel | Eden's position_varying_pass.cpp; 153da38c6 | a varying pass reads the position generic from the fragment's own place, on every driver but NVIDIA's (`render.c`, `draw_varying`) | shader_direct_regressions |
| Fragment shader interlock on AMD waits on more than the fragments that overlap: a feedback draw cost more with it (AMD iGPU, 59.1 ms against 55.3 drawn twice) | 242c613fe | a draw that samples its own target at its pixel uses the interlock on NVIDIA only, and is drawn twice elsewhere (`device.c`, `feedback_mode`) | vulkan_headless (`vulkan_index_draws.inc`) |

## Mesh shaders

An Ounce game's mesh draws (`docs/platforms/nx/ounce.md#draws`) are the one place where a draw submits work to a
stage other than the graphics pipeline's first, and the shape that has faulted or hung the drivers is the task and
mesh stages **in one submission**: a task workgroup launches mesh workgroups, and the hardware copies its payload into
each of them. NVIDIA's driver faulted the GPU with that shape in most menu runs of an Ounce game, which its line in
the ban table records. RADV on RDNA4 is reported to hang it on an RX 9070 XT (`NX2navi#930`), at about 32 fps where a
compute path ran at 96 to 105. On an RX 6700 XT (RADV, Mesa 26.2.6) the same shape drew 213,192 mesh draws in one run
from boot into a race with no fault, and an RX 9060 XT (RADV GFX1200, Mesa 26.2.6) 48,000 in a 90 second run of the
first scene of one game, so the family alone does not decide it.

Our way around the shape is `vk_mesh_path` (`docs/user/settings.md`): Auto, the default, runs the task program as a
compute dispatch that writes its payload, its counts and the draw's workgroup record into a storage buffer, and draws
the mesh program alone, with no task stage. That keeps the console's programs and their outputs and removes the
gang submission; `Native` puts the two stages back together for a device whose path is measured to work. A direct
draw and an indirect one both take the compute path: the indirect one's task count is its record's, and the dispatch
reads it there (`vkCmdDispatchIndirect`), so the host needs no value the GPU holds. One buffer serves the draws of a
frame, with a barrier around each dispatch; the push constant gives the slots it holds, and the task program stores
the groups it dispatched in the draw's record for the mesh program to read, so both agree on the slots' places. A
device that binds graphics descriptors in heaps keeps the native path: the heap bind is the graphics pipeline's, and
a compute dispatch cannot use it.

A mesh workgroup that was not launched (its x is at or past its task workgroup's count) **calls
`OpSetMeshOutputsEXT` with 0 and 0 before it returns**: RADV (Mesa 26.2.6, RX 9060 XT) hangs the GPU on a mesh
workgroup that returns without it, which is what a direct `OpReturn` in the culled case did. Nothing else of the
mesh program changes.

The buffer holds as many task workgroups as the payload's size allows (`sm86_mesh_task_cap`, 327 for the 3200-byte
payload one game's largest programs take, 1024 for the smallest). A draw of more task workgroups than that keeps the
first ones, which the log says once for a direct draw.

What keeps the compute path from regressing: `vulkan_headless_mesh` draws a task program that writes a depth into its
payload and a mesh program that reads it, through the native path, the compute path, and an indirect draw's record,
and checks the same depths (`tests/vulkan_mesh_draw.inc`); `vulkan_headless_mesh_native` draws them with the native
path chosen. The `vulkan:` stats line counts the mesh draws that take the compute path (`N with a compute task`).

Measured here (RX 9060 XT, RADV Mesa 26.2.6, an Ounce game's first 3D scene, the same script): the native path hangs
the GPU the moment an **indirect** task+mesh gang is submitted (three resets, the hang dump naming
`DISPATCH_TASKMESH_INDIRECT_MULTI_ACE` with the native `TaskEXT` shader); the compute path ran 120 seconds with
23,204 mesh draws through it, none skipped, and 90 seconds of the same scene at 36.9 frames a second against 35.8 for
the native build's run, so the dispatch it adds costs nothing measurable there.

## The ban table

Each line of `src/common/gpu/vk/extension_bans.txt`, by driver, as `scripts/check-docs.py --bans` writes it: a change
goes in the table, and the rows below are written again from it. The table's comments say what the renderer does
without each extension or feature.

<!-- made by scripts/check-docs.py --bans from src/common/gpu/vk/extension_bans.txt: edit the table -->
| Driver | Key | Action | Versions | System | GPUs | What breaks | Source | Tested again by | Last tested |
|---|---|---|---|---|---|---|---|---|---|
| `amd-proprietary` | `shaderFloat16` | off | all | any | every GPU | gets FP16 math wrong from its 2026 versions in one game, by Eden's account | Eden PR 3661 (9d2341eaea, 2026-03-03) | `new:amberlith:the HADD2, HMUL2 and HFMA2 cases against the console's` | never |
| `amd-proprietary` | `VK_EXT_descriptor_heap` | off | all | any | every GPU | crashes a game at start with descriptor heaps (Adrenalin 26.7.1 on an RX 7900 XTX) | DXVK 076922bd24 (2026-09-19), issue 5837 | `headless:--vk-descriptors-mode=3` | never |
| `amd-proprietary` | `VK_EXT_extended_dynamic_state3.extendedDynamicState3ColorBlendEquation` | warn | all | any | every GPU | broke blend enable and equation as dynamic state (2023); Eden lifted the ban | yuzu d6d43e11a3 (2023-08-01); lifted in Eden PR 3074 (5edcdea78f, 2026-01-07) | `new:amberlith:blending suites with blend state dynamic` | never |
| `amd-proprietary` | `VK_EXT_depth_clamp_control` | off | all | any | every GPU | writes wrong depth values to the depth buffer with depth clamp control | Dolphin 59e9267e3d (2025-07-26, PR 13830), DriverDetails.cpp:168; Eden's driver bug list (docs/DriverBugs.md:16, 9cf3a04af3) | `headless:` | never |
| `amd-proprietary` | `primitiveRestartEnable` | warn | all | windows | every GPU | resets the GPU with primitive restart on GCN 4 | Dolphin DriverDetails.cpp:108, VulkanContext.cpp:514 | `new:amberlith:strips with restart indices` | never |
| `amd-proprietary` | `VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT` | warn | all | any | every GPU | breaks cube compatible images on GCN 4 and older GPUs, which Eden tells by their lack of shaderFloat16 | Eden's driver bug list:11; Eden vulkan\_device.cpp:615 (4dc69c06f7) | `new:amberlith:cube maps sampled after their faces are drawn to` | never |
| `amdvlk` | `VK_EXT_extended_dynamic_state3.extendedDynamicState3ColorBlendEquation` | warn | all | any | every GPU | broke blend enable and equation as dynamic state (2023); Eden lifted the ban | yuzu d6d43e11a3 (2023-08-01); lifted in Eden PR 3074 (5edcdea78f, 2026-01-07) | `new:amberlith:blending suites with blend state dynamic` | never |
| `amdvlk` | `VK_EXT_depth_clamp_control` | off | all | any | every GPU | writes wrong depth values to the depth buffer with depth clamp control, as Dolphin has it for every AMD driver but RADV | Dolphin 59e9267e3d (2025-07-26, PR 13830), DriverDetails.cpp:168, its AMD driver every one but RADV (VulkanContext.cpp:993-1004) | `headless:` | never |
| `anv` | `VK_EXT_extended_dynamic_state3.extendedDynamicState3AlphaToCoverageEnable` | off | all | any | every GPU | has driver bugs with alpha to coverage as dynamic state | ANGLE vk\_renderer.cpp:6787, crbug 577864200 | `new:amberlith:alpha to coverage cases with it dynamic` | never |
| `anv` | `VkViewport.minDepth` | warn | all | any | every GPU | breaks a viewport whose minDepth is above its maxDepth | Dolphin DriverDetails.cpp:122; Eden's driver bug list:55 | `new:amberlith:depth with a reversed viewport range` | never |
| `intel-windows` | `VK_EXT_vertex_input_dynamic_state` | off; `vertex_input_dynamic_state=on` goes over it | `< 27.20.100.0` | any | every GPU | breaks dynamic vertex input | yuzu 69f9b97e7e (2021-06-15), bounded in ddbf851ef6 (2022-12-27), kept by Eden; Eden's driver bug list:24 | `headless:--vertex-input-dynamic-state=on` | never |
| `intel-windows` | `VK_EXT_vertex_input_dynamic_state` | off | `>= 27.20.100.0` | any | every GPU | still has issues with dynamic vertex input by ANGLE's account, which keeps it off on every version | ANGLE vk\_renderer.cpp:6632, anglebug 42265637 | `headless:--vertex-input-dynamic-state=on` | never |
| `intel-windows` | `VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE` | off | `< 100.9684` | any | every GPU | breaks dynamic vertex strides | ANGLE vk\_renderer.cpp:6652 | `headless:` | never |
| `intel-windows` | `shaderFloat16` | off | all | any | every GPU | crashes its SPIR-V compiler on a game's shaders that use FP16 | yuzu ef6cc3aa1d (2021-01-27), kept by Eden | `new:headless:FP16 arithmetic in each stage, compiled and read back` | never |
| `intel-windows` | `VK_EXT_extended_dynamic_state3.extendedDynamicState3AlphaToCoverageEnable` | off | all | any | every GPU | fails framebuffer fetch and state change tests with alpha to coverage as dynamic state | ANGLE vk\_renderer.cpp:6787 | `new:amberlith:alpha to coverage cases with it dynamic` | never |
| `intel-windows` | `VK_EXT_extended_dynamic_state3.extendedDynamicState3DepthClipNegativeOneToOne` | off | all | any | every GPU | corrupts rendering with the depth clip range as dynamic state | ANGLE vk\_renderer.cpp:6815 | `headless:` | never |
| `intel-windows` | `VK_SHADER_STAGE_COMPUTE_BIT` | warn | `>= 101.4096 < 101.4382` | any | every GPU | breaks compute (its 0.405.0 to 0.405.286, as VK\_MAKE\_VERSION reads them) | Eden's driver bug list:26; Eden vulkan\_device.h:1070 (4dc69c06f7) | `headless:` | never |
| `mali` | `VK_EXT_vertex_input_dynamic_state` | off; `vertex_input_dynamic_state=on` goes over it | `< 48.0.0` | any | every GPU | applies vkCmdBindVertexBuffers2's strides to the wrong binding (Arm errata SDEN-3735689) | ANGLE vk\_renderer.cpp:6632 | `headless:--vertex-input-dynamic-state=on` | never |
| `mali` | `VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE` | off | `< 48.0.0` | any | every GPU | applies vkCmdBindVertexBuffers2's strides to the wrong binding (Arm errata SDEN-3735689) | ANGLE vk\_renderer.cpp:6652 | `headless:` | never |
| `mali` | `VK_EXT_graphics_pipeline_library` | off | all | any | every GPU | runs unacceptably slower with pipeline libraries | ANGLE vk\_renderer.cpp:6928, anglebug 404581992 | `new:amberlith:the graphics suites timed with pipeline libraries on and off` | never |
| `mali` | `extendedDynamicState` | warn | `< 44.1.0` | any | every GPU | has several dynamic state bugs, fixed in r44p1 (r44p0 on Pixels) | ANGLE vk\_renderer.cpp:6612 (issuetracker.google.com 285124778, 285196249, 286224923, 287318431) | `amberlith:coherence` | never |
| `mali` | `extendedDynamicState` | warn | `>= 46.0.0 < 51.0.0` | any | every GPU | has dynamic state regressions on job manager GPUs (Midgard and Bifrost), fixed in r51 | ANGLE vk\_renderer.cpp:6612 (issuetracker.google.com 336411904) | `amberlith:coherence` | never |
| `mali` | `VK_DYNAMIC_STATE_CULL_MODE` | warn | `< 52.0.0` | any | every GPU | culls points and lines with the cull mode set by vkCmdSetCullMode (Arm errata SDEN-3735689) | ANGLE vk\_renderer.cpp:6663 | `new:amberlith:points and lines drawn with front and back culling` | never |
| `mali` | `VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY` | warn | all | any | every GPU | breaks topology as dynamic state with geometry shaders | ANGLE vk\_renderer.cpp:6684 | `new:amberlith:geometry shader draws switching topology` | never |
| `mali` | `primitiveRestartEnable` | warn | all | any | every GPU | breaks primitive restart | Dolphin DriverDetails.cpp:110 | `new:amberlith:strips with restart indices` | never |
| `mali` | `VkVertexInputAttributeDescription.format` | warn | all | any | every GPU | has no USCALED or SSCALED vertex formats, which yuzu emulates in the shader and the renderer passes as they are | yuzu 158a1896ec (2023-02-18) | `new:amberlith:scaled vertex attributes of each size read back` | never |
| `mali` | `VK_MEMORY_PROPERTY_HOST_CACHED_BIT` | warn | all | any | every GPU | reads back through cached memory much slower than through coherent memory, the CPU in the kernel's \_\_pi\_\_\_inval\_cache\_range, where the renderer's readback rings are cached | Dolphin DriverDetails.cpp:130; Eden's driver bug list:43 | `new:headless:a readback through each memory type, timed` | never |
| `mali` | `VK_DYNAMIC_STATE_STENCIL_WRITE_MASK` | warn | `< 43.0.0` | any | every GPU | ignores a dynamic stencil write mask with discard or alpha to coverage where the pipeline's static mask is 0 | ANGLE vk\_renderer.cpp:7092; Eden's driver bug list:44 | `new:amberlith:stencil writes beside a discard, the mask set dynamically` | never |
| `mali` | `VK_FORMAT_R32G32B32A32_SFLOAT` | warn | all | android | every GPU | breaks the mipmaps of 32-bit float RGBA images on some devices | Eden's driver bug list:48 | `new:amberlith:an RGBA32F texture's levels sampled` | never |
| `moltenvk` | `VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE` | off | all | any | every GPU | fails every pipeline that lists it: Metal has no dynamic stride | ScarletNX 5d9691ab2 (2026-10-21) | `headless:` | 2026-10-21 broken, version not recorded |
| `moltenvk` | `VkViewport.minDepth` | warn | all | any | every GPU | breaks a viewport whose minDepth is above its maxDepth | Dolphin DriverDetails.cpp:127; Eden's driver bug list:55 | `new:amberlith:depth with a reversed viewport range` | never |
| `moltenvk` | `maxVertexInputAttributes` | warn | all | any | every GPU | breaks a draw with more than 16 vertex attributes or bindings, where it reports more | Eden's driver bug list:50; Eden vulkan\_device.cpp:650 (4dc69c06f7) | `new:headless:a draw with 17 vertex attributes` | never |
| `moltenvk` | `EarlyFragmentTests` | warn | all | macos | every GPU | may corrupt the fragments a program with early depth tests discards, on Apple GPUs of family 4 and newer | Dolphin DriverDetails.cpp:155; Eden's driver bug list:51 | `new:amberlith:a discarding fragment program with early tests` | never |
| `moltenvk` | `HelperInvocation` | warn | all | macos | every GPU | reads a fragment's HelperInvocation inverted on AMD GPUs | Dolphin DriverDetails.cpp:138; Eden's driver bug list:17 | `new:headless:a fragment program's HelperInvocation read back along a triangle's edge` | never |
| `moltenvk` | `shaderSampledImageArrayNonUniformIndexing` | warn | all | macos | every GPU | locks the GPU up with dynamic sampler indexing on Intel GPUs, which the texture arrays index by the handle | Dolphin DriverDetails.cpp:159; Eden's driver bug list:27 | `headless:` | never |
| `moltenvk` | `OpGroupNonUniformUMax` | warn | all | macos | every GPU | gives garbage for a subgroup max in a program that can discard, on Intel GPUs | Dolphin DriverDetails.cpp:143; Eden's driver bug list:28 | `new:headless:an SM86 fragment program's REDUX beside a KIL read back` | never |
| `moltenvk` | `VK_EXT_robustness2.nullDescriptor` | warn | all | ios | every GPU | has issues with null descriptors that no report describes | Eden's driver bug list:52 | `headless:` | never |
| `nvidia` | `VK_EXT_vertex_input_dynamic_state` | off; `vertex_input_dynamic_state=on` goes over it | `< 580.119.02` | any | every GPU | breaks dynamic vertex input | Eden PR 3379 (9b1f6801a5, 2026-02-05) | `headless:--vertex-input-dynamic-state=on` | never |
| `nvidia` | `shaderFloat16` | off | all | any | every GPU | gets FP16 math wrong on Ampere and newer GPUs by yuzu's account, which Eden still follows | yuzu 7277d7fe96 (2021-07-03); Eden's driver bug list:18 | `new:amberlith:the HADD2, HMUL2 and HFMA2 cases against the console's` | never |
| `nvidia` | `shaderDenormPreserveFloat16` | off | all | any | every GPU | lets a DenormPreserve for 16 bits keep 32-bit denormals too, over a DenormFlushToZero for 32 bits | vkd3d-proton device.c:10180 | `new:headless:one shader declaring both, its FP32 subnormals read back` | never |
| `nvidia` | `VK_KHR_present_id2` | off | `< 595.0.0` | any | every GPU | hangs games at their first frames with present\_id2 and present\_wait2 (590.48, with Smooth Motion on) | DXVK dxvk\_device\_info.cpp:610, issue 5507 | `new:amberlith:a windowed run with frame_pacing=2` | never |
| `nvidia` | `VK_EXT_descriptor_heap` | off | `< 595.84.0` | any | every GPU | runs descriptor heaps slower than its later drivers do | DXVK dxvk\_device\_info.cpp:506 (b5fadf8e5a, 2026-06-18) | `new:headless:--bench-descriptors under vk_descriptors_mode=3 against vk_descriptors_mode=1` | never |
| `nvidia` | `VK_EXT_mesh_shader` | off | all | any | every GPU | faulted the GPU in most menu runs of an Ounce game with its mesh draws on an RTX 2080: Xid 31 and 109, then a lost device, and 2 of 7 runs still hung once both mesh stages saw the sets of both | ScarletNX 0002310032 and 62f2ec0570 (2026-11-05), docs/platforms/nx/ounce.md#draws | `headless:--vk-ext-mesh-shader=on` | 2026-11-05 broken on 615.78.08 (RTX 2080) |
| `nvidia` | `VK_EXT_graphics_pipeline_library` | off | `< 531.0.0` | any | every GPU | draws transient glitches with pipeline libraries | ANGLE vk\_renderer.cpp:6928, anglebug 42266655 | `new:amberlith:the graphics suites with pipeline libraries on` | never |
| `nvidia` | `VK_EXT_extended_dynamic_state3.extendedDynamicState3RasterizationSamples` | off | `< 610.0.0` | any | every GPU | fails multisample tests with rasterization samples as dynamic state (seen on 535, not on 610) | ANGLE vk\_renderer.cpp:6839 | `new:amberlith:multisample suites with samples dynamic` | never |
| `nvidia` | `VK_EXT_depth_clamp_control` | off | all | any | every GPU | lists the extension and its depthClampControl but gives no vkCmdSetDepthClampRangeEXT: the renderer crashed calling it, and with the range left out a menu drew wrong | #2280 (621.01 on Windows); a tester's runs on 620.70.06 under Linux, clean with the extension off | `new:headless:the depth clamp range as dynamic state, its command present` | 2026-11-15 broken on 620.70.06 and 621.01 |
| `nvidia` | `vkCmdClearAttachments` | warn | all | any | every GPU | can lock the GPU up clearing part of a multisampled target, several clears in one command buffer | Dolphin DriverDetails.cpp:116; Eden's driver bug list:23 | `new:amberlith:partial clears of a multisampled target, several a frame` | never |
| `panvk` | `textureCompressionASTC_LDR` | warn | all | any | every GPU | decodes an illegal or HDR ASTC block as transparent black where the specification says opaque magenta (seen on a Mali-G76); native\_astc=0 decodes them as the specification says | #1898 | `amberlith:astc: an illegal block is the error colour` | 2026-11-12 broken on 26.99.99 (Mali-G76 MC4) |
| `powervr` | `VK_EXT_extended_dynamic_state3.extendedDynamicState3DepthClampEnable` | off | all | any | every GPU | fails dEQP tests with depth clamp as dynamic state | ANGLE vk\_renderer.cpp:6808 | `new:amberlith:depth clamp cases with it dynamic` | never |
| `powervr` | `VK_EXT_extended_dynamic_state3.extendedDynamicState3PolygonMode` | off | all | any | every GPU | fails a polygon mode test with polygon mode as dynamic state | ANGLE vk\_renderer.cpp:6830 | `new:amberlith:polygon mode cases with it dynamic` | never |
| `powervr` | `VK_EXT_extended_dynamic_state3.extendedDynamicState3AlphaToOneEnable` | off | all | any | every GPU | fails tests and loses the device with alpha to one as dynamic state | ANGLE vk\_renderer.cpp:6795 | `new:amberlith:alpha to one cases with it dynamic` | never |
| `powervr` | `VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY` | warn | all | any | every GPU | renders wrong with topology as dynamic state | ANGLE vk\_renderer.cpp:6684, b/472763050 | `new:amberlith:draws switching topology` | never |
| `powervr` | `shaderCullDistance` | warn | all | any | every GPU | culls nothing for a shader that writes both clip and cull distances (BXM-8-256) | PPSSPP thin3d\_vulkan.cpp:1147 | `new:amberlith:a draw with clip and cull distances both written` | never |
| `qualcomm` | `VK_EXT_vertex_input_dynamic_state` | off; `vertex_input_dynamic_state=on` goes over it | all | android | every GPU | draws edenprobe's coherence suite wrong with dynamic vertex input | ScarletNX 5722fc560 (2026-11-04); ANGLE vk\_renderer.cpp:6632 keeps it off too (anglebug 381384988) | `amberlith:coherence` | 2026-11-04 broken on 512.800.48 |
| `qualcomm` | `shaderDenormFlushToZeroFloat32` | off | all | any | every GPU | miscompiles shaders that declare DenormFlushToZero for 32 bits: one game's waterfall shader drew its background's colour on an Adreno 740 | ScarletNX 33bf51294 (2026-10-22); Eden PR 4297 (60a474b8df, 2026-08-24) found the same | `new:headless:a fragment shader declaring DenormFlushToZero whose FMUL, FADD and FFMA results are read back` | 2026-10-22 broken on 512.676 |
| `qualcomm` | `shaderInt64` | warn | all | any | every GPU | has 64-bit integers and atomics turned off by Eden, which gives no reason | Eden PR 2732 (b389a72697, 2025-10-14); Eden's driver bug list:32 | `new:amberlith:64-bit integer arithmetic in a shader` | never |
| `qualcomm` | `Fma` | warn | all | any | every GPU | rounds an Fma's product before the add, as OpFMul then OpFAdd, where the console's FFMA keeps it exact (seen on an Adreno 830) | #1654 | `amberlith:fmath: a multiply and an add in one call` | 2026-11-12 broken on 512.800.48 (Adreno 830) |
| `qualcomm` | `OpImageSampleExplicitLod` | warn | all | any | every GPU | picks a Grad sample's level of detail from its gradients once for each 2x2 quad, so a pixel whose gradients differ from its quad neighbour's reads the neighbour's level (seen on an Adreno 830) | #1916 | `amberlith:lod: textureGrad along x picks levels 0 to 3` | 2026-11-12 broken on 512.800.48 (Adreno 830) |
| `qualcomm` | `VK_STENCIL_FACE_BACK_BIT` | warn | all | any | every GPU | gives back faces the front face's stencil reference when vkCmdSetStencilReference sets the two apart, in Z24S8 and ZF32\_X24S8 (seen on an Adreno 830) | edenprobe 0.12.0's stencil suite on an Adreno 830 phone (2026-11-12) | `amberlith:stencil: z24s8: each winding takes its own face's reference` | 2026-11-12 broken on 512.800.48 (Adreno 830) |
| `qualcomm` | `ClipDistance` | warn | all | any | every GPU | fails to make a pipeline whose evaluation stage reads clip distances and writes them on (VK\_ERROR\_UNKNOWN, its log saying the shader did not compile), and the draws that need it are dropped (seen on an Adreno 830) | edenprobe 0.12.0's geometry suite on an Adreno 830 phone (2026-11-12) | `amberlith:geometry: a positive first clip distance passed on by the evaluation stage clips nothing` | 2026-11-12 broken on 512.800.48 (Adreno 830) |
| `qualcomm` | `VK_EXT_blend_operation_advanced` | off | all | any | `0x6020200,0x7000201` | misrenders pixels along shared primitive edges with advanced blending (Adreno 622 and 702) | ANGLE vk\_renderer.cpp:6093 | `new:amberlith:advanced blend over triangles that share an edge` | never |
| `qualcomm` | `VK_EXT_custom_border_color` | warn | all | any | every GPU | crashed creating samplers with custom border colours in a game (2023); Eden lifted the ban | yuzu b2aeb50229 (2023-04-01); lifted in Eden PR 3042 (f7f6a4cde4, 2025-11-21) | `new:amberlith:samplers with custom border colours of every format` | never |
| `qualcomm` | `timelineSemaphore` | warn | all | any | every GPU | does not run timeline semaphores properly on all of its drivers | yuzu 1462db4694 (2023-07-03) | `new:headless:timeline waits and signals across the two queues` | never |
| `qualcomm` | `primitiveRestartEnable` | warn | all | any | every GPU | breaks primitive restart | Dolphin DriverDetails.cpp:152 | `new:amberlith:strips with restart indices` | never |
| `qualcomm` | `VkViewport.minDepth` | warn | all | any | every GPU | breaks a viewport whose minDepth is above its maxDepth | Dolphin DriverDetails.cpp:125; Eden's driver bug list:55 | `new:amberlith:depth with a reversed viewport range` | never |
| `qualcomm` | `VK_FORMAT_D32_SFLOAT` | warn | all | any | every GPU | leaves 32-bit float depth uncleared by a clear | Dolphin DriverDetails.cpp:120; Eden's driver bug list:37 | `new:amberlith:D32 depth clears read back` | never |
| `qualcomm` | `vkCmdClearAttachments` | warn | `< 512.762.12` | any | every GPU | races clears inside a pass with the draws around them | ANGLE vk\_renderer.cpp:6380, issuetracker.google.com 166809097 | `new:amberlith:clears between draws in one pass` | never |
| `qualcomm` | `VkVertexInputAttributeDescription.format` | warn | all | any | every GPU | has no USCALED or SSCALED vertex formats, which yuzu and Eden emulate in the shader and the renderer passes as they are | yuzu 158a1896ec (2023-02-18); Eden's driver bug list:31 | `new:amberlith:scaled vertex attributes of each size read back` | never |
| `qualcomm` | `maxVertexInputBindings` | warn | all | any | every GPU | takes 32 vertex bindings where it reports fewer, and the renderer skips a draw past the count reported | Eden's driver bug list:33; Eden vulkan\_device.cpp:659 (4dc69c06f7) | `new:headless:a draw with 32 vertex bindings` | never |
| `qualcomm` | `VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE` | warn | all | any | every GPU | corrupts the viewport state when rasterizer discard is turned on, until the viewport is set again | Eden's driver bug list:34 | `new:amberlith:draws after a transform feedback pass with rasterization off` | never |
| `qualcomm` | `vkCmdCopyImageToBuffer` | warn | all | any | every GPU | allocates a staging image for each copy from an optimally tiled image, which slows the renderer's readbacks | Dolphin 81c817c54d (2022-09-23), DriverDetails.cpp:163; Eden's driver bug list:36 | `new:headless:a render target read back, timed` | never |
| `radv` | `VK_EXT_graphics_pipeline_library` | warn | all | any | every GPU | crashed creating graphics pipelines with it under ANGLE, which keeps it off on RADV; DXVK and vkd3d-proton use it there | ANGLE vk\_renderer.cpp:6928, crbug 1497512 | `new:headless:pipelines made from libraries and fast-linked` | never |
| `radv` | `VK_EXT_extended_dynamic_state3.extendedDynamicState3DepthClampEnable` | warn | `< 23.1.0` | any | every GPU | broke depth clamp as dynamic state by yuzu's account, a ban Eden lifted | yuzu 856838f7ce (2023-05-13); lifted in Eden PR 3074 (5edcdea78f, 2026-01-07) | `new:amberlith:depth clamp cases with it dynamic` | never |
| `radv` | `VK_EXT_extended_dynamic_state3.extendedDynamicState3ColorBlendEquation` | warn | all | any | every GPU | broke blend enable and equation as dynamic state (2023); Eden lifted the ban | yuzu da83afdeaf (2023-03-15); lifted in Eden PR 3074 (5edcdea78f, 2026-01-07) | `new:amberlith:blending suites with blend state dynamic` | never |
| `samsung` | `shaderFloat16` | warn | all | any | every GPU | has FP16 turned off by ANGLE, which gives no reason | ANGLE vk\_renderer.cpp:7364 | `new:headless:FP16 arithmetic in each stage, compiled and read back` | never |
| `samsung` | `VK_EXT_extended_dynamic_state3` | off | all | any | every GPU | has bugs with extended dynamic state 3, blend enable and equation among them | ANGLE vk\_renderer.cpp:6732 (issues.angleproject.org/579500670); Eden PR 3074 (5edcdea78f, 2026-01-07) | `new:amberlith:blending suites with blend state dynamic` | never |
| `turnip` | `OpConvertSToF` | off | all | any | every GPU | truncates a 32-bit signed integer it converts to a float, where the console rounds to nearest even (seen on an Adreno 610) | edenprobe's intmath suite on an Adreno 610 phone (2026-11-11) | `amberlith:intmath: a signed integer converts to the nearest float, ties to even` | 2026-11-11 broken on 26.2.99 (Adreno 610) |
| `turnip` | `OpConvertUToF` | off | all | any | every GPU | truncates a 32-bit unsigned integer it converts to a float, where the console rounds to nearest even (seen on an Adreno 610) | edenprobe's intmath suite on an Adreno 610 phone (2026-11-11) | `amberlith:intmath: an unsigned integer converts to the nearest float, ties to even` | 2026-11-11 broken on 26.2.99 (Adreno 610) |
| `turnip` | `Fma` | warn | all | any | every GPU | rounds an Fma's product before the add, as OpFMul then OpFAdd, where the console's FFMA keeps it exact (seen on an Adreno 610) | #1654 | `amberlith:fmath: a multiply and an add in one call` | 2026-11-12 broken on 26.2.99 (Adreno 610) |
| `turnip` | `OpImageSampleExplicitLod` | warn | all | any | every GPU | picks a Grad sample's level of detail from its gradients once for each 2x2 quad, so a pixel whose gradients differ from its quad neighbour's reads the neighbour's level (seen on an Adreno 610) | #1916 | `amberlith:lod: textureGrad along x picks levels 0 to 3` | 2026-11-12 broken on 26.2.99 (Adreno 610) |
| `turnip` | `VK_SHADER_STAGE_GEOMETRY_BIT` | off | all | any | every GPU | lists the geometry stage for subgroup operations (supportedStages) and gives wrong ballot and shuffle results there, which lost a game's meshes and textures; Qualcomm's own driver does not list the stage | Eden PR 4344 (b846577037, 2026-09-01) | `new:headless:a geometry shader's ballot and shuffle results read back` | never |
| `turnip` | `OpGroupNonUniformShuffle` | off | all | any | every GPU | compiles a shuffle whose index differs between invocations into a loop, a hardware shuffle for each distinct index, as many as 32 for an HMMA's gather: an Adreno 610 ran the HMMA bench 35 times faster staged through shared memory (684.9 ms a dispatch to 19.6), where Qualcomm's driver on an Adreno 830 and RADV ran it 5.4 and 2.2 times slower so; no feature or property says it | Mesa's src/freedreno/ir3/ir3\_lower\_subgroups.c (make\_shuffle\_uniform, main of 2026-11-04: every Adreno with shfl); ScarletNX #2037 | `new:headless:an HMMA program's dispatch timed gathered and staged, as shader_sm86_gpu's SNX_HMMA_BENCH does` | 2026-11-13 a loop on 26.2.99 (Adreno 610) |
| `turnip` | `VK_EXT_custom_border_color` | warn | all | any | every GPU | crashed creating samplers with custom border colours in a game (2023); Eden lifted the ban | yuzu b2aeb50229 (2023-04-01); lifted in Eden PR 3042 (f7f6a4cde4, 2025-11-21) | `new:amberlith:samplers with custom border colours of every format` | never |
| `turnip` | `textureCompressionASTC_LDR` | warn | all | any | every GPU | decodes an illegal or HDR ASTC block as transparent black where the specification says opaque magenta, and sRGB ASTC texels up to 2 steps off (seen on an Adreno 610); native\_astc=0 decodes them as the specification says but for a step of sRGB | #1655 | `amberlith:astc: an illegal block is the error colour` | 2026-11-11 broken on 26.2.99 (Adreno 610) |
| `turnip` | `timelineSemaphore` | warn | all | any | every GPU | has timeline semaphore problems on some devices (a ZTE handset's Snapdragon 870) | yuzu 1462db4694 (2023-07-03) | `new:headless:timeline waits and signals across the two queues` | never |
| `turnip` | `timelineSemaphore` | off | all | android | every GPU | emulates timeline semaphores over Android's KGSL kernel driver, which took up to half of DXVK's frame rate on an Adreno 735 with Turnip 24.3.0 | DXVK issue 4484 (2024-11; Turnip's werman there: "On KGSL ... we have timeline semaphores emulated") | `new:headless:timeline waits and signals across the two queues` | never |
| `turnip` | `maxVertexInputBindings` | warn | all | any | every GPU | takes 32 vertex bindings where it reports fewer, and the renderer skips a draw past the count reported | Eden's driver bug list:33; Eden vulkan\_device.cpp:659 (4dc69c06f7) | `new:headless:a draw with 32 vertex bindings` | never |
| `turnip` | `VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE` | warn | all | any | every GPU | corrupts the viewport state when rasterizer discard is turned on, until the viewport is set again | Eden's driver bug list:34 | `new:amberlith:draws after a transform feedback pass with rasterization off` | never |
<!-- end of the rows scripts/check-docs.py --bans makes -->

## Known elsewhere, not hit here

The rows of Eden's driver bug list that the renderer does not reach, and why. The rows it does reach are lines of the
table, their source naming the row.

| Row | What | Why not here |
|---|---|---|
| 12 | AMD GCN 4: `VK_EXT_sampler_filter_minmax` | the renderer does not use the extension: a min or max sample is done in the shader |
| 13 | AMD: the last attribute wrong where offset + stride * count passes the buffer's size | not checked |
| 14 | AMD GCN: 2D mipmapped arrays of more than 6 square layers | not checked |
| 15 | RADV: FP16 corrupt with the LLVM back end | RADV compiles with ACO unless `RADV_DEBUG=llvm` |
| 19, 25 | NVIDIA from 510 and Intel's Windows driver: no MSAA to MSAA blits | Vulkan forbids a blit from a multisampled image |
| 20 | NVIDIA: no shader stencil export | no shader of the renderer exports stencil |
| 21 | NVIDIA: barriers in conditional flow | Eden's own code turns them off on Intel (vulkan_device.cpp:517), not NVIDIA; not checked |
| 22, 53 | NVIDIA before Turing, MoltenVK on iOS: push descriptors | the renderer uses no push descriptors |
| 29 | Intel's Mesa driver: lines in geometry shaders with clip distances written in the vertex shader | not checked: no source names the case |
| 30 | Qualcomm: heap exhaustion past 75% of `maxSamplerAllocationCount` (65536 on the A8xx) | the renderer keeps 3072 samplers at most (`texture.c`, `vk_sampler`) |
| 35 | Qualcomm: concurrent fence waits | not checked |
| 38 | Qualcomm's Android R drivers: resources released before a command pool's reset | not checked: a bug of a few months' drivers |
| 39, 40, 41 | Adreno 4xx and 5xx (Pixel races, OpPhi, an extended blend function) | GPUs with no Vulkan 1.2, which the renderer needs (`device.c`) |
| 42 | Mali: a green screen in various places | no case to check against |
| 45 | Mali: a fence wait after a device loss | a lost device ends the run |
| 46, 47 | Galaxy S7 and S9: suballocated images, primary command buffers only | Mali GPUs with no Vulkan 1.2 |
| 49 | Mali: more than 32 matrix temporaries | the translators declare no matrix type |
| 54 | PowerVR: a clear load op on a framebuffer of another render pass | the renderer uses dynamic rendering, with no framebuffer objects |
| 55 | AMD: a viewport whose minDepth is above its maxDepth | Dolphin, which the list follows there, names no AMD driver; not checked |
| OpenGL | Multithreaded shader compiles on Android; a glslang fix | the renderer is Vulkan |
