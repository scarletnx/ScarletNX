<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# Performance regression tests

[Testing](TESTING.md) · [Building](BUILD.md) · [Development](DEVELOPMENT.md). Two small tests that time a CPU path
and a GPU path, warn on a small regression, and fail on one that a second run confirms. Neither uses a game, a window
or a frame.

## The goal, and what is covered now

The goal has two halves: do not make a game slower while it runs, and do not make compiling or recompiling a game
slower. The current two tests cover the hottest paths of the first half and **not the second**:

- **Playing.** `perf_cpu` times a recompiled A64 loop, the hot path of a game's code. `perf_gpu` times the block
  linear shader every texture upload and render target write-back runs. They are hot-path proxies: they do not time
  a frame, a draw call, a scene, the HLE services, staging or present pacing.
- **Compiling and recompiling.** `perf_compile` runs the real `scarletnx -r` path over a generated NRO with a fresh
  cache, one job and a fresh output folder, and times the CPU. It covers the LLVM compile, code generation, module
  link and deploy. It is a time, not a ratio, so its baseline is per runner and architecture.
- **A full game.** A real frame or scene needs a game or a synthetic render loop. It is not in these tests.

## In one minute

- **`perf_cpu`** asks the real LLVM backend to compile a 15 instruction A64 loop, then runs it against the same loop
  written in C. It alternates short samples of the two on one thread, so the machine’s speed cancels, and reports the
  median ratio. About a second.
- **`perf_gpu`** runs the real block linear shader over 8 MiB against a tiny copy shader in the same workgroup shape
  and dispatch grid, and compares their GPU timestamps. About three seconds on hardware.
- **`perf_compile`** recompiles a generated NRO with 512 functions and about 38,000 A64 instructions through the real
  `scarletnx -r`, with a fresh cache and one job, and measures the CPU time of the compile and deploy. About a second
  a run, five runs a block.
- A ratio near 1 means ScarletNX’s side costs about what the reference costs. A higher ratio means ScarletNX’s side
  got slower relative to the reference.
- A **canary** of 3 CPU rounds or 2 GPU pairs runs first. Only a regression over 1.5 times the hard ceiling, confirmed
  by a second canary block, stops before the full measurement.
- A **warning** is advisory, also gets a confirmation block, and says in its GitHub annotation that it may be
  variance. A **failure** needs the 99.9% lower bound over the line in two independent blocks.

## What they measure

Each test times two workloads together in the same process and compares their times. A ratio cancels most of how fast
the machine is, how loaded it is and what its clock is doing, because both workloads see the same machine. Only a
change that slows one side more than the other moves the number.

| Test | Workload | Reference | Rounds |
| --- | --- | --- | --- |
| `perf_cpu` | A 15 instruction A64 loop recompiled by the LLVM backend in this process | The same loop written in C, operation for operation | 3 canary rounds, then 401 short ABBA pairs, about a second |
| `perf_gpu` | The block linear deswizzle shader every texture upload and render target write-back runs, 8 MiB a dispatch | A compute shader copying the same 16 byte runs in the same workgroup shape and dispatch grid | 2 canary pairs, then 201 pairs, about three seconds on hardware |
| `perf_compile` | `scarletnx -r` over a generated 512-function NRO | No ratio: the CPU time of the real compile, link and deploy, with a fresh cache | 2 canary compiles, then five compiles a block |

The CPU test counts only the timing thread's CPU time, so another process does not add to it. It compiles with one
job, runs on one thread, pins itself to its processor on Linux and asks for a performance core on macOS, and takes two
samples of each side in an ABBA order in every round. The GPU test uses one compute queue, two 8 MiB buffers and a
few dispatches a sample, with no swapchain. Each sample repeats the dispatch enough times to cover a few
milliseconds, and device timestamps measure the dispatches where the device has them. A host whose wall clock is the
fallback puts the submission cost in both sides.

Either test reports the median ratio of its rounds, the interquartile spread of those ratios, and a 95% lower bound on
the median estimated from that spread.

Before the full block, a canary takes three CPU rounds or two GPU pairs and compares its median to **1.5 times the
ceiling**. A crossing runs a second canary block. Only when both are over that line at 99.9 percent confidence does
the test fail before the full block; otherwise the full measurement decides. The canary runs inside the same step and
process, so it costs a few milliseconds and requires no second binary or setup.


## What it can see, and what it cannot

These are canaries for the exact paths they time, not a game benchmark. A change that slows both the ScarletNX side
and its reference by the same amount is invisible, because only their ratio is reported. The reference is test-only,
so a real ScarletNX change normally moves only one side.

| Change | Caught? | Why |
| --- | --- | --- |
| The LLVM backend emits more or worse instructions for the loop’s integer, flag, select, shift, multiply and branch patterns | Yes, `perf_cpu` | Those are the 15 A64 instructions the loop runs |
| More overhead in the generated-code ABI or the loop dispatch | Yes, `perf_cpu` | The same function is entered and returns through the same ABI |
| A regression in SIMD, floating point, crypto, load/store, atomics or exception instructions | No | The loop does not execute them |
| A slower JIT compiler, a colder cache or a slower recompile | No | Compilation is outside the timed region |
| A slower multithreaded path or a service/HLE call | No | One thread, no service calls |
| A regression in `blocklinear.glsl`’s address math, bounds check or memory pattern | Yes, `perf_gpu` | That is the shader timed |
| A driver or compiler change that makes the block linear shader slower but leaves a simple copy alone | Yes, `perf_gpu` | The ratio moves |
| A change that makes every shader slower equally | No | The reference copy slows too, so the ratio cancels |
| ASTC, texture conversion, VIC, NVDEC, 3D draw or other shader pipelines | No | The GPU test runs one compute shader |
| Command recording, descriptor and pipeline setup, staging, barriers, guest-memory write-back, present pacing | No | Setup is outside the timing, and the workload is one dispatch |
| A real frame, a draw call, a loading screen or a game’s scene | No | There is no game, window or frame |
| A smaller regression than the noise band and the 95%/99.9% bounds | No | It is reported as a pass, or as advisory if in the warning band |
| A broken app bundle, a missing MoltenVK/framework, or a packaging or startup failure | No | These are separate test binaries, not the `.app`; the headless runtime smoke test covers the bundle |
| A regression on a different GPU than the one selected | No | One device is chosen, in the runtime’s order (discrete, then integrated) |
| A regression in the CPU test on a different machine state or CPU model | Only as far as the baseline key reaches | The ratio is per architecture and device kind, not per CPU model; a Mac on Apple silicon has its own key |

A regression outside this list can still be real and important. It needs a game run, a profile of the real path, or a
test for the path in question.

## Why a ratio

An absolute time would say how fast this machine is, not whether the code got worse. Both sides of a ratio run in the
same process on the same machine, so its clock, its load and its model mostly cancel. The baseline then only has to
describe the *relative* cost, which is far more stable across machines than a time. The cost is that the test cannot
detect absolute regressions that affect both sides, and that a ratio still moves with the machine’s state; the
baseline key, the noise bounds, the confirmation blocks and the advisory warning exist for that.

## What can move the number without a ScarletNX regression

- The machine changing its clock, its core type or its background load. The paired samples, the noise bounds, the
  architecture key and the Apple-silicon key are for this.
- A change to the test itself: the C mirror, the reference shader, the sample count or the thresholds. The ratio is
  only comparable while those stay put; a test change is reviewed like a runtime change.
- A host compiler or flag change that makes the C mirror faster or slower. The test is built with the project’s
  compiler, and the baseline is not expected to survive an arbitrary toolchain change.
- A CPU model or disk change moving `perf_compile`’s absolute time. Its baseline is per runner and architecture, and
  it uses a fresh cache and a warm-up compile so the page cache is not the difference.
- A driver change that affects one shader more than the other is a real user-facing regression and is reported, even
  though it is not a ScarletNX code change.

## Pass, warn, retest, fail

The baseline line for a key is `reference warn% fail% ceiling`:

- `reference` is the best stable ratio main has measured.
- `warn%` and `fail%` raise the reference to the two lines.
- `ceiling` is the hard backstop for a regression too large to be worth a retest.
- A sixth field, `locked`, keeps the ratchet from moving a line a person set for an intended trade-off.

A tier is entered from an estimate that includes the block's own noise: a **95% lower bound** for a warning and a
**99.9% lower bound** for a failure.

| Tier | Condition | Result |
| --- | --- | --- |
| Pass | 95% lower bound below the warning line | Green |
| Warn | 95% lower bound above the warning line, but the 99.9% bound is not over the failure line | A second block runs to confirm, then a green `::warning` annotation that says whether it was confirmed and that it may be variance |
| Retest | 99.9% lower bound above the failure line, or the ratio is over the ceiling | A second independent block runs |
| Fail | the second block's 99.9% lower bound is also above the failure line | `::error` annotation, nonzero exit |

A warning may be a false positive and says so in its annotation. The confirmation block makes it less likely, and it
costs another second only when a warning is already in reach. A failure is not raised on one run, nor on a difference
that the run's own noise could explain: both independent blocks have to put their 99.9% lower bound over the line.
A first block over the line that the second does not confirm becomes a warning, not a failure.

The annotations are GitHub Actions workflow commands, so a warning appears on the run without holding the merge. The
perf tests are their own CI steps and run the test binary directly rather than through `ctest -V`: CTest prefixes a
test's output with its number, which would stop GitHub parsing the `::warning` and `::error` lines. The warning text
always says that it is advisory and may be variance; the failure text says that two independent blocks agreed at
99.9 percent confidence.

On a machine that cannot give a trustworthy number, a local run skips with ctest's skip code (77):

- fewer than two online processors, or a one minute load average that leaves no processor idle;
- less than 128 MiB of free memory;
- no Vulkan loader or compute device (the GPU test);
- an interquartile spread over 50% of the median.

CI never skips: `SNX_PERF_REQUIRE=1` (or a plain `CI` environment variable) turns every skip into a failure with the
reason. A spread over 70% fails there rather than reporting a median. `SNX_PERF_SKIP=1` forces a local skip, and
`SNX_PERF_VERBOSE=1` prints every round.

## Platforms

| Platform | `perf_cpu` | `perf_gpu` | `perf_compile` | CI |
| --- | --- | --- | --- | --- |
| Linux x86_64, aarch64 | yes | yes, lavapipe | yes | **The merge gate**, 2 minute cap, tight bands |
| macOS arm64 | yes, `cpu_aarch64_darwin` | local with MoltenVK; not built in CI | yes, `compile_aarch64_darwin` | Advisory Mac job, 2 minute cap, wider bands |
| Windows x86_64 | yes, `cpu_x86_64_windows` | no Vulkan 1.3 ICD on the hosted runner | yes, `compile_x86_64_windows` | Not run in CI, local only |

The Linux runner is the one whose variance is low enough to hold a merge. The Mac job is already `continue-on-error`,
so its `::error` is visible without blocking, and its bands start much wider until its measurements say otherwise.
The Windows tests are built and can be run locally, but this project has no Windows runner that can time them, and
its hosted runners have no Vulkan device for the GPU test. Their keys are wide for the same reason.

A platform with more variance gets a wider band before it gets a tighter one. The Linux keys stay the useful fail
gate for a major regression.

## Running them

```sh
ctest --test-dir build -R '^perf_' --output-on-failure
```

`perf_compile` needs the built `scarletnx` and no GPU. It generates its NRO; no game, keys or extracted files are
needed. `ctest --test-dir build -R '^perf_compile$' --output-on-failure` runs it, and the CI step runs it directly
for the same annotation reason.

`perf_gpu` is built only in a build with `-DSNX_TEST_VULKAN=ON`, the same build as the other headless Vulkan tests.
It needs a Vulkan 1.3 device with a graphics and compute queue, the runtime's own requirement, and it takes the same
device order: a discrete card first, then an integrated one. An integrated GPU or APU needs nothing extra, because
the two buffers are filled and written on the GPU and never mapped. More than one device does not skip: a ratio
measured on any working one answers the same question. On Linux, Mesa's lavapipe serves (`vulkan-swrast` on Arch); a
Homebrew MoltenVK on macOS is found under its usual prefix, and `SNX_VULKAN_LIBRARY` names one elsewhere.

`SNX_PERF_BASELINE` reads a baseline other than the checkout's. `SNX_PERF_RESULT` writes the measured key and its
median, lower and upper bounds, for `scripts/perf-baseline.py`.

## Where main's baseline comes from

The committed [`tests/perf_baseline.txt`](../tests/perf_baseline.txt) is the reference. A pull request runs against
the file **from its base branch**, fetched by the CI step, never the copy it carries, so a change cannot relax its own
gate. A push to main runs the tests against the committed file and writes a proposed next baseline as a workflow
artifact and a step summary diff. A maintainer applies the parts that are improvements. The ratchet lowers a
reference only when the whole 95% interval of main's run is below it, so an improvement tightens the test, a small
regression passes inside the band but never moves the line, and regressions cannot accumulate. A reviewed edit can
raise a line for an intended trade-off, and `locked` keeps the ratchet off it.

The workflow that applies the proposed file automatically (a bot pull request) can be added without changing the
tests. Until then the proposal is reviewed like any other baseline change.

## Cost and margin

GitHub Actions is free for public repositories on the standard hosted runners, which this project uses. The Linux
step adds about ten seconds to a job that already builds for tens of minutes, and the advisory Mac step adds another
few seconds to its own job. They use one processor and one small GPU workload, so the cost is wall time and no billed
minutes. A separate job would add a runner start and a rebuild, so the tests stay steps in the jobs that already
built them.

More samples are cheap and the current counts are already heavier than the first version. At 401 CPU pairs the 95%
interval is roughly ±2% when a block's spread is 20%, and wider when the machine is noisier; at 201 GPU pairs it is
roughly ±2% to ±5% by the same measure. A warning adds one more block only when it is in reach. That makes a 5%
failure line plausible on a quiet, consistent runner, and a 3% warning advisory rather than a guarantee. The reported
lower bounds show the interval for each run. The limit is not compute: it is the bias between runs and between
runners, which more samples inside one run cannot remove. A Mac on Apple silicon shows
the CPU ratio moving by much more than that between sessions as its performance and efficiency cores change what the
C mirror costs. A 3% gate across main and a pull request needs both revisions measured in the same job on the same
machine (an A/B run against the base build), or a per-runner reference with its own history. The current baseline is
per architecture and device kind, not per CPU model.

The CPU ratio has a key of its own on Apple silicon (`cpu_aarch64_darwin`) for that reason. A new architecture or
runner kind gets a key or a wider band before it gets a tighter one.

## Changing a line

Run the test on a quiet machine, note the ratio and the reason in the commit. A slower workload raises the ratio, so
lowering a line tightens the test and raising one allows an intended trade-off. Do not raise a line to make a red run
green: first find what the ratio measured. The tests catch a regression in the parts they time, not a slow service, a
slow loader, a slow shader compiler or a slow frame, which need a profile of the real path
(`docs/DEVELOPMENT.md`).
