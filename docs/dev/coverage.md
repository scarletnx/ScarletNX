<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# Coverage

## Index

* [What coverage measures here](#what-coverage-measures-here)
* [A coverage build](#a-coverage-build)
  * [Why continuous mode](#why-continuous-mode)
  * [Configure and build](#configure-and-build)
* [Collecting a profile](#collecting-a-profile)
  * [From edenprobe](#from-edenprobe)
  * [From the tests](#from-the-tests)
  * [From a game or a homebrew program](#from-a-game-or-a-homebrew-program)

[Development](development.md) · [edenprobe](edenprobe.md) · [build](build.md) · [profiling](profiling.md).

This page is a guide to measuring which of ScarletNX's own code a workload runs: edenprobe's suites, the tests, a
game. The goal is the project's surface area: which services, SVCs, renderer paths and loader branches anything
exercises, and which nothing does. Those last are the *unaccounted-for* paths: code that has never run under any
check, so a bug in it shows up first in a player's game.

The examples use LLVM's source-based coverage (`clang -fprofile-instr-generate -fcoverage-mapping`, `llvm-profdata`,
`llvm-cov`). gcov and GCC gives the GCC route. The numbers quoted are from a FreeBSD 15.1 machine
with Clang 23, at `origin/main` of 2026-11-15; yours will differ by commit and host.

Profiles, reports and the recompiled programs they came from stay local, like the games: keep them in the
gitignored `work/`.

## What coverage measures here

Coverage counts the runtime's own C and C++ code: the HLE kernel and services, the SVCs, the NVN and Maxwell front
ends, the Vulkan renderer, the shader translators, the loaders, the recompiler's driver and LLVM backend. It does not
count a game's code. The recompiled module (`<id>_code.so`) and the code the JIT compiles are made by the
recompiler, not by the compiler that instruments the build. What a game's code reaches is a different question,
answered by the static censuses and the log.

Two processes contribute to a game's run:

| Process | What it covers |
|---|---|
| The driver (`scarletnx -r`, or the first step of `scripts/edenprobe.py`) | `nx/loader`, `common/cpu` (decoder, function discovery, LLVM IR emission, the linker), `nx/runtime/driver` |
| The game's runtime (`recompiled/<id>/<id>`, a copy of the same `scarletnx`) | everything the game asks for at run time |

Both are the same binary, so one `llvm-cov` call reads both profiles against `build/cov/scarletnx`.

## A coverage build

### Why continuous mode

The runtime leaves through `plat_exit_now()` (`src/nx/runtime/platform/plat_posix.c`), which calls `_exit` on
purpose, so that no atexit handler runs against threads that are still going. Both LLVM's profile runtime and gcov
write their counters from an atexit handler, so an instrumented runtime that ends normally writes an **empty**
profile. edenprobe's launcher also kills a launch that hangs, which writes nothing either.

LLVM's *continuous mode* solves both. The counters live in a memory-mapped file from the program's start, so what
ran is on disk however the process ends: `_exit`, a crash, `SIGKILL`. It takes `%c` in `LLVM_PROFILE_FILE`, and on
ELF hosts other than Linux and Fuchsia the counters must be relocatable at run time, which
`-mllvm -runtime-counter-relocation` asks for. A plain instrumented build run through `_exit` left a 0-byte
`.profraw`; the same with continuous mode left the counts.

### Configure and build

A build folder of its own, with the instrumentation in `CMAKE_C_FLAGS`/`CMAKE_CXX_FLAGS`. The game modules'
runtime library (`rtlib.c`) is compiled with its own flag list (`src/common/cpu/recompiler/llvm/CMakeLists.txt`),
so it is not instrumented and the modules still link with no imports.

```sh
F="-fprofile-instr-generate -fcoverage-mapping -mllvm -runtime-counter-relocation"
cmake -S . -B build/cov -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
    -DSNX_DEPLOY=OFF -DSNX_JEMALLOC=OFF \
    "-DCMAKE_C_FLAGS=$F" "-DCMAKE_CXX_FLAGS=$F" \
    -DCMAKE_EXE_LINKER_FLAGS=-fprofile-instr-generate \
    -DCMAKE_SHARED_LINKER_FLAGS=-fprofile-instr-generate \
    -DCMAKE_MODULE_LINKER_FLAGS=-fprofile-instr-generate
cmake --build build/cov --target scarletnx          # the runtime and driver; add the test targets for ctest
```

- Use the same LLVM for `clang`, `llvm-profdata` and `llvm-cov`: the raw profile format changes between versions.
  On FreeBSD, give the compilers and the `SNX_LLVM_*` paths of one LLVM, as [Build](build.md) says, for example
  `clang23`/`clang++23` and `-DSNX_LLVM_CONFIG=/usr/local/llvm23/bin/llvm-config` (with `SNX_LLVM_OBJDUMP`,
  `SNX_LLVM_OPT`, `SNX_LLVM_AS` from the same folder).
- `SNX_DEPLOY=OFF` keeps the instrumented runtime from being copied over every game in your data folder, as the
  Tracy preset does. A run through `build/cov/scarletnx` still puts it beside the game it runs.
- Clang warns that `-runtime-counter-relocation` is unused at link time. The warning is harmless.
- The build instruments the dependencies CPM builds too (zstd, FSR2, ...). Reports leave them out by path
  (Reports).
- Keep `-runtime-counter-relocation` on every ELF host. Without it, continuous mode is refused where the profile
  runtime cannot map the counters in place.

## Collecting a profile

`LLVM_PROFILE_FILE` names where each process writes. Use `%p` (the process ID) so the driver and every launch get
files of their own, and `%c` for continuous mode. Use an absolute path, since the runtime changes folder.

### From edenprobe

[edenprobe](edenprobe.md) is the broadest single workload: hundreds of cases over the CPU, the kernel's scheduler,
the filesystem, video, display, audio and the SDK hooks, each judged against the console. `scripts/edenprobe.py`
passes its environment to the recompile and to each launch, so the variable is all it takes:

```sh
mkdir -p work/cov/prof
LLVM_PROFILE_FILE="$PWD/work/cov/prof/%c%p.profraw" \
python3 scripts/edenprobe.py run --runtime build/cov/scarletnx --nro work/cov/edenprobe.nro --out work/cov/ep \
    --mode "quick notime cpu fs video display kernel audio hooks" \
    --baseline tests/edenprobe/linux-x86_64.txt --timeout 1200
```

Use the release CI pins (`EDENPROBE_VERSION` and `EDENPROBE_SHA256` in `.github/workflows/build.yml`), so the
baseline holds. A build older than 0.13.0 opens `time:u` itself and stops in `sm` at its first launch. With CI's
words, v0.16.1 passed its 826 cases (2 `skip (banned)`) on the instrumented runtime, in a 12-second launch after a
28-second recompile. It left two `.profraw` files, one from the recompile and one from the launch.

What the words reach, by folder (lines run, of the lines in that folder):

| Folder | Lines | Run |
|---|---|---|
| `nx/hle/kernel` | 2689 | 52.5% |
| `nx/hle/svc` | 1597 | 45.0% |
| `nx/memory` | 2618 | 41.4% |
| `common/cpu` (mostly the recompile) | 22446 | 36.8% |
| `nx/loader` | 4178 | 33.2% |
| `nx/nn` | 3793 | 26.9% |
| `nx/hle/services` | 14096 | 17.7% |
| `nx/gpu/vk` | 42940 | 11.3% |
| `nx/hle/hook` | 3497 | 9.2% |
| `nx/gpu/nvn` | 9617 | 0.3% |
| `nx/gpu/shader` | 20440 | 0.3% |

The last two rows follow from the words. CI's words leave out `graphics`, edenprobe's drawing suites
([The mode words](edenprobe.md#the-mode-words)), so the shader translator and NVN are barely touched. Each
word is a lever on the surface. A run per word set, merged, shows what each adds; `--setting KEY=VALUE` runs the
same cases through another path of the runtime (`vk_memory=2`, `auto_stub_ipc=0`, `nce=0`, ...).

The Android build of edenprobe (`--apk`) runs the same way on `scarletnx-android` and covers `src/android`.

### From the tests

ctest runs each test as its own program. Build the test targets in `build/cov` too, and run them with the same
variable:

```sh
cmake --build build/cov                                   # every target, the tests among them
LLVM_PROFILE_FILE="$PWD/work/cov/prof-tests/%c%p.profraw" ctest --test-dir build/cov -j6
```

Each test binary has its own coverage mapping, so `llvm-cov` needs every binary that wrote a profile, the first
as the object and the rest as `-object` (some tests are scripts that run `scarletnx`, `snx-shaderc` or another
driver, hence the wide pattern):

```sh
objs=$(find build/cov -type f -perm -u+x \( -name 'snx-*' -o -name 'scarletnx*' \) | sed '2,$s/^/-object=/')
llvm-cov report $objs -instr-profile=work/cov/tests.profdata
```

The tests reach what edenprobe cannot (the shader translator through its corpora, the Vulkan renderer through
`vulkan_headless`, the 3DS, Vita and NDS drivers, the GUI's logic), and edenprobe reaches what the tests do not
(whole SVC and service sequences as a program makes them). Their union is the project's checked surface.

### From a game or a homebrew program

Any run through `build/cov/scarletnx` collects the same way:

```sh
LLVM_PROFILE_FILE="$PWD/work/cov/prof-game/%c%p.profraw" \
    build/cov/scarletnx --game="$games/<id>" --seconds=120 --input_replay=1
```

A game's profile says which runtime paths real software reaches. Diffed against the checks' profile, it gives the
paths that games depend on and no check holds (Code one run reaches and another does not).
The profile names only the runtime's functions, not the game's, but which services and NVN paths a game uses is
still the game's behaviour: keep game profiles local.

