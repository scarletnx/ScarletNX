<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# Graphics pipeline libraries

This is the design for building graphics pipelines from `VK_EXT_graphics_pipeline_library` (GPL) parts. Each
shader stage compiles once, as a library. A draw links the libraries with its state in a few milliseconds. A worker
then builds the optimized pipeline in the background, and the draws switch to it when it is ready. On a device
without GPL the renderer works as it does now.

Status: design. Nothing in this file is implemented yet.

## Index

* [The problem](#the-problem)
* [What the device gives](#what-the-device-gives)
* [The four parts](#the-four-parts)
  * [Where each PipeKey field goes](#where-each-pipekey-field-goes)
  * [State that becomes dynamic](#state-that-becomes-dynamic)
  * [The multisample state of the fragment shader part](#the-multisample-state-of-the-fragment-shader-part)
* [Layouts](#layouts)
* [Keys and caches](#keys-and-caches)
* [The draw: link, then optimize](#the-draw-link-then-optimize)
* [Across runs](#across-runs)
* [The programs a game ships](#the-programs-a-game-ships)
* [Interactions](#interactions)
* [Without the extension](#without-the-extension)
* [Measurement](#measurement)
* [Commits](#commits)
* [Rejected alternatives](#rejected-alternatives)
* [What comes from vkd3d-proton](#what-comes-from-vkd3d-proton)
* [Open questions](#open-questions)
* [Risks](#risks)

## The problem

`pipe_create` (`src/nx/gpu/vk/pipeline.c`) builds each graphics pipeline whole: all of its shader stages and all of
the draw state in `PipeKey` (`src/nx/gpu/vk/vk.h`) go into one `vkCreateGraphicsPipelines`. Two things follow:

- The driver compiles every stage again for each new state combination of the same shaders. A new blend mode or a
  new render target format on a known shader costs a full compile.
- The Ounce game has SM86 vertex programs of about 9,350 instructions and 1.7 MB of SPIR-V. One pipeline with such
  a program takes 6 to 10 s in the NVIDIA driver.

The workers (`pipeline_threads`, `pipe_worker`) make new pipelines while the GPU thread goes on. The recording
thread waits for each one at its bind (`vk_pipeline_resolve` through `bind_pipeline_run` in `record.c`). A draw never
skips for its pipeline, because the skip broke one game's menus (the comment above `typedef struct Pipe`). So the
time of a full compile is a stall in the frame.

The lock fixes of 92df965c9 moved the waits for the GPU out of `vk_lock`. Before them, a landing held `vk_lock` up to
7.9 s while the recording thread waited for a pipeline. They did not change how long the recording thread waits. This
design makes that wait short. It does not change the lock order or the places that wait.

## What the device gives

The RTX 2080 (NVIDIA 615.78.08) reports these, from `vulkaninfo`:

| Feature or property | Value |
| --- | --- |
| `graphicsPipelineLibrary` | true |
| `graphicsPipelineLibraryFastLinking` | true |
| `graphicsPipelineLibraryIndependentInterpolationDecoration` | true |
| `extendedDynamicState2PatchControlPoints`, `extendedDynamicState2LogicOp` | true |
| `extendedDynamicState3` (all 31 members, among them `PolygonMode`, `DepthClampEnable`, `ProvokingVertexMode`, `ConservativeRasterizationMode`, `ExtraPrimitiveOverestimationSize`, `RasterizationSamples`, `SampleMask`, `AlphaToCoverageEnable`, `AlphaToOneEnable`, `ColorBlendEnable`, `ColorBlendEquation`, `ColorWriteMask`, `LogicOpEnable`, the coverage modulation states) | true |
| `dynamicPrimitiveTopologyUnrestricted` | true |
| `attachmentFeedbackLoopDynamicState` | true |
| `vertexInputDynamicState`, `descriptorBuffer`, `shaderObject`, `pipelineBinaries`, `maintenance5`, `maintenance6` | true |

RADV has GPL with fast link on all of its GPUs, and most of EDS3. ANV has GPL. The design must not assume a member of
EDS3 that a device does not report: each one is a choice at device creation (see the table below).

## The four parts

GPL splits a graphics pipeline into four parts. Each part is a pipeline made with `VK_PIPELINE_CREATE_LIBRARY_BIT_KHR`
and a `VkGraphicsPipelineLibraryCreateInfoEXT` that names it:

- **Vertex input interface (VI)**: `pVertexInputState`, `pInputAssemblyState`. Cheap.
- **Pre-rasterization shaders (PR)**: the vertex, tessellation and geometry stages (later mesh and task),
  `pViewportState`, `pRasterizationState`, `pTessellationState`, the layout, and `viewMask` of
  `VkPipelineRenderingCreateInfo`. This is the expensive part for the Ounce game.
- **Fragment shader (FS)**: the fragment stage, `pDepthStencilState`, a `pMultisampleState` when sample shading is
  on, the layout and `viewMask`.
- **Fragment output interface (FO)**: `pColorBlendState`, `pMultisampleState`, the attachment formats of
  `VkPipelineRenderingCreateInfo` and `VkAttachmentSampleCountInfoNV`. Cheap.

Only PR and FS compile shaders. VI and FO hold fixed-function state, and the driver makes them in microseconds. So
the plan keeps most fixed-function state static in VI and FO, where a new value costs almost nothing. It makes state
dynamic only where it would otherwise go into PR or FS. The optimized pipeline then keeps static blend state, which
is the best case for the GPU.

### Where each PipeKey field goes

| `PipeKey` field | Part | How |
| --- | --- | --- |
| `vs`, `tcs`, `tes`, `gs` | PR | the modules, in the PR key |
| `patch` | PR | dynamic (`VK_DYNAMIC_STATE_PATCH_CONTROL_POINTS_EXT`) with `extendedDynamicState2PatchControlPoints`, else in the PR key |
| `topo_class` | VI | in the VI key. The topology is dynamic already (`VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY`) |
| `polygon` | PR | dynamic with `extendedDynamicState3PolygonMode`, else in the PR key |
| `depth_clamp` | PR | dynamic with `extendedDynamicState3DepthClampEnable`, else in the PR key |
| `vi` | VI | in the VI key. All zero with dynamic vertex input, as now |
| `ncolor`, `color[8]`, `depth`, `stencil` | FO | in the FO key (formats cannot be dynamic) |
| `blend[8]` | FO | in the FO key |
| `feedback` | FS and FO | dynamic with `has_feedback_dynamic` (the case here). Without it, the flag goes on both parts and on the link (see the open questions) |
| `provoking_last` | PR | dynamic with `extendedDynamicState3ProvokingVertexMode`, else in the PR key |
| `logic_op` | FO | in the FO key |
| `arrays` | PR and FS | each part keeps the bits of its own sets (see [Layouts](#layouts)) |
| `samples` | FO | in the FO key. Also in the FS key when `sample_shading` is set |
| `coverage` | FO | in the FO key (rare state, mixed samples) |
| `sample_off`, `alpha_to_coverage`, `alpha_to_one` | FO | in the FO key. Also in the FS key when `sample_shading` is set |
| `sample_shading` | FS and FO | in both keys |
| `conservative` | PR | dynamic with `extendedDynamicState3ConservativeRasterizationMode` and `ExtraPrimitiveOverestimationSize`, else in the PR key |
| `fs` | FS | the module (or `null_fs()`), in the FS key |

Some state goes into every part and into the link, and it is the same for all pipelines of a run. These are the
`VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT` flag with `has_descriptor_buffers`
(VUID-VkGraphicsPipelineCreateInfo-flags-12355 and 12356), the `VkPipelineRobustnessCreateInfoEXT` with
`has_vertex_robustness2`, and `viewMask` 0. They are not in the keys.

### State that becomes dynamic

The draws set these in `render.c` beside the dynamic state that they set now (the `DYN` macro and `g_dyn`, so a value
goes into the command buffer only when it changes). Each one needs a `rec_CmdSet...` wrapper in `record.c`, and the
`DYN` cache must start again for each command buffer, as it does now.

| State | Call | Feature |
| --- | --- | --- |
| polygon mode | `vkCmdSetPolygonModeEXT` | `extendedDynamicState3PolygonMode` |
| depth clamp | `vkCmdSetDepthClampEnableEXT` | `extendedDynamicState3DepthClampEnable` |
| provoking vertex | `vkCmdSetProvokingVertexModeEXT` | `extendedDynamicState3ProvokingVertexMode` |
| conservative mode and its size | `vkCmdSetConservativeRasterizationModeEXT`, `vkCmdSetExtraPrimitiveOverestimationSizeEXT` | the two `extendedDynamicState3` members, with `has_conservative` |
| patch control points | `vkCmdSetPatchControlPointsEXT` | `extendedDynamicState2PatchControlPoints` |

Each state becomes dynamic only on the GPL path, and only when its feature is there. A state without its feature
stays in the PR key, as a static value. This keeps the whole-pipeline path of today unchanged, and a device with
partial EDS3 still works. `g_vk` gets one bit for each state (`g_vk.dyn_pr`, a mask), which the key split and the
draw both read. The device must enable `VK_EXT_extended_dynamic_state3` and the members that it uses, and
`extendedDynamicState2PatchControlPoints`.

Depth clamp as dynamic state keeps the meaning of today. Without `VK_EXT_depth_clip_enable`, depth clip is the
opposite of depth clamp, for a static value and for a dynamic one.

Each part lists only the dynamic states of its own subset in its `pDynamicState`, as vkd3d-proton does with its
`VKD3D_VERTEX_INPUT_DYNAMIC_STATE_MASK` and `VKD3D_FRAGMENT_OUTPUT_DYNAMIC_STATE_MASK`. The link and the optimized
pipeline get the union from their parts. One function (`pipe_dynamic(part, out)`) builds each list from the list of
`pipe_create` now, so the whole pipeline and the parts cannot disagree.

### The multisample state of the fragment shader part

The FS part can take `pMultisampleState` as NULL, and the FO part must have one. When the FS part has one, the two
must be "identically defined" (VUID-VkGraphicsPipelineCreateInfo-pLibraries-06635). When sample shading is on, the
FS part must have one, and it must also match (06636, 06637). So:

- With `sample_shading` 0 (nearly all draws), the FS part has no multisample state. Its key does not hold `samples`,
  and one FS part links with every sample count.
- With `sample_shading` 1, the FS key holds the whole multisample key (`samples`, `coverage`, `sample_off`,
  `alpha_to_coverage`, `alpha_to_one`). One function builds the `VkPipelineMultisampleStateCreateInfo` for both
  parts from the same fields, so the two are the same by construction.

## Layouts

The layouts now are `vk_playout(arrays)`. They are 32 at most, one for each value of the five bits of `arrays`. Set
N is `g_vk_dslayout[bit N][N]`, by `shader_vk_set`: set 0 vertex (and compute), set 1 fragment, sets 2 to 4
tessellation control, tessellation evaluation and geometry. There is one push constant range of 80 bytes for
`VK_PUSH_STAGES`.

A link with a PR part and an FS part must use layouts that agree. Without `VK_PIPELINE_LAYOUT_CREATE_INDEPENDENT_SETS_BIT_EXT`,
both libraries must use the same layout (VUID-VkGraphicsPipelineCreateInfo-pLibraries-06681 and layout-07827).
That would put bit 1 (the indexed textures of the fragment program) into the PR key, and the vertex bits into the FS
key. A 1.7 MB vertex program would then compile two times, once for each kind of fragment program that it meets. The
startup compile could not know which.

So the GPL path makes every graphics layout with `INDEPENDENT_SETS`:

- The PR part uses the layout of its own sets: sets 0, 2, 3 and 4 from the bits of its stages, set 1
  `VK_NULL_HANDLE`. The graphicsPipelineLibrary feature allows null set layouts
  (VUID-VkPipelineLayoutCreateInfo-graphicsPipelineLibrary-06753).
- The FS part uses a layout with set 1 only (the others null).
- The link and the optimized pipeline use the union, `vk_playout(arrays)`, made with the same flag
  (VUID-VkGraphicsPipelineCreateInfo-flags-06729 and 06730).

All three have the same push constant range, as the push constant VUIDs (06620, 06621) need. The null-set layouts
come from a second table beside `g_playouts`, keyed by the part (PR or FS) and the bits of the part. That is 16 PR
layouts and 2 FS layouts at most.

Vulkan calls two layouts compatible for set N only when both or neither have `INDEPENDENT_SETS`. Thus the binds of
graphics descriptor sets must use the same family as the pipeline. They do: `render.c` binds with
`vk_playout(k->arrays)`, and `vk_playout` makes the flagged family when GPL is on. Push constants go through
`g_vk_playout`. Push constant compatibility looks only at the ranges, so `g_vk_playout` stays good for that.

Compute must keep a layout without the flag. Now `g_vk.cs_playout` is `g_vk_playout` when there are no descriptor
buffers (`vk_pipeline_init`). With GPL on, `vk_pipeline_init` makes `g_vk.cs_playout` apart, from
`g_vk.cs_dslayout` without the flag, as it does now for descriptor buffers. The bind in `compute.c:1890` already
uses `g_vk.cs_playout`.

Descriptor buffers: the set layouts keep `VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT`. Every part,
the link and the optimized pipeline get `VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT`. The offsets of the bindings
in a set do not change with `INDEPENDENT_SETS`, because the set layouts are the same objects. The descriptor buffer
code in `descriptors.c` needs no change, but the commit must check it under the validation layer.

## Keys and caches

Each part has a key struct with no padding, zeroed before it is filled, and compared with `memcmp`. This is the
method of `PipeKey` now and of vkd3d-proton's `vkd3d_vertex_input_pipeline_desc` and
`vkd3d_fragment_output_pipeline_desc`. One function splits a `PipeKey` into the four keys:

```c
typedef struct { u32 topo_class; VertexLayout vi; } PipeViKey;
typedef struct {
    VkShaderModule vs, tcs, tes, gs;
    u32 arrays;                       /* bits 0, 2, 3, 4 */
    u32 patch, polygon, depth_clamp;  /* 0 where g_vk.dyn_pr makes them dynamic */
    u32 provoking_last, conservative;
} PipePrKey;
typedef struct {
    VkShaderModule fs;                /* or null_fs() */
    u32 arrays;                       /* bit 1 */
    u32 feedback;                     /* 0 with has_feedback_dynamic */
    u32 sample_shading;
    u32 samples, sample_off, alpha_to_coverage, alpha_to_one;  /* 0 without sample_shading */
    VkCoverage coverage;
} PipeFsKey;
typedef struct {
    u32 ncolor, color[8], depth, stencil;
    struct { u32 en, sc, dc, cop, sa, da, aop, mask; } blend[8];
    u32 logic_op, samples, sample_off, alpha_to_coverage, alpha_to_one, sample_shading, feedback;
    VkCoverage coverage;
    u32 has_fs;                       /* the write masks are 0 without a fragment program, as in pipe_create */
} PipeFoKey;
void vk_pipe_split(const PipeKey* k, PipeViKey* vi, PipePrKey* pr, PipeFsKey* fs, PipeFoKey* fo);
```

`vk_pipe_split` is a pure function of `PipeKey` and of the device bits (`g_vk.dyn_pr`, `has_feedback_dynamic`). A
test in `tests/` checks it, with no device: two keys that differ only in a field of one part give the same keys of
the other parts.

The caches:

- Four tables of `PipeLib` entries, one table for each part. An entry holds the key, its hash, the `VkPipeline` of
  the library, a ready flag, and a queued flag, as `Pipe` does now. The tables live in a new file,
  `src/nx/gpu/vk/pipeline_lib.c`, with one global (`g_plib`), by the rule of `snx-code-quality`. pipeline.c is 2,200
  lines now. The new file keeps the parts apart from the whole pipelines.
- The GPU thread looks up and adds entries, as it does for `g_pipeh` now (`pipe_entry` runs under `vk_lock` on the
  GPU thread). The workers read an entry only through its ready flag, with release and acquire order, as
  `Pipe.ready` does now. The reading thread of the pipeline file adds its entries to its own table, and
  `pf_merge` puts them in on the GPU thread, as it does for `g_pf.pipes` now.
- The library pipelines live as long as the run, as the shader modules and the pipelines do now. A count is
  thousands of PR and FS parts at most, and tens of VI and FO parts.
- `Pipe` gets the indices of its four parts, a `fast` pipeline (the link) and an `opt` pipeline (the optimized one).
  `Pipe.p` stays as "the best one now", for the code that reads it.

PR and FS parts use `g_pcache`, so the driver keeps their compiled code across runs in `vk_pipelines.bin`. VI and FO
parts and the links use no cache: they are cheap, and a link with LTO off gives no data worth a cache entry.

## The draw: link, then optimize

`vk_pipeline_get_deferred` keeps its contract: it gives a pipeline or a stand-in, and the recording thread waits for a
stand-in at its bind. Inside it, a new `Pipe` on the GPL path goes this way:

1. `vk_pipe_split` gives the four keys. Each key finds its entry, or adds one.
2. A part that is new goes to the workers. A PR or FS part goes to the head of the draws' queue. A VI or FO part is
   made on the GPU thread at once, because it costs microseconds. A part that the startup pass queued
   (`prewarm`) moves to the draws' queue, as `pipe_promote` does now.
3. When the PR and FS parts are ready, a worker links the four with `VkPipelineLibraryCreateInfoKHR`, with no
   `LINK_TIME_OPTIMIZATION` flag. This is the fast link. It sets `Pipe.fast` and the ready flag.
4. The same worker then queues the optimized pipeline on a third queue, below the draws' queue and the file's queue.
   It is the same link with `VK_PIPELINE_CREATE_LINK_TIME_OPTIMIZATION_BIT_EXT`, into `g_pcache`. All parts are made
   with `VK_PIPELINE_CREATE_RETAIN_LINK_TIME_OPTIMIZATION_INFO_BIT_EXT` (VUID-VkGraphicsPipelineCreateInfo-flags-06609).
   When it is done, it sets `Pipe.opt`.

If all parts are ready at step 1, the GPU thread makes the fast link itself, in about 1 ms, and gives the real
pipeline with no stand-in. The measurement must show if that cost is small enough on the GPU thread. If it is not, the
link goes to a worker as in step 3.

The stand-in resolves to the best pipeline at its bind. `vk_pipeline_resolve`, `vk_pipeline_ready` and
`vk_pipeline_now` read `opt` first, then `fast`, each with acquire order. They wait (only `resolve`) while neither is
there, which is the time of a part compile and a link. The wait never includes the optimized pipeline.

The swap:

- `vk_pipeline_get_deferred` gives the stand-in while `opt` is not ready, and the `opt` handle after. `render.c`
  keeps the handle in `g_draw.pipe` and binds again when it changes (`g_dyn.pipe`). Thus a draw after the swap binds
  `opt`. A draw that kept the stand-in resolves to `opt` at its next bind, and `g_dyn` starts again with each command
  buffer.
- The `g_draw.check` path in `render.c` compares `vk_pipeline_ready` of two handles of the same `Pipe` at one time.
  Both give the same result, so the swap does not make it report a difference.
- The `fast` pipeline goes through `vk_defer_call` on the GPU thread when the GPU thread first sees `opt` ready. The
  frame slot that it goes to recycles after its last submission. All binds of `fast` are in that frame or before it,
  because the recording thread drains in order. `opt` is set before `fast` goes, so no resolve reads a freed handle.
- With `async_pipelines=1`, `vk_pipeline_now` reports pending only while there is no `fast`. Draws skip for the time
  of a part compile, not of a whole pipeline.

The lock fixes of 92df965c9 need nothing more. The workers take no `vk_lock`. The recording thread waits at the bind
as before, with no renderer lock held, and the wait is now short.

When the device has no fast link (`graphicsPipelineLibraryFastLinking` false), a link is a compile. The GPL path then
stays off (see [Without the extension](#without-the-extension)).

## Across runs

The pipeline file (`pipeline_file.c`, `<game>.vkshaders`) keeps shaders (`PF_SHADER`, `PF_SOURCE`) and whole pipeline
keys (`PF_PIPE`). The design adds no record type. A `PF_PIPE` key gives its four part keys through `vk_pipe_split`.
So at startup the reading thread (`pf_load_pipe`) queues on the file's queue, in this order:

1. The PR and FS parts of each `PF_PIPE`, each one time (the part tables find the repeats). This is the slow work.
2. The link of each `PF_PIPE`, when its parts are ready.
3. The optimized pipeline of each `PF_PIPE`, on the lowest queue.

With warm caches, step 1 is a hit in `g_pcache` for each part, and step 3 a hit for each pipeline. The file key
(`pf_key`, `PF_GUEST_VERSION`) does not change, because a `PipeKey` keeps its meaning. A field that becomes 0 for
dynamic state is set to 0 only in the part key, never in the record.

`vk_pipelines.bin` keeps what the driver compiled, for parts and for optimized pipelines. The NVIDIA driver also has
its own disk cache. With both warm, a second run compiles nothing.

Fossilize and the Granite post (part 6 of its tour of the Vulkan backend) fill the pipeline tables of a run from the
create infos of earlier runs, replayed on many threads at startup. The pipeline file is that already, for whole
pipelines. This design only adds the parts in front of it.

## The programs a game ships

The startup pass (`pf_ship_pass`, `ship_program`, `ship_make`) translates about 7,400 programs into
`<game>.vkshipped`, with no draw state. It makes no modules and no pipelines now. A draw takes a module from the file
when its key matches (`pf_ship_take`).

After the pass, a worker can compile the parts of these programs:

- A vertex program (`ST_VS`, and a VertexA merged with its VertexB in `ship_dual`) becomes a PR part with only the
  vertex stage. A fragment program becomes an FS part.
- Geometry and tessellation evaluation programs are skipped now (their keys hold draw state). They stay skipped.

What the pass must guess for each part, from no draw:

| What | The guess | A wrong guess costs |
| --- | --- | --- |
| the module's key (`mask`) | the pass's key now: all words 0, the clip word from `ship_clip` | a new translation and a part compile at the draw, as now |
| the last stage before rasterization | the vertex program is the last (`ship_clip`) | the same |
| `arrays` | the program's own `indexed` bit, known after the translation | nothing (the bit comes from the program) |
| polygon, depth clamp, provoking vertex, conservative, patch | none, they are dynamic | a PR compile, only on a device without that dynamic state |
| `feedback` | none with `has_feedback_dynamic` | an FS compile, only without it |
| sample shading | off (FS part with no multisample state) | an FS compile for a draw with sample shading |
| transform feedback | none | the module differs, so a new translation |

A part compile for 7,400 programs is a lot of driver time and driver memory. A 1.7 MB vertex program takes seconds.
So the pass does not keep these parts. A worker makes each part into `g_pcache` and destroys it at once. This is
the method that Fossilize uses to warm a driver cache. When a draw asks for the part later, its make is a hit in the
cache. Its cost is the hash and the lookup, not the compile. The parts go on the lowest queue, behind the draws'
work. They stop when the game exits, and `g_pcache` is saved by `vk_pipeline_save` as now.

The module that a part needs must exist for the make. A worker makes a module from the SPIR-V of the ship record,
makes the part, then destroys both. The draw's module later is a new `VkShaderModule` with the same SPIR-V. The
driver's cache key is the SPIR-V and the create info, not the module handle, so the hit holds. This must be measured
(see the open questions).

The pass can mark which programs it warmed, by a flag in the `PF_SHIPPED` record set, so that the next run warms only
the programs that are new. The `ship_ready` flag already tells the draws when the pass is done. The warm work starts
after it.

## Interactions

- **Dynamic vertex input** (`g_vk.vertex_input_dynamic`): the VI part lists `VK_DYNAMIC_STATE_VERTEX_INPUT_EXT` and
  holds no layout. Then the VI key is only `topo_class`, and there are four VI parts at most. Without it, the VI key
  holds the layout, and a new layout costs a VI part (microseconds) and a link, never a shader compile. This is the
  first gain on drivers where `vk_vi_choose` turns dynamic vertex input off. `VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE`
  is also VI state.
- **Feedback loops**: with `has_feedback_dynamic`, the draws set `vkCmdSetAttachmentFeedbackLoopEnableEXT` as now,
  and the flag is in no key. Without it, `feedback` goes into the FS and FO keys and the flag goes on those parts and
  on the link. The spec text for which part takes the flag must be checked first (see the open questions).
- **Transform feedback**: the XFB decorations are in the module (`ShXfb` in `vk_shader_get`), so they are in the PR
  key by the module. The rasterization stream is PR state. It is 0 now. It can become dynamic with
  `extendedDynamicState3RasterizationStream` later.
- **Tessellation**: `tcs` can be the passthrough of `vk_tess_passthrough`, made for each patch size. So with a
  passthrough, the patch size is in the PR key through the module, also with dynamic patch control points. A game's
  own control program makes the patch size dynamic.
- **Mesh and task stages** (branch `feat/sm86-gaps`, later): they are PR stages. A mesh pipeline has no VI part
  (vkd3d-proton leaves out `VERTEX_INPUT_INTERFACE` for mesh). Their sets need slots in the layout table, which the
  mesh work adds. The split function then gives a VI key of none.
- **Geometry passthrough**: the passthrough mask is in the geometry program's module key, so it is in the PR key.
- **Interpolation**: a separate PR part and FS part need the interpolation decorations on both sides, or
  `graphicsPipelineLibraryIndependentInterpolationDecoration`. vkd3d-proton turns GPL off without it. This design
  does the same. It does not check the translator's output for decorations.
- **`SignedZeroInfNanPreserve`** and the other float controls are in each module. They stay correct, because each part
  compiles its own modules.
- **Robustness**: `VkPipelineRobustnessCreateInfoEXT` with `vertexInputs` goes on every part and on the link. Then the
  vertex fetch is robust in whichever part the driver compiles it.
- **The pipeline cache save**: `vk_pipeline_save` writes `g_pcache` after `PCACHE_QUIET_NS` with no new pipeline.
  Part makes and optimized pipelines must also update `g_pcache_used` (`pcache_note`), so a save does not hold the
  cache's lock while a draw's part compiles.

## Without the extension

`g_vk.has_gpl` is set only when all of these hold:

- the device has `VK_EXT_graphics_pipeline_library` and `graphicsPipelineLibrary`
- `graphicsPipelineLibraryFastLinking` and `graphicsPipelineLibraryIndependentInterpolationDecoration`
- the setting `pipeline_libraries` is not `0`

The setting is new, in `src/nx/runtime/settings.txt` and `docs/SETTINGS.md`: `0` off, `1` on, default `1`.
It is a `startup` setting, because the layouts depend on it. It gives a player a way to turn GPL off on a driver that
draws it wrong.

Without `has_gpl`, nothing changes: the layouts have no `INDEPENDENT_SETS`, `pipe_create` makes whole pipelines, the
dynamic state of the table above stays static, and the pipeline file works as now. The PR, FS, VI and FO code is
not reached. `vk_pipeline_file_key` does not change: a `PF_PIPE` record means the same pipeline in both modes, and
the driver's own cache data in `vk_pipelines.bin` keeps the parts and the whole pipelines apart.

`pipe_slow_say` and the stats line say which path a pipeline took, so a log of a player shows it.

## Measurement

The goal is the stall of a new shader in a frame, and how often it occurs. Measure the same scene and script on the
build before the change and after it, three times each, with empty caches and with warm caches. Run without the
validation layer for times.

- **The slow pipeline log**: `pipe_slow_say` (one second or more) gets a line for each kind of make:
  `a pipeline library took N ms (pre-rasterization, vs ...)`, `a pipeline link took N ms`, and
  `an optimized pipeline took N ms`. The last kind is not a stall, so the line says that it was in the background.
  Count the lines that name a stall: `grep -c 'took .* ms to make' games/<id>/<id>.log` for the build before, and the
  library and link lines for the build after.
- **Tracy zones**: "pipeline make" stays for the whole pipeline. The new zones are "pipeline library make" (with the
  part and the stage hashes), "pipeline link" and "pipeline optimize". "pipeline wait" stays on the recording thread.
  In a capture, the sum and the largest "pipeline wait" are the stall. The `mcp__tracy__compare_captures` tool
  compares two captures.
- **The stats line**: `vk_pipeline_stats` gets the parts made, their cache hits, the links, the link ms, and the
  optimized pipelines made. The wait ms (`g_pipe_wait_ns`) is the number to compare.
- **The stalls in each run**: the count of "pipeline wait" zones over 50 ms, and the largest. On the Ounce game with
  empty caches, the build before waits 6 to 10 s for each new pipeline of a large vertex program. The build after
  must wait that long only one time for each vertex program (its PR part), and only a link for each new state of it.
  With the shipped programs warmed, it must wait for no compile of a shipped program.
- **The GPU time**: a draw on the fast link can be slower on the GPU than on the optimized pipeline. Compare the GPU
  frame time (`gpuprof.c`) over a scene after the optimized pipelines are ready. It must be the same as the build
  before.

## Commits

Each commit keeps the renderer working, and each runs under the validation layer (with
`VK_KHRONOS_VALIDATION_VALIDATE_SYNC=true`) against the same scene on the build before it. A commit adds no
validation message.

1. **`[gpu] Query graphics pipeline libraries and extended dynamic state 3`**. `device.c` queries and enables
   `VK_EXT_graphics_pipeline_library`, `VK_EXT_extended_dynamic_state3` and the EDS2 patch control points, and sets
   `g_vk.has_gpl` and `g_vk.dyn_pr`. The new setting `pipeline_libraries`, with its row in `docs/SETTINGS.md`. Nothing
   uses them yet. Test: the log line of the device choice, `ctest -R 'vulkan_headless|docs_check'`, the headless test
   under the layer.
2. **`[gpu] Make the graphics layouts with independent sets for pipeline libraries`**. The flagged layouts, the
   null-set layouts of the PR and FS parts, and a compute layout apart. Whole pipelines use the flagged layouts when
   `has_gpl`. Test: the headless test under the layer with descriptor buffers on and off (`descriptor_buffers`), and a
   game scene with the same screenshot hash as the build before.
3. **`[gpu] Set pre-rasterization state dynamically on the library path`**. The `rec_CmdSet...` wrappers, the `DYN`
   entries in `render.c`, and the dynamic states in whole pipelines when `has_gpl`. Test: the edenprobe cases for
   polygon mode, depth clamp, provoking vertex and conservative rasterization, and the headless test.
4. **`[gpu] Split a pipeline key into its four library parts`**. `vk_pipe_split` and the key types, with a test in
   `tests/` that runs with no device. Test: the new ctest.
5. **`[gpu] Link a draw's pipeline from graphics pipeline libraries`**. `pipeline_lib.c`: the four tables, the part
   makes on the workers, the fast link, the stand-in that resolves to the link. Test: the headless test under the
   layer, a game run through a scene with new shaders, and the stats line (links made, wait ms).
6. **`[gpu] Make the optimized pipeline in the background and switch to it`**. The third queue, the LTO link, the
   swap and the deferred destroy of the fast link. Test: a Tracy capture that shows "pipeline optimize" on the workers
   and no "pipeline wait" for it, and the GPU frame time against the build before.
7. **`[gpu] Make the pipeline file's library parts at startup`**. `pf_load_pipe` queues the parts, then the links,
   then the optimized pipelines. Test: a second run with warm files makes no part that is not a cache hit
   (the stats line).
8. **`[gpu] Warm the driver cache with the parts of the programs a game ships`**. The make and destroy of the PR and
   FS parts after the pass, on the lowest queue. Test: a run with empty caches, then a run with warm files on the
   Ounce game. The second run must show no library line over one second for a shipped vertex program.
9. **`[docs] Record the measured stalls of pipeline libraries`**. This file gets the numbers of the measurement above.

Commits 1 to 3 change no draw result. They can land first, and each one can be checked alone.

## Rejected alternatives

- **`VK_EXT_shader_object`**. It removes pipelines, and the RTX 2080 has it natively. But it has no optimized
  pipeline in the background, so the GPU always runs the unlinked code. Each draw must set all state dynamically,
  which needs all of EDS3 on every device. RADV has it, and ANV only through a layer. The pipeline file and the
  driver cache would need a second format (shader binaries). GPL keeps one model, pipelines, with a fallback that is
  the code of today.
- **One PR and FS library per pipeline, as vkd3d-proton makes for each D3D12 pipeline state**. vkd3d-proton gets
  the shaders of a D3D12 pipeline state together, with most of its state. It puts them in one library and links a
  VI or FO part for the state that it learns late. Our shaders come one stage at a time, and the same vertex program
  meets many fragment programs. A library of two stages would compile the large vertex program again for each new
  fragment program.
- **Blend, logic op and sample state as EDS3 dynamic state**. They are FO state, and an FO part costs microseconds. To
  make them dynamic would gain nothing at the link, and the optimized pipeline would lose the static blend state. It
  would also need more EDS3 members on every device.
- **No `INDEPENDENT_SETS`, with a key that holds both stages' array bits**. See [Layouts](#layouts): a vertex program
  would compile one time for each kind of fragment program that it meets.
- **`VK_KHR_pipeline_binary` for the parts**. The device has it. But `g_pcache` and the driver's disk cache already
  keep the compiled parts. A binary format of our own would be a second cache to keep in step, with no gain shown.
  It can come back if the cache hit of a part costs too much (see the open questions).
- **Skip a draw while its pipeline compiles**. It broke one game's menus and 190 edenprobe cases. A draw still never
  skips. Only `async_pipelines=1` skips, as now.

## What comes from vkd3d-proton

vkd3d-proton (LGPL-2.1-or-later, `libs/vkd3d/state.c` and `libs/vkd3d/device.c`, read at commit `865b12ba0`) gives
the method, not code to copy:

- The VI part and the FO part as small libraries in tables of the device, keyed by a zeroed struct with `memcmp`, and
  made when a lookup misses (`d3d12_device_get_or_create_vertex_input_pipeline`,
  `d3d12_device_get_or_create_fragment_output_pipeline`).
- A list of dynamic states for each part (`VKD3D_VERTEX_INPUT_DYNAMIC_STATE_MASK`,
  `VKD3D_FRAGMENT_OUTPUT_DYNAMIC_STATE_MASK`).
- `RETAIN_LINK_TIME_OPTIMIZATION_INFO` on every part, and `LINK_TIME_OPTIMIZATION` only on the pipeline that it keeps
  (`d3d12_pipeline_state_link_pipeline_variant`). Its late variants take the fast link alone, "to avoid stutter".
- GPL off without `graphicsPipelineLibraryIndependentInterpolationDecoration` (`device.c`).
- No mesh pipeline has a VI part.

Our code differs in each place: separate PR and FS parts, independent sets, a background LTO link for every pipeline,
and our own tables and workers. So the plan is to write the code new, with no line ported. If a commit ports a
function, it keeps the copyright lines of that file (Józef Kucia and Henri Verbeet for CodeWeavers, in the header of
`state.c`), names vkd3d-proton as the source, and adds a row to `docs/CREDITS.md`. LGPL-2.1-or-later is on the list of
licenses that the project can port (CLAUDE.md).

## Open questions

- **The feedback loop flag without its dynamic state**: which parts must have
  `VK_PIPELINE_CREATE_COLOR_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT`? The VUIDs in `validusage.json` of the installed layer
  do not say. Check the spec section of the flag, then the layer on a device without
  `attachmentFeedbackLoopDynamicState` (lavapipe, or the setting forced off in a scratch build).
- **The cost of a cache hit for a 1.7 MB part**: the warm method of the shipped programs depends on it. Measure a
  part make with a warm `g_pcache` on the Ounce game. If it is more than tens of milliseconds, keep the parts of the
  vertex programs alive instead, and measure the driver memory that they hold.
- **The driver's cache key with a new module handle**: check that a part made from a new `VkShaderModule` with the same
  SPIR-V is a hit (the creation feedback's `APPLICATION_PIPELINE_CACHE_HIT` bit).
- **The fast link on the GPU thread**: is about 1 ms small enough there, or must the link always go to a worker?
  Measure the "pipeline link" zone.
- **The GPU cost of the fast link**: how much slower is a draw on the fast link than on the optimized pipeline on
  NVIDIA and RADV? It sets how much the background queue matters.
- **`INDEPENDENT_SETS` on RADV and ANV**: does it cost GPU time on descriptor binds? Compare the GPU frame time with
  GPL on and off on an AMD GPU, which this machine does not have.
- **Driver memory**: thousands of parts and pipelines live for the whole run. Measure the memory of the process with
  GPL on and off over a long run.
- **Robustness on parts**: check under the layer that `VkPipelineRobustnessCreateInfoEXT` on each part and on the link
  is valid, and that vertex fetch stays robust on the fast link.

## Risks

- A driver can draw a linked pipeline wrong where the whole pipeline is right. The setting `pipeline_libraries=0` is
  the way out, and edenprobe on lavapipe (which has GPL) checks the path in CI.
- The layouts change for all graphics pipelines when GPL is on. A missed bind with a layout of the other family is a
  validation error, not a wrong draw on most drivers, so the layer finds it.
- The swap adds a pipeline bind in each command buffer after an optimized pipeline is ready. That is one bind for
  each changed pipeline, not for each draw.
- The startup warm of the shipped programs competes with the game for CPU. It runs on the lowest queue only, and the
  `pipeline_threads` setting limits it with the other work.
