<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# Driver quirks

What the GPU drivers that ScarletNX runs on get wrong, or do in a way that the renderer has to work around: what
breaks, where that is known from, what the renderer does about it and where, and what keeps it from coming back.

## Index

* [Where a workaround lives](#where-a-workaround-lives)
* [AMD's proprietary driver and AMDVLK](#amds-proprietary-driver-and-amdvlk)
* [RADV](#radv)
* [NVIDIA](#nvidia)
* [Intel](#intel)
* [Qualcomm's Adreno driver](#qualcomms-adreno-driver)
* [Turnip](#turnip)
* [Both Adreno drivers on Android](#both-adreno-drivers-on-android)
* [Arm's Mali driver](#arms-mali-driver)
* [PanVK](#panvk)
* [PowerVR](#powervr)
* [Samsung](#samsung)
* [MoltenVK](#moltenvk)
* [Every driver but NVIDIA's](#every-driver-but-nvidias)
* [Known elsewhere, not hit here](#known-elsewhere-not-hit-here)

## Where a workaround lives

- **The ban table**, `src/common/gpu/vk/extension_bans.txt` ([Driver bans](../../dev/development.md#driver-bans)): a
  line per driver and versions, with the extension or feature, what breaks, the source and the case that tests it
  again. An `off` line takes an extension out of the device's list, or turns a feature off where the renderer has a
  way without it. A `warn` line is only said in the log: the renderer has no way around it, or the ban is not proven
  for the way the renderer uses the feature. Every line that holds is said at the start, with the device, the driver,
  its version and the source. `vk_ext_<name>=on` goes over every line on an extension, and `vulkan_ban_ignore=KEY` over
  one key, to test a driver again.
- **Code that asks the driver**: a `driverID` or a vendor checked in the renderer, each listed below with its function
  and the commit that brought it.
- **A rule of `tests/architecture.py`**, where the workaround is to never use something at all.

`tests/vk_extension_bans.c` loads the table the build carries and checks each line's format; "a case" below means it
also checks that line on a stand-in device of that driver. `vk_pipeline_parts`, `vk_vertex_input` and
`vulkan_headless` load the table too. `snx-test-vk-bans --devices` lists the lines that hold for this machine's
drivers, `--match qualcomm 512.800.48 android` for a driver given by hand.

The sources: the case where it broke (a game, a test suite, a measure, an issue), a driver bug, or another project's
commit or file at a commit, read at the hashes the table's head names. A source that names no case is marked
*weak*: the line stays, and a test on that driver is what would settle it. Where Eden's driver bug list (Eden's
`docs/DriverBugs.md` at 9cf3a04af3) has the same bug, its row is cited beside the original as "Eden's list:ROW", the
line of that file.

## AMD's proprietary driver and AMDVLK

| What breaks | Source | What the renderer does, where | Kept by |
|---|---|---|---|
| Wrong depth values written with `VK_EXT_depth_clamp_control` | Dolphin 59e9267e3d (PR 13830), DriverDetails.cpp:168, for every AMD driver but RADV; Eden's list:16 | `off` on both drivers: a clamped depth stays in the viewport transform's range (`device.c`, `has_depth_clamp_range`) | vk_extension_bans, a case for each |
| A game crashes at its start with descriptor heaps (Adrenalin 26.7.1, RX 7900 XTX) | DXVK 076922bd24, issue 5837 | `VK_EXT_descriptor_heap` `off`: descriptors go through buffers or sets | vk_extension_bans |
| FP16 math wrong in one game from the 2026 drivers | Eden PR 3661 (9d2341eaea) | `shaderFloat16` `warn` | vk_extension_bans |
| Blend enable and equation as dynamic state (2023), a ban Eden lifted | yuzu d6d43e11a3, *weak*; lifted in Eden PR 3074 | `extendedDynamicState3ColorBlendEquation` `warn`, both drivers | vk_extension_bans |
| Primitive restart resets the GPU on GCN 4 (Windows) | Dolphin DriverDetails.cpp:108, VulkanContext.cpp:514 | `primitiveRestartEnable` `warn`: the renderer has no way around it | vk_extension_bans |
| Cube compatible images on GCN 4 and older | Eden vulkan_device.cpp:615 (4dc69c06f7), *weak*; Eden's list:11 | `VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT` `warn` | vk_extension_bans, a case |
| A demote or kill inside a loop lets the Windows compiler drop the quad's helpers while they hold the loop open: the loop never ends, until the GPU resets | ec9c208c6 | a fragment program's flow discards after its loop, never inside (`translate.h`, `discarded`) | none |
| No `D24_UNORM_S8_UINT` on every GPU | df80e8ade | D24S8 is made as `D32_SFLOAT_S8_UINT`, which holds the same values (`format.c`) | none |

## RADV

| What breaks | Source | What the renderer does, where | Kept by |
|---|---|---|---|
| Graphics pipelines crashed in the driver under ANGLE, which keeps them off there; DXVK and vkd3d-proton use them | ANGLE vk_renderer.cpp:6928, crbug 1497512 | `VK_EXT_graphics_pipeline_library` `warn` | vk_extension_bans |
| Depth clamp as dynamic state before 23.1, a ban Eden lifted | yuzu 856838f7ce, *weak*; lifted in Eden PR 3074 | `extendedDynamicState3DepthClampEnable` `warn` | vk_extension_bans, vk_pipeline_parts |
| Blend enable and equation as dynamic state (2023), a ban Eden lifted | yuzu da83afdeaf, *weak*; lifted in Eden PR 3074 | `extendedDynamicState3ColorBlendEquation` `warn` | vk_extension_bans |
| An attribute fetched at an offset that is not a multiple of its component's size (a stride of 69 for 4-byte floats) hung the GPU; Vulkan asks for that alignment | 283766472 | the stream is copied at a stride rounded to 4 (`render.c`, `guest_stride`, restride) | vulkan_headless (`vulkan_vertex_restride.inc`) |
| A point whose center is on a pixel's edge covers the pixel before it, where the console takes the pixel after | df3393042 | points move by a step of the subpixel grid up the guest's window axes (`render.c`, `point_nudge`) | none |
| A subgroup shuffle reads nothing from a helper lane the triangle's edge left out, where the console's SHFL reads it: a line along the edge | 3963d78b8 | a SHFL within a quad is a quad operation (`forms.c`, `shader_shfl_quad`) | none |
| amdgpu looks up every user-pointer buffer's pages at each submission, and RADV lists every buffer the device holds: 380 submissions a present cost 70% of a core | 5e226ae99 | on amdgpu (RADV, AMDVLK, AMD's Linux driver) vertices and constant buffers are copied, not imported (`uma.c`, `vk_uma_imports_dear`; `uma_copies` overrides) | vulkan_headless |
| NIR rounds a double to a float and back as no conversion, which hid every step of a directed rounding (lavapipe too) | 16784830f | the round trip is `NoContraction` (`sm86_translate.c`, `to_float_and_back`) | none |
| No blit of `D32_SFLOAT_S8_UINT` | 3db868fe1 | a one to one depth blit is a copy (`surface.c`) | none |

## NVIDIA

| What breaks | Source | What the renderer does, where | Kept by |
|---|---|---|---|
| Dynamic vertex input before 580.119.02 | Eden PR 3379 (9b1f6801a5), *weak*: the PR names no case | `VK_EXT_vertex_input_dynamic_state` `off`; `vertex_input_dynamic_state=on` goes over it | vk_extension_bans and vk_vertex_input, cases |
| Games hang at their first frames with present_id2 before 595 (590.48 with Smooth Motion) | DXVK dxvk_device_info.cpp:610, issue 5507 | `VK_KHR_present_id2` `off`: present_wait v1 | vk_extension_bans, a case |
| Transient glitches with pipeline libraries before 531 | ANGLE vk_renderer.cpp:6928, anglebug 42266655 | `VK_EXT_graphics_pipeline_library` `off` | vk_extension_bans |
| Multisample tests fail with rasterization samples as dynamic state (535, not 610) | ANGLE vk_renderer.cpp:6839 | `extendedDynamicState3RasterizationSamples` `off` before 610 | vk_extension_bans |
| Descriptor heaps run slower before 595.84 | DXVK dxvk_device_info.cpp:506 (b5fadf8e5a) | `VK_EXT_descriptor_heap` `warn` | vk_extension_bans |
| FP16 math wrong on Ampere and newer | yuzu 7277d7fe96, *weak*; Eden's list:18 | `shaderFloat16` `warn` | vk_extension_bans |
| A DenormPreserve for 16 bits keeps 32-bit denormals too, over a DenormFlushToZero for 32 | vkd3d-proton device.c:10180 | `shaderDenormPreserveFloat16` `warn` | vk_extension_bans |
| A partial clear of a multisampled target can lock the GPU up, several in one command buffer | Dolphin DriverDetails.cpp:116; Eden's list:23 | `vkCmdClearAttachments` `warn` | vk_extension_bans |
| The compiler deadlocked with pipelines made on several threads at once (580.178.04, FreeBSD, GT 1030) | 0aae8f941 | pipelines are made one at a time on NVIDIA (`vk_common.h`, `snx_vk_compile_serial`) | none |
| The memory budget of the BAR window counts the whole system's use (615.78): read as this process's, each frame's ring was freed and made again | 738f08448 | the room is the budget alone where use and budget add up to the heap (`cmd.c`, `bar_room`) | none |
| A 64 MiB ring in the BAR window mapped with no room in the kernel and reported success; CPU writes then ran at 18 MB/s (615.78, open module) | ad34ae723 | a ring in the window is 32 MiB at most, the rest goes to host memory (`cmd.c`, `BAR_RING_MAX`) | none |
| A GPU that stopped (Xid 109) signals nothing and reports no device lost | dc9a66b32 | GPU waits go in slices of a second, and a semaphore that has not moved for `VK_GPU_STUCK_NS` ends the run with the hang logged (`cmd.c`) | none |
| An allocation past 256 MiB fails where `maxMemoryAllocationSize` is larger (FreeBSD); a GT 1030 states 4 GiB and gives 240 MiB at once | 2ffa49851, 7f08e9143 | no allocation is bigger than `g_vk.alloc_cap`, which comes down at the first failure (`memory.c`, `vk_alloc_cap_lower`) | none |
| `shaderDenormFlushToZeroFloat32` is false, yet the GPU flushes the console's FTZ operations itself (615, RTX 2080); the flush per instruction made pipelines compile twice as long | e0bf932c1 | a probe measures it, and a device that flushes is translated without the flush (`ftz_probe.c`, `SH_FTZ_NATIVE`) | shader_direct_regressions |
| The compiler (621.24) made code that faulted the GPU of a pair of complementary guarded writes, and crashed on variants of it | 09f2f9732 | such a pair is one select (`sm86_translate.c`) | shader_sm86 |
| dma-buf mmap rejects a nonzero offset (Linux) | 27afddb0c | a full mapping at offset 0, moved with mremap (`plat_posix.c`) | none |
| The driver pins an import's pages | 606022c5e | the imports over pages that change are dropped before the change (`uma.c`, `pages_replaced`) | none |

## Intel

| What breaks | Source | What the renderer does, where | Kept by |
|---|---|---|---|
| Dynamic vertex input on the Windows driver before 27.20.100.0 | yuzu 69f9b97e7e, its bound in ddbf851ef6, *weak*; Eden's list:24 | `VK_EXT_vertex_input_dynamic_state` `off`; `vertex_input_dynamic_state=on` goes over it | vk_extension_bans and vk_vertex_input, cases |
| Dynamic vertex input still has issues from 27.20.100.0, and ANGLE keeps it off on every version | ANGLE vk_renderer.cpp:6632, anglebug 42265637 | `warn` | vk_extension_bans |
| A binding's stride as dynamic state before 100.9684 | ANGLE vk_renderer.cpp:6652, anglebug 42266992 | `VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE` `off`: the stride goes in the pipeline | vk_extension_bans |
| The Windows driver's SPIR-V compiler crashes on a game's FP16 shaders | yuzu ef6cc3aa1d; ANGLE anglebug 440941211 too | `shaderFloat16` `off`: the shaders do it in 32 bits | vk_extension_bans, a case |
| Alpha to coverage as dynamic state (Windows driver and ANV) | ANGLE vk_renderer.cpp:6787, crbug 577864200 | `extendedDynamicState3AlphaToCoverageEnable` `off` | vk_extension_bans |
| The clip range as dynamic state corrupts rendering (Windows driver) | ANGLE vk_renderer.cpp:6815 | `extendedDynamicState3DepthClipNegativeOneToOne` `off`: the range is the last stage's | vk_extension_bans, vk_pipeline_parts |
| Compute on the Windows driver's 101.4096 to 101.4382 (0.405.0 to 0.405.286 as VK_MAKE_VERSION reads them) | Eden vulkan_device.h:1070 (4dc69c06f7), *weak*; Eden's list:26 | `VK_SHADER_STAGE_COMPUTE_BIT` `warn`: compute has no way around | vk_extension_bans, a case |
| ANV breaks a viewport whose minDepth is above its maxDepth | Dolphin DriverDetails.cpp:122 | `VkViewport.minDepth` `warn` | vk_extension_bans |
| The Windows driver (igdumdim64) sets an unhandled-exception filter after the runtime's, which hid the reports of a fault in the game's code | fc4774857 | the fault is reported by a vectored handler, first in the chain (`plat_win32.c`) | none |
| Mesa's driver keeps two memory types over the same system memory, so the cached one costs the GPU nothing | 82069fa2c | the rings the CPU fills use cached memory there by default (`cmd.c`, `stage_cached`; `staging_memory` overrides) | none |

## Qualcomm's Adreno driver

| What breaks | Source | What the renderer does, where | Kept by |
|---|---|---|---|
| Dynamic vertex input drew edenprobe's coherence suite wrong (512.800.48); ANGLE keeps it off on every version | 5722fc560; ANGLE vk_renderer.cpp:6632, anglebug 381384988 | `VK_EXT_vertex_input_dynamic_state` `off` on Android | vk_extension_bans and vk_vertex_input, cases |
| Shaders that declare DenormFlushToZero for 32 bits miscompile: one game's waterfall drew its background's colour (Adreno 740, 512.676) | 33bf51294; Eden PR 4297 found the same | `shaderDenormFlushToZeroFloat32` `off`: FTZ per instruction | vk_extension_bans, a case |
| Under DenormFlushToZero a float load flushes an integer's bits that look like a denormal, and a jump table of small offsets reads 0 | 83f59d7e8 | constant buffer words load as integers (`emit_memory.c`) | none |
| An Fma rounds its product before the add (Adreno 830) | #1654 | `Fma` `warn` | vk_extension_bans, a case |
| A Grad sample picks its level once for each 2x2 quad (Adreno 830) | #1916 | `OpImageSampleExplicitLod` `warn` | vk_extension_bans, a case |
| Back faces take the front face's stencil reference (Adreno 830) | edenprobe 0.12.0's stencil suite (2026-11-12) | `VK_STENCIL_FACE_BACK_BIT` `warn` | vk_extension_bans, a case |
| A pipeline whose evaluation stage passes clip distances on fails to compile (Adreno 830) | edenprobe 0.12.0's geometry suite (2026-11-12) | `ClipDistance` `warn` | vk_extension_bans, a case |
| Pixels along shared edges misrender with advanced blending (Adreno 622 and 702) | ANGLE vk_renderer.cpp:6093 | `VK_EXT_blend_operation_advanced` `off` on those GPUs | vk_extension_bans, a case |
| Clears inside a pass race the draws around them before 512.762.12 | ANGLE vk_renderer.cpp:6380, issuetracker.google.com 166809097 | `vkCmdClearAttachments` `warn` | vk_extension_bans |
| 32-bit float depth is left uncleared by a clear | Dolphin DriverDetails.cpp:120; Eden's list:37 | `VK_FORMAT_D32_SFLOAT` `warn` | vk_extension_bans |
| A viewport whose minDepth is above its maxDepth | Dolphin DriverDetails.cpp:125; Eden's list:55 | `VkViewport.minDepth` `warn` | vk_extension_bans |
| Primitive restart | Dolphin DriverDetails.cpp:152 | `primitiveRestartEnable` `warn` | vk_extension_bans |
| Samplers with custom border colours crashed in a game (2023), a ban Eden lifted | yuzu b2aeb50229; lifted in Eden PR 3042 | `VK_EXT_custom_border_color` `warn` | vk_extension_bans |
| Timeline semaphores do not run properly on all of its drivers | yuzu 1462db4694, *weak* | `timelineSemaphore` `warn` | vk_extension_bans |
| 64-bit integers and atomics, which Eden turns off with no reason given | Eden PR 2732 (b389a72697), *weak*; Eden's list:32 | `shaderInt64` `warn` | vk_extension_bans |
| No USCALED or SSCALED vertex formats, which yuzu and Eden emulate in the shader; the renderer passes them as they are | yuzu 158a1896ec; Eden's list:31 | `VkVertexInputAttributeDescription.format` `warn` | vk_extension_bans, a case |
| Takes 32 vertex bindings where it reports fewer, and a draw past the count it reports is skipped (`render.c`) | Eden vulkan_device.cpp:659 (4dc69c06f7), *weak*; Eden's list:33 | `maxVertexInputBindings` `warn` | vk_extension_bans, a case |
| Turning rasterizer discard on corrupts the viewport state until it is set again | Eden's list:34, *weak* | `VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE` `warn` | vk_extension_bans, a case |
| `vkCmdCopyImageToBuffer` from an optimally tiled image allocates a staging image for each copy | Dolphin 81c817c54d, DriverDetails.cpp:163; Eden's list:36 | `warn`: readbacks copy that way | vk_extension_bans, a case |
| A descriptor pool's memory is made whole when the pool is: 98 MiB a frame's pool, 470 MiB for 1024 sets that could all hold texture arrays, 18 to 32 ms to make one | 51d7060be, 47446c32e | pools are sized for the sets that hold arrays apart, and a frame slot keeps the pools its last 512 uses took (`pipeline.c`, `vk_pool_sizes`; `cmd.c`, `vk_desc_pools_keep`) | none |
| The memory budget is a share of the phone's memory, blind to what the game and other processes hold: Android's low memory killer ended a game with the caches never trimmed (Adreno 830) | ffb7def43 | the caches grow by what the system can still give above a reserve (`uma_budget.h`) | vk_uma_budget |

## Turnip

| What breaks | Source | What the renderer does, where | Kept by |
|---|---|---|---|
| A 32-bit integer converted to a float is truncated, where the console rounds to nearest even (Adreno 610) | edenprobe's intmath suite (2026-11-11), 285b372be | `OpConvertSToF` and `OpConvertUToF` `off`: I2F.F32 converts the two halves and adds them | vk_extension_bans, cases |
| Lists the geometry stage for subgroup operations and gives wrong ballot and shuffle results there | Eden PR 4344 (b846577037) | `VK_SHADER_STAGE_GEOMETRY_BIT` `off`: a geometry program votes and shuffles as one lane | vk_extension_bans, cases |
| A shuffle whose index differs between lanes compiles to a loop, a hardware shuffle for each index: an HMMA ran 35 times slower (Adreno 610) | Mesa ir3_lower_subgroups.c; #2037 | `OpGroupNonUniformShuffle` `off`: HMMA stages A and B through shared memory | vk_extension_bans, cases |
| An Fma rounds its product before the add; a Grad sample picks its level once a quad (Adreno 610) | #1654, #1916 | `warn` | vk_extension_bans, a case |
| Illegal and HDR ASTC blocks decode transparent black, sRGB blocks up to 2 steps off (Adreno 610) | #1655 | `textureCompressionASTC_LDR` `warn`: `native_astc=0` decodes them as the specification says | vk_extension_bans, a case |
| Timeline semaphore problems on some devices (a Snapdragon 870) | yuzu 1462db4694 | `timelineSemaphore` `warn` | vk_extension_bans |
| Samplers with custom border colours (2023), a ban Eden lifted | yuzu b2aeb50229; lifted in Eden PR 3042 | `VK_EXT_custom_border_color` `warn` | vk_extension_bans |
| Vertex bindings and rasterizer discard, as on Qualcomm's driver | Eden's list:33 and 34, *weak* | `maxVertexInputBindings` and `VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE` `warn` | vk_extension_bans, a case |
| An Adreno 6xx has five hardware descriptor sets, and Turnip keeps one for dynamic buffer offsets: four bound sets, where the renderer's layout takes one for each of five stages, and every tessellation and geometry draw was skipped | 338b38978 | `tu_dont_reserve_descriptor_set=true` before any driver opens (`gpu_driver_load.c`, `turnip_sets`) | tests/gpu_driver_load.c |
| With that option, a dynamic buffer reads zeros and faults the GPU at address 0 in every layout, and the limits still say 16 (Mesa 273feeb6, by design) | 7976953b2 (#1940) | no descriptor of `src/nx` is a dynamic buffer: the UI gives its offset as a push constant (`ui.c`, `ui.vert`) | `tests/architecture.py`, `check_dynamic_descriptors` |

## Both Adreno drivers on Android

| What breaks | Source | What the renderer does, where | Kept by |
|---|---|---|---|
| The kernel (kgsl) ends a GPU job that runs past 2 s, and its fence never signals: an Adreno 610 took 7.1 s for a 2048x2048 level of the ETC2 build | 7bc183f09 | BCn is encoded again in submissions of 16384 blocks at most (`texconv.c`, `TC_ENCODE_SUBMIT_BLOCKS`) | astc_vulkan |

## Arm's Mali driver

| What breaks | Source | What the renderer does, where | Kept by |
|---|---|---|---|
| `vkCmdBindVertexBuffers2` applies strides to the wrong binding before r48 | ANGLE vk_renderer.cpp:6632 and 6652 (Arm errata SDEN-3735689) | `VK_EXT_vertex_input_dynamic_state` and `VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE` `off` | vk_extension_bans, cases |
| Pipeline libraries run unacceptably slower | ANGLE vk_renderer.cpp:6928, anglebug 404581992 | `VK_EXT_graphics_pipeline_library` `off` | vk_extension_bans |
| Dynamic state bugs before r44p1, and regressions on job manager GPUs from r46 to r51 | ANGLE vk_renderer.cpp:6612 (issuetracker.google.com 285124778, 336411904 and others) | `extendedDynamicState` `warn` | vk_extension_bans |
| `vkCmdSetCullMode` culls points and lines before r52 | ANGLE vk_renderer.cpp:6663 (Arm errata SDEN-3735689) | `VK_DYNAMIC_STATE_CULL_MODE` `warn` | vk_extension_bans |
| Topology as dynamic state with geometry shaders | ANGLE vk_renderer.cpp:6684, *weak*: "there seems to be a bug" | `VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY` `warn` | vk_extension_bans |
| Primitive restart | Dolphin DriverDetails.cpp:110 | `primitiveRestartEnable` `warn` | vk_extension_bans |
| A dynamic stencil write mask is ignored with discard or alpha to coverage where the pipeline's static mask is 0, before r43 | ANGLE vk_renderer.cpp:7092; Eden's list:44 | `VK_DYNAMIC_STATE_STENCIL_WRITE_MASK` `warn` | vk_extension_bans, a case |
| Readbacks through cached memory are much slower than through coherent memory, the CPU in the kernel's `__pi___inval_cache_range` | Dolphin DriverDetails.cpp:130; Eden's list:43 | `VK_MEMORY_PROPERTY_HOST_CACHED_BIT` `warn`: the readback rings are cached | vk_extension_bans, a case |
| No USCALED or SSCALED vertex formats, as on Adreno | yuzu 158a1896ec | `VkVertexInputAttributeDescription.format` `warn` | vk_extension_bans, a case |
| Mipmaps of 32-bit float RGBA images on some devices | Eden's list:48, *weak* | `VK_FORMAT_R32G32B32A32_SFLOAT` `warn` | vk_extension_bans, a case |

## PanVK

| What breaks | Source | What the renderer does, where | Kept by |
|---|---|---|---|
| Illegal and HDR ASTC blocks decode transparent black (Mali-G76) | #1898 | `textureCompressionASTC_LDR` `warn`: `native_astc=0` decodes them | vk_extension_bans, a case |
| The driver reads an import's pages on the CPU from the thread that holds the renderer's lock, up to a page a trap holds (#1875) | 9b874f118 | no window of guest memory that holds a trapped page is imported (`uma.c`, `imp_can`) | none |

## PowerVR

| What breaks | Source | What the renderer does, where | Kept by |
|---|---|---|---|
| dEQP fails with depth clamp as dynamic state; a polygon mode test with polygon mode; tests and the device with alpha to one | ANGLE vk_renderer.cpp:6808, 6830 and 6795 | `extendedDynamicState3DepthClampEnable`, `PolygonMode`, `AlphaToOneEnable` `off` | vk_extension_bans |
| Renders wrong with topology as dynamic state | ANGLE vk_renderer.cpp:6684, b/472763050 | `VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY` `warn` | vk_extension_bans |
| Culls nothing for a shader that writes both clip and cull distances (BXM-8-256) | PPSSPP thin3d_vulkan.cpp:1147 | `shaderCullDistance` `warn` | vk_extension_bans |

## Samsung

| What breaks | Source | What the renderer does, where | Kept by |
|---|---|---|---|
| Extended dynamic state 3, blend enable and equation among it | ANGLE vk_renderer.cpp:6732 (issues.angleproject.org 579500670); Eden PR 3074 | `VK_EXT_extended_dynamic_state3` `off` | vk_extension_bans |
| FP16, which ANGLE turns off with no reason given | ANGLE vk_renderer.cpp:7364, *weak* | `shaderFloat16` `warn` | vk_extension_bans |

## MoltenVK

| What breaks | Source | What the renderer does, where | Kept by |
|---|---|---|---|
| Fails every pipeline that lists a binding's stride as dynamic state: Metal has none | 5d9691ab2 | `VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE` `off`: the stride goes in the pipeline | vk_extension_bans, a case |
| A viewport whose minDepth is above its maxDepth | Dolphin DriverDetails.cpp:127; Eden's list:55 | `VkViewport.minDepth` `warn` | vk_extension_bans |
| Each timestamp gets a Metal encoder of its own that waits for the work before it: a scene ran 10 times slower, and the timestamps are unreliable on Apple GPUs | MoltenVK issues 1585 and 2378; 1e2067249 | `vk_gpu_profile` runs as 0 on a portability driver (`gpuprof_setting.h`) | vk_gpuprof_setting |
| Breaks with more than 16 vertex attributes or bindings, where it reports more | Eden vulkan_device.cpp:650 (4dc69c06f7), *weak*; Eden's list:50 | `maxVertexInputAttributes` `warn` | vk_extension_bans, a case |
| Discarded fragments of a program with early depth tests may come out corrupted, on Apple GPUs of family 4 and newer (macOS) | Dolphin DriverDetails.cpp:155; Eden's list:51 | `EarlyFragmentTests` `warn` | vk_extension_bans, a case |
| `HelperInvocation` reads inverted on AMD GPUs (macOS) | Dolphin DriverDetails.cpp:138; Eden's list:17 | `HelperInvocation` `warn` | vk_extension_bans, a case |
| Dynamic sampler indexing locks the GPU up on Intel GPUs (macOS): the texture arrays index by the handle | Dolphin DriverDetails.cpp:159; Eden's list:27 | `shaderSampledImageArrayNonUniformIndexing` `warn` | vk_extension_bans, a case |
| A subgroup max in a program that can discard gives garbage on Intel GPUs (macOS); the SM86 translator makes one of REDUX | Dolphin DriverDetails.cpp:143; Eden's list:28 | `OpGroupNonUniformUMax` `warn` | vk_extension_bans, a case |
| Null descriptors cause issues no report describes (iOS) | Eden's list:52, *weak* | `VK_EXT_robustness2.nullDescriptor` `warn` | vk_extension_bans, a case |

## Every driver but NVIDIA's

| What breaks | Source | What the renderer does, where | Kept by |
|---|---|---|---|
| A GPU that clips a huge primitive (AMD, Intel, Arm) interpolates the clipped vertices' inputs with less precision than the position, where the console interpolates the vertices' own values: a generic holding the position misses the fragment's pixel | Eden's position_varying_pass.cpp; 153da38c6 | a varying pass reads the position generic from the fragment's own place, on every driver but NVIDIA's (`render.c`, `draw_varying`) | shader_direct_regressions |
| Fragment shader interlock on AMD waits on more than the fragments that overlap: a feedback draw cost more with it (AMD iGPU, 59.1 ms against 55.3 drawn twice) | 242c613fe | a draw that samples its own target at its pixel uses the interlock on NVIDIA only, and is drawn twice elsewhere (`device.c`, `feedback_mode`) | vulkan_headless (`vulkan_index_draws.inc`) |

## Known elsewhere, not hit here

The rows of Eden's list that the renderer does not reach, and why.

| Eden's list | What | Why not here |
|---|---|---|
| 12 | AMD GCN 4: `VK_EXT_sampler_filter_minmax` | the renderer does not use the extension: a min or max sample is done in the shader |
| 13 | AMD: the last attribute wrong where offset + stride * count passes the buffer's size | not checked |
| 14 | AMD GCN: 2D mipmapped arrays of more than 6 square layers | not checked: no case seen |
| 15 | RADV: FP16 corrupt with the LLVM back end | RADV compiles with ACO unless `RADV_DEBUG=llvm` |
| 19, 25 | NVIDIA from 510 and Intel's Windows driver: no MSAA to MSAA blits | Vulkan forbids a blit from a multisampled image |
| 20 | NVIDIA: no shader stencil export | the renderer writes stencil by copies, not by the shader |
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
| 49 | Mali: matrix lowering in SkSL | the translator emits no matrices |
| 54 | PowerVR: a clear load op on a framebuffer of another render pass | the renderer uses dynamic rendering, with no framebuffer objects |
| 55 | AMD: a viewport whose minDepth is above its maxDepth | Dolphin, which the list follows there, names no AMD driver; not checked |
| OpenGL | Multithreaded shader compiles on Android; a glslang fix | the renderer is Vulkan |
