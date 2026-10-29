# Architecture

## Index

* [Runtime boundaries](#runtime-boundaries)
* [Recompiler + module ABI](#recompiler--module-abi)
  * [Module ABI](#module-abi)
* [Runtime ownership](#runtime-ownership)
  * [Guest exec](#guest-exec)
  * [GPU submit/present](#gpu-submitpresent)
  * [Renderer coherence + lifetime](#renderer-coherence--lifetime)
  * [Videodec/VIC channels](#videodecvic-channels)

ScarletNX compiles AArch64 code into native modules through LLVM IR.
The CPU implementation is shared by Horizon HLE/Maxwell and CPU-only Linux AArch64 ABI runtimes.
Source map: [CLAUDE.md](../CLAUDE.md).

## Runtime boundaries

| Source / CMake target | Ownership |
| --- | --- |
| `src/common/` / `scarletnx-common` | AArch64 CPU state, instruction semantics, code discovery/emission, native module execution, runtime callback interface, host utilities |
| `src/nx/` / `scarletnx-nx` | Horizon SVC/kernel/services/filesystems, NSO/NRO loading, Switch memory, Maxwell and graphics/media backends, Switch frontend/tooling |
| `src/gnuabi/` / `scarletnx-gnuabi` | Linux syscall/errno/structure translation, ELF loading, isolated VFS, Linux process/memory/signal/futex compatibility |

The experimental `src/3ds/` / `scarletnx-3ds` target supplies board profiles,
decrypted CIA/NCCH/ExeFS/RomFS extraction, or the same read in place from the mapped CIA, an independent checked page-memory
controller and a static-AOT pipeline for Linux and Windows hosts:
extraction -> the driver's offline LLVM compiler/host linker (only when the module is missing or stale) ->
a native module (an ELF shared object, or a DLL on Windows) -> checked process/ARM/Thumb dispatch. `snx_ctr_process` maps raw
ExHeader segments/BSS/stack/TLS; `snx_ctr_native` validates/loads module ABI 1.
The driver links LLVM for its compiler, the modules do not. The runtime uses neither MCJIT nor NX/GNU
ABI code. `common/filesystem/` / `scarletnx-filesystem` owns NX and CTR's shared
indexed host tree, immutable file mappings and on-demand opens. CTR uses its own
level-3 RomFS serializer/file objects and wire protocol, not Switch FS IPC.
Host staging/process utilities are also reused.

The shared A32 backend's A57/flat defaults are unchanged, and its CPU ABI is 3. ARM11 ISA
and callback memory are explicit codegen options, never global 3DS defines. The
shared backend CMake target is initialized once when several runtimes request it.
The compiler exhaustively emits one-instruction entries for images up to 4 KiB.
Larger images use bounded offline CFG discovery, code-pointer candidates and a
bounded PC-relative ADD jump window, with blocks of at most 32 instructions.
It emits 128 entries per LLVM chunk and links all PIC objects with a separate
ABI-1 full-geometry index object; unproven targets remain NULL. There is no
runtime discovery or compilation. SuperTux draws its title screen and plays its
music through this path, without input yet. See [3DS port](3DS.md) for coverage limits, absent HLE/devices
and remaining cache/ISA work.

The common target has no runtime-specific imports. NX and GNU ABI each link
common and have no dependency on each other. The GNU ABI driver is the library
`scarletnx-gnuabi` (its file `scarletnx-gnuabi-runtime`), command line included, which
`scarletnx` runs as `scarletnx-gnuabi`. `SNX_BUILD_NX=OFF` builds GNU ABI without fetching or
linking NX graphics/media packages. CTest checks include ownership and the actual
transitive CMake dependency graph and target source ownership.

`src/common/runtime/interface.h` defines `RtAPI`; the active runtime supplies
the callbacks on `CPU.rt`. NX routes SVC to Horizon: its modules call each SVC's own entry through
`RtAPI.svcs[imm]`, the number being a constant in the code. GNU ABI accepts `SVC #0`,
reads Linux syscall number `x8` and arguments `x0` through `x5`, and writes result
`x0`. CPU helpers and instruction emission are shared; Horizon handlers are never
used as Linux syscalls.

`scarletnx` is every driver the build has: `src/multicall/main.c` is the only `main`, and runs
the driver its name says from the library of the driver's own folder (`snx_switch_program`,
`snx_ctr_program`, `snx_vita_program`, `scarletnx-gnuabi`, `snx_agb_program`), so LLVM and SDL
are in one program once. On Linux and macOS `scarletnx-switch`, `scarletnx-3ds`,
`scarletnx-vita`, `scarletnx-gnuabi` and `scarletnx-agb` are symlinks to it. Windows has no
links: the driver's name goes first (`scarletnx.exe scarletnx-3ds ...`), and the launcher runs
the drivers that way (`snx_menu_has_driver`). A game's copy runs its driver too: a GBA game's by
its `.ini` (`system=gba`), a 3DS or Vita game's by its title ID, any other the Switch's (or, in a
build without it, the one driver there is). The 3DS and Vita
drivers read what a module was compiled for from its file before loading it, and compile first
when it is stale, so a module is loaded once.
The original game-name entry points and CLI remain available. GNU ABI has an
independent CPU-only entry point; see [GNUABI.md](GNUABI.md) for implemented
syscalls, ELF execution, filesystem scope, and differential testing.

## Recompiler + module ABI

NX tooling under `src/nx/runtime/driver/` and `src/nx/loader/`:
a dump, read in place, or `work/<Game>/{exefs,romfs}` -> `recompiled/<id>/`: `<id>[.exe]`, `<id>_code.{so,dll}`,
`<id>_data/`, and the dump's path as `source=` in the game's `<id>.ini` (`<id>.source`, a link to it, from older
versions):

An update or a DLC is recorded beside its game (`snx_addon_name`), its own `.ini` naming only its dump, and checked
in the game's `addons=` (`common/utilities/game_ini.c`, which the driver and the launcher write). An update also gets a
program, `recompiled/<id>/<id>_<its title ID>`; a DLC runs its game's code. The runtime (`main.c`'s `addon_init`) runs an
update's program with the game's `.ini` and data folder, and `fs.c` lays the checked DLCs' data folders and then the
update's over that folder (`fs_tree_overlay`, which takes only their ExeFS, RomFS, control data and `system/`). It
reads an update's dump with the game's in one `SnxDump` (its Patch RomFS reads the game's NCA), and each checked DLC's
in one of its own (`HleFs.aocs`), whose image `OpenDataStorageByDataId` serves by its title ID.

A game's mods are the folders of `<id>_data/mods/` (`common/utilities/mods.c`), each played unless the game's
`mods_off=` names it. The data folder's index leaves `mods/` out (`fs_tree_init_skipping`). The recompile loads the
program with the played mods' ExeFS files over its own (`snx_exefs_mods`) and records their stamp in `<id>.abi`, which
the driver compares (`stamped`) to recompile a game whose mods changed. The runtime's `fs.c` serves those files in
place of the game's when the stamp matches, grafts each played mod's `romfs/` as `mods.<i>`, and `romfs_init` makes
the RomFS image up from the game's (its `romfs/` folder, or its image's tables) with the mods' laid over it, a file of
the game's image read from that image.

- `driver.c`: CLI dispatch only; `extract.c` and `recompile.c` own their workflows.
- `container.c`: a dump opened with hactool, its keys loaded and its NCAs picked, shared by the extraction, the
  recompiler (a dump given to `-r`, its ExeFS read through `SnxExefsReader`) and the runtime's `fs.c`. Without
  Extract dump, `fs.c` maps the dump `source=` names (else the one `<id>.source` links to) and serves `exefs/*` and the RomFS image from
  it through hactool's storage chains (`snx_hac.h`'s `SnxHacFs`), the sections decrypted, an update's patch applied
  and compressed blocks expanded as they are read. Nothing is cached.
- `src/nx/loader/loader.c`: NSO/NRO, LZ4, SHA-256, NPDM, MOD0, symbols/relocs/FDEs.
- `src/common/cpu/aarch64/a64_decoder.c`: the A64 decoder `tools/a64dec` generates from QEMU's `a64.decode` and
  ScarletNX's `a64.profile` (`src/common/cpu/aarch64/decode/`): each op's flow and the feature it needs, so a target's
  feature set (the Switch's Cortex-A57 by default) decides what is UNDEF.
- `src/common/cpu/recompiler/codegen.c`: function/CFG discovery over platform-neutral image views.
  `src/nx/loader/codegen_view.c` adapts NX images; `module.c` holds what the backends share (the dispatch entry
  table, chunk labels, progress lines); chunks batch whole functions up to `SNX_CHUNK_INSTRUCTIONS`, 90000 (20000 on Android).
- `toolchain.c`: the runtime build for deployment.
- The code generator, LLVM (`src/common/cpu/recompiler/llvm/`, a required dependency), builds LLVM IR with the
  LLVM-C API: `ir_int.c`, `ir_ldst.c`, `ir_fpu.c` and `ir_vec*.c`, ported from QEMU's `translate-a64.c`, emit each
  op of the generated decoder, `ir_codegen.c` the functions of `codegen.c`, and
  `src/nx/loader/module_llvm.c` the chunks and `SnxModule` of `module.c`. It links the objects itself (`link_elf.c`,
  `link_coff.c`, `link_macho.c`) with `rtlib.c`, the helpers compiled when the recompiler is built, into a module
  with no imports, for the host it runs on: x86-64 (ELF or COFF), AArch64 (ELF, or on macOS and iOS a Mach-O dylib
  with an ad hoc signature, either with a veneer for a branch past 128 MiB) or PowerPC64 (ELF, ELFv1 or ELFv2, with a stub for a branch past 32 MiB: [PowerPC64](./PPC64.md)). `tests/llvm_qemu_differential.c` runs its step functions against
  `qemu-aarch64 -cpu max` (`tests/qemu_ref/`) on random states, for words of every row of the generated decoder, with
  the groups known to differ in `tests/llvm_qemu_expected.txt` (a new group, or a listed one that no longer differs,
  fails it; `--sweep` runs more of each), loaded as a module, with the JIT's loader and in the emitters' portable
  forms. `tests/a64_ported.c` runs rEden's cases through the steps against their expected values and QEMU,
  `tests/llvm_module.c` a whole program against the end state QEMU gives it, `tests/llvm_bench.c` times it on
  assembly kernels.
- `-r --layout FILE` orders the module's code by a profile. `tools/layout.py record` samples a scene's taken branches
  (`perf record -j any,u`), and `tools/layout.py order` clusters each function after its most frequent caller (C3,
  as lld's `CallGraphSort.cpp`) and writes their guest addresses. `module_llvm.c` gives each listed function a code
  section of its own (`SNX_LINK_HOT_TEXT` and its rank), and `link_elf.c` puts those sections first, in rank order.
  No instruction changes. ELF modules for this host only, and only when no object needs veneers.
- The generated code polls `CPU.poll` on backward branches inside a function and at the `D_` dispatch, as `codegen.c`'s C
  does, so a loop without SVCs gives its core to a waiting thread of higher priority. A backward branch's poll then
  leaves the function: it calls the function itself at the branch's target, which its entry and `D_` send to the
  target's label. The call is not in the loop, so `ir_promote.c` reloads no register after it inside the loop.
- The LLVM backend's speed: guest memory and the CPU structure are separate alias scopes (registers stay in host
  registers) and guest accesses are volatile (each happens, as on the hardware); logical immediates and SIMD
  immediates are folded; FP arithmetic, compares and conversions run inline on the host's IEEE unit when the result is
  provably the soft-float helper's, flags included (`ir_fp_fast`); the AdvSIMD and crypto encodings are inline instead
  of `snx_simd`, AES as S-box lookups where the host has no AES-NI. Build == host == target: the
  generated code uses the host's extensions (`snx_ir_host`, from LLVM's host detection), and the runtime library's
  helpers are portable C that LLVM compiles for each module's CPU; nothing is detected at run time. The emitters'
  x86 paths follow the target triple: AES-NI, PCLMULQDQ (PMULL, CRC32 by Barrett
  reduction), SSE4.2 CRC32C, VPTERNLOG (BSL/BIT/BIF, signed saturation), PAVGB/PAVGW (rounding halving adds, signed
  ones biased by 0x80), GF2P8AFFINEQB (8-bit shifts, bit reversal); the tricks are wunkolo's (wunkolo.github.io).
- `src/common/utilities/common.c` and `process.c`: host files/processes; argv-based subprocesses.
  NX extraction retains its isolated key environment.

Host tooling is strict C99 with SDK fixed-width types, formatting and OS APIs.
The runtime and generated guest code retain their existing Clang/ABI requirements.
Normal builds link the driver into the runtime; `SNX_TOOLS_ONLY` builds the same CLI independently.

Shared CPU ABI (`src/common/cpu/aarch64/`): guest functions take `CPU*`; `snx_lookup` dispatches indirect branches
through `CPU.disp` (relaxed loads: slots go from NULL to a function while threads run), and a pc without a function
goes to `RtAPI.miss`. `CPU.rt` supplies `RtAPI` callbacks: SVC (and NX's table of SVCs by number), UNDEF, BRK, hints, CNTVCT, FPCR, SEV/WFE, poll,
miss, and the 16-byte compare-and-swap of STXP on RISC-V and LoongArch hosts; no named runtime imports. NX's miss (`src/nx/runtime/rtapi.c`) runs the runtime JIT's code
(`src/nx/runtime/guest_jit.c` over `src/common/cpu/recompiler/llvm/jit_service.c`). There is no interpreter: a pc
without code yet is queued in the live misses' queue and its thread waits for the compile off its guest core, and a pc
the JIT can't compile (or a host where it can't start: Windows before 10 1803) ends the game with the reason. Compile
threads (host threads, not guest threads) take the queued pcs in batches: the functions at them and their callees
found by `snx_codegen_discover`, emitted by the LLVM backend into one module, loaded into a code arena with a copy of
`rtlib.c` (`code_loader.c`, packed into shared memory mapped once executable and once writable) and published into
`CPU.disp`. At start the hints of earlier runs are queued behind every miss. The pc is recorded as a hint either way,
so the next recompile has it. Each unit compiled is appended to `<game>.jitcache` in the cache folder, and a later run
loads it instead of compiling when the guest code it came from is the same byte for byte and the file was written with
the same ABI, IR object version, target triple, LLVM, runtime library, host CPU and settings. Its calls into the AOT
module go through `snx_aot_<pc>` symbols that the loader resolves at each load, so the object holds no host address.
`cpu_helpers.c` + `helpers/`: SIMD, crypto, FP.

GNU supplies an optional fetch callback that checks execute permissions and
compares the live instruction against the compiled word. Modified code falls
back to shared interpreter semantics. NX leaves this callback unset and keeps
its existing execution path.

### Module ABI

A game module (`<id>_code.so`, `.dll` on Windows) is code the recompiler made for one contract with the runtime:
- the `CPU` layout and the `SNX_FP_*` numbers in `src/common/cpu/aarch64/cpu.h`;
- the results of the helpers generated code calls (`cpu_helpers.c`);
- `RtAPI` and `SNX_SVC_COUNT` in `src/common/runtime/interface.h`;
- the module descriptor in `src/nx/loader/module_abi.h`.

`SNX_ABI` in cpu.h versions it. The recompiler writes it into each module (`SnxModule.abi`) and into its game's
`<id>.abi` stamp; the runtime refuses a module of any other, its message naming both ("ABI 12, where this one runs
13"), and the driver recompiles a game stamped with another when it is played.

Bump `SNX_ABI` for any change to the contract: a field of `CPU`, `RtAPI`, `SnxModule` or `SnxModInfo` added, removed,
moved or resized; a helper's results; an `SNX_FP_*` number; `SNX_SVC_COUNT`; an RtAPI function's arguments. Bump it
too for an addition the games should get though older modules would run without it (13's `snx_variants()` table). A
change to the recompiler's own output for the same contract leaves it alone, and every game keeps its old code until
it is recompiled (`-r`, or R in Games).

The module's `snx_routes()` table (`SnxRoutes`, with its own `SNX_ROUTES_VERSION`) is read as optional and left
`SNX_ABI` alone: it names the symbols whose calls the module sends through their dispatch slots
([Hooks](HOOKS.md#how-a-call-reaches-a-hook)). A module without it sends the calls to every function it exports through
their slots, which every hook also works with.

A bump costs little. The recompile it starts takes each code unit's object from the unit cache
(`src/nx/loader/unit_cache.c`), which keys it on the unit's IR, not on `SNX_ABI`: a bump that leaves the IR alone
reuses every unit, and one that changes it (a field moved) compiles them again, as it must. For a game of 255 units
that recompile takes 24 to 28 s, against 740 s with an empty cache. `SNX_IR_OBJECT_VERSION` (`ir.h`) rises when
`snx_ir_module_object` makes another object from the same IR.

`ctest -R module_contract` (`tests/module_contract.c`) holds the contract's layout as it was at the last bump: each
field's offset and size, the structs' sizes and the numbers above. A change to any of them fails it, naming the field,
until `SNX_ABI` rises and the table the failure prints is recorded there. It cannot see a helper's results, an RtAPI
function's arguments, or a field added into a struct's tail padding: for those the list above is the rule.

| `SNX_ABI` | What changed (cpu.h) |
| --- | --- |
| 7 | `CPU.poll` and `RtAPI.poll` |
| 8 | the FP/SIMD emitters and discovery |
| 9 | `RtAPI.svcs` |
| 10 | `RtAPI.miss`, and `CPU.disp` read with relaxed atomic loads |
| 11 | the reworked LLVM backend's modules |
| 12 | `RtAPI.cas128`, and `snx_cas128` takes the CPU |
| 13 | the AOT module's `snx_variants()` table, read as optional |
| 14 | `RtAPI.code_changed` for IC IVAU, and `SnxModule`'s function table with the byte each AOT function checks first |
| 15 | calls to an exported function go through its dispatch slot, which a hook of the runtime's may hold |
| 16 | `stx`'s result for a store-exclusive whose range the armed monitor covers |

#### Code the game patches

Mods for Atmosphère (Skyline, exlaunch) hook a game by writing branches into its code through a writable view of it
(`svcMapProcessMemory`), then make the new instructions visible the way Arm requires, with `IC IVAU` on each line
they wrote. The AOT code was compiled from the bytes before, so since SNX_ABI 14 `IC IVAU` reaches the runtime
(`snx_ic_ivau`, `RtAPI.code_changed`), and `jit_code_changed` (`runtime/guest_jit.c`) finds the AOT functions compiled
from the line by the descriptor's `fn_lo` and `fn_len`. Each one loses its dispatch slots and its entries in
`entry_fn` (which the JIT calls AOT functions through), then its byte in `SnxModule.stale` is set. Every AOT function
reads its byte first (`stale_check` in `ir_codegen.c`) and, once it is set, runs its entry through dispatch, where the
lookup finds no code and the JIT compiles the instructions as they are now, the patch included. A direct call from
other AOT code arrives at the same check, so a hook in the middle of a function takes effect for every caller. A call
returning into an AOT function reads the byte too: a loader may patch a function from inside a call it makes, and once
the byte is set the function leaves for the thread's dispatch loop at the return address, which runs the new code from
there. The
JIT's code goes as any other code change drops it (`jit_code_invalidate`): `snx_jit_invalidate` drops every unit
whose span of guest code the change touches, all its leaders, since a unit's functions call each other directly. That
covers hooks in code the game maps itself (NROs), which only the JIT runs.

## Runtime ownership

| `src/nx/` directory | Responsibility |
| --- | --- |
| `runtime/` | Module loading, INI, clock, `RtAPI`, crashes; `settings.txt`, every setting (`tools/settingsgen` generates `snx_settings.h`, `g_settings` and the tables the runtime, the launcher and the recompiler read them with) |
| `hle/kernel/`, `hle/svc/`, `memory/` | Objects, threads, sync, SVC, Switch memory, IPC |
| `hle/services/`, `hle/filesystem/` | Services, nvdrv, guest filesystem services |
| `gpu/` and `gpu/maxwell/` | GPFIFO, Maxwell engine implementation, shaders, Vulkan renderer |
| `gpu/video/` | host1x, NVDEC H.264 and VP9 decoding through FFmpeg, VIC |
| `runtime/platform/` | Host APIs, SDL3/headless, input, screenshots, timed exit, the in-game menu, the shader loading screen |

### Guest exec

Guest thread = host thread + `CPU` + TLS. `hle/kernel/thread.c`: one owner/core, four cores; release on kernel block,
sleep/yield, IPC, WFE. Waiters: priority then FIFO; a waiter of higher priority takes the core at the holder's next SVC
or backward branch (`CPU.poll`, `RtAPI.poll`). A ready thread that would wait
behind its core's holder runs on an idle core of its mask, and an idle core takes such a waiter (only
`svcSleepThread(0)` keeps a yielding thread on its core). A mutex owner runs at its best waiter's priority
(`hle/kernel/sync.c`).

`memory/mem.c`: one guest VA reservation; `GP(addr) = g_mem + addr`, accessed directly by generated code.

SVC -> its wrapper in `hle/svc/svc.c`'s tables, generated by `tools/svcgen` from `hle/svc/hos.svc` (each SVC's
registers in the 64-bit and 32-bit ABIs), which calls the SVC's handler; `svcSendSyncRequest` -> `hle/kernel/ipc.c` -> session's HLE service object, whose dispatcher
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

The shader programs a game ships are translated at startup, on the renderer's shader file thread rather than the GPU
thread, into `<id>.vkshipped` (`prepare_shaders`: before the game's first thread runs, behind main.c's loading screen,
or in the background as it runs). `gpu/shipped_pass.c` walks the RomFS and the executables' segments, and
`gpu/shader/shipped.c` finds the programs by NVN's code wrapper (or a bare NVN shader file's layout) in their bytes,
and in the Yaz0 SARCs whose names list a shader container. `gpu/vk/pipeline.c` translates each into the file, which a
draw's lookup reads from when it has no shader of its own. The files the walk found nothing in are kept in
`<id>.vkscan` by their name, place and size, under the hash of the walk's sources (`cmake/ship_walk.cmake`) rather than
the build, so a new build or driver reads only the files that hold programs again.

### Renderer coherence + lifetime

- **Vk**: Vulkan 1.3 in windowed or headless mode, with guest buffer mirrors; separate compute queue when available,
  graphics queue otherwise.

Invalidation: host write tracking + `mem_unmap` -> `gpu_mem_changed`. Vk treats zeroed unmap/heap-return ranges
as written at the next cache check. Write tracking (`plat_ww_scan`) is userfaultfd's asynchronous write protection
with `PAGEMAP_SCAN` on Linux 6.7 and later, and the reservation's write watch on Windows. Every other POSIX host
(macOS, FreeBSD, Android, older Linux; `SNX_WW=signal` on any Linux) protects the pages a scan reported and takes the
first write to each in the fault handler, which opens the page again. A system call can't fault into the handler, so
the runtime opens what the kernel will write before the call (`plat_ww_open`, next to `own_acquire`).

`Renderer.save`: optional pipeline-cache persistence every 300 frontend ticks (~5s) + exit, concurrent with execution.
Exit skips renderer teardown and guest-thread joins.

### Videodec/VIC channels

Each NVDEC/VIC fd owns a `host1x_open` channel, registers and syncpoints; NVDEC also owns a decoder.
NVDEC writes guest surfaces; VIC composites output.
