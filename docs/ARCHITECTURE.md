# Architecture

## Index

* [Runtime boundaries](#runtime-boundaries)
* [Recompiler + module ABI](#recompiler--module-abi)
* [Runtime ownership](#runtime-ownership)
  * [Guest exec](#guest-exec)
  * [GPU submit/present](#gpu-submitpresent)
  * [Renderer coherence + lifetime](#renderer-coherence--lifetime)
  * [Videodec/VIC channels](#videodecvic-channels)

ScarletNX compiles AArch64 code into native modules through LLVM IR (or libgccjit).
The CPU implementation is shared by Horizon HLE/Maxwell and CPU-only Linux AArch64 ABI runtimes.
Source map: [CLAUDE.md](../CLAUDE.md).

## Runtime boundaries

| Source / CMake target | Ownership |
| --- | --- |
| `src/common/` / `scarletnx-common` | AArch64 CPU state, instruction semantics, code discovery/emission, native module execution, runtime callback interface, host utilities |
| `src/nx/` / `scarletnx-nx` | Horizon SVC/kernel/services/filesystems, NSO/NRO loading, Switch memory, Maxwell and graphics/media backends, Switch frontend/tooling |
| `src/gnuabi/` / `scarletnx-gnuabi` | Linux syscall/errno/structure translation, ELF loading, isolated VFS, Linux process/memory/signal/futex compatibility |

The experimental `src/3ds/` / `scarletnx-3ds` target supplies board profiles,
decrypted CIA/NCCH/ExeFS/RomFS extraction and an independent checked page-memory
controller. `SNX_3DS_AOT=ON` adds a native Linux static-AOT pipeline:
extraction -> the driver's offline LLVM compiler/host linker (only when the module is missing or stale) ->
native ELF -> checked process/ARM/Thumb dispatch. `snx_ctr_process` maps raw
ExHeader segments/BSS/stack/TLS; `snx_ctr_native` validates/loads module ABI 1.
The driver links LLVM for its compiler, the modules do not. The runtime uses neither MCJIT nor NX/GNU
ABI code. `common/filesystem/` / `scarletnx-filesystem` owns NX and CTR's shared
indexed host tree, immutable file mappings and on-demand opens. CTR uses its own
level-3 RomFS serializer/file objects and wire protocol, not Switch FS IPC.
Host staging/process utilities are also reused.

The shared A32 backend's A57/flat defaults and CPU ABI 2 are unchanged. ARM11 ISA
and callback memory are explicit codegen options, never global 3DS defines. The
shared backend CMake target is initialized once when several runtimes request it.
The compiler exhaustively emits one-instruction entries for images up to 4 KiB.
Larger images use bounded offline CFG discovery, code-pointer candidates and a
bounded PC-relative ADD jump window, with blocks of at most 32 instructions.
It emits 128 entries per LLVM chunk and links all PIC objects with a separate
ABI-1 full-geometry index object; unproven targets remain NULL. There is no
runtime discovery or compilation. SuperTux reaches its first SVC through this
path, not gameplay. See [3DS port](3DS.md) for coverage limits, absent HLE/devices
and remaining cache/ISA work.

The common target has no runtime-specific imports. NX and GNU ABI each link
common and have no dependency on each other. The GNU executable target is
`scarletnx-gnuabi-run`, whose output is `scarletnx-gnuabi`; its library output is
`scarletnx-gnuabi-runtime`. `SNX_BUILD_NX=OFF` builds GNU ABI without fetching or
linking NX graphics/media packages. CTest checks include ownership and the actual
transitive CMake dependency graph and target source ownership.

`src/common/runtime/interface.h` defines `RtAPI`; the active runtime supplies
the callbacks on `CPU.rt`. NX routes SVC to Horizon. GNU ABI accepts `SVC #0`,
reads Linux syscall number `x8` and arguments `x0` through `x5`, and writes result
`x0`. CPU helpers and instruction emission are shared; Horizon handlers are never
used as Linux syscalls.

`scarletnx-erista`, `scarletnx-mariko`, and `scarletnx-aula` share the existing
`scarletnx` NX implementation through Unix symlinks or Windows executable copies.
The original game-name entry points and CLI remain available. GNU ABI has an
independent CPU-only entry point; see [GNUABI.md](GNUABI.md) for implemented
syscalls, ELF execution, filesystem scope, and differential testing.

## Recompiler + module ABI

NX tooling under `src/nx/runtime/driver/` and `src/nx/loader/`:
`work/<Game>/{exefs,romfs}` -> `games/<id>/`: `<id>[.exe]`, `<id>_code.{so,dll}`, `<id>_data/`:

- `driver.c`: CLI dispatch only; `extract.c` and `recompile.c` own their workflows.
- `src/nx/loader/loader.c`: NSO/NRO, LZ4, SHA-256, NPDM, MOD0, symbols/relocs/FDEs.
- `src/common/cpu/aarch64/a64.c` + `a64_templates.inc`: shared instruction semantics for the backends, gnuabi's C path and the tests' reference interpreter.
- `src/common/cpu/recompiler/codegen.c`: function/CFG discovery over platform-neutral image views.
  `src/nx/loader/codegen_view.c` adapts NX images; `module.c` holds what the backends share (the dispatch entry
  table, chunk labels, progress lines); chunks batch whole functions up to `SNX_CHUNK_INSTRUCTIONS`, 90000 (20000 on Android).
- `toolchain.c`: the runtime build for deployment.
- Code generators (`--backend`, `src/common/cpu/recompiler/backend.h`): `llvm` (the default and the recommended
  one; `src/common/cpu/recompiler/llvm/`, LLVM a required dependency) builds LLVM IR with the LLVM-C API: `ir_a64.c`, `ir_simd.c` and `ir_fp.c` emit each
  `a64_templates.inc` case as the IR of the C it holds, `ir_codegen.c` the functions of `codegen.c`, and
  `src/nx/loader/module_llvm.c` the chunks and `SnxModule` of `module.c`. It links the objects itself (`link_elf.c`,
  `link_coff.c`) with `rtlib.c`, the helpers compiled when the recompiler is built, into a module
  with no imports, for the host it runs on: x86-64 (ELF or COFF) or AArch64 (ELF, with a veneer for a branch past
  128 MiB). `tests/llvm_differential.c` runs both backends' step functions on random
  states (every pattern, failing if any instruction has no emitter: a backend refuses such a module), `tests/llvm_module.c`
  a whole program built by both, `tests/llvm_bench.c` times both on assembly kernels. `gcc` (libgccjit,
  optional, built when libgccjit is found: `cmake/gccjit.cmake`, `SNX_GCCJIT_ROOT`) is for DEC Alpha EV67 hosts,
  which LLVM doesn't generate code for; everywhere else `llvm` is the backend to use. Ported on x86-64 for now:
  `src/common/cpu/recompiler/gccjit/jit.c` emits `ir_codegen.c`'s control flow, and `jit_a64.c`, `jit_fp.c` and
  `jit_simd.c` each instruction as the LLVM emitters do (cpu.h's helpers written out for the word's constant
  fields, SIMD a lane at a time, no host intrinsics: GCC picks the instructions with `-march=native`). Guest
  accesses are relaxed atomics (libgccjit 14 drops volatile on dereferences); `src/nx/loader/module_gccjit.c`
  compiles each chunk to an object and lets GCC's driver link the module with the chunks and the runtime library.
  Nothing else (`-nostdlib`): the driver runs only `as` and `ld` and links none of GCC's start files, libgcc or the
  C library, so a packaged libgccjit works next to another GCC or none.
  libgccjit compiles one context at a time per process (GCC's state is global, behind one lock), so
  `gccjit/jit_units.c` compiles the chunks `--jobs` at a time in processes of their own (one after another in the
  recompiler's process with one job, and on Windows, which has no `fork`).
  `llvm_differential --gccjit` (also failing on any instruction without an emitter) and `llvm_module` check it against the
  other backends. On `llvm_bench` it matches LLVM on integer, FP and calls; SIMD is several times slower (scalar
  lanes, GCC's SLP vectorizer off: it packs scalar code's CPU fields into vectors).
- Both backends poll `CPU.poll` on backward branches inside a function and at the `D_` dispatch, as `codegen.c`'s C
  does, so a loop without SVCs gives its core to a waiting thread of higher priority.
- The LLVM backend's speed: guest memory and the CPU structure are separate alias scopes (registers stay in host
  registers) and guest accesses are volatile (each happens, as on the hardware); logical immediates and SIMD
  immediates are folded; FP arithmetic, compares and conversions run inline on the host's IEEE unit when the result
  is provably the soft-float helper's, flags included (`ir_fp_fast`); the AdvSIMD and crypto encodings are inline
  instead of `snx_simd`, all but AES on a host without AES-NI (and on libgccjit). Build == host == target: the
  generated code uses the host's extensions (`snx_ir_host`, from LLVM's host detection) and the runtime library is
  built with `-march=native`, its helpers choosing paths from the compiler's feature macros; nothing is detected at
  run time. x86 paths sit under `__x86_64__` or the target triple: AES-NI, PCLMULQDQ (PMULL, CRC32 by Barrett
  reduction), SSE4.2 CRC32C, VPTERNLOG (BSL/BIT/BIF, signed saturation), PAVGB/PAVGW (rounding halving adds, signed
  ones biased by 0x80), GF2P8AFFINEQB (8-bit shifts, bit reversal); the tricks are wunkolo's (wunkolo.github.io).
- `src/common/utilities/common.c` and `process.c`: host files/processes; argv-based subprocesses.
  NX extraction retains its isolated key environment.

Host tooling uses C90 syntax with SDK fixed-width types, formatting and OS APIs.
The runtime and generated guest code retain their existing Clang/ABI requirements.
Normal builds link the driver into the runtime; `SNX_TOOLS_ONLY` builds the same CLI independently.

Shared CPU ABI (`src/common/cpu/aarch64/`): guest functions take `CPU*`; `snx_lookup` dispatches indirect branches
through `CPU.disp` (relaxed loads: slots go from NULL to a function while threads run), and a pc without a function
goes to `RtAPI.miss`. `CPU.rt` supplies `RtAPI` callbacks: SVC, UNDEF, BRK, hints, CNTVCT, FPCR, SEV/WFE, poll,
miss; no named runtime imports. NX's miss (`src/nx/runtime/rtapi.c`) runs the runtime JIT's code
(`src/nx/runtime/guest_jit.c` over `src/common/cpu/recompiler/llvm/jit_service.c`). There is no interpreter: a pc
without code yet is queued at the front and its thread waits for the compile off its guest core, and a pc the JIT
can't compile (or a host where it can't start: Windows before 10 1803) ends the game with the reason. Compile
threads (host threads, not guest threads) take the queued pcs in batches: the functions at them and their callees
found by `snx_codegen_discover`, emitted by the LLVM backend into one module, loaded into a code arena with a copy of
`rtlib.c` (`code_loader.c`, packed into shared memory mapped once executable and once writable) and published into
`CPU.disp`. At start the hints of earlier runs are queued behind every miss. The pc is recorded as a hint either way,
so the next recompile has it. Each unit compiled is appended to `<game>.jitcache` in the cache folder, and a later run
loads it instead of compiling when the guest code it came from is the same byte for byte and the file was written with
the same ABI, LLVM, runtime library, host CPU and settings. Its calls into the AOT module go through `snx_aot_<pc>`
symbols that the loader resolves at each load, so the object holds no host address.
`cpu_helpers.c` + `helpers/`: SIMD, crypto, FP.

GNU supplies an optional fetch callback that checks execute permissions and
compares the live instruction against the compiled word. Modified code falls
back to shared interpreter semantics. NX leaves this callback unset and keeps
its existing execution path.

NX module ABI is in `src/nx/loader/module_abi.h`; the shared CPU layout is in
`src/common/cpu/aarch64/cpu.h`. Rebuild game modules after shared ABI changes;
bump `SNX_ABI` for ABI changes. Version mismatches reject loading.

## Runtime ownership

| `src/nx/` directory | Responsibility |
| --- | --- |
| `runtime/` | Module loading, INI, clock, `RtAPI`, crashes |
| `hle/kernel/`, `hle/svc/`, `memory/` | Objects, threads, sync, SVC, Switch memory, IPC |
| `hle/services/`, `hle/filesystem/` | Services, nvdrv, guest filesystem services |
| `gpu/` and `gpu/maxwell/` | GPFIFO, Maxwell engine implementation, shaders, Vulkan renderer |
| `gpu/video/` | host1x, NVDEC H.264 and VP9 decoding through FFmpeg, VIC |
| `runtime/platform/` | Host APIs, SDL3/headless, input, screenshots, timed exit |

### Guest exec

Guest thread = host thread + `CPU` + TLS. `hle/kernel/thread.c`: one owner/core, four cores; release on kernel block,
sleep/yield, IPC, WFE. Waiters: priority then FIFO; a waiter of higher priority takes the core at the holder's next SVC
or backward branch, any taken branch in the interpreter (`CPU.poll`, `RtAPI.poll`). A ready thread that would wait
behind its core's holder runs on an idle core of its mask, and an idle core takes such a waiter (only
`svcSleepThread(0)` keeps a yielding thread on its core). A yield gives the core only to a waiter of the same or better
priority, except `svcSleepThread(-2)`, which gives it to any waiter. A mutex owner runs at its best waiter's priority
(`hle/kernel/sync.c`).

`memory/mem.c`: one guest VA reservation; `GP(addr) = g_mem + addr`, accessed directly by generated code.

SVC -> `hle/svc/svc.c`; `svcSendSyncRequest` -> `hle/kernel/ipc.c` -> session's HLE service object, whose dispatcher
is generated from the interface's `.swipc` file and calls the service's typed handler ([IPC interfaces](IPC.md)). A
service the runtime doesn't have gets sm's stand-in object, which answers every command with success.

[Filesystem](FILESYSTEM.md): startup scan; indexed lookup, metadata, enumeration, mutations.

### GPU submit/present

1. NVN command buffers -> `/dev/nvhost-gpu` GPFIFO ioctl -> `hle/services/nvdrv.c` -> `gpu_submit` (`gpu/gpu.c`).
2. Copy entries to GPU queue. `nv.submit_lk` orders fence promises and cross-channel submits. A submission's fence
   is the stream's first wait on the GPU thread, whoever increments it, and the ioctl returns at once, as nvgpu's does.
3. GPU-thread dispatch: 3D, MME, compute, 2D, DMA, inline-to-memory. Draws -> `g_renderer` (`gpu/renderer.h`).
   Shaders: Maxwell SASS -> SPIR-V (`gpu/shader/`). Each channel's jobs run in order. A stream that waits (a semaphore
   acquire, a syncpoint wait, its fence) parks its channel (`gpu_park`), even inside a macro, and the thread runs the
   other channels' jobs until the wait is met or times out. A release from another stream or a syncpoint increment
   wakes it, and a release the CPU writes is polled every 0.5 ms while nothing else can run.
4. Vk `stream_incr` lands compute writes in guest memory before incrementing the syncpoint.
5. vi waits on the fences no submission promised (a host1x engine's, the CPU's), then queues present (`gpu_call`)
   after the jobs of the channels that promised the others. Other channels' jobs don't hold it. The GPU thread checks
   the remaining fences. `present_buffer` uses the matching RT and vi's crop, or converts guest memory.

### Renderer coherence + lifetime

- **Vk**: Vulkan 1.3 in windowed or headless mode, with guest buffer mirrors; separate compute queue when available,
  graphics queue otherwise.

Invalidation: host write tracking + `mem_unmap` -> `gpu_mem_changed`. Vk treats zeroed unmap/heap-return ranges
as written at the next cache check.

`Renderer.save`: optional pipeline-cache persistence every 300 frontend ticks (~5s) + exit, concurrent with execution.
Exit skips renderer teardown and guest-thread joins.

### Videodec/VIC channels

Each NVDEC/VIC fd owns a `host1x_open` channel, registers and syncpoints; NVDEC also owns a decoder.
NVDEC writes guest surfaces; VIC composites output.
