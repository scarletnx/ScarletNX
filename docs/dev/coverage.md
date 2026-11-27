<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# Coverage

## Index

* [What coverage measures here](#what-coverage-measures-here)
* [A coverage build](#a-coverage-build)
  * [Why continuous mode](#why-continuous-mode)
  * [Configure and build](#configure-and-build)
* [Collecting a profile](#collecting-a-profile)
  * [From Amberlith](#from-amberlith)
  * [From the tests](#from-the-tests)
  * [From a game or a homebrew program](#from-a-game-or-a-homebrew-program)
* [Reading the profile](#reading-the-profile)
  * [Merge](#merge)
  * [Reports by file and by folder](#reports-by-file-and-by-folder)
  * [Line by line](#line-by-line)
* [Finding the unaccounted-for paths](#finding-the-unaccounted-for-paths)
  * [Code nothing runs](#code-nothing-runs)
  * [Code one run reaches and another does not](#code-one-run-reaches-and-another-does-not)
  * [What the log says is missing](#what-the-log-says-is-missing)
  * [The static censuses](#the-static-censuses)
  * [Closing a gap](#closing-a-gap)
* [gcov and GCC](#gcov-and-gcc)
* [Other hosts](#other-hosts)
* [Pitfalls](#pitfalls)

[Development](development.md) · [Amberlith](amberlith.md) · [build](build.md) · [profiling](profiling.md).

This page is a guide to measuring which of ScarletNX's own code a workload runs: Amberlith's suites, the tests, a
game. The goal is the project's surface area: which services, SVCs, renderer paths and loader branches anything
exercises, and which nothing does. Those last are the *unaccounted-for* paths: code that has never run under any
check, so a bug in it shows up first in a player's game.

The examples use LLVM's source-based coverage (`clang -fprofile-instr-generate -fcoverage-mapping`, `llvm-profdata`,
`llvm-cov`). [gcov and GCC](#gcov-and-gcc) gives the GCC route. The numbers quoted are from a FreeBSD 15.1 machine
with Clang 23, at `origin/main` of 2026-11-15; yours will differ by commit and host.

Profiles, reports and the recompiled programs they came from stay local, like the games: keep them in the
gitignored `work/`.

## What coverage measures here

Coverage counts the runtime's own C and C++ code: the HLE kernel and services, the SVCs, the NVN and Maxwell front
ends, the Vulkan renderer, the shader translators, the loaders, the recompiler's driver and native back ends. It does not
count a game's code. The recompiled module (`<id>_code.so`) and the code the JIT compiles are made by the
recompiler, not by the compiler that instruments the build. What a game's code reaches is a different question,
answered by [the static censuses](#the-static-censuses) and the log.

Two processes contribute to a game's run:

| Process | What it covers |
|---|---|
| The driver (`scarletnx -r`, or the first step of `scripts/amberlith.py`) | `nx/loader`, `common/cpu` (decoder, function discovery, the native back end, the linker), `nx/runtime/driver` |
| The game's runtime (`recompiled/<id>/<id>`, a copy of the same `scarletnx`) | everything the game asks for at run time |

Both are the same binary, so one `llvm-cov` call reads both profiles against `build/cov/scarletnx`.

## A coverage build

### Why continuous mode

The runtime leaves through `plat_exit_now()` (`src/nx/runtime/platform/plat_posix.c`), which calls `_exit` on
purpose, so that no atexit handler runs against threads that are still going. Both LLVM's profile runtime and gcov
write their counters from an atexit handler, so an instrumented runtime that ends normally writes an **empty**
profile. Amberlith's launcher also kills a launch that hangs, which writes nothing either.

LLVM's *continuous mode* solves both. The counters live in a memory-mapped file from the program's start, so what
ran is on disk however the process ends: `_exit`, a crash, `SIGKILL`. It takes `%c` in `LLVM_PROFILE_FILE`, and on
ELF hosts other than Linux and Fuchsia the counters must be relocatable at run time, which
`-mllvm -runtime-counter-relocation` asks for. A plain instrumented build run through `_exit` left a 0-byte
`.profraw`; the same with continuous mode left the counts.

### Configure and build

A build folder of its own, with the instrumentation in `CMAKE_C_FLAGS`/`CMAKE_CXX_FLAGS`. The game modules'
runtime library (`rtlib.c`) is compiled with its own flag list (`src/common/cpu/recompiler/CMakeLists.txt`),
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
  On FreeBSD, take the two tools from the folder of the Clang that built it (`/usr/local/llvm23/bin` for `clang23`).
- `SNX_DEPLOY=OFF` keeps the instrumented runtime from being copied over every game in your data folder, as the
  Tracy preset does. A run through `build/cov/scarletnx` still puts it beside the game it runs.
- Clang warns that `-runtime-counter-relocation` is unused at link time. The warning is harmless.
- The build instruments the dependencies it compiles from CPMUtil's packages too (zstd, FSR2, ...). Reports leave them out by path
  ([Reports](#reports-by-file-and-by-folder)).
- Keep `-runtime-counter-relocation` on every ELF host. Without it, continuous mode is refused where the profile
  runtime cannot map the counters in place.

## Collecting a profile

`LLVM_PROFILE_FILE` names where each process writes. Use `%p` (the process ID) so the driver and every launch get
files of their own, and `%c` for continuous mode. Use an absolute path, since the runtime changes folder.

### From Amberlith

[Amberlith](amberlith.md) is the broadest single workload: hundreds of cases over the CPU, the kernel's scheduler,
the filesystem, video, display, audio and the SDK hooks, each judged against the console. `scripts/amberlith.py`
passes its environment to the recompile and to each launch, so the variable is all it takes (`REFS`, the baselines'
package, as [amberlith.md](amberlith.md) finds it):

```sh
mkdir -p work/cov/prof
LLVM_PROFILE_FILE="$PWD/work/cov/prof/%c%p.profraw" \
python3 scripts/amberlith.py run --runtime build/cov/scarletnx --nro work/cov/amberlith.nro --out work/cov/ep \
    --mode "quick notime cpu fs video display kernel audio hooks" \
    --baseline "$REFS/amberlith/linux-x86_64.txt" --timeout 1200
```

Use the release CI pins (`AMBERLITH_VERSION` and `AMBERLITH_SHA256` in `.github/workflows/build.yml`), so the
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
| `nx/hle/hook` (now in `nx/nn`) | 3497 | 9.2% |
| `nx/gpu/nvn` | 9617 | 0.3% |
| `nx/gpu/shader` | 20440 | 0.3% |

The last two rows follow from the words. CI's words leave out `graphics`, Amberlith's drawing suites
([The mode words](amberlith.md#the-mode-words)), so the shader translator and NVN are barely touched. Each
word is a lever on the surface. A run per word set, merged, shows what each adds; `--setting KEY=VALUE` runs the
same cases through another path of the runtime (`vk_memory=2`, `auto_stub_ipc=0`, `nce=0`, ...).

The Android build of Amberlith (`--apk`) runs the same way on `scarletnx-android` and covers `src/android`.

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

The tests reach what Amberlith cannot (the shader translator through its corpora, the Vulkan renderer through
`vulkan_headless`, the 3DS, Vita and NDS drivers, the GUI's logic), and Amberlith reaches what the tests do not
(whole SVC and service sequences as a program makes them). Their union is the project's checked surface.

### From a game or a homebrew program

Any run through `build/cov/scarletnx` collects the same way:

```sh
LLVM_PROFILE_FILE="$PWD/work/cov/prof-game/%c%p.profraw" \
    build/cov/scarletnx --game="$games/<id>" --seconds=120 --input-replay=1
```

A game's profile says which runtime paths real software reaches. Diffed against the checks' profile, it gives the
paths that games depend on and no check holds ([Code one run reaches and another does not](#code-one-run-reaches-and-another-does-not)).
The profile names only the runtime's functions, not the game's, but which services and NVN paths a game uses is
still the game's behaviour: keep game profiles local.

## Reading the profile

### Merge

```sh
llvm-profdata merge -sparse work/cov/prof/*.profraw -o work/cov/ep.profdata
llvm-profdata merge -sparse work/cov/ep.profdata work/cov/tests.profdata -o work/cov/all.profdata   # a union
```

A `.profraw` with continuous mode is the size of the counters (about 8 MB for `scarletnx` here), whatever ran.
`-sparse` drops the functions that never ran from the merged file.

### Reports by file and by folder

```sh
X='(\.cache/|/build/|/usr/)'      # CPMUtil's sources, generated files, the system's headers
llvm-cov report build/cov/scarletnx -instr-profile=work/cov/ep.profdata -ignore-filename-regex="$X" \
    > work/cov/ep-report.txt
llvm-cov report build/cov/scarletnx -instr-profile=work/cov/ep.profdata -show-functions src/nx/hle/svc/svc.c
```

The report has a row per file with regions, functions, lines and branches, each run and missed. Its paths drop the
common prefix (`nx/hle/svc/svc.c`). The per-folder table above is that report summed:

```sh
awk 'NR>2 && NF>=10 && $1 ~ /\// { n = split($1, p, "/"); d = p[1]; if (n > 2) d = d "/" p[2];
     if (n > 3 && p[1] == "nx") d = d "/" p[3]; L[d] += $8; M[d] += $9 }
     END { for (d in L) printf "%6.1f%% %7d %s\n", L[d] ? 100 * (L[d] - M[d]) / L[d] : 0, L[d], d }' \
    work/cov/ep-report.txt | sort -k3
```

A folder at 0% with many lines is a part the workload never enters: the GUI, the other drivers (`3ds`, `vita`,
`nds`, `android`), `glslc`. That is expected for an Amberlith run; it is the tests' job.

### Line by line

```sh
llvm-cov show build/cov/scarletnx -instr-profile=work/cov/ep.profdata -format=html -output-dir=work/cov/html \
    -ignore-filename-regex="$X" -show-branches=count -show-line-counts-or-regions
llvm-cov export build/cov/scarletnx -instr-profile=work/cov/ep.profdata -format=lcov > work/cov/ep.lcov
```

The HTML has an index by file, and each file with its counts in the margin: a red line never ran, and a branch
shows how many times each side was taken. `-format=lcov` feeds `genhtml` or an editor's coverage gutter
(VS Code's Coverage Gutters, for example). `llvm-cov export` without `-format` writes JSON with every function and
its count, which the scripts below read: about 100 MB for this run, even with `-skip-expansions`, which leaves out
the macro expansions' regions.

## Finding the unaccounted-for paths

### Code nothing runs

The functions with a zero count, by file, are the list to start from:

```sh
llvm-cov export build/cov/scarletnx -instr-profile=work/cov/ep.profdata -skip-expansions > work/cov/ep.json
python3 - work/cov/ep.json src/nx/hle/services/ <<'EOF'
import collections, json, os, sys
d = json.load(open(sys.argv[1]))['data'][0]
cold, total, names = collections.Counter(), collections.Counter(), collections.defaultdict(list)
for f in d['functions']:
    path = f['filenames'][0]
    if sys.argv[2] not in path: continue
    name = os.path.basename(path); total[name] += 1
    if not f['count']:
        cold[name] += 1; names[name].append(f['name'].split(':')[-1])
for name, n in cold.most_common(): print(f'{n:4d}/{total[name]:4d} {name}  {", ".join(names[name][:4])}')
EOF
```

From the Amberlith run above, part of what it printed:

```
 219/ 231 misc.c  apm_configuration, apm_set_configuration, apm_cpu_boost, ...
 153/ 179 am.c  am_display_mode_changed, new_appletae, am_user_account_switch_lock, ...
 129/ 133 gsv.c  new_gsv_session, gsv_native_memory_cpu, gsv_native_image_reserve, ...
 105/ 105 acc.c  acc_playing, new_acc, new_acc_system, ...
  70/ 111 nvdrv.c  nvmap_addr_by_id, gmmu_translate, gmmu_run, ...
  68/  70 bsd.c  network_set_room, network_interface, network_online, ...
  65/  65 ssl.c  new_ssl, new_ssl_system, nn_ssl_sf_ISslService_CreateContext, ...
```

Read each in three ways:

- **Whole services never opened** (`acc.c`, `ssl.c`): Amberlith never asks for them. A game does (`acc` is the user
  account), so a game's profile covers them, but nothing *checks* them.
- **Services opened, most commands not** (`am.c`, `misc.c`): the handlers the generated dispatcher calls
  (`nn_<service>_sf_<interface>_<command>`) for commands no case sends. Each is a command whose answer nobody holds
  against the console's.
- **Branches inside covered functions**: functions at 100% can still have error paths that never ran
  (`-show-branches=count` in `llvm-cov show`). The kernel's and the SVCs' are the important ones: what a wrong
  handle, an unaligned address or a full table answers.

Not every gap needs a case. Code for another host (`#ifdef _WIN32`), debug settings and the GUI are reached by
other means. The point is to know which gaps are deliberate.

### Code one run reaches and another does not

Two profiles, each exported to JSON, and the functions the second runs that the first never does:

```python
"""covdiff.py A.json B.json [PATH]: the functions B runs and A never does (llvm-cov export), by file"""
import collections, json, sys
def ran(path, under):
    d = json.load(open(path))['data'][0]
    return {(f['filenames'][0], f['name']) for f in d['functions'] if f['count'] and under in f['filenames'][0]}
under = sys.argv[3] if len(sys.argv) > 3 else '/src/'
only = ran(sys.argv[2], under) - ran(sys.argv[1], under)
by = collections.defaultdict(list)
for path, name in only: by[path.split('/src/', 1)[-1]].append(name.split(':')[-1])
for path in sorted(by, key=lambda p: -len(by[p])):
    print(f'{len(by[path]):4d} {path}: {", ".join(sorted(by[path])[:4])}')
```

```sh
python3 work/cov/covdiff.py work/cov/checks.json work/cov/game.json /src/nx/   # what a game needs that no check runs
python3 work/cov/covdiff.py work/cov/before.json work/cov/after.json            # what a new case or word added
```

For example, an Amberlith run with the words `quick notime kernel hooks` against the full CI words: the CI words
add the VP9 decoder (`nx/gpu/video/vp9_rebuild.cpp`, 55 functions), nvdrv's control device (`nvdrv.c`, 38), the
audio renderer's DSP (`audren_dsp.c`, 34), Vulkan command recording (`vk/cmd.c`, `vk/record.c`) and the GPU's hold on
guest memory (`vk/owned.c`, `memory/own.c`). That is how to tell which mode word or which case pays for which part
of the runtime.

The useful pairs:

| A | B | What B minus A is |
|---|---|---|
| Amberlith and the tests | a game, or several merged | runtime paths a game depends on that no check holds: candidates for new Amberlith cases or tests |
| a game, before a fix | the same game, after it | what the fix made the game reach (a service it now gets past, a renderer path it now takes) |
| one setting's value | another's (`--setting`) | the code that setting switches, which the baseline runs only one side of |
| `origin/main` | a branch | new code the branch's checks do not run |

### What the log says is missing

Coverage shows the code that ran. The runtime also logs the code that does not exist: what a program asked for and
found nothing behind. Read the run's logs beside the coverage report:

| Log line | What is missing | Where it comes from |
|---|---|---|
| `[undeclared] <service> cmd N -> ok` | a command the service's `.swipc` does not declare, answered with success and zeros | `stub_ok` in `hle/kernel/ipc.c`; `auto_stub_ipc=1`, the default |
| `[unknown] <service> cmd N -> UnknownCommandId` | the same, answered as the console answers an unknown command | `ipc_unknown`; `auto_stub_ipc=0`, or an interface's `@fallback(ipc_unknown)` |
| `sm: <name> is not implemented, its commands answer without doing anything` | a whole service | `hle/services/sm.c` |
| `svc NN (<Name>): not implemented ..., NotImplemented` | an SVC still marked `stub` in `hos.svc` | `hle/svc/svc.c` |
| `nvn: the game resolved NAME, which nvn doesn't implement` | an NVN function the game looked up; the game stops if it calls it | `gpu/nvn/nvn_loader.c` |

`@stub` commands in a `.swipc` file answer success with no handler and no log line
([IPC interfaces](../platforms/nx/ipc.md)): `grep -n '@stub' src/nx/hle/services/*.swipc` lists them.
`--auto-stub-ipc=0` turns every undeclared command into the console's error, which makes a game fail where it
makes the call instead of later. `--log=2` adds the `ipc:` lines, the service commands served and their times.

```sh
grep -hE '\[(undeclared|unknown)\]|not implemented|doesn.t implement' work/cov/ep/launch-*.log | sort | uniq -c
```

The CI Amberlith run logs none of these: its cases only ask for what the runtime has. A game's log is where they
appear.

### The static censuses

What a game *could* call, from its code and without running it, is in the census tools (pure Python, over the
modules `scarletnx --dump-modules` reads; their output names the game's functions, so it stays local):

```sh
python3 tools/nvn_missing.py work/Game            # the NVN procedures the game's driver has that nvn does not implement
python3 tools/export_census.py report work/exefs/<title>   # the module's SDK imports and exports no hook runs yet
python3 tools/hook_census.py work/Game            # how many of a game's calls a hook could reach
python3 tools/nvn_census.py work/Game             # the game's NVN driver: procedure table, command memory writers
```

The censuses say what exists to be called; coverage says what was called; the log says what was called and missing.
A function in all three lists (the game imports it, the runtime has it, no check runs it) is the strongest
candidate for a check.

### Closing a gap

- **An Amberlith case**, when the question has an answer the console gives: a service command, an SVC's error, a
  scheduler behaviour. [A case from a game report](amberlith.md#a-case-from-a-game-report) has the MCP tools
  (`amberlith_scaffold`, `amberlith_check`, `amberlith_run`, `amberlith_finding`). Rerun the coverage
  afterwards: the diff against the run before says the case reaches what it was meant to.
- **A ctest**, when the code is the runtime's own (the renderer, a cache, a decoder): `tests/CMakeLists.txt`.
- **A word or a setting** in an existing run, when the code is already checked by cases that are not in the run
  (`graphics`) or behind a setting the baseline leaves at one value.
- **Nothing**, when the path is deliberate: write down why, so the next survey does not ask again.

## gcov and GCC

GCC's coverage (`--coverage`, `.gcno` beside each object at build time, `.gcda` written at exit) works too, with two
differences:

1. **No continuous mode.** The counters are written by an atexit handler, which `_exit` skips. A preloaded shim that
   calls `__gcov_dump` before `_exit` writes them:

   ```c
   /* dump_on_exit.c: cc -shared -fPIC -O2 dump_on_exit.c -o dump_on_exit.so -ldl */
   #define _GNU_SOURCE
   #include <dlfcn.h>
   #include <unistd.h>
   void _exit(int code) {
       void (*dump)(void) = (void (*)(void))dlsym(RTLD_DEFAULT, "__gcov_dump");
       void (*real)(int) = (void (*)(int))dlsym(RTLD_NEXT, "_exit");
       if (dump) dump();
       real(code);
       for (;;) {}
   }
   ```

   `dlsym` finds `__gcov_dump` only in a program that exports its symbols: link with `-rdynamic`
   (`-DCMAKE_EXE_LINKER_FLAGS="--coverage -rdynamic"`). Run with `LD_PRELOAD=$PWD/dump_on_exit.so`. A process that
   is killed or crashes still writes nothing. The same shim does not work for LLVM's `__llvm_profile_write_file`,
   which is hidden; use continuous mode there.
2. **One set of files.** Every process writes into the same `.gcda` files in the build folder (their absolute
   paths are compiled in), merging under a lock, and a later run adds to an earlier one. Delete them before each
   campaign (`find build/gcov -name '*.gcda' -delete`), and use `GCOV_PREFIX`/`GCOV_PREFIX_STRIP` to send one run's
   files elsewhere.

```sh
cmake -S . -B build/gcov -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ \
    -DSNX_DEPLOY=OFF -DSNX_JEMALLOC=OFF -DCMAKE_C_FLAGS=--coverage -DCMAKE_CXX_FLAGS=--coverage \
    "-DCMAKE_EXE_LINKER_FLAGS=--coverage -rdynamic"
cmake --build build/gcov --target scarletnx
LD_PRELOAD=$PWD/dump_on_exit.so python3 scripts/amberlith.py run --runtime build/gcov/scarletnx ...
gcovr -r . build/gcov --exclude '.cache/' --html-details work/cov/gcov/index.html
lcov -c -d build/gcov -o work/cov/gcov.info && genhtml work/cov/gcov.info -o work/cov/gcov-html
```

`clang --coverage` writes the same gcov format, read with `llvm-cov gcov`. Prefer source-based coverage with
Clang: its regions are exact to the expression, it survives `_exit` and crashes, and each process gets its own
file.

## Other hosts

- **Linux.** As above.
- **macOS.** Continuous mode is native on Darwin, without `-runtime-counter-relocation`. Not tried here.
- **Windows.** `plat_win32.c` leaves through `_exit` too. Use clang-cl with `-fprofile-instr-generate
  -fcoverage-mapping` and `%c` in `LLVM_PROFILE_FILE`, if your LLVM's profile runtime supports continuous mode on
  Windows; otherwise only the processes that return from `main` write a profile. Not tried here.
- **Android.** The app's process writes only where it may, so `LLVM_PROFILE_FILE` must name a folder of the app's
  own, and the `.profraw` files come back with `adb`. Not tried here. `scarletnx-android` on a desktop host
  (Amberlith's `--apk`) collects as the desktop does.
- **The 3DS, Vita, NDS and GBA drivers** are binaries of their own (`scarletnx-3ds`, ...) built in the same folder
  and instrumented by the same flags: run them with the variable and pass each binary as an `-object`.

## Pitfalls

- **Counts are not times.** Coverage says which code ran and how often. It says nothing about speed: an instrumented
  build is slower, so never time it ([Profiling](profiling.md)).
- **Counts race.** The counters are not atomic by default, so concurrent threads lose increments. Whether a line
  ran is exact; how many times is approximate. `-fprofile-update=atomic` makes the counts exact, at a cost.
- **Timeouts.** The instrumented runtime is slower; raise `--timeout` in `scripts/amberlith.py` and
  `--seconds` budgets if launches start to be killed.
- **Mixed versions.** A `.profraw` from one LLVM version is unreadable by another's `llvm-profdata`.
- **A stale binary.** `llvm-cov` maps counts through the binary's coverage mapping. A profile from one build read
  against another build mixes up functions or refuses (`function hash mismatch`). Keep each profile with the build
  that made it.
- **Header and generated code.** Inline functions in headers (`services_int.h`) are counted once per file that
  includes them; generated dispatchers live under `build/` and are left out by the filter above. Leave the filter
  off to see them.
- **0% is not dead code.** A path for another host, a debug setting, or the other drivers is reached by other
  workloads. Coverage of one workload ranks gaps; the union of all of them finds dead code.
