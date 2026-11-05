<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# NVN rendering audit and synthetic checks

## Index

* [Scope and fixes](#scope-and-fixes)
* [Additional rendering support](#additional-rendering-support)
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
| Advanced blending | Portable fixed-function Porter-Duff modes and SCREEN for supported premultiplied, uncorrelated inputs. Selected nonpremultiplied source modes also have exact mappings. | Independent numerical reference plus 13 real GPU blend cases. This is a subset of the API. |
| Coverage modulation | On native mixed-sample hardware, color-only coverage supports NONE/RGB/ALPHA/RGBA, linear modulation and the 16-entry custom table. Pipeline keys retain active coverage state. | NVIDIA checks all 17 representable coefficients with 2x/4x raster sampling. AMD explicitly skips this feature. |

The fixed-state payloads share one opcode and remain outside the serialized draw-state ABI. Per-draw state only
copies the enabled fixed-state arrays. Pipeline keys separate host capabilities and effective coverage state.
Generated tessellation modules stay cached by patch size and interface mask for repeated draws.
Program metadata tables grow instead of crashing after 1,024 shader updates. Their shared binary lookup retains
immutable declarations and capture layouts for old command recordings. Insertions also handle shader threads that
finish out of serial order. A 1,153-update regression checks growth, every saved serial and old/new recordings.

## Repeatable checks

Configure with `SNX_TEST_VULKAN=ON`, then build before running the harness. It needs only this checkout, the built
fixtures and a Vulkan driver. It does not build or install dependencies. Keep the source and binaries unchanged during
its run. Supply a new output directory for each run.

```sh
cmake --build build --target snx-test-nvn-descriptors snx-test-nvn-tex-storage \
  snx-test-nvn-commands snx-test-nvn-draw-banks snx-test-nvn-copy-formats snx-test-nvn-names \
  snx-test-nvn-advanced-blend snx-test-vk-coverage snx-test-vk-tess-passthrough snx-test-vulkan -j6
python3 tools/check_nvn_rendering.py --out .cache/nvn-render-check-new
```

`--icd /path/to/driver.json` adds another driver to the default driver. Each GPU runs the full headless output suite
with descriptor sets and descriptor buffers, synchronous recording and staging memory. A driver returning 77 is
reported as skipped, never as passed. Each GPU run must include the exact-output markers for all required NVN
features. Optional hardware features need their own explicit PASS or SKIP marker. The JSON preserves feature
skips even when the other checks pass. `all_gpu_features_passed` cannot be true with skips.

The output contains runtime source hashes in `sources.json`, binary hashes, per-process logs and `results.json`.
It checks binary hashes before/after each process and source hashes after the suite. These detect changes during a
run. They do not prove that a binary was built from the captured sources. Build first.

The expanded suite passed all 13 selected checks: nine native fixtures and both descriptor paths on the NVIDIA RTX
5050 Laptop GPU and AMD Radeon 780M. Only AMD coverage modulation reports a feature skip. Thus
`all_selected_passed` is true and `all_gpu_features_passed` is false. Source and per-run binary hashes stayed unchanged.

Related native and Python checks pass. ASan/UBSan checks pass for draw state, copy formats and frontend/program
metadata. All four generated control-stage SPIR-V variants pass `spirv-val --target-env vulkan1.3`. Expanded compiler
warnings on 17 changed runtime translation units show no new warnings. Four existing warnings remain.

The full 450-test run on this host passed 444 tests and skipped three. `gamepad` failed its SDL dummy-driver
keyboard-focus assertion. `vulkan_headless_uma` and `vulkan_headless_descriptor_buffers_uma` failed the placed-view
coherence assertion. Both UMA failures also reproduce on unchanged upstream. The two-driver matrix above uses
staging memory (`--vk_memory=1`).

Mutation checks deliberately break the transform-feedback count, coverage coefficient index and tessellation input
vertex index. Their respective tests fail. The sources are restored before the final hardware matrix.

## Remaining gaps

These rendering gaps need further work:

* Artistic advanced blend equations and disjoint/conjoint overlap modes still need ordered destination access or
  another exact implementation. Unsupported cases retain the ordinary blend fallback with a diagnostic.
* Coverage modulation needs a portable implementation on AMD and other devices without native mixed-sample
  coverage. Mixed depth/color sample counts, sample shading and fragment SampleId use are not implemented by this path.
* Evaluation-only tessellation rejects explicit per-vertex point size, layer, viewport and clip-distance forwarding.
  Persisting its generated control modules in the startup pipeline cache needs separate support.
* Three-dimensional copy format conversion and transformed packed depth/stencil formats without host blit support
  remain gaps. Invalid NVN flag combinations are rejected rather than counted as missing features.
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
