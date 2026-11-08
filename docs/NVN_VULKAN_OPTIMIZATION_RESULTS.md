<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# NVN to Vulkan optimization results

These results measure specific CPU preparation paths and report separate streaming observations.
They do not establish an overall gameplay speedup, a 600% improvement, or a win over native NVN.
No native-console NVN measurement was available.

## Index

* [Scope and machine](#scope-and-machine)
* [Mechanisms](#mechanisms)
* [Paired CPU measurements](#paired-cpu-measurements)
* [Method and reproduction](#method-and-reproduction)
* [Separate GPU and game observations](#separate-gpu-and-game-observations)
* [Validation recorded before integration](#validation-recorded-before-integration)
* [Validation after integration](#validation-after-integration)
* [CI fixture follow-up](#ci-fixture-follow-up)

## Scope and machine

The measured baseline is `fa4abaa1c5329857d259282c54e59a6dd6f41592`.
These measurements predate the integration rebase onto upstream `f2464ce3`.
They describe the measured component implementations, not a new measurement after that rebase.
The host was an AMD Ryzen 9 7950X3D on Fedora Linux 45, with Clang 23.1.4 and `-O2`.
GPU checks and the separate ASTC experiment used an NVIDIA GeForce RTX 4090, driver 615.78.08.

## Mechanisms

- Geometry records compare the consumed state before marking it dirty. Viewport conversion visits changed
  slots; device origin and depth-mode changes invalidate all slots. State and generation checks cover both paths.
- Pending-page queries test page masks within a group instead of walking each page. The one-page case below
  is a negative control: this workload did not improve.
- Ownership cuts remove a consecutive run of fully covered entries with one suffix move. Reference counts,
  partial ranges and offsets retain their existing behavior. Landing also combines adjacent cleanup cuts.
- CPU-only texture and render-target input uses host upload staging. GPU-owned or pending input keeps the
  GPU staging path and its barriers. Render-target reloads still load the full required source spans.
- ASTC upload-budget accounting retires completed encoder work by its submission timeline. Oversized pending
  work retains its protective wait. Pipeline-cache snapshots defer queued or active graphics compilation.
- Pacing keeps the nominal period when display refreshes do not divide it closely. At 349.98 Hz, the old
  12-refresh rounding produced about 29.165 FPS for a nominal 30 FPS period. The cadence test covers this case.
- Resource counters separate nested CPU resource intervals. They can overlap pipeline and wait counters;
  these groups must not be added together or described as GPU execution timestamps.

## Paired CPU measurements

The table uses median nanoseconds per operation. Reduction is `100 * (1 - candidate / baseline)` using
the two medians; it is not the median of paired speed ratios. A negative reduction means a slower median.

| Workload | Operations/process | Baseline ns/op | Candidate ns/op | Reduction | Faster paired rounds |
| --- | ---: | ---: | ---: | ---: | ---: |
| Equal geometry | 1,000,000 | 233.3080 | 33.2660 | 85.742% | 5/5 |
| One changed viewport | 1,000,000 | 191.2220 | 53.4810 | 72.032% | 5/5 |
| Pending query, 1 page | 2,000,000 | 2.8020 | 2.8075 | -0.196% | 1/5 |
| Pending query, 16 pages | 2,000,000 | 6.2920 | 2.7915 | 55.634% | 5/5 |
| Pending query, 63 pages | 2,000,000 | 16.6805 | 2.7625 | 83.439% | 5/5 |
| Pending query, 64 pages | 2,000,000 | 17.4520 | 2.3790 | 86.368% | 5/5 |
| Cut 64 owners, retain 64 | 4,096 | 2,417.8298 | 176.0127 | 92.720% | 7/7 |
| Cut 1,024 owners, retain 1,024 | 256 | 666,854.4062 | 4,004.9219 | 99.399% | 7/7 |
| Cut 4,096 owners, retain 4,096 | 64 | 10,589,246.3906 | 16,091.8594 | 99.848% | 7/7 |

All 102 fixture-emitted measurements are in [the sanitized CSV](benchmarks/nvn_vulkan_cpu_2026-11-07.csv).
Its two initial `#` lines carry the license; skip those lines when reading it as CSV.
`round` retains the report's zero-based index. `order` is the process position, 1 or 2, within that round.
`fixture_sha256` identifies a synthetic benchmark executable, not a source revision or game module.
The original candidate labels are `optimized` for geometry/pending pages and `bulk` for ownership.

## Method and reproduction

The harness ran serial paired processes in shuffled order with seed 539: five rounds for geometry and
pending pages, seven for ownership. It required exact-output markers and identical workload dimensions,
and checked executable identity and SHA-256 before and after each run. Shared libraries were held fixed,
but were not hashed. Builds and other timing jobs were stopped during these measurements.
Geometry uses `clock()` CPU time. Each pending-page sample is already the median of five internal
`clock()` loops. Ownership sums `timespec_get(TIME_UTC)` wall intervals around cuts, excluding map resets.
These different timers and workloads must not be combined into one score.

Build both fixture versions before timing. The historical baseline executables used the old component
implementations with the synthetic benchmark workloads. `cpu-baseline/` below stands for those prepared
executables; the baseline revision alone does not contain all newly added fixture modes.
For geometry, construct a compatibility fixture from the baseline's `tests/nvn_draw_banks.c`: add the current
`geometry_records`, `apply_geometry_records`, `bench_geometry` functions and `--bench-geometry` dispatch, retaining
all timed state/generation checks. The later equal-geometry and partial-viewport regression functions belong only
in the candidate's full regression fixture. Use the compatible current header for both implementations; the old
implementation ignores its two added metadata fields. Its original implementation source SHA-256 was
`955e1bc7e7fcb680cf3c848b7e9eecceb7873a20c84ea7db99937d1a979cca5b`.

The historical baseline used a manual `-std=gnu11 -O2 -Wall -Wextra -Werror` build. The formal candidate used
CMake's `-O2 -g`, `-UNDEBUG` and OS defines. The complete historical compatibility-fixture snapshot was not retained.
For a new comparison, compile the same compatibility fixture against both implementations with identical flags
and generated headers, then run the candidate's complete regression fixture separately. These commands reproduce
the harness arguments, not the original executable hashes:

```sh
python3 tools/bench_gpu_hotpaths.py --kind hotpath \
  --baseline cpu-baseline/snx-test-nvn-draw-banks \
  --candidate optimized=build/tests/snx-test-nvn-draw-banks --arg=--bench-geometry \
  --rounds 5 --seed 539 --out .cache/nvn-proof-geometry
python3 tools/bench_gpu_hotpaths.py --kind hotpath \
  --baseline cpu-baseline/snx-test-pending-pages \
  --candidate optimized=build/tests/snx-test-pending-pages --arg=--bench \
  --rounds 5 --seed 539 --out .cache/nvn-proof-pending
python3 tools/bench_gpu_hotpaths.py --kind hotpath \
  --baseline cpu-baseline/snx-test-memory-own-cut \
  --candidate bulk=build/tests/snx-test-memory-own-cut --arg=--bench \
  --rounds 7 --seed 539 --out .cache/nvn-proof-ownership
```

Use fresh output directories. See [the harness guide](GPU_HOTPATH_BENCH.md) for construction and rejection rules.

## Separate GPU and game observations

A synthetic 4096 by 4096 ASTC 8x8 upload experiment compared existing RGBA8 and BC7 modes in the same
candidate binary. Three processes ran three repetitions per format, always RGBA8 before BC7. After removing
each format's first repetition, the six warm samples had medians of 2.10 ms and 34.85 ms through GPU completion
(`vk_astc_upload` followed by `vk_finish`), a 16.60x ratio. This is a configuration comparison, not patch A/B.
The input includes invalid/error-color blocks. Fixed order, GPU clocks and caches limit generalization.
Decoded image payload is 64 MiB versus 16 MiB; actual allocation bytes were not measured in that experiment.
Exact-pixel ASTC fixtures passed separately; the timed large-texture repetitions do not themselves check pixels.

In TotK 1.4.3, an opening burst with 983 render targets and 103 uploads was observed at 200 ms versus an earlier
1,619 ms. Route, caches and run state were uncontrolled. This does not assign the difference to this patch.
Three paired 30-second uncapped runs reached loading screens only. Their median paired FPS ratio was 1.0231,
with one slower candidate pair. They establish neither gameplay performance nor gameplay rendering correctness.
Cold graphics compilation and isolated slow frames remained. A separate six-pipeline compile-flag probe found
no case faster by more than 3% in every paired round; no gameplay flag change is justified by that probe.

## Validation recorded before integration

The saved results record 26 selected NVN, memory and harness CTests passing. Later streaming checks record
10 CPU and 9 GPU tests passing, including exact CPU/GPU-owned/pending staging bytes, ASTC upload retirement,
the protective upload wait, all 14 ASTC footprints and a 4 MiB allocation-cap test. Headless variants cover
default descriptors, descriptor buffers/heaps, forced UMA and disabled dynamic vertex input where supported.
ASan and UBSan checks passed for NVN draw state, ownership cuts and pending pages.
These batches overlap and must not be added into a total test count. Feature skips remain explicit.
The recorded `runtime_architecture` check failed on NX-only source-ownership configuration for `snx_splash`
and `snx_window_icon`. Khronos validation layers were unavailable; no validation-layer-clean claim is made.

## Validation after integration

Source integration was checked at `c6335ae1`, based on upstream `f2464ce3`, with Clang 23.1.4 and the existing
NX-only `RelWithDebInfo` build (`-O2 -g`, deployment off). The runtime and all changed-area test targets built.
The locally staged static LLVM toolchain required `CMAKE_C_STANDARD_LIBRARIES=-lstdc++` for C test executables.

The full local CTest attempt registered 512 tests: 501 passed, four skipped, five could not run, and two failed.
The new report initially lacked its required index and documentation-index link. Those were corrected and the
documentation check passed on rerun, giving 502 passing cases across the full attempt plus that rerun.
The remaining incomplete cases are explicit:

- `runtime_architecture` reports the previously observed NX-only ownership classifications of `snx_splash`
  and `snx_window_icon`.
- `llvm_fp_reply_abi`, `llvm_lowering`, `llvm_object_symbols`, `llvm_atomic_rmw` and `elf_relocate` could not run
  because their executables did not link against the local static LLVM target components. The broad build therefore
  did not complete every test target.
- `qemu_ref_selftest`, `gdbstub_native`, `llvm_qemu_differential` and `llvm_block_fuzz` skipped for local prerequisites.

All configured Vulkan/ASTC CTests passed after integration, including the updated banded target load/writeback
cases, descriptor-buffer and UMA variants, partial textures, ASTC exact pixels, split allocations, upload waits and
upload retirement. An additional full headless run with explicit descriptor heaps plus UMA passed and checked exact
CPU/GPU-owned/pending staging bytes. These correctness runs are not performance measurements.

The standalone filesystem-tree ASan/UBSan run passed 901 checks. Its standalone service runner could not link all
of its dependencies; the configured filesystem CTests passed. Windows, macOS, mobile and other GPU vendors were
not checked locally. Khronos validation layers remained unavailable. The interactive TotK observations above came
from the pre-integration runtime; the player's running process was left open during publication checks.

```sh
cmake --build build --target scarletnx snx-test-vulkan snx-test-astc-vulkan \
  snx-test-tex-upload-rows snx-test-vk-resource-stats snx-test-runtime-present \
  snx-test-vk-pipeline-save snx-test-vk-pipeline-probe-flags -j 2
ctest --test-dir build --output-on-failure -j 1 --timeout 120
build/src/nx/snx-test-vulkan -H --vk_descriptors_mode=2 --vk_memory=2
ctest --test-dir build -R '^(docs_check|tracked_material)$' --output-on-failure
python3 tests/run_filesystem_tree.py --cc clang --sanitize --only tree
```

## CI fixture follow-up

The initial sanitizer CI build exposed a fixture dependency on linker garbage collection. ASan retained constant
shader callback tables from the full `pipeline.c` translation unit, keeping unrelated shader/driver dependencies.
The cache activity and saving implementation now resides in a shared private include with unchanged function bodies.
The fixture compiles that implementation alone and keeps all deterministic interleaving and persistence checks.
Its section-GC options were removed. ASan and UBSan remain enabled.

The original instrumented fixture link failure was reproduced locally. The focused fixture passed both the
`SNX_TEST_SANITIZERS=ON` CMake build/test and the ordinary build/test. The runtime rebuilt and the full Vulkan
headless regression passed after the extraction. These checks do not change the historical timing measurements.

Windows CI also exposed Unix-specific assumptions in the Python benchmark fixtures. Fake runtimes now run through
`sys.executable` and the real subprocess runner, the valid source path is native and absolute, and settings hashes
use their original bytes, including CRLF. Rejected-run cases check their intended failure reason. All 14 tests
passed locally on Linux. Native Windows was unavailable locally, so native verification is delegated to CI.

This public evidence includes synthetic fixture measurements and aggregate observations only.
