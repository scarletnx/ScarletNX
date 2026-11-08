<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# Paired GPU preparation benchmarks

`tools/bench_gpu_hotpaths.py` compares synthetic CPU work without a game. It requires exact-output checks from
each executable. It runs processes serially in a seeded, shuffled order, compares identical workloads, and hashes
each executable before and after every run. It does not build, install packages, change settings, or apply a winner.
The result is a workload-specific timing, not game FPS or a fastest configuration for all games.

[Recorded NVN/Vulkan measurements](nvn-vulkan-optimization.md) include all synthetic samples, the test
coverage, and the limits of separate GPU and game observations.

Build the baseline and candidate before measuring. Keep both unchanged and stop other builds and timing jobs.
For the existing NVN inline-bank fixture:

```sh
python3 tools/bench_gpu_hotpaths.py --kind inline --baseline /local/before \
    --candidate updated=build/tests/snx-test-nvn-draw-banks --out .cache/inline-comparison
```

The existing indexed-handle fixture includes the old linear algorithm and several index strategies in one executable:

```sh
python3 tools/bench_gpu_hotpaths.py --kind indexed --baseline build/tests/snx-test-vk-indexed-handles \
    --candidate index8=build/tests/snx-test-vk-indexed-handles --indexed-limit 8 --out .cache/indexed-comparison
```

The baseline selects `linear_limit=-1`; candidates select `--indexed-limit`. The same per-round seed goes to both
processes. Other emitted strategies cannot enter that comparison. Exact markers are still required.

New fixtures can use a common marker:

```text
GPU_HOTPATH_BENCH workload=storage_lookup_recent operations=20000 ns=40000 exact=1 results=65 bytes=131072
```

Use a distinct workload name for each input shape. Required fields are `workload`, `operations`, `ns`, and `exact=1`.
Other fields must contain integer counters. `bytes`, `results`, `stages`, and `range_bytes`, when present, are immutable
input dimensions. They must match across all candidates and rounds. `--dimension KEY` adds another required input
dimension. Output cost counters such as `copied_bytes`, `uploads`, and `staged_bytes` may differ and are retained.
Each native fixture must check actual bytes and generations before it emits `exact=1`; the Python marker alone
does not prove correctness.

By default executables receive `--bench`. Repeat `--arg=VALUE` to replace that default with an explicit argument
list. Repeat `--variant-arg=NAME=VALUE` to append arguments for one strategy. A one-binary baseline mode can use:

```sh
python3 tools/bench_gpu_hotpaths.py --kind hotpath --baseline /local/inline-fixture \
    --candidate cached=/local/inline-fixture --variant-arg=baseline=--baseline --out .cache/upload-comparison
python3 tools/bench_gpu_hotpaths.py --kind hotpath --baseline /local/storage-before \
    --candidate cached=build/src/nx/snx-test-vulkan --arg=-H --arg=--vk_memory=1 --arg=--vk_record=0 \
    --arg=--bench-storage-lookup --out .cache/storage-comparison
```

Arguments are passed directly to the executable without a shell. `--candidate` can occur more than once. Candidate
names must be distinct, and `baseline` is reserved. The default is seven paired rounds, with a minimum of three.
`--rounds`, `--seed`, and `--timeout` control the run. Use a new `--out` directory for each experiment; existing
output is never overwritten. The default directory is `.cache/gpu-hotpaths`, which Git ignores.

The report keeps complete logs, commands, binary hashes, raw samples, and input dimensions. It reports each workload's
median time per operation, paired speedup, faster paired rounds, and median counters. It does not combine different
workloads into a universal score. An unsupported executable, timeout, output mismatch, changed dimensions, or changed
binary stops the comparison and writes a failed report. Shared libraries are not hashed; keep them fixed too.

`--index PATH` can attach the hash and API coverage fields from a local JSON index to the report as optional
experiment metadata. The benchmark needs no index to run. Keep generated results local.

```sh
python3 tests/bench_gpu_hotpaths_test.py
```

The unit tests require only Python's standard library and run no benchmark executable.

The indexed-map fixture measures the renderer's staging allocator and map copy path for one, two, five and ten
active tables. Both strategies run from the same executable. Before timing, GPU buffer copies check every active
word and untouched padding. GPU completion is outside the measured CPU interval.

```sh
python3 tools/bench_gpu_hotpaths.py --kind hotpath --baseline build/src/nx/snx-test-vulkan \
    --candidate active=build/src/nx/snx-test-vulkan --arg=-H --arg=--vk_memory=1 --arg=--vk_record=0 \
    --variant-arg=baseline=--bench-indexed-map=0 --variant-arg=active=--bench-indexed-map=1 \
    --index /local/contract-index.json --out .cache/indexed-map-comparison
```

The contract-index format attaches API and contract counts, plus the input file hash. It does not copy reference
identifiers or text into the benchmark report. Static priority and measured cost remain separate evidence. The
comparison measures map allocation and copying; it does not include a whole draw or claim a game FPS improvement.

## Paired game runs

`tools/bench_nvn_game.py` runs unchanged baseline and candidate runtimes against the same compiled game module.
Select an update by its own `*_code.so` path. Prepare the game and update before measuring, and stop builds first.

```sh
python3 tools/bench_nvn_game.py --game /local/game/updated_game_code.so \
    --baseline /local/before/scarletnx --candidate /local/after/scarletnx \
    --frames 600 --rounds 3 --timeout 300 --uncapped \
    --user-data /local/scarletnx-data --keys /local/prod.keys \
    --script /local/scene.snxtas --out .cache/game-comparison
```

`--script` accepts frame-based `.snxtas` events (`frame:buttons:lx,ly,rx,ry`), including button releases. Omitting it
gives neutral input. The harness copies the initial saves, settings, users and keys into private run folders, and
starts each run with fresh runtime and driver caches. External dump paths remain read-only shared inputs. Keep those
files unchanged. `--uncapped` applies `fps_limit=0` and `vsync=0` equally to both runtimes. Rendering settings stay
those of the source game. Keys remain private local files and their contents are never included in reports.

The report keeps commands, binary and module hashes, screenshots, logs and individual paired times. A crash, timeout,
missing or uniform screenshot, changed resolution, changed binary or incorrect frame count rejects the experiment.
The primary metric is total process wall time, including startup, preparation and the final screenshot. Runtimes that
emit `unattended_ns` also report the game's interval separately. That interval includes guest initialization, loading
and the final screenshot. Neither metric isolates NVN translation or GPU execution.

For uncapped games whose asynchronous presents overshoot the frame limit, replace `--frames 600` with `--seconds 30`
and omit `--script`. This mode measures actual frame count divided by `unattended_ns`, compares candidate/baseline
throughput ratios, and retains process wall time separately. It requires the runtime timing marker and permits only
neutral input. Scripted scenes and external native measurements require the strict frame mode. Timed runs can finish
in different animation states and need the same scene review as frame-limited runs.

Inspect the screenshots and the reached scene before interpreting timings. A matching final image does not establish
correct rendering throughout a run. Loading screens, frame pacing, filesystem caches, clocks and nondeterministic
game behavior can hide or imitate changes. These results do not compare against native NVN. Optional `--native` input
requires explicit matching workload, scene, version, frame count, input hash and end-to-end timing metadata. It stays
labeled as an unverified external comparison on different hardware. A throughput increase of 600% would require a
7x speed ratio for the same workload.

```sh
python3 tests/bench_nvn_game_test.py
```

## Streaming stalls

Slow-frame logs include internal texture pipeline creation and GPU timeline waits as well as guest shaders and
graphics pipelines. Resource CPU intervals name image requirements, allocation, destruction, target setup/reload,
reclamation and buffer work. Resource categories exclude nested resource intervals; they can overlap pipeline and
wait intervals, so these groups must not be added together. These are elapsed times inside CPU calls, not GPU
execution timestamps.

On a GPU without native ASTC, `astc_mode=1` decodes to exact RGBA8 and avoids BC7 re-encoding. The decoded image
payload is four times the size of BC7. Test this per game with sufficient memory and watch the cache/budget log.
The ASTC fixture's `--bench` compares identical input blocks through GPU completion; its timings do not measure game
FPS or native NVN. Its normal mode checks pixels against CPU references, including partial updates and images whose
allocations are split into pieces.

To investigate a driver compile stall, extract exact local cache records while the game is closed, then compare
fresh headless-test processes:

```sh
python3 tools/prepare_pipeline_probe.py --cache /local/game.vkshaders \
    --largest 6 --out /local/pipeline-probes
python3 tools/bench_pipeline_compile.py --binary build/src/nx/snx-test-vulkan \
    --manifest /local/pipeline-probes/manifest.json --rounds 3 --out /local/compile-comparison
```

The probe uses a fresh application cache and requests disabled driver disk caches. It changes compilation flags
only inside the test fixture. It measures compilation without draws and cannot establish rendering correctness or
justify enabling those flags for gameplay. Captured packages contain game-derived shaders and must stay local.
