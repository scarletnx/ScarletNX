<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# NVN rendering audit and synthetic checks

## Index

* [Scope and fixes](#scope-and-fixes)
* [Additional rendering support](#additional-rendering-support)
* [Follow-up support and restrictions](#follow-up-support-and-restrictions)
  * [GPU event waits](#gpu-event-waits)
  * [Shader subroutines](#shader-subroutines)
  * [Copy format conversion](#copy-format-conversion)
  * [Advanced blend paths](#advanced-blend-paths)
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
| Evaluation-only tessellation | A generated control shader forwards patch positions and generic float attributes, then writes fixed levels. NVN program control metadata selects the domain, spacing and output primitive. | Exact positions and colors for selected input vertices 0/1/2, level-zero patch rejection, patch sizes 1/3/4/32 SPIR-V validation and program metadata lifetime tests. |
| Transform feedback | Save, pause, resume and restore GPU counter controls per owner. A GPU shader converts captured bytes and stride into indirect draw arguments. | Exact counter data, owner isolation, control padding and a capture/redraw round trip. No CPU counter readback in the runtime. |
| Advanced blending | Exact ordinary-blend reductions, native advanced operations and an ordered shader path. Each path has separate device and draw restrictions, described below. | Independent numerical references, pipeline capability tests and Vulkan equation, overlap, mask and discard checks. |
| Coverage modulation | On native mixed-sample hardware, color-only coverage supports NONE/RGB/ALPHA/RGBA, linear modulation and the 16-entry custom table. Pipeline keys retain active coverage state. | NVIDIA checks all 17 representable coefficients with 2x/4x raster sampling. AMD explicitly skips this feature. |

The fixed-state payloads share one opcode and remain outside the serialized draw-state ABI. Per-draw state only
copies the enabled fixed-state arrays. Pipeline keys separate host capabilities and effective coverage state.
Generated tessellation modules stay cached by patch size and interface mask for repeated draws.
Program metadata tables grow instead of crashing after 1,024 shader updates. Their shared binary lookup retains
immutable declarations and capture layouts for old command recordings. Insertions also handle shader threads that
finish out of serial order. A 1,153-update regression checks growth, every saved serial and old/new recordings.

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
remains unsupported. The compute converter does not handle integer, compressed or three-component RGB16F/RGB32F
conversion. RGBA4 can use exact same-format nearest transforms, but it has no descriptor for numeric conversion.

Same-format depth/stencil transforms use the host image blit when available. Otherwise the GPU copies each aspect to
scratch memory, gathers the requested nearest texels and copies each aspect back. This supports D32FS8 mirrors and
scaling without host blit support. It preserves depth and stencil independently. It does not add linear depth/stencil
filtering or arbitrary conversion between depth/stencil formats.

`vulkan_nvn_copy_conversions.inc` checks volume UNORM and float conversion, packed nearest/bilinear transforms,
sRGB transfer, rendered depth/stencil transforms and untouched row padding. `nvn_copy_formats` checks frontend format
selection and rejects unsupported conversion rather than copying incompatible bytes.

### Advanced blend paths

The renderer uses three GPU paths for advanced state on color target 0. Blend enable and ordinary logic-op state
still control whether that state applies. Other color targets must have no active writes for the native and shader
paths. Shader and pipeline keys retain the effective equation and its parameters.

| Path | Supported behavior | Restrictions |
| --- | --- | --- |
| Ordinary blend reduction | PLUS, MINUS and RED/GREEN/BLUE selectors. With normalized destination values: ZERO/SRC/DST, premultiplied uncorrelated Porter-Duff modes and SCREEN, plus nonpremultiplied SRC and SRC_OVER. | Only the parameter combinations with an exact fixed-function equation qualify. Other correlated Porter-Duff and artistic equations need another path. |
| Native advanced blend | Maps all 46 NVN advanced modes to `VK_EXT_blend_operation_advanced`, including artistic/HSL operations and disjoint/conjoint overlap. | The device must provide coherent operations. Its limits must permit the attachment count, operation, source premultiplication and overlap. Without `advancedBlendAllOperations`, only the extension's required operation subset qualifies. |
| Ordered shader blend | Computes the 46 equations with an ordered destination read/modify/write under fragment shader interlock. Supports source premultiplication, all three overlap modes, component masks and alpha-test discard. | Requires pixel interlock, fragment storage writes and storage-image reads/writes without format. The target must be a single-layer, storage-capable, non-sRGB 2D color image without MSAA. No depth/stencil attachment, mixed-sample coverage, alpha-to-coverage or alpha-to-one. |

The shader path also rejects integer targets, additional fragment outputs, depth/sample-mask outputs, indexed texture
tables, shader image accesses, layer/viewport outputs and texture reads that alias the destination. It uses the
existing destination image directly, without a CPU composite or an extra whole-draw blend pass. Ordered interlock
preserves overlap between primitives within one draw.

Unpremultiplied sources retain their original RGB values. Premultiplied color normalization preserves exact zero
and one endpoints, and HARDMIX compares its threshold before division. This avoids large discontinuous errors from
the division accuracy permitted by the [Vulkan SPIR-V environment](https://docs.vulkan.org/spec/latest/appendices/spirvenv.html).

`NormalizedDst` is a value-range hint. It does not select destination premultiplication. Native and shader paths keep
the advanced equations' premultiplied destination interpretation for either value of the hint. The ordinary reduction
path requires the hint only for equations that depend on normalized destination values.

States outside these gates retain ordinary blend behavior. They do not have an exact advanced implementation.
The native GPU fixture checks only combinations that the device advertises. The shader fixture disables the native
path to exercise interlock, including two overlapping primitives, component masks and discarded fragments. Numerical
checks use a tolerance of `0.003` for native operations and `0.00005` for the shader path. These account for device
arithmetic precision and do not permit missing equations or ordering errors.

## Repeatable checks

Configure with `SNX_TEST_VULKAN=ON`, then build before running the harness. It needs only this checkout, the built
fixtures and a Vulkan driver. It does not build or install dependencies. Keep the source and binaries unchanged during
its run. Supply a new output directory for each run.

```sh
cmake --build build --target snx-test-nvn-descriptors snx-test-nvn-tex-storage \
  snx-test-nvn-commands snx-test-nvn-draw-banks snx-test-nvn-copy-formats snx-test-nvn-names \
  snx-test-nvn-advanced-blend snx-test-vk-coverage snx-test-vk-tess-passthrough \
  snx-test-nvn-queue-lifetime snx-test-vk-advanced-blend snx-test-vulkan -j6
python3 tools/check_nvn_rendering.py --validation --record both --out .cache/nvn-render-check-new
```

`--icd /path/to/driver.json` adds another driver to the default driver. Each GPU runs the full headless output suite
with descriptor sets and descriptor buffers and staging memory. `--record both` checks synchronous and threaded
command recording; the default checks synchronous recording. `--validation` enables the installed validation layer.
A driver returning 77 is
reported as skipped, never as passed. Each GPU run must include the exact-output markers for all required NVN
features. Optional hardware features need their own explicit PASS or SKIP marker. The JSON preserves feature
skips even when the other checks pass. `all_gpu_features_passed` cannot be true with skips.

The output contains runtime source hashes in `sources.json`, binary hashes, per-process logs and `results.json`.
It checks binary hashes before/after each process and source hashes after the suite. These detect changes during a
run. They do not prove that a binary was built from the captured sources. Build first.

Before these follow-up features, the suite passed all 13 selected checks: nine native fixtures and both descriptor
paths on the NVIDIA RTX 5050 Laptop GPU and AMD Radeon 780M. Only AMD coverage modulation reported a feature skip.
Thus `all_selected_passed` was true and `all_gpu_features_passed` was false. Source and per-run binary hashes stayed
unchanged.

For that earlier revision, related native and Python checks passed. ASan/UBSan checks passed for draw state, copy
formats and frontend/program metadata. All four generated control-stage SPIR-V variants passed
`spirv-val --target-env vulkan1.3`. Expanded compiler warnings on 17 changed runtime translation units showed no new
warnings. Four existing warnings remained.

The full 450-test run on this host passed 444 tests and skipped three. `gamepad` failed its SDL dummy-driver
keyboard-focus assertion. `vulkan_headless_uma` and `vulkan_headless_descriptor_buffers_uma` failed the placed-view
coherence assertion. Both UMA failures also reproduce on unchanged upstream. The two-driver matrix above uses
staging memory (`--vk_memory=1`).

Earlier mutation checks deliberately broke the transform-feedback count, coverage coefficient index and tessellation
input vertex index. Their respective tests failed. The sources were restored before that hardware matrix.

The follow-up passed all 19 selected checks: 11 native fixtures and eight Vulkan runs across the same two GPUs,
descriptor sets/buffers and synchronous/threaded recording. Validation was enabled. No VUID or threading errors were
reported. AMD explicitly skipped native advanced blending and mixed-sample coverage; its ordered shader blend path
passed. Source and per-run binary hashes stayed unchanged.

Each Vulkan run checked 79,764 blend equation cases. All 276 generated equation modules passed
`spirv-val --target-env vulkan1.3`. ASan/UBSan passed for queue lifetime, frontend/program metadata and copy formats.
Four mutations broke retained texture pointers, absolute subroutine addresses, sRGB decoding and blend endpoints;
the respective native or Vulkan checks rejected each mutation. The source bytes were restored and rebuilt.

The full Clang build passed. Expanded warnings on 18 runtime files found the same four existing warnings. All 17
Python harness tests and the documentation checks passed. The full 469-test run passed 463 tests and skipped three.
The gamepad focus assertion and the two forced-UMA placed-view assertions listed above remained the three failures.

## Remaining gaps

These rendering gaps need further work:

* Advanced blend states outside the native, ordinary-reduction and interlock gates above retain the ordinary blend
  fallback. In particular, the shader path does not cover depth/stencil, multisampling, sRGB targets or general MRT.
* Coverage modulation needs a portable implementation on AMD and other devices without native mixed-sample
  coverage. Mixed depth/color sample counts, sample shading and fragment SampleId use are not implemented by this path.
* Evaluation-only tessellation rejects explicit per-vertex point size, layer, viewport and clip-distance forwarding.
  Persisting its generated control modules in the startup pipeline cache needs separate support.
* Copy conversion still excludes integer/compressed conversion, volume linear filtering and arbitrary depth/stencil
  format changes. Invalid NVN flag combinations are rejected rather than counted as missing features.
* Host features such as tessellation, depth bounds, large points and transform feedback still require device
  support. Synthetic tests on two devices do not establish behavior on every driver.

## Performance limits

The fixes retain GPU copies, swizzles and conversions. Z mirroring changes the slice index inside existing loops,
without adding GPU launches. Empty direct draws now avoid submission entirely. Indexed setters batch their selected
state in one record. Dirty-bank spans, equal-bind checks, selective replay, descriptor address reuse and recent
storage-result lookup reduce CPU preparation work.

The changing-channel-mask control exposed a regression from rebuilding unrelated blend factors and equations.
Channel-mask-only updates now compare and apply their eight bytes, advancing the same blend generation only when
needed. A draw with no dirty groups returns after checking draw parameters and device depth mode. Tests compare
every final draw-state byte, all eight target masks, and mixed mask/blend changes in either record order.

Seven serial shuffled paired rounds of one million operations on the Ryzen 9 270 produced these CPU medians:

| Synthetic workload | Original baseline, ns/operation | Final, ns/operation | CPU time reduction |
| --- | ---: | ---: | ---: |
| Change channel masks for every draw | 49.921 | 16.441 | 67.1% |
| Rebind equal packed state | 73.869 | 31.561 | 57.3% |
| Rebind equal UBO, SSBO and texture | 39.938 | 24.451 | 38.8% |

The baseline is the saved binary from before the earlier equal-bind optimization. These figures include that
earlier work. The candidate wins all seven paired rounds for each workload, with exact output gates on every sample.
Absolute timings vary with host conditions. This serial comparison measures the CPU preparation routines.
Keep raw samples and binary hashes for each local run.

Use [the paired CPU harness](GPU_HOTPATH_BENCH.md) to compare the selected CPU workloads with matched fixtures.
