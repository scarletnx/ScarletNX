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
| Graphics storage images | Vertex, tessellation control/evaluation, geometry and fragment stages have their own image and buffer-image bindings and integer format variants. Graphics layouts share the whole-pipeline descriptor budget. Compute retains an independent layout and its full per-stage budget. | Frontend bank tests, generated modules and per-stage load/store fixtures. |
| Indexed multisample textures | Dynamic handle tables retain multisample image types and explicit sample operands for 2D and 2D-array textures. Integer and floating-point view types stay distinct. | Six compiler fixtures and a two-texture, four-sample GPU fetch fixture. |
| Texture gradients and offsets | 3D and cube-array gradient samples retain dimensions, faces and layers. Proven uniform texel offsets become exact `ConstOffset` operands; changed words select another shader variant. | 170 strict samples, a separate minified cube-edge probe, and 24 changed-offset/mip samples. |
| Half texture results | Texture and half-arithmetic result packing uses exact nearest-even conversion. Native float16 conversion requires explicit host rounding, denormal and signed-zero controls; other devices use integer significand rounding. | 128 real R32F TEXS/TLDS/TLD4S/HADD2 cases, plus independent conversion checks. |
| Cross-stage image aliases | A draw uses compatible sampled/storage layouts across all stages and retains its complete write access until the next dependency. Target snapshots finish before the programmable blend transition. | TES stores with FS sampling and TES/GS target snapshots across consecutive draws, without host waits. |
| Shader code reads | Coherence requests grow from a short prefix instead of landing unrelated GPU results in a fixed 128 KiB window. BRX retains the full-window path because its proof inspects later branches. | Nine hash/length differential cases and a regression that rejects a forced full-window GPU wait. |
| DrawTexture | A generated vertex/fragment pair rasterizes the rectangle with the bound sampler, component masks, blending and alpha tests. Depth/stencil tests and conservative rasterization follow the API's DrawTexture rules. | Dedicated sampler, edge clamp, blend, alpha-test, write-mask and MSAA fixtures. |
| Evaluation-only tessellation | A generated control shader forwards patch positions, generic attributes, point size and clip distances, then writes fixed levels. Clip interfaces use eight elements across stages. Earlier layer/viewport outputs are inert before evaluation. | Selected input vertices, fixed levels, point/clip payloads, opposite producer/consumer clip indices, interface bounds and generated-module fixtures. |
| Transform feedback | Save, pause, resume and restore GPU counter controls per owner. A GPU shader converts captured bytes and stride into indirect draw arguments. | Exact counter data, owner isolation, control padding and a capture/redraw round trip. No CPU counter readback in the runtime. |
| Advanced blending | Exact ordinary-blend reductions, native advanced operations and an ordered shader path. Each path has separate device and draw restrictions, described below. | Independent numerical references, pipeline capability tests and Vulkan equation, overlap, mask and discard checks. |
| Coverage modulation | On native mixed-sample hardware, color-only coverage supports NONE/RGB/ALPHA/RGBA, linear modulation and the 16-entry custom table. Pipeline keys retain active coverage state. | NVIDIA checks all 17 representable coefficients with 2x/4x raster sampling. AMD explicitly skips this feature. |
| Custom sample positions | Exact sample grids, sixteenth-pixel coordinates and lower-left grid phase enter the pipeline key. Repeated grids reduce only when their patterns are identical. Depth images retain compatible sample-location state. | CPU keys and GPU custom-raster fixtures, with explicit device limits. |
| Coverage to color | Native coverage output or a safe post-depth shader path writes signed/unsigned single-component integer masks. The shader path retains the early-test and output restrictions described below. | Per-sample depth rejection and signed/unsigned mask fixtures. |
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

`tools/check_nvn_contracts.py` indexes API declarations, runtime resolver routes, lookup hooks and handler bodies.
The current inventory contains 699 declaration names across 562 canonical APIs, including 137 inline fastpath
variants. All ordinary core API names have a runtime lookup. The 24 unmatched declarations comprise 21 vendor
extensions and three internal debug validators.

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

### Advanced blend paths

The renderer uses three GPU paths for advanced state on color target 0. Blend enable and ordinary logic-op state
still control whether that state applies. NVN advanced blending supports one color target, target 0. Multiple active
color targets have undefined API behavior and are not a missing MRT feature. Shader and pipeline keys retain the
effective equation and its parameters.

| Path | Supported behavior | Restrictions |
| --- | --- | --- |
| Ordinary blend reduction | PLUS, MINUS and RED/GREEN/BLUE selectors. With normalized destination values: ZERO/SRC/DST, premultiplied uncorrelated Porter-Duff modes and SCREEN, plus nonpremultiplied SRC and SRC_OVER. | Only the parameter combinations with an exact fixed-function equation qualify. Other correlated Porter-Duff and artistic equations need another path. |
| Native advanced blend | Maps all 46 NVN advanced modes to `VK_EXT_blend_operation_advanced`, including artistic/HSL operations and disjoint/conjoint overlap. | The device must provide coherent operations. Its limits must permit the attachment count, operation, source premultiplication and overlap. Without `advancedBlendAllOperations`, only the extension's required operation subset qualifies. |
| Ordered shader blend | Computes the 46 equations with an ordered destination read/modify/write under fragment shader interlock. Supports source premultiplication, all three overlap modes, component masks, array layers, sRGB transfer and multisample storage images. Guest storage images use separate bindings. Indexed texture tables retain target-feedback checks. | Requires pixel interlock, fragment storage writes and storage-image reads/writes without format. The target must be storage-capable. MSAA also requires multisample storage images and a supported raster sample count. Depth/stencil requires safe early tests, with PostDepthCoverage for multisample masks. Mixed-sample coverage, alpha-to-coverage and alpha-to-one remain outside this shader path. |

The shader path rejects integer targets, volume targets, additional fragment outputs and depth/sample-mask outputs.
The blend attachment occupies the image binding after the guest storage images, so both must fit the same stage
budget. A pipeline without blending retains its ordinary guest image capacity. Indexed tables resolve their entries
before applying target-feedback snapshots and ordering. The shader path uses the destination image directly.
Ordered interlock preserves overlap between primitives within one draw.

Safe early tests mean that the guest program explicitly requests early tests, or that moving tests earlier cannot
change discard, alpha-test or storage side effects. A general late-depth program with discard cannot use this shortcut.
Forcing early tests can update depth/stencil for a fragment that later discards. PostDepthCoverage does not remove
that semantic difference. Native advanced blending remains available for supported device and format combinations.

Unpremultiplied sources retain their original RGB values. Premultiplied color normalization preserves exact zero
and one endpoints, and HARDMIX compares its threshold before division. This avoids large discontinuous errors from
the division accuracy permitted by the [Vulkan SPIR-V environment](https://docs.vulkan.org/spec/latest/appendices/spirvenv.html).

`NormalizedDst` is a value-range hint. It does not select destination premultiplication. Native and shader paths keep
the advanced equations' premultiplied destination interpretation for either value of the hint. The ordinary reduction
path requires the hint only for equations that depend on normalized destination values.

States outside these gates do not have an exact ordered-shader implementation. The native GPU fixture checks only
combinations that the device advertises. The shader fixture disables the native
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
  snx-test-vk-sample-locations snx-test-shader-indexed-ms snx-test-shader-tcs-dead-outputs \
  snx-test-shader-direct-regressions snx-test-shader-float-rounding snx-test-shader-integer-half snx-test-shader-blend-images \
  snx-test-shader-half-native snx-test-shader-half-packing snx-test-shader-txd-uniform \
  snx-test-nvn-queue-lifetime snx-test-vk-advanced-blend snx-test-vulkan -j6
ctest --test-dir build -R 'nvn_contracts|vk_sample_locations|vk_tess_passthrough|shader_(indexed_ms|tcs_dead_outputs|float_rounding|integer_half|blend_images|half_native|half_packing|txd_uniform)'
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

The earlier follow-up revision passed 19 selected checks across the NVIDIA RTX 5050 Laptop GPU and AMD Radeon
780M, descriptor sets/buffers and synchronous/threaded recording. That validation result applies to that revision.
AMD explicitly skipped native advanced blending and mixed-sample coverage, while its ordered shader blend path
passed. Earlier mutation checks exercised transform-feedback counts, coverage indexing, tessellation input selection,
retained metadata, subroutine addresses, sRGB transfer and blend endpoints.

The current revision passed all 29 selected checks: 21 native fixtures and eight complete Vulkan runs across
NVIDIA RTX 5050 Laptop and AMD Radeon 780M, descriptor sets/buffers and synchronous/threaded recording. Source
and executable hashes remained unchanged during the matrix. Both descriptor paths also passed the forced-UMA
suite after the new fixtures received isolated guest-memory ranges.

Each GPU run checks nine shader-read cases, 8,192 integer-to-half results, 768 floating-point narrowing results, 128 half texture/arithmetic
results, 170 strict gradient samples and 24 uniform-offset samples, in addition to the existing rendering suite.
The native conversion path and software fallback have separate tests. Disabling map invalidation makes the
changed-table regression fail. Restoring the old full-window shader read makes the no-host-wait regression fail.
Both mutations were restored byte for byte before rebuilding. A third regression puts another mapping into the
source hole left by `mremap`; whole-range cleanup fails it, while prefix/suffix cleanup preserves the mapping.

The integrated 489-test CTest run passed 485 checks and skipped three optional QEMU checks. The SDL keyboard-focus
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

## Remaining gaps

These valid rendering combinations still need broader support or exact device behavior:

* General late-depth advanced blending with discard, alpha tests or storage side effects needs native support or
  an exact GPU implementation of depth/stencil and blending. Forcing early tests is not equivalent.
* Mixed-sample coverage needs a portable path on devices without native support. Separate depth/color sample
  counts and shader invocation frequency must retain their semantics. A final whole-draw resolve cannot preserve
  overlap-dependent primitive blending. NVN requires the raster count to match depth/stencil samples when those
  tests run, and to be at least the color sample count.
* Alpha-to-coverage uses the host's coverage pattern and dither behavior. The NVN dither switch is retained, but
  exact programmable dither behavior still needs an implementation.
* Conservative subpixel precision bias is retained in draw state. The renderer currently uses the host's raster
  precision instead of an exact implementation of the requested bias.
* Shader forms that require minimum-LOD operands need exact translation and the corresponding host feature gates.
* Per-invocation TXD texel offsets still shift normalized coordinates using the base-level texture size. Under
  minification this differs from a selected-mip offset. Literal and proven uniform `ConstOffset` operands are exact.
* The NVIDIA minified cube-edge probe above remains a driver-specific discrepancy, distinct from strict center
  and magnification-edge coverage.

Custom sample grids must fit the device's exact grid, coordinate precision and compatible depth-image support.
Geometry streams also depend on `geometryStreams` and the separate `transformFeedbackStreamsLinesTriangles`
property. Tessellation, depth bounds, large points, descriptor budgets and storage image formats retain their
device limits. These limits are distinct from undefined API inputs and from incomplete shader translation.

## Performance limits

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
