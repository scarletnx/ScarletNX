<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# NVN rendering audit and synthetic checks

## Index

* [Scope and fixes](#scope-and-fixes)
* [Additional rendering support](#additional-rendering-support)
* [API inventory](#api-inventory)
* [Follow-up support and restrictions](#follow-up-support-and-restrictions)
  * [GPU event waits](#gpu-event-waits)
  * [Shader subroutines](#shader-subroutines)
  * [Copy format conversion](#copy-format-conversion)
  * [Texture minimum LOD and gradient offsets](#texture-minimum-lod-and-gradient-offsets)
  * [Advanced blend paths](#advanced-blend-paths)
  * [Sample masks and coverage](#sample-masks-and-coverage)
  * [Descriptors and GPU ownership](#descriptors-and-gpu-ownership)
* [Repeatable checks](#repeatable-checks)
* [Remaining gaps](#remaining-gaps)
* [Performance limits](#performance-limits)

## Scope and fixes

The regression fixtures use generated state, shaders and pixels without game data. They test command state,
recorded-state lifetime and Vulkan output. Passing these fixtures does not establish full NVN compatibility.

| Case | Previous behavior and correction | Regression coverage |
| --- | --- | --- |
| Tessellation storage buffers | TCS/TES table offsets were zero, so bindings were discarded. Use bank-0 tables `0x210` and `0x310`. | `nvn_draw_banks`, every graphics stage and storage slot |
| Patch control points | `SetPatchSize` recorded nothing. Record and replay the size into the pipeline key. | `nvn_draw_banks`, `nvn_names` |
| Empty instanced draws | Zero instances became one instance. Direct zero/negative instance counts emit no draw. Indirect counts stay on the GPU. | `nvn_draw_banks`, renderer callback counts |
| Compute uniform buffers | Direct slot sizes retained unrounded byte counts. Round to 256 bytes while retaining the requested VPC descriptor size. | `nvn_draw_banks`, boundary sizes through 65536 |
| Indexed viewport state | Only slot 0 received scissor/depth range changes. Record selected ranges and retain independent state for all 16 slots. Also correct fractional/negative clip metadata, with guest float endpoint precision. | `nvn_draw_banks`, `nvn_names`, snapshot lifetime and command memory accounting |
| 1D array copies and surfaces | Public Y/height were treated as texel rows. Normalize them to layer/count. Physical surfaces have one row and a separate layer count. | `nvn_copy_formats`, `nvn_names`, real Vulkan output |
| Mirrored Z copies | Mirror flags did not reverse slice order. Reverse buffer image order for B2T/T2B and source slices for T2T. | `nvn_copy_formats`, real Vulkan output and untouched padding |
| Cube-face views | A cube face viewed as 2D did not add the selected layer's pitch to its address. Apply the view layer offset independently of view target. | `nvn_descriptors`, all six faces |
| Sampler defaults | Minification used nearest filtering with mipmaps and minimum LOD was negative. NVN defaults are linear without mipmaps and minimum LOD zero. | `nvn_names`, getters and encoded descriptor bits |

Cube-face fixtures check the descriptors. The Vulkan integration case
directly reads all seven layers of an uploaded 1D-array image, then checks mirrored texture copies, view offsets, strided downloads and
untouched padding. Reading the image independently prevents matching upload/download errors from cancelling out.

## Additional rendering support

| Area | Implemented behavior | Synthetic evidence |
| --- | --- | --- |
| Partial stencil clears | A masked stencil draw preserves unselected bits. Depth clears, scissors, array layers and zero masks retain their separate effects. | Exact D24S8/D32FS8 depth/stencil aspect copies on both GPUs. Undefined X8 padding is excluded. |
| Copy transforms | GPU paths support valid X/Y/Z mirrors, nearest scaling and supported linear image blits. Logical bounds exclude GOB padding. Ordinary copies retain their existing DMA path. | Buffer/image copies, layer offsets, transform combinations, nearest scaling, linear filtering and untouched padding. |
| Fixed state | Record/replay alpha reference, point size, depth bounds and default tessellation levels. Setters snapshot their inputs. | Frontend state assertions and exact RGBA32F point, alpha-test and depth-bound output. |
| 3D engine fixed state | The 3D engine's own draws (deko3d's, a game's own driver's) take the point size from SET_POINT_SIZE (0x546), as the NVN path's take theirs. | `draw_state_mx` and exact RGBA32F points of side 4, 1 and 8 drawn through the 3D engine. |
| Graphics storage images | Vertex, tessellation control/evaluation, geometry and fragment stages have their own image and buffer-image bindings and integer format variants. Graphics layouts share the whole-pipeline descriptor budget. Compute retains an independent layout and its full per-stage budget. | Frontend bank tests, generated modules and per-stage load/store fixtures. |
| Indexed multisample textures | Dynamic handle tables retain multisample image types and explicit sample operands for 2D and 2D-array textures. Integer and floating-point view types stay distinct. | Six compiler fixtures and a two-texture, four-sample GPU fetch fixture. |
| Texture minimum LOD | Supported TEX/TEX.B LC forms retain the packed layer, coordinate order and unsigned minimum LOD with eight fractional bits. Fragment samples use `MinLod`; other stages clamp their explicit level. | `shader_texture_limits` and `vulkan_texture_limits.inc`, including array coordinates and bias. |
| Texture gradients and offsets | 3D and cube-array gradient samples retain dimensions, faces and layers. With maintenance8, varying texel offsets use one native `Grad|Offset` sample. Older devices use exact constant-offset dispatch with known-bit pruning and bounded radix selections. | Fragment/compute operand oracles, uniform changes, minified texels, signed offset extrema, exhaustive fallback routing checks and a separate minified cube-edge probe. |
| Half texture results | Texture and half-arithmetic result packing uses exact nearest-even conversion. Native float16 conversion requires explicit host rounding, denormal and signed-zero controls; other devices use integer significand rounding. | 128 real R32F TEXS/TLDS/TLD4S/HADD2 cases, plus independent conversion checks. |
| Cross-stage image aliases | A draw uses compatible sampled/storage layouts across all stages and retains its complete write access until the next dependency. Target snapshots finish before the programmable blend transition. | TES stores with FS sampling and TES/GS target snapshots across consecutive draws, without host waits. |
| Shader code reads | Coherence requests grow from a short prefix instead of landing unrelated GPU results in a fixed 128 KiB window. BRX retains the full-window path because its proof inspects later branches. | Nine hash/length differential cases and a regression that rejects a forced full-window GPU wait. |
| DrawTexture | A generated vertex/fragment pair rasterizes the rectangle with the bound sampler, component masks, blending and alpha tests. Depth/stencil tests and conservative rasterization follow the API's DrawTexture rules. | Dedicated sampler, edge clamp, blend, alpha-test, write-mask and MSAA fixtures. |
| Evaluation-only tessellation | A generated control shader forwards patch positions, generic attributes, point size and clip distances, then writes fixed levels. Clip interfaces use eight elements across stages. Earlier layer/viewport outputs are inert before evaluation. | Selected input vertices, fixed levels, point/clip payloads, opposite producer/consumer clip indices, interface bounds and generated-module fixtures. |
| Transform feedback | Save, pause, resume and restore GPU counter controls per owner. A GPU shader converts captured bytes and stride into indirect draw arguments. | Exact counter data, owner isolation, control padding and a capture/redraw round trip. No CPU counter readback in the runtime. |
| Advanced blending | Exact ordinary-blend reductions, native advanced operations, fragment interlock and source capture followed by an ordered per-primitive resolve. A guarded D32F/S8 GPU shadow also retains late tests for eligible whole draws. Each path has separate device and draw restrictions, described below. | Independent equation references, capability checks, overlap and late depth/stencil fixtures, discarded fragments and guest atomic side effects. |
| Coverage modulation | Native mixed-sample coverage supports NONE/RGB/ALPHA/RGBA and custom coefficients. A guarded portable path reduces invariant sources to one color sample per target across up to seven targets, with optional raster-sample depth/stencil. Equal raster/color counts use a shader multiplier. | Pixel fixtures cover masks, custom coefficients, overlapping primitive blending, per-target blend/masks/conversion, depth/stencil and table changes. Native mixed-sample checks retain explicit capability skips. |
| Custom sample positions | Exact sample grids, sixteenth-pixel coordinates and lower-left grid phase enter the pipeline key. Repeated grids reduce only when their patterns are identical. Depth images retain compatible sample-location state. | CPU keys and GPU custom-raster fixtures, with explicit device limits. |
| Sample masks and alpha coverage | Guest SampleMask output retains its real output-register value across translation and cache reuse. Software coverage selects a constant prefix or a deterministic spatial dither pattern, intersected with guest and fixed masks. Independent software alpha-to-one follows original-alpha coverage/tests. | Generated register/routing proofs and C1/MSAA threshold, NaN, mask, dither and alpha-to-one fixtures. |
| Coverage to color | Native coverage output or ordered source capture writes signed/unsigned single-component integer masks. Capture unions surviving samples before writing each surviving sample. A guarded C1 D32F/S8 shadow path retains native integer output only for fragments that pass late tests. | Per-sample depth rejection, signed/unsigned masks, late-test ordering and forced split-invocation fixtures. |
| Graphics descriptors | Descriptor heaps use compact resource/sampler index runs and frame-owned encodings. Descriptor buffers and sets remain fallback paths. Repeated encodings and complete sets can be reused without changing live bytes. | Layout/limit oracles, registry lifetime checks, rendered backend transitions and separate submission measurements. |
| Presentation rotation | A GPU quad handles 90-degree rotation together with crop and flip coordinates. | Presentation rotation, crop and flip fixtures. |
| Linux placed-memory aliases | When a DMA buffer rejects a nonzero mmap offset, map the whole allocation at offset zero and move the requested subrange with `mremap`. This preserves shared backing without a CPU copy. | POSIX mapping, concurrent hole reuse and placed-view coherence fixtures. |

The fixed-state payloads share one opcode and remain outside the serialized draw-state ABI. Per-draw state only
copies the enabled fixed-state arrays. Pipeline keys separate host capabilities and effective coverage state.
Generated tessellation modules stay cached by patch size and interface mask for repeated draws. The persistent
shader file owns their modules and retains a small source description for regeneration after a cache rebuild.
Generated DrawTexture modules also use the shader cache.
Program metadata tables grow instead of crashing after 1,024 shader updates. Their shared binary lookup retains
immutable declarations and capture layouts for old command recordings. Insertions also handle shader threads that
finish out of serial order. A 1,153-update regression checks growth, every saved serial and old/new recordings.

## API inventory

`tools/check_nvn_contracts.py` audits runtime resolver routes, lookup hooks and handler bodies. Lookup coverage is
separate from per-command rendering behavior.

The report separates empty bodies, constant returns and explicit limitation signals. It also identifies reviewed
contexts such as debug operations, compression hints, memory coherence and runtime-owned metadata. These findings
do not prove a missing rendering feature. A nontrivial handler also does not prove that every shader, format or
recorded command path works. The inventory reports no compatibility percentage or inferred performance gain.

Reference-only identifiers use stable IDs by default. `--include-reference-names` enables names for local review.
Reports omit reference source text and paths. Priority labels use documented API-name and dependency heuristics.

The command-memory callback switch now retains its setting independently of the control-memory callback switch.
Its regression fixture checks suppression, re-enable and control-memory callback independence.

## Follow-up support and restrictions

These paths extend the earlier checks. Their availability depends on the command, shader interface, format and
host device. They do not establish complete NVN support.

### GPU event waits

`WaitEvent` records the event's GPU address, wait mode and comparison value. An unmet wait yields its queue's replay
job. Other queues can continue and signal the event. Replay retains its nested CALL position and resumes after the
condition succeeds, without repeating earlier draws or copies. Later work on the same queue stays behind the wait.

The supported conditions are EQUAL and GEQUAL_WRAP. GEQUAL_WRAP compares the signed 32-bit difference, so the
comparison follows wraparound ordering. Event WRITE and ADD signals enter the renderer's ordered release stream.
Replay submits work before the wait so that an earlier GPU operation can release the event. An event that no producer
releases can leave its queue pending.

Submissions that contain event or sync waits copy their command records before replay. The snapshot flattens nested
CALL/JUMP records in execution order. Resource addresses still refer to guest data read at execution time. A fence
before a wait can therefore signal and permit command-memory reuse without changing the suspended commands.
Submissions without waits retain the direct replay path.

Both recorded `WaitSync` and queue-side sync waits retain the syncpoint ID and value visible when submitted. Reusing
the guest sync object for another fence does not change that target. Replay checks the actual syncpoint before it
allows dependent work to continue. Present jobs also retain global display order across queues.

`nvn_queue_lifetime` checks queue order and retained replay state. `gpu_park` checks cooperative queue scheduling.
`vulkan_nvn_events.inc` checks a real GPU copy on a second queue before the waiting queue consumes the changed bytes.

### Shader subroutines

`ProgramSetSubroutineLinkage` validates and copies each stage's linkage map. Its immutable handle retains the shader
location and map after the caller changes its memory or replaces the program. `SetProgramSubroutines` copies the
selected uniform values into the command record. Replay resolves function IDs and updates the corresponding compiler
constant table in bank 1. It validates the whole selection before any table write and groups adjacent changed entries.

Linkage maps must match the program's declared subroutine stages. The parser accepts at most one map per stage,
checks all payload spans and limits each map to 16 MiB. Table entries must fit the stage's constant bank, up to 64 KiB.
An update supports at most 16,384 values. Invalid maps fail installation. Invalid uniform selections produce no partial
table update.

The translator supports relative constant-buffer CAL and absolute JCAL/JMP/JMX targets inside the translated shader.
Calls retain a 32-entry return stack per invocation. A deeper call terminates that invocation. Absolute-flow shader
keys include the program address, so code at different locations cannot reuse the wrong address base. Calls use the
dispatch-loop path, which can cost more GPU work and compiler time than structured flow.

`nvn_names` checks map validation, recorded values and metadata lifetime. `shader_direct_regressions` checks indirect
flow instructions. `vulkan_nvn_subroutines.inc` changes a fragment subroutine through recorded linkage updates and
checks exact RGBA32F output for relative and absolute calls across cached pipeline reuse.

### Copy format conversion

Copies keep the ordinary image or DMA path when it can preserve the required result. A compute memory-gather path
also handles nearest conversion and X/Y/Z mirrors for volume slices. The CPU prepares coordinate tables. The GPU
reads, converts and writes texels, with no CPU pixel conversion or texel readback.

The conversion path supports these color formats:

* R, RG, RGBA and RGBX with 8/16-bit UNORM or SNORM components, or 16/32-bit float components.
* RGB5/RGB5A1, RGB565, RGB10A2 and R11G11B10F, plus the implemented BGR/BGRA counterparts.
* RGBA8, RGBX8, BGRA8 and BGRX8 with sRGB transfer for RGB. Alpha stays linear.

Missing source components use zero RGB and alpha one. RGBX/BGRX alpha uses one. Normalized conversion retains the
original rational value where possible. Half and packed-float outputs round once to nearest-even. sRGB transfer uses
shader floating-point operations. Its tests allow `1e-6` linear error and one encoded UNORM code value.

Two-dimensional copies can use bilinear filtering through the compute path when both format descriptors support it.
Coordinates use texel centers and clamp to logical image bounds, excluding padded GOB columns. Volume filtering
with linear filtering is invalid NVN input. Integer/compressed cross-format conversion and arbitrary depth/stencil
format conversion also fall outside the copy contract. These combinations are not renderer feature gaps.
Three-component RGB16F/RGB32F texture formats have no COPY2D conversion support. Non-renderable COPY2D formats,
including RGBA4 and packed RGB16 formats, retain exact byte-preserving nearest transforms.

Same-format depth/stencil transforms use the host image blit when available. Otherwise the GPU copies each aspect to
scratch memory, gathers the requested nearest texels and copies each aspect back. This supports D32FS8 mirrors and
scaling without host blit support. It preserves depth and stencil independently. It does not add linear depth/stencil
filtering or arbitrary conversion between depth/stencil formats, which are invalid copy requests.

`vulkan_nvn_copy_conversions.inc` checks volume UNORM and float conversion, packed nearest/bilinear transforms,
sRGB transfer, rendered depth/stencil transforms and untouched row padding. `nvn_copy_formats` checks frontend format
selection and rejects unsupported conversion rather than copying incompatible bytes.

### Texture minimum LOD and gradient offsets

TEX and TEX.B LC support the decoded automatic and bias forms. Their extra packed word precedes coordinates even
for a non-array texture: the low half retains the layer, and the unsigned high half gives minimum LOD in units of
1/256. Fragment sampling uses the native `MinLod` operand and requires `shaderResourceMinLod`. Other stages have no
implicit derivatives and clamp their explicit level instead. Unsupported LC encodings remain rejected rather than
being interpreted as another operand layout.

TXD and TXD.B retain explicit gradients for 1D, 2D, 3D, cube and supported array textures. Signed four-bit texel
offsets are supported for 1D, 2D and 3D, with array layers outside the spatial offset. A 1D texture's emulated Y
coordinate stays 0.5 and its Y derivatives and texel offset stay zero. Cube gradient sampling does not add an invalid
cube texel-offset operand.

When the device advertises and enables `VK_KHR_maintenance8` and `shaderImageGatherExtended`, a varying offset
becomes one native `Offset` sample, retaining the instruction's gradients, level, bias and depth-reference operands.
Signed nibble extraction supplies its texel offset; coordinates, array layers and descriptor indices retain their
original operands. Constant-buffer offsets remain
runtime data on this path, so reading them does not create a CPU-specialized shader variant. Pipeline feature keys
separate this lowering from devices that require the portable path. Literal and proven constant offsets can still
use one `Grad|ConstOffset` sample.

Without maintenance8, proven fixed constant-buffer words can specialize one `ConstOffset` sample; changed words
invalidate that variant. Other varying offsets select exact native samples across their finite domain: at most
16, 256 or 4,096 cases for one, two or three spatial dimensions. Known-bit proofs remove unreachable cases,
including when only the array layer varies. Explicit-level and gradient sampling execute one selected leaf.
Automatic and bias sampling execute the compatible constant-offset samples in uniform control flow, then select
the result, preserving implicit derivatives. Both paths apply offsets to the selected mip's texels rather than
shifting normalized coordinates based on level zero.

Portable selections have at most 16 successors and Phi inputs. They form a radix tree instead of one 4,096-input
merge. Generated routing oracles check every full-domain selector and reduced domains, including signed extrema,
descriptor routing and the selected result. Separate fragment/compute module fixtures check the maintenance8
sample and offset/layer operands. The largest portable domains still contain thousands of sample cases; driver
compilation cost remains a limitation on devices without maintenance8. Module structure alone does not establish
GPU output or compilation time for either path.

### Advanced blend paths

The renderer uses the following GPU paths for advanced state on color target 0. Blend enable and ordinary logic-op state
still control whether that state applies. NVN advanced blending supports one color target, target 0. Multiple active
color targets have undefined API behavior and are not a missing MRT feature. Shader and pipeline keys retain the
effective equation and its parameters.

| Path | Supported behavior | Restrictions |
| --- | --- | --- |
| Ordinary blend reduction | PLUS, MINUS and RED/GREEN/BLUE selectors. With normalized destination values: ZERO/SRC/DST, premultiplied uncorrelated Porter-Duff modes and SCREEN, plus nonpremultiplied SRC and SRC_OVER. | Only the parameter combinations with an exact fixed-function equation qualify. Other correlated Porter-Duff and artistic equations need another path. |
| Native advanced blend | Maps all 46 NVN advanced modes to `VK_EXT_blend_operation_advanced`, including artistic/HSL operations and disjoint/conjoint overlap. | The device must provide coherent operations. Its limits must permit the attachment count, operation, source premultiplication and overlap. Without `advancedBlendAllOperations`, only the extension's required operation subset qualifies. |
| Ordered shader blend | Computes the 46 equations with an ordered destination read/modify/write under fragment shader interlock. Supports source premultiplication, all three overlap modes, component masks, array layers, sRGB transfer and multisample storage images. Guest storage images use separate bindings. Indexed texture tables retain target-feedback checks. | Requires pixel interlock, fragment storage writes and storage-image reads/writes without format. The target must be storage-capable. MSAA also requires multisample storage images and a supported raster sample count. Depth/stencil uses safe early tests with PostDepthCoverage for multisample masks, or the guarded late-shadow subset below. Mixed-sample coverage, alpha-to-coverage and alpha-to-one remain outside this shader blend path. |
| Ordered late D32F/S8 shadow | Copies native depth and optional stencil aspects to a GPU-only buffer. Original guest stages run as one draw; fragment side effects precede ordered late depth/stencil tests and conditional advanced color updates. The GPU copies changed aspects back after the draw. | One color target and one D32F or D32FS8 depth target, one sample/layer, the ordinary viewport depth range and no guest FragDepth, early-test mode, active queries, predication, depth clamp, depth bias, conservative rasterization or layer/viewport exports. Depth feedback aliases are rejected. Requires ordered pixel interlock, fragment storage writes, sufficient storage-buffer descriptors, unrestricted depth range and signed-zero/infinity/NaN preservation; advanced color keeps its storage-image requirements. |
| Source capture and resolve | Runs the original fragment shader into host-only source and validity attachments, with native late depth/stencil, discard, alpha tests and guest storage side effects. A compute shader then applies the advanced equation to surviving samples before the next primitive. | Direct point, line and triangle lists, including indexed lists without restart, with direct instances. Complete nonindexed lists also retain transform feedback and vertex storage writes. Tessellation, geometry shaders, indirect draws and indexed vertex side effects remain outside this path. The target must support storage access; multisample targets need multisample storage images. Only the first color output is supported. |

The late-shadow path also rejects textures in stages before the fragment shader, indexed fragment texture tables
and texel-buffer images in any stage. These resource paths need a complete depth/stencil alias proof before they
can use the shadow path.

The advanced interlock shader path rejects integer targets, volume targets, additional fragment outputs and depth/sample-mask outputs.
The blend attachment occupies the image binding after the guest storage images, so both must fit the same stage
budget. A pipeline without blending retains its ordinary guest image capacity. Indexed tables resolve their entries
before applying target-feedback snapshots and ordering. The shader path uses the destination image directly.
Ordered interlock preserves overlap between primitives within one draw.

Safe early tests mean that the guest program explicitly requests early tests, or that moving tests earlier cannot
change discard, alpha-test or storage side effects. A general late-depth program with discard cannot use this shortcut.
Forcing early tests can update depth/stencil for a fragment that later discards. PostDepthCoverage does not remove
that semantic difference. The late-shadow subset instead runs those tests after guest discard and alpha decisions.
Its stencil fail/depth-fail operations occur before color rejection, and every invocation leaves its interlock
before a failed fragment is discarded. Coverage-to-color uses the same tests with a native integer color output;
it does not need an integer storage image. Tessellation, geometry, indirect and indexed commands can stay whole on
this subset, preserving their original shader invocation and transform-feedback behavior. Fixed-point depth,
multisample/layered depth and the other excluded states still need a broader solution. Native advanced blending
remains available for supported device and format combinations.

Unpremultiplied sources retain their original RGB values. Premultiplied color normalization preserves exact zero
and one endpoints, and HARDMIX compares its threshold before division. This avoids large discontinuous errors from
the division accuracy permitted by the [Vulkan SPIR-V environment](https://docs.vulkan.org/spec/latest/appendices/spirvenv.html).

`NormalizedDst` is a value-range hint. It does not select destination premultiplication. Native and shader paths keep
the advanced equations' premultiplied destination interpretation for either value of the hint. The ordinary reduction
path requires the hint only for equations that depend on normalized destination values.

Capture preserves guest late tests instead of forcing early tests. The original fragment shader runs once for each
primitive, and its guest image writes are not repeated by the resolve. Source and validity attachments remain outside
the guest surface cache and writeback lists. A sampled alias of the real destination uses the original draw's
snapshot. Scratch images and tile buffers retire with their frame when the target shape changes.
Splitting a draw into primitives also requires a vertex-program proof that excludes subgroup, lane-group and clock
dependencies. Indexed lists with vertex writes or transform feedback remain excluded; complete nonindexed lists
preserve their original vertex invocation count. The whole-draw late-shadow path does not split those invocations.

An optional tile list restricts resolve work to tiles reached by non-helper fragment invocations. Native late tests
can reject every sample in a listed tile; the validity attachment still prevents destination writes. Each resolved
tile resets its own flag. An incomplete capture forces a flag clear before reuse. A GPU-only adaptive dispatch is
experimental and remains opt-in; its extra dispatch and barrier are not assumed to improve performance.

States outside these gates do not have an exact portable implementation. The native GPU fixture checks only
combinations that the device advertises. The shader fixture disables the native
path to exercise interlock, including two overlapping primitives, component masks and discarded fragments. Numerical
checks use a tolerance of `0.003` for native operations and `0.00005` for the shader path. These account for device
arithmetic precision and do not permit missing equations or ordering errors.

### Sample masks and coverage

The translator writes guest SampleMask from the register after the declared color outputs, preserving that value
when the same program is translated again. A depth result uses its separate output register. The mask is not
replaced by an input-coverage value or by a cached register number.
The fixed pipeline mask also applies to bit 0 of a single-sample target; a zero mask preserves its color and depth.

With dithering disabled, alpha-to-coverage supports 1, 2, 4, 8 and 16 samples through a shader prefix mask:
`(1 << floor(clamp(alpha, 0, 1) * samples)) - 1`. NaN selects no samples. The result intersects the guest output mask;
the fixed pipeline sample mask still applies. With dithering enabled, an ordered 4x4 threshold refines the covered
sample count, and a pixel-dependent rotation distributes the selected sample indices. Endpoints still select no
samples for alpha zero and all samples for alpha one. This is a deterministic implementation of the API's spatial
dither contract, not a claim to reproduce an undocumented native pattern.

Coverage and alpha tests use the original alpha. Independently of coverage enable, software alpha-to-one replaces
every floating-point color alpha with one and disables native
alpha-to-one; integer color outputs retain their bits. An integer primary output skips these alpha operations.
This software path needs no host `alphaToOne` feature. The ordering follows
[Vulkan multisample coverage](https://docs.vulkan.org/spec/latest/chapters/fragops.html#fragops-covg).
The current pipeline uses ordinary rasterization. A future smooth-line mode would need a separate ordering gate
because its native line-coverage alpha factor occurs after fragment shading.

Coverage-to-color accepts the supported signed or unsigned single-component R8/R16/R32 integer target. The ordered
capture path currently requires target 0 alone, no logic operation and the same direct-list restrictions as source
capture above. It reads all validity samples before writing the union mask to each surviving sample, so host sample
shading cannot narrow the mask when the fragment shader runs in separate invocations. A direct post-depth fragment
mask alone cannot establish that union when host invocations split. If every attachment and rasterization use one
sample, a post-depth fragment mask is the whole pixel mask. This shader path also supports DrawTexture, MRT and
indirect draws. It requires post-depth coverage, no alpha-to-coverage or shader depth/sample-mask output, and either
explicit early tests or no discard, alpha test or shader storage writes. DrawTexture always uses the latter gate.
Native coverage output remains available under its own capability checks.
The guarded single-sample D32F/S8 shadow path also supports late-test coverage-to-color without forcing early
tests or splitting a whole draw. It preserves the original guest side effects, applies fixed/guest sample bit 0,
and discards native integer color output after the interlock when the late tests fail. Its depth and draw-state
restrictions are the same as the late-shadow subset above.

Color and depth/stencil targets retain independent sample layouts. Native coverage modulation supports a depth
sample count equal to the raster count, including a larger count than color. Equal raster/color counts use a
fragment multiplier for active custom coefficients and retain the original sample shading. The multiplier requires
signed-zero/infinity/NaN preservation, uses the original alpha for tests, and applies alpha-to-one before modulation.
Both software dither modes retain that ordering with alpha modulation. A disabled mode or a unity coefficient
does not create an otherwise redundant pipeline. Masked clears separate unlike attachments so their pipelines retain legal sample
counts; color-only clears preserve depth/stencil.

The portable mixed-coverage path captures each primitive into separate source attachments and one shared validity
attachment at the raster sample count. It supports one through seven contiguous single-sample color targets,
subject to the host attachment limit, reserving a separate output for validity. Each target retains its own native
blend equation, logic operation, component mask and format conversion. Resolve resources for every target are
prepared before changing any destination. A fragment pass counts surviving samples, discards empty pixels, applies
the selected coefficient, and uses native color state once per target and primitive. Depth/stencil runs
against the original raster-sample attachment during capture. This retains overlapping primitive order and the
distinction between zero coverage and a covered zero-valued source. Coefficients use push constants so table changes
reuse shaders and pipelines. Immutable source/valid descriptors are reused only within their frame and image
lifetimes. Guest output-register order and types remain separate from the extra validity output.

The reduction reads ordinary sampled images, so it does not need multisample storage images or formatless storage
reads/writes. Its validity image uses R8 when the device supports that format at the required extent and sample
count, with RGBA8 as fallback. This reduces the logical validity payload from four bytes to one byte per raster
sample without reducing source precision. Physical allocation and compression remain implementation dependent.
The advanced-blend compute path retains its separate storage-image requirements.

This fallback requires noninteger targets starting at slot 0 with equal extents, one layer, distinct native images
and nonaliasing guest backing. The initial MRT path also requires contiguous mapped backing for each target.
Absent fragment outputs preserve their bound targets, and zero component masks skip their final resolves.
Partial output declarations with effective component writes remain excluded until all blending inputs are proven.
Filled triangle lists, the vertex splitting proof and a translator proof of sample-invariant fragment sources are
required. Fragment inputs, textures, constant-buffer interfaces, other guest memory, sample-dependent instructions,
active advanced blending and dual-source blending do not qualify. Optional depth/stencil must have the raster
sample count. A final resolve of an arbitrary whole draw would not preserve these semantics.

### Descriptors and GPU ownership

Color and depth/stencil discard hints retain their command order and replay against immutable target snapshots.
The renderer receives a whole-surface hint only for a proven unpadded, single-layer view covered by every scissor,
with no active conditional predicate. Partial views, volumes, other layers and padding keep their contents. A
discard may legally preserve contents; this hint can avoid a future attachment load but does not remove stores
that were already recorded.

`vk_descriptors=0` selects sets, `1` selects descriptor buffers, and `2` prefers `VK_EXT_descriptor_heap`. The default
`3`, Automatic, is `2` on a GPU that shares the system's memory and `0` on one with memory of its own; `2` selects
heaps, buffers, then sets according to device support. Existing shader set/binding decorations map to
separate resource and sampler index runs; heap pipelines use their own push-data state. Auxiliary compute pipelines
keep legacy descriptor pools. Legacy binds invalidate heap state, and the descriptor-set bind generation forces
descriptor buffers to be rebound after an auxiliary pass. Pipeline, complete-set and binding caches retain the
selected backend in their state.

Heap capacity includes the implementation's reserved ranges. Resource and sampler frame partitions are clamped
independently to the queried limits, with exact descriptor sizes and alignments. Sampler encodings are interned by
their opaque bytes; all comparisons use CPU shadows. Live heap bytes, index tables and buffer descriptors remain
immutable until the frame fence and command-pool reset permit reuse. A failed heap write does not submit midway
through draw setup: the complete draw retries through buffers or sets. Buffer arenas are allocated lazily when used.

Heap resource-cache entries use 32 bytes with an exact 28-byte key; sampler entries use 16 bytes and verify hash
matches against every opaque encoding byte in the CPU shadow. The 4,096 resource and 2,048 sampler entry capacities
are unchanged. This saves 49,152 bytes of CPU metadata without reducing the descriptor heap or its cache capacity.
Consecutive identical indexed image descriptors reuse their first resolved index. Unformatted buffer misses can
encode up to eight descriptors per native call, with distinct scratch destinations and publication only after a
successful call. Logical descriptor counts and actual native call counts remain separate benchmark fields.

Only active indexed map rows are staged, and unchanged words retain their staging address within the frame. Indexed
image bindings still contain complete 256-element arrays, including null entries. Changes to another descriptor can
therefore require another full index table even when its image encodings are reused. Compact heaps can retire more
often for that workload. The separate [descriptor measurements](GPU_DESCRIPTOR_BENCH.md) report copied bytes,
physical allocation, cache storage, encoding calls/hits and retirement counts for each backend and workload. They
include retirement waits and recording completion in CPU submission timing. Binary content checks surround every
gate and measurement, and measured device names must match the renderer gate.

The latest upstream GPU ownership changes remain in place. Eligible graphics storage-image ranges stay GPU-owned
until the CPU touches them; a retained owner supplies the original bytes if a CPU merge later becomes necessary.
All five graphics stages transfer or release their retained references on commit or retry. Pending buffer lookup
uses covered pages, pitch-linear texture updates copy changed rows, and DMA copies only affected owned ranges and
tiles. These mechanisms preserve the write-tracking and ownership fallbacks for ranges that cannot stay on the GPU.

## Repeatable checks

Configure with `SNX_TEST_VULKAN=ON`, then build before running the harness. It needs only this checkout, the built
fixtures and a Vulkan driver. It does not build or install dependencies. Keep the source and binaries unchanged during
its run. Supply a new output directory for each run.

```sh
cmake --build build --target snx-test-nvn-descriptors snx-test-nvn-tex-storage \
  snx-test-nvn-commands snx-test-nvn-draw-banks snx-test-nvn-copy-formats snx-test-nvn-names \
  snx-test-nvn-advanced-blend snx-test-vk-coverage snx-test-vk-tess-passthrough \
  snx-test-vk-sample-locations snx-test-shader-indexed-ms snx-test-shader-tcs-dead-outputs \
  snx-test-shader-direct-regressions snx-test-shader-float-rounding snx-test-shader-integer-half snx-test-shader-blend-images \
  snx-test-shader-half-native snx-test-shader-half-packing snx-test-shader-txd-uniform \
  snx-test-nvn-queue-lifetime snx-test-vk-advanced-blend snx-test-vulkan -j6
ctest --test-dir build -R 'nvn_contracts|vk_sample_locations|vk_tess_passthrough|shader_(indexed_ms|tcs_dead_outputs|float_rounding|integer_half|blend_images|half_native|half_packing|txd_uniform)'
python3 tools/check_nvn_rendering.py --validation --record both --out .cache/nvn-render-check-new
```

`--icd /path/to/driver.json` adds another driver to the default driver. Each GPU runs the full headless output suite
with descriptor sets, descriptor buffers and descriptor heaps, using staging memory. `--record both` checks synchronous and threaded
command recording; the default checks synchronous recording. `--validation` enables the installed validation layer.
A driver returning 77 is
reported as skipped, never as passed. Each GPU run must include the exact-output markers for all required NVN
features. Optional hardware features need their own explicit PASS or SKIP marker. The JSON preserves feature
skips even when the other checks pass. `all_gpu_features_passed` cannot be true with skips.

The output contains runtime source hashes in `sources.json`, binary hashes, per-process logs and `results.json`.
It checks binary hashes before/after each process and source hashes after the suite. These detect changes during a
run. They do not prove that a binary was built from the captured sources. Build first.

The earlier follow-up revision passed 19 selected checks across the NVIDIA RTX 5050 Laptop GPU and AMD Radeon
780M, descriptor sets/buffers and synchronous/threaded recording. That validation result applies to that revision.
AMD explicitly skipped native advanced blending and mixed-sample coverage, while its ordered shader blend path
passed. Earlier mutation checks exercised transform-feedback counts, coverage indexing, tessellation input selection,
retained metadata, subroutine addresses, sRGB transfer and blend endpoints.

The preceding integrated revision passed all 29 selected checks: 21 native fixtures and eight complete Vulkan runs across
NVIDIA RTX 5050 Laptop and AMD Radeon 780M, descriptor sets/buffers and synchronous/threaded recording. Source
and executable hashes remained unchanged during the matrix. Both descriptor paths also passed the forced-UMA
suite after the new fixtures received isolated guest-memory ranges.

Each GPU run checks nine shader-read cases, 8,192 integer-to-half results, 768 floating-point narrowing results, 128 half texture/arithmetic
results, 170 strict gradient samples and 24 uniform-offset samples, in addition to the existing rendering suite.
The native conversion path and software fallback have separate tests. Disabling map invalidation makes the
changed-table regression fail. Restoring the old full-window shader read makes the no-host-wait regression fail.
Both mutations were restored byte for byte before rebuilding. A third regression puts another mapping into the
source hole left by `mremap`; whole-range cleanup fails it, while prefix/suffix cleanup preserves the mapping.

The earlier integrated 489-test CTest run passed 485 checks and skipped three optional QEMU checks. The SDL keyboard-focus
check failed under the dummy video driver. All Vulkan checks passed. The five POSIX platform variants also passed
after the alias cleanup change. All 55 Linux and MinGW syntax checks passed for the changed C sources.
A full Windows cross-link remains unavailable locally because the target LLVM import library is absent.

AMD explicitly skips native advanced blending and native mixed-sample coverage. NVIDIA 615.78.08 returns mip 1
for a separate cross-face cube-gradient probe whose analytic result is mip 2. That exact driver/result combination
is reported as a skip; every other mismatch fails. The 170 strict center and magnification-edge cases remain
mandatory. The report's `all_gpu_features_passed` is false when any feature is skipped.

Validation used an unmodified upstream Khronos layer at revision
[0eb481b](https://github.com/KhronosGroup/Vulkan-ValidationLayers/commit/0eb481b2d48ea4697d0aa49bb3f39e6c0123f569),
version 1.4.367. The installed 1.4.363 layer incorrectly sums clip-distance inputs and outputs; the newer layer checks
each interface separately. No validation errors were filtered. Use a current layer for the clip-forwarding checks.

After integrating upstream `f97e460a`, the full Clang build and 69 selected CPU tests passed. The rendering
harness covers CPU/shader executables and complete GPU runs with sets, buffers and heaps, recording off/on, on
NVIDIA RTX 5050 Laptop and AMD Radeon 780M. Every validation diagnostic now fails the run, including messages
whose VUID appears on a later line. The report records executable hashes, the GPU source snapshot and explicit
capability skips. Four separate storage-buffer retention runs passed on those devices and recording modes.
All 264 exported coverage/depth modules passed Vulkan 1.3 SPIR-V validation.

Storage-only fixtures declare no unused color outputs. The headless test exits through the runtime's immediate-exit
helper after its assertions and output flush, preserving every exit status. This avoids unloading the video driver
while detached driver workers still run; the NVIDIA recording-thread run previously reached its final PASS and then
stalled in driver library cleanup.

## Remaining gaps

These valid rendering combinations still need broader support or exact device behavior:

* Late-depth advanced blending outside both the guarded single-sample D32F/S8 shadow subset and supported source
  capture lists still needs broader support. Fixed-point, multisample or layered depth, guest FragDepth, depth
  bias/clamp, conservative rasterization, predication, queries and depth-feedback cases remain gated on the shadow
  path. Tessellation, geometry, indirect draws and indexed vertex side effects can use that subset, but remain
  outside source capture. Forcing early tests is not equivalent.
* Portable mixed-sample coverage still needs broader interpolated and sample-dependent fragment shaders, multiple
  color samples, sparse/noncontiguous or aliased MRT backing and targets beyond the available capture attachments.
  The per-primitive path supports proved invariant sources, up to seven noninteger C1 targets and optional
  depth/stencil at the raster count. A whole-draw resolve cannot preserve overlap-dependent blending.
* Portable alpha dithering implements the published spatial behavior, not a bit-identical undocumented hardware
  pattern. Smooth-line coverage ordering would require more work if that raster mode is introduced.
* Conservative subpixel precision bias is retained in draw state. The renderer currently uses the host's raster
  precision instead of an exact implementation of the requested bias.
* Minimum-LOD forms beyond the decoded TEX/TEX.B automatic and bias cases remain unsupported. Fragment `MinLod`
  requires the host feature. Unproven LC encodings on other texture instructions are still rejected.
* On devices without maintenance8, full varying 3D TEX/TXD domains still generate thousands of native sample cases.
  Correct routing does not establish an acceptable shader compilation cost on every such driver; the maintenance8
  path and common literal or proven-uniform fallback paths remain compact.
* The NVIDIA minified cube-edge probe above remains a driver-specific discrepancy, distinct from strict center
  and magnification-edge coverage.

Custom sample grids must fit the device's exact grid, coordinate precision and compatible depth-image support.
Geometry streams also depend on `geometryStreams` and the separate `transformFeedbackStreamsLinesTriangles`
property. Tessellation, depth bounds, large points, descriptor budgets and storage image formats retain their
device limits. These limits are distinct from undefined API inputs and from incomplete shader translation.

## Performance limits

The maintenance8 TXD fixture checked 3,216 bound/bindless 1D, 2D, array and 3D samples on NVIDIA 615.78.08 and
AMD RADV 26.2.6, with zero validation messages from layer 1.4.367. The complete fixture process took 0.57 seconds
on NVIDIA (401 MiB peak RSS) and 0.08 seconds on AMD (105 MiB peak RSS). Earlier expanded 3D variants exceeded
the NVIDIA test's 4 GiB RSS budget before completing. These are fixture process costs, not GPU kernel times or
frame-rate comparisons. All 152 exported texture modules passed `spirv-val`; only the 22 native-offset modules
used its maintenance8 `--allow-offset-texture-operand` option. Portable dispatch modules used ordinary Vulkan 1.3
validation.

Copies, swizzles, conversions and composition remain GPU operations. The Linux alias change preserves shared pages
without a CPU copy. Generated shader caches avoid repeated module construction, and indexed tables reuse unchanged
uniform data within a frame. These mechanisms do not establish an overall CPU reduction or a frame-rate gain.

The measured optimization packs and stages only the active indexed-table map rows. Seven serial, shuffled paired
rounds with exact-output gates produced these component timings:

| Active indexed tables | Baseline median, ns/operation | Candidate median, ns/operation | CPU time reduction | Faster paired rounds |
| --- | ---: | ---: | ---: | ---: |
| 1 | 1857.822 | 98.339 | 94.7% | 7/7 |
| 2 | 909.439 | 193.923 | 78.7% | 7/7 |
| 5 | 1004.337 | 462.797 | 53.9% | 7/7 |
| 10 | 914.208 | 924.648 | -1.1% | 2/7 |

These synthetic results measure map preparation and staging, not the full renderer, GPU time, total CPU use or game
FPS. The maximum-size case shows no measured improvement. Absolute timings depend on the host and run conditions.
Keep the paired raw samples, workload parameters and binary hashes with each local comparison.

Use [the paired CPU harness](GPU_HOTPATH_BENCH.md) to compare the selected CPU workloads with matched fixtures.
