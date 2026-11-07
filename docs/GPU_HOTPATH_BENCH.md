<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# Paired GPU preparation benchmarks

`tools/bench_gpu_hotpaths.py` compares synthetic CPU work without a game. It requires exact-output checks from
each executable. It runs processes serially in a seeded, shuffled order, compares identical workloads, and hashes
each executable before and after every run. It does not build, install packages, change settings, or apply a winner.
The result is a workload-specific timing, not game FPS or a fastest configuration for all games.

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
