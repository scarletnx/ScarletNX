<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# 3DS port

## Index

* [Status: stage 1 + stage 2a + stages 2b1–2b4 + initial 3a/3b/FS](#status-stage-1--stage-2a--stages-2b12b4--initial-3a3bfs)
  * [Kernel-bootstrap subset (2b4)](#kernel-bootstrap-subset-2b4)
  * [Initial CTR IPC/services (3a)](#initial-ctr-ipcservices-3a)
  * [Current limits and trust](#current-limits-and-trust)
  * [Build and test](#build-and-test)
* [Source provenance and licensing](#source-provenance-and-licensing)
* [Boundaries and prerequisites for stage 2b](#boundaries-and-prerequisites-for-stage-2b)
* [Staged roadmap (2a implemented; 2b partially implemented)](#staged-roadmap-2a-implemented-2b-partially-implemented)
  * [2. CIA extraction, memory controller and static execution](#2-cia-extraction-memory-controller-and-static-execution)
  * [3. Basic HLE OS and a SwIPC-derived generator](#3-basic-hle-os-and-a-swipc-derived-generator)
  * [4. Audio DSP](#4-audio-dsp)
  * [5. Video graphics](#5-video-graphics)
  * [6. HID and remaining peripherals](#6-hid-and-remaining-peripherals)

## Status: stage 1 + stage 2a + stages 2b1–2b4 + initial 3a/3b/FS

`scarletnx-3ds` extracts **decrypted application CIAs**. With `SNX_3DS_AOT=ON`,
it performs **extract -> offline static AOT -> native module -> checked ARM/Thumb
execution**. Both asset-free witnesses and the supplied SuperTux CIA's native
startup gate pass. SuperTux completes minimal APT startup, creates a native
background thread, initializes HID and **reaches and executes main** at
`0x00167d88` (50,489 blocks), then opens its reconstructed RomFS. It stops at
FS `ControlArchive` (timestamp) after 50,629 blocks / 61 SVCs, having executed
scalar VFP in `gettimeofday`, but before its first
file read. It is **not playable**; no first frame or audio yet. See the
[SuperTux integration target](3DS-SUPERTUX.md) for the pinned input and next blockers.

Text is bounded at **16 MiB**, using conservative static CFG discovery for larger
images. This is not complete indirect-target coverage or a general 3DS runtime.
Scalar VFPv2 now supports F32/F64 arithmetic/conversions and bounded atomic RAM
transfers; short vectors, enabled exceptions and legacy extended-save forms
remain unsupported. Stage 2b4 adds serialized ARM11 checked exclusives and
a bounded kernel-bootstrap subset. Stage 2 still needs broader discovery/ISA
validation and cache/resume. Initial stage 3a adds a SwIPC-derived CTR generator,
checked transport and bounded service sessions. Initial 3b adds the minimal
APT/thread/event/mutex/PTM/HID startup personality and ARM11 UADD8/SEL.
Full HLE/IPC and stages 4–6 remain pending. No firmware, keys, title assets or Azahar CPU engine are included.

Implemented:

- Stage 1: `SNX_BUILD_3DS`, independent driver, Old/New 3DS board profiles,
  physical-range bounds checks, lifecycle/base ticks, GUI CIA drops/browser/
  recents and strict routing to the 3DS driver without Switch-only flags.
- `src/3ds/loader/`: streaming CIA sections; signature-dependent ticket/TMD
  offsets; title, boot-index, content bitmap, unique ID/index, content-info and
  SHA-256 validation. Boot content is selected by its TMD index, not table order.
  All present contents and container metadata are materialized on disk.
- Decrypted application NCCH/ExHeader, checked process segment/BSS metadata,
  ExeFS files and hashes, bounded backward LZSS/BLZ `.code` expansion, and IVFC
  level-3 RomFS extraction into a host tree. Traversal/invalid UTF-16, metadata
  directory/file traversal cycles, overlaps, duplicate paths and excessive expansion are rejected.
- Shared NX host utilities (`common` filesystem paths, file IO, private staging,
  cleanup, publication) are reused; the NX kernel/NSO loader/FS service are not
  linked. Container/content hashes and version-1 image identities use OpenSSL
  EVP SHA-256, with incremental hashing for streamed data; no custom SHA is used.
- `src/3ds/memory/ctr_memory.h`: independent page-based address spaces, shared
  physical RAM/VRAM/DSP backings, aliases, R/W/X checks, checked cross-page RAM
  transfers and explicit aligned MMIO callbacks. Failed map/protect/unmap and
  RAM range-validation operations do not partially change guest memory/mappings.
  No flat host pointer is exposed.
- Initial 2b: shared A32 codegen has independent, explicitly selected ARM11 ISA
  and checked-memory options. Existing A57 APIs/defaults are shared, and
  `SnxArmCPU` ABI 3's `rt` lets the CTR adapter decide unsupported instructions
  and CP15 itself (`cpu/arm/README.md`); NULL keeps the exits. No 3DS global
  compiler define affects other boards.
- `src/3ds/cpu/ctr_cpu.h`: versioned checked CPU state, USR initialization and
  callbacks connecting statically generated integer instructions to the page
  controller, including aliases, R/W/X, whole-range multiword RAM validation and
  ordinary MMIO. Every instruction checks execute permission and code identity.
  Static O0/O2 native witnesses execute against this adapter without LLVM/JIT
  linked into their executables.
- Stage 2b2: raw ExHeader/expanded `.code` revalidation, RX/R/RW process mappings,
  zeroed page-appended BSS, guarded stack and one private initial TLS page.
  `TPIDRURO` points at this page. Initial 3b adds per-thread zeroed TLS slots
  and serialized native context scheduling, not a host-thread/multicore scheduler.
- The offline LLVM compiler and native host linker, part of `scarletnx-3ds`:
  private staging/publication, versioned module ABI and exact image
  identity. Images up to 4096 bytes retain exhaustive one-instruction ARM/Thumb
  entries. Stage 2b3 adds conservative static CFG discovery for larger images:
  direct branches/calls, conditional continuations, literal/ADR code pointers,
  initialized RO/data pointer candidates, legacy Thumb BL pairs and a bounded
  PC-relative ADD jump window. Initial 3b also recognizes adjacent unsigned
  CMP / LDRLS PC-indexed tables with at most 256 entries; no unbounded sweep.
  Blocks contain at most 32 guest instructions;
  bounded LLVM chunks are disposed between emissions, then statically linked.
  There is no runtime discovery/compilation. Uncovered targets stop explicitly.
- Homebrew `.data` byte counts including BSS are accepted only if subtracting
  BSS fits initialized pages; BSS never becomes required input `.code` bytes.
  All segment/expansion bounds and content hashes are still checked.
- Native loader/dispatcher checks ABI, ISA, memory mode, CPU sizes, table counts
  and image identity. It dispatches on PC + CPSR.T, enforces execution permission
  and a block budget, and reports missing entries, faults and unsupported SVCs.
  The runtime and generated modules do **not** link LLVM or an interpreter/JIT.

### Kernel-bootstrap subset (2b4)

- Process-owned, reference-counted typed objects and 256 generation-tagged handles.
  Process/main-thread pseudo-handles identify real root objects. Duplicates retain
  identity, close releases references, stale handles are rejected until finite
  generation rollover, and table/object exhaustion returns errors without leaks.
- SVC `0x21` creates real address-arbiter objects. `0x22` implements signal with no
  waiting peers, signed predicates, conditional decrement and immediate timeout.
  Initial 3b adds priority-ordered peer signaling. A required wait blocks the
  actual thread and pins its object until a genuine wakeup. A native peer can
  run; dispatch alone never completes a pending call. Finite pending deadlines
  remain unsupported before mutation; timers are not implemented.
- SVC `0x23` closes handles and `0x27` duplicates them. SVC `0x2a`, type 0, reports
  actual allocated FCRAM pages for the modeled memory regions. Other queries
  return an explicit unsupported result. ExitProcess (`0x03`) remains handled.
- Read-only configuration page at `0x1ff80000`, backed by board AXI WRAM rather
  than charged to application FCRAM. The limited kernel personality reports
  version 2.58 and memory mode 0 (Old, 64 MiB application) or 6 (New, 124 MiB).
  Other ExHeader memory modes, resource-limit capabilities and kernel/firmware
  configuration fields are not fully modeled.
- ControlMemory (`0x01`) validates page ranges/permissions and supports commit,
  free and protect of owned heap/linear allocations. Normal heap uses explicit
  VAs in `0x08000000..0x10000000` and may span discontiguous physical holes.
  Linear memory is physically contiguous with `PA = VA - 0x14000000 + 0x20000000`.
  Recycled pages are zeroed. Requests are preflighted and failed publication is
  rolled back. Aliases, region flags and executable heap permissions are refused.
  This is not full ControlMemory behavior for static image/shared mappings.
- ARM11 checked LDREX/STREX B/H/word/double and CLREX use optional checked-ABI-1
  reservation transactions. The bus owns a saturating board-wide write/mapping
  epoch, so kernel writes, aliases and other address spaces invalidate reservations
  even after an ABA write. Unrelated writes may fail exclusives conservatively.
  MMIO is rejected. All execution and mutations remain serialized, with no host
  concurrent/multicore scheduler claim. A57 defaults and both CPU ABIs are unchanged.

### Initial CTR IPC/services (3a)

SVC `0x2d` connects to the actual `srv:` endpoint; `0x32` dispatches checked
TLS command words; `0x35` identifies the actual process root. A separate SwIPC
`--ctr` mode emits typed handlers without HIPC/CMIF or successful default stubs.
`srv:` supports client registration and service lookup with real session
ownership. Initial 3b adds a one-application APT startup lifecycle, real recursive
lock/events, serialized thread contexts/waits/wakeups, PTM base/model queries
and HID handles/read-only mapping. HID sampling is not implemented. FS supports
PID/SDK initialization, an actual missing-SD error and read-only SelfNCCH RomFS
OpenFileDirectly, bounded OpenArchive/OpenFile/CloseArchive and FSFILE
Read/GetSize/Close. Archive tokens are FS-private monotonic 64-bit identifiers,
not kernel handles; 32 live bindings are allowed and stale IDs never revive.
NX and CTR share `common/filesystem/`
indexing, immutable file mappings and fallback opens, not their IPC protocols or
RomFS metadata. Only extracted `romfs/` is exposed; no original container reads.
Writes, general archives/paths and SD/save backing remain unsupported. Reads
preflight the complete guest destination and stage host IO before publication.
Host-paced GetSystemTick and a read-only UTC shared-page snapshot support time
queries, not cycle accuracy or timers. FSUSER ControlArchive GET_TIMESTAMP
(`0x80d`) resolves a bounded UTF-16 archive path and returns the real backing
file's host modification time as a 3DS datetime (milliseconds since 1900 UTC);
missing paths return `0xc8804478`, stale tokens `0xc8804465`, and save-commit or
unknown actions stop. Unknown commands stop; absent-service
waits block without a fake response. SuperTux's next blocker is the archive
timestamp control request; no timestamp is fabricated.

`gsp::Gpu` is modeled with real bounded resources: RegisterInterruptRelayQueue
returns the genuine `0x2a07` first-initialization result, one writable 0x1000
GX/interrupt shared block and up to four relay threads with real interrupt-queue
slots and guest-created events. AcquireRight/TryAcquireRight/ReleaseRight own a
single right; WriteHWRegs/WithMask update a checked 4 KiB GX/LCD register file;
FlushDataCache/InvalidateDataCache are coherent no-ops behind a real process
handle check. TriggerCmdReqQueue executes RequestDma and MemoryFill as whole-span
checked transactions and CacheFlush as a coherent no-op. DisplayTransfer and
TextureCopy run on the transfer engine (`src/3ds/gpu/transfer.c`, ported from Azahar's
software blitter): tiled/linear conversion in both directions, the five framebuffer
formats, the X and XY box filters and FLIP/CROP skew, texture copies with input and output
gaps. GX commands address memory as the GSP module does (its virtual-to-physical windows
for the linear heaps, VRAM and New 3DS extra RAM), and the engine reads and writes
physical memory as a bus master, so a VRAM the CPU maps read-only still receives
transfers. RequestDma resolves the application's pages to physical ones the same way.
MemoryFill honors its control halfwords (trigger bit, 16/24/32-bit width) and fills from
start to end rounded down to 8 bytes. Each command raises its own interrupt (DMA, PSC0 or
PSC1, PPF) on the right holder's relay queue, a failing command stores its error and the
FAILED status as GSP does, and the queue goes on. SubmitCmdList writes the list's
address and size to the first command buffer channel and runs it on the PICA command
processor (`src/3ds/gpu/pica.c`, ported from Azahar's `PicaCore`): masked register
writes, batched and sequential extra data, shader program, swizzle and uniform uploads,
the lighting, fog and procedural-texture LUTs, default attributes and immediate mode,
command buffer jumps and the P3D interrupt request with autostop. A list outside memory
raises P3D without running. Draws run in software, ported from Azahar's software
renderer: the shader interpreter (`gpu/shader.c`), vertex loading with the indexed
vertex cache and primitive assembly (`gpu/draw.c`), and clipping, rasterization,
texturing in the fourteen texture formats with cube and projected textures, fragment
lighting, the six texture combiner stages, fog, the alpha, stencil and depth tests,
blending and logic ops into the tiled framebuffer in physical memory (`gpu/raster.c`,
`gpu/lighting.c`, `gpu/texture.c`). Geometry shaders, procedural textures and shadow
textures and rendering are not ported yet: a draw that needs one stops the run and
names it. The ExHeader's address-mapping capabilities
are applied at process creation (VRAM, DSP RAM, New 3DS extra RAM), as Azahar's
`HandleSpecialMapping` does; MMIO areas stay unmapped. SetBufferSwap latches real
framebuffer descriptors (RGBA8/RGB565; other layouts stop rather than being
shown with invented colors). Display timing is host-paced 60 Hz: latched frames
present at vblank waits and overdue swaps, and waits on the relay event complete
only at real vblank deliveries. QueryMemory (SVC `0x02`) reports actual mapping
regions with coarse free/IO/static/code/private/shared classes, MapMemoryBlock
accepts the guest mappable range and registers mappings, and HLE shared pages
are reserved from board FCRAM past the application pool.

Presentation is **Vulkan only** (volk; no linked Vulkan library, no shaders):
the device converts the guest framebuffer to RGBA8 and shows the landscape-
rotated frame live in an SDL3 Vulkan window through a real swapchain. Bounded
PPM snapshots land in `<output>/frames` in every mode. Without a display the
window path is unavailable and frames are only recorded; without a Vulkan
device the presenter reports unavailability and no frame is fabricated.

Audio runs on a high-level DSP (`src/3ds/audio/`, ported from Azahar's HLE audio core):
dsp::DSP (`services/dsp.c`) takes the application's pipe writes, interrupt events, the
semaphore and component loads. Initialize answers on the audio pipe with the DSP RAM
addresses of the shared structures, and from then on every 160-sample frame (4.889 ms,
paced by the host clock like the vblank) reads the region the application finished,
plays the 24 sources' buffers out of FCRAM (PCM8, PCM16 and GC-ADPCM; no interpolation
or linear, polyphase playing as linear as in Azahar), applies their simple and biquad
filters and gain ramps, mixes the three intermediate mixes with the aux buses and delay
effects, writes the statuses and final samples into the other region and raises the
audio pipe's interrupt. Reverb, the limiter, the compressor and surround output are
stubs in Azahar too. The binary pipe's AAC decoder is not ported: a request on it stops
the run and names it. Frames play on the default device through SDL3 and their first two
minutes are recorded to `<output>/audio.wav`. libctru's ndsp loads
`sdmc:/3ds/dspfirm.cdc` and leaves audio off without it, so a stand-in header goes there
when the title's SD has none: the HLE DSP never runs the component, and a dumped file is
used as it is. cfg:u, cfg:s and cfg:i (`services/cfg.c`) answer GetConfig for the sound
output mode (stereo, Azahar's default), which ndsp reads, and stop on any other block.
With the software renderer below full speed, the audio has gaps after rendered frames:
the guest's mixing threads get no time while a frame renders.

See [CTR IPC](CTR-IPC.md) for the strict generator subset, descriptor/permission
checks, quotas, malformed-request policy, tests and unimplemented operations.
Unknown SVCs still stop explicitly. No service/device success is fabricated.

```sh
build/3ds/scarletnx-3ds -x --output work/my-title game.cia
```

The output is a directory containing `contents/*.app`, `exefs/` (with
expanded/page-padded `.code`), `romfs/`, raw container/NCCH/ExHeader metadata and
`manifest.ini` with source/code hashes and segment layout. The default directory
is `work/3ds/<name>`, named after the CIA's file as the Switch driver names a dump's, in the data folder the Switch driver uses
(`--repo DIR`, else a portable copy's folder, else the user's data folder, from a build tree too),
which the user's on Linux keeps in `~/.cache/scarletnx/work/3ds/`. No source CIA needs to be mounted by
the future guest FS. Extraction is privately staged, then published only after
all checks succeed; failures remove staging files. As in the Switch driver, a
finished extraction already there is used as it is, never extracted again or
hashed. Keep the input and output parent trusted and
not concurrently modified; extraction is not a sandbox against hostile host
processes racing parent directory changes.

`-x`/`--extract` stops after extraction and `-r`/`--recomp` compiles without
playing, as the Switch driver's do. Otherwise the driver loads
`<extracted>/native/code.so` and compiles it first only when needed: when it is
missing, or no longer loads or matches the build's module ABI or the extracted
image's identity (a changed `.code`). `-r` always compiles again. A new module
replaces the old one only once it is complete. `native/code.o` holds the
descriptor/index; `native/block-*.o` hold the guest code. All objects are retained
for review. There is no compilation during dispatch. A failed compile removes
private native staging; the extracted tree remains. The GUI uses this same
driver path, not a separate engine.

A launch runs until the game exits or its window is closed, as a Switch game does.
`--max-blocks N` stops it after N native blocks instead. A larger-image block may execute up to 32 instructions; this is not a cycle or
exact instruction budget. Each instruction still checks fetch/identity/permissions.
`--model old|new` selects RAM backing (default Old), not a different ISA or
compiler. `--board-info` prints physical regions without allocating RAM.
`Guest ExitProcess: r0=...` prints a register diagnostic, **not a POSIX guest exit
status**. Successful handling of SVC 0x03 returns host status zero. Exit codes:

- 0: help/info, extract-only success, or handled guest ExitProcess.
- 1: malformed data, IO/limit/existing-output failure, ABI mismatch or guest fault.
- 2: invalid CLI arguments or missing/non-CIA file path.
- 3: unsupported format/ISA-backend/SVC, disabled/missing compiler, text-size cap,
  missing static target, exhausted execution budget or a pending unsupported wait.

### Current limits and trust

Only decrypted application CIA/NCCH content is supported, not encrypted layers,
updates, DLC, system titles or nonstandard NCCH media units/platforms. Input and
total materialized output are each capped at 1 GiB; metadata tables at 16 MiB,
code at 64 MiB, entries at 100000, and RomFS nesting at 64. Host filenames must
be portable safe components (maximum 255 UTF-8 bytes); unsafe/reserved names
are rejected rather than silently renamed. Host paths with control characters
are refused to prevent log/banner injection. Larger/unsupported inputs fail closed.

**Hashes are integrity checks, not signature authentication.** RSA/ECDSA
signatures, certificate trust, ticket rights and ExHeader access-descriptor
signatures are not authenticated. RomFS superblock and entire CIA-content
hashes are checked, but the internal IVFC hash tree is not separately verified.
AOT execution is opt-in and **not a host security sandbox**. Run only images you
trust: compiler correctness is not isolation, and native loading can execute host
constructors before descriptor validation. The CLI only loads its freshly built
private output, never a preexisting cache. Do not treat ABI/hash checks, host
native modules or an editable extraction manifest as authenticated permissions.

The image loader derives layout from raw ExHeader and requires exact page-packed
expanded `.code`; it ignores `manifest.ini` for execution. Its compatibility hash
covers a versioned domain, full ExHeader and full expanded code. It is not yet a
complete cache key (CIA/resources, compiler/build/configuration versions and
resume policy still need a separate design). Initial AOT also requires a
word-multiple declared text length. Text-page padding beyond declared text has
no static entry, even if its page is executable. Larger images also have NULL
slots for unproven indirect targets; these fail explicitly without recompilation.
The static graph is capped at 524288 discovered instructions and 131072 entries;
the entry cap was raised for genuine scalar-VFP fallthrough coverage, not runtime discovery.
exceeding either aborts compilation, never silently truncates the graph. Objects
together are capped at 256 MiB and the linked module at 512 MiB. Tiny-image code
uses O2; larger-image chunks use O1, with 128 entries per LLVM module.
No scheduling, events, service
IPC, resource mappings, handles or device support are implied by the initial
private TLS/stack setup.

### Build and test

With the normal Clang/CMake prerequisites (no LLVM SDK, SDL or media dependencies
needed for the extraction/memory-only configuration):

```sh
cmake -S . -B build/3ds -DSNX_BUILD_NX=OFF -DSNX_BUILD_GNUABI=OFF -DSNX_BUILD_GUI=OFF
cmake --build build/3ds
ctest --test-dir build/3ds --output-on-failure
build/3ds/scarletnx-3ds --model new --board-info
```

Use `-DSNX_BUILD_GUI=ON` to include the launcher and its regression tests; SDL3
is then required/fetched as usual. `-DSNX_BUILD_3DS=OFF` omits the driver (the
GUI diagnoses its absence). The profile decoder test needs no LLVM. Additional
static AOT tests are explicitly opt-in:

```sh
cmake -S . -B build/arm11 -DSNX_BUILD_NX=OFF -DSNX_BUILD_GNUABI=OFF \
  -DSNX_BUILD_GUI=OFF -DSNX_TEST_ARM_AOT=ON
cmake --build build/arm11
ctest --test-dir build/arm11 --output-on-failure
```

This requires a native LLVM SDK (select with `SNX_LLVM_CONFIG` if needed).
`SNX_TEST_ARM_DIFF=ON` additionally requires Python `unicorn==2.1.4`, and enables
ARM11 MPCore plus shared A57 integer/VFP/NEON/Thumb regressions. See
`src/common/cpu/arm/README.md` for the codegen contract and exact limitations.
Those test options alone do not enable the pipeline. To build/run it on a
**native 64-bit Linux host** with Clang and the LLVM SDK:

```sh
cmake -S . -B build/ctr-aot -DSNX_BUILD_NX=OFF -DSNX_BUILD_GNUABI=OFF \
  -DSNX_BUILD_GUI=OFF -DSNX_3DS_AOT=ON
cmake --build build/ctr-aot
ctest --test-dir build/ctr-aot --output-on-failure
build/ctr-aot/scarletnx-3ds --output work/small-title --max-blocks 100000 small.cia
```

The compiler calls the host C compiler selected by CMake to link its PIC object;
that compiler must remain installed at the configured path. The compiler is part of
`scarletnx-3ds`, which links LLVM as `scarletnx` does; the compiled modules need no
LLVM library.
Use both `SNX_3DS_AOT=ON` and `SNX_TEST_ARM_AOT=ON` for pipeline plus CPU tests.
The bounded CFG unit test needs no LLVM. To run the optional external SuperTux
bootstrap gate, set `SNX_TEST_3DS_SUPERTUX=/absolute/path/to/Supertux.cia`.
`SNX_TEST_ARM_DIFF=ON` additionally compares its first-SVC GPR/CPSR and complete
data/stack/TLS snapshots against pinned Unicorn ARM11 MPCore. A second CPU
comparison reaches ConnectToPort with test-scripted HLE effects and complete
heap/linear snapshots. The second comparison validates CPU behavior under that
transcript, not hardware kernel semantics. A third comparison continues through
scripted `srv:` connection/registration effects to before APT lookup, again matching
all GPRs/CPSR and complete RAM snapshots without independently proving IPC policy.
Unicorn is never a runtime engine. The same native witness subsequently observes
CRT entry into main, successful RomFS file/archive opens, scalar VFP execution
and the next unsupported archive timestamp request.
Separate native CIA and unit tests exercise actual worker wakeups, resources
and rollback. Service policy is not independently hardware-validated.
No sample binaries are downloaded or distributed.

## Source provenance and licensing

The initial shallow checkouts were:

- ScarletNX: `a9258f1eaa3a975f84ad2216950b3f62ddc92ba0`.
- Azahar: `68202bc4ff7edf9d7280ec0dcd6572b7cd1ae26e`.

Stage 1 uses physical-layout facts from Azahar `src/core/memory.h`, the tick
constants from `src/core/core_timing.h`, and the system-initialization design in
`src/core/core.cpp`. Stage 2a references `src/core/file_sys/{cia_container,
title_metadata,ticket,signature,ncch_container}.*`, `src/core/loader/ncch.cpp`
and `src/core/hle/romfs.cpp`. The bounded BLZ implementation credits the
Citra/Azahar algorithm; the extractor and page controller are new C code.
Initial 2b CPU/profile and bus code is new project C code using ARMv6K/legacy
Thumb architectural semantics; it does not copy an Azahar CPU engine. The
shared emitter retains its existing notices. LLVM's `mpcore` assembler model
and Unicorn 2.1.4's ARM11 MPCore model are test references, not runtime engines.
Stage 2b2 process mappings/BSS policy refer to Azahar `src/core/loader/ncch.cpp`
and kernel process layout at the same revision; the new image/module/compiler/
dispatch implementation is project C code, not an adapted Azahar CPU engine.
Stage 2b4 references GPL-2.0-or-later kernel `svc.cpp`, `errors.h`,
`handle_table.cpp`, `address_arbiter.cpp`, `config_mem.*`, `memory.cpp` and
`process.cpp` at the same revision for SVC register/result semantics, object
lifetimes, configuration offsets and memory-region facts. The new C object,
allocator and reservation implementation does not import an Azahar CPU engine.
Stage 5's transfer engine and GX queue port Azahar at `a4b335e` (the revision current
on 2026-10-16): `video_core/renderer_software/sw_blitter.cpp`, `video_core/pica/regs_external.h`,
`video_core/utils.h` and `common/color.h` (GPL-2.0-or-later), and the command semantics of
`video_core/gpu.cpp` and `core/hle/service/gsp/gsp_gpu.cpp` (GPL-3.0-or-later); the
special mappings follow `core/hle/kernel/{process,memory}.cpp` (GPL-2.0-or-later).
The command processor ports `video_core/pica/pica_core.cpp`, `shader_setup.cpp`,
`packed_attribute.h`, `pica_types.h` and the register layouts of `regs_*.h`
(GPL-2.0-or-later) at the same revision.
The software pipeline ports `video_core/shader/shader_interpreter.cpp`,
`video_core/pica/{shader_unit,vertex_loader,primitive_assembly,output_vertex}.cpp`,
`video_core/renderer_software/{sw_rasterizer,sw_clipper,sw_texturing,sw_framebuffer,sw_lighting}.cpp`
and `video_core/texture/{texture_decode,etc1}.cpp` (GPL-2.0-or-later), with the shader
instruction encoding of nihstro's `shader_bytecode.h` (BSD-3-Clause, notice kept in
`gpu/shader.c`).
Stage 4's HLE DSP ports Azahar at `a4b335e`: `audio_core/hle/{hle,source,mixers,filter,effects}.cpp`,
the layouts of `audio_core/hle/shared_memory.h`, `audio_core/{codec,interpolate}.cpp`, and
`core/hle/service/dsp/dsp_dsp.cpp`, with the sound output mode block of
`core/hle/service/cfg/{cfg,cfg_defaults}.cpp` (GPL-2.0-or-later). The wire forms are
checked against libctru's `services/dsp.c` and `services/cfgu.c`.
Stage 3a references GPL-2.0-or-later `core/hle/ipc.h`, `kernel/hle_ipc.cpp`,
`kernel/svc.cpp`, `service/sm/{srv.cpp,sm.h}` and `service/fs/fs_user.cpp` at that
revision for wire/result/initialization facts. Its new C transport/services and
separate SwIPC emitter are project code; see [CTR IPC](CTR-IPC.md).
Azahar is not a build dependency.
Azahar's root license is GPLv3; individual files carry GPLv2-or-later or
GPLv3-or-later notices. Future adaptations must retain each source's notices
and record its origin/revision here; select the GPLv3-compatible terms where
applicable. Audit third-party/external components separately rather than assuming
that every dependency has the root license.

## Boundaries and prerequisites for stage 2b

The 3DS application CPU is ARM11/ARMv6K with VFP, **not** the Cortex-A57
ARMv8-A AArch32 default in `src/common/cpu/arm/`. That backend's existing
user-mode API still uses a single flat `mem`/`mem_size` range.
Mapping a sparse 3DS address space into that ABI would make holes accessible and
would bypass process permissions, aliases and device callbacks.

Remaining requirements beyond the initial native bootstrap pipeline:

1. The initial ARM11 integer profile is implemented. Complete VFPv2 (including
   short-vector FPSCR behavior), missing ARMv6K instructions and alignment-mode
   validation. Current ordinary unaligned transfers model unaligned support
   enabled/alignment checking disabled, not every ARM11 CP15 configuration.
2. The separate checked-memory ABI and CTR adapter implement integer and
   bounded scalar ARM11 VFP transfers. ARM11 checked exclusives now have serialized bus-owned reservations,
   including alias/ABA invalidation. Concurrent scheduling and broader monitor
   validation remain pending. General vector transfers, A57 checked exclusives and
   checked Thumb table branches remain unsupported. Never pass an A64 `CPU*` or
   an NX module to an A32 block.
3. Module ABI 1 supports exhaustive tiny images and bounded conservative CFG
   discovery/chunking for larger images. Its full-geometry pointer tables permit
   NULL slots; the ABI and CPU layouts are unchanged. Improve computed-target
   coverage beyond the current pointer heuristics and 128-word PC-relative ADD
   window. Add validated resume/cache and scheduling. No interpreter/runtime JIT.
4. Keep all CTR code under `src/3ds/`, with only `common` dependencies. Extract
   reusable host FS/tree operations from NX into common rather than linking the
   NX kernel/services/Maxwell runtime into CTR. Keep 3DS archive IDs, permissions,
   title identity and saves in the CTR adapter. Architecture tests enforce the
   runtime boundary.

The page controller is now connected through an opt-in `SnxArmCheckedCPU`
callback ABI, while the original CPU block/state ABI is deliberately unchanged.
The adapter is not linked to the legacy execution runner. The A32 runner in
`src/common/cpu/arm/exec.c` compiles blocks while a program runs and is **not** used by this pipeline.
`SNX_3DS_AOT` selects the separate offline compiler and ARM11/checked codegen
options; shared A57 emission and CPU ABI defaults remain unchanged.

The tick domain remains 268,111,856 Hz, with 4,481,136 base ticks per frame, as
in Azahar. New 3DS CPU scaling is a separate future scheduler concern. There is
still no interrupt scheduling, ARM9 emulation or boot ROM. Synthetic CIAs and
the supplied SuperTux startup execute natively, not complete games/retail titles.

## Staged roadmap (2a implemented; 2b partially implemented)

Each stage should be one independently reviewable commit/diff, with all earlier
stages as its base. Stubs must return explicit unsupported errors, not pretend
that an unimplemented service/device succeeded.

### 2. CIA extraction, memory controller and static execution

The extraction and independent memory-controller subset is implemented as 2a.
Stage 2b1 supplies ARM11 integer profile selection and checked-memory integration.
Stage 2b2 adds module ABI/loading, bounded exhaustive ARM/Thumb static dispatch,
ExHeader process/stack/TLS setup and the asset-free extract/AOT/run witness.
Stage 2b3 removes the tiny-image restriction with bounded chunked static CFG
compilation and passes the external SuperTux first-SVC native bootstrap gate.
Stage 2b4 adds kernel-bootstrap objects, configuration, checked allocations and
serialized ARM11 exclusives. That checkpoint reached ConnectToPort before `main`;
initial 3a passed `srv:` registration and stopped at absent APT:S. Initial 3b
passes the bounded APT startup/thread/HID path and reaches main. The minimal
shared-backed FS increment opens RomFS. Scalar VFPv2 and its offline CFG
fallthrough now pass `gettimeofday`; the next stop is archive timestamp control.
**Stage 2 is not complete**: broader discovery/ISA coverage, full VFP, concurrent
reservation/scheduling semantics, validated resume/cache and wider runtime
validation remain pending.

Reference Azahar `src/core/file_sys/{cia_container,ticket,title_metadata,
ncch_container,romfs_reader}.*`, `src/core/hle/service/am/am.cpp` (installation),
`src/core/loader/ncch.*`, `src/core/memory.*` and kernel process/VM code.

- CIA is an installable container, not directly executable code. Validate all
  section sizes/alignment, signature-dependent ticket/TMD body offsets, content
  bitmap/indices and hashes; select the application NCCH, not the first content
  unconditionally. Distinguish updates/DLC, absent contents and encrypted layers.
- Implement the ARM11/memory/module prerequisites above. Split NCCH ExHeader,
  ExeFS `.code` (including BLZ), RomFS, segment permissions, BSS and TLS. Preserve
  metadata needed for process setup. Fail closed for unsupported crypto/formats;
  accept user-supplied keys only, never distribute keys/firmware.
- Always materialize extracted content into `work/3ds/<title>/...`, using the
  shared NX-style host filesystem utilities. Extract RomFS safely into a tree;
  no mount-on-demand of the original CIA. Reject traversal, duplicates, malformed
  offsets, oversized/decompression-bomb inputs and symlink escapes.
- Pipeline: **extract -> recompile -> run**. Reuse stage supervision and shared
  FS/tree logic, not NX's NSO/A64 loader. Publish staged extraction/compiler
  outputs atomically. Cache identity includes input hashes, keys/configuration
  identity without storing secrets, ISA, module ABI and compiler version; never
  reuse an incomplete build. Launch only compatible native modules.
- Gate: synthetic CIA/NCCH/RomFS fixtures, malformed/truncated/overflow cases,
  memory permissions/alias/MMIO tests, ARM/Thumb native witness, resume/cache
  invalidation and an asset-free end-to-end extract/AOT/run test.

### 3. Basic HLE OS and a SwIPC-derived generator

Reference Azahar kernel/SVC/IPC code and `src/core/hle/service/`. Use
`tools/swipc/swipc.c` as the parser/schema/codegen basis, with a **separate CTR
wire adapter**: 3DS TLS command words, normal/translation counts, handles,
static/mapped buffers and response layout are not Switch HIPC/CMIF.

Initial 3a implements a strict generated subset, real session lifetimes, client
registration/lookup and FS caller/SDK association. Initial 3b adds the observed
APT startup, native threads and bounded PTM/HID resources, not a full applet or
input subsystem. The minimal FS increment reuses NX's common indexed/mapped
host backing for read-only SelfNCCH access. A synthetic native CIA reads and
resolves a real extracted file; the full stage/OS/filesystem witness below
remains unfinished. Archive timestamp control blocks the title before its first FS read.

- Threads, handles, events, waits, scheduling, SVCs, TLS, shared memory and result
  codes; checked guest buffers and handle lifetimes at every IPC boundary.
- Start with `srv:`, `fs:USER`, `APT:S`/`APT:U`, configuration/time and resource services;
  implement shared-memory/event plumbing needed by later DSP/GSP/HID stages.
- Gate: generated dispatch against hand-encoded requests, malformed descriptors,
  unknown commands, handle transfer/close, IPC-buffer bounds, blocking/wakeup,
  process exit and a small homebrew OS/filesystem witness.

### 4. Audio DSP

Reference `src/audio_core/{dsp_interface.*,hle/*}` and
`src/core/hle/service/{dsp,csnd}/`. Port HLE DSP shared memory, voices, decoding,
resampling, mixing/effects, pipes/semaphores and interrupt events; adapt output to
ScarletNX's host audio boundary. Do not substitute silence for implemented DSP.
LLE/firmware behavior needs a separately licensed/dependency-audited integration.
Gate: asset-free PCM/ADPCM decoding/mixing golden tests, pipe/event lifecycle,
ring-buffer wrap, sample-rate timing and deterministic offline rendering.

### 5. Video graphics

Reference Azahar GSP/GPU/LCD, `src/video_core/pica/`, shader and software renderer
first, then Vulkan as an acceleration path. PICA200 is **not Maxwell**.
Implement GSP shared queues/interrupts, GPU command lists, physical-memory access,
DMA/fills/display transfers, texture formats, shader behavior, framebuffer layout
and top/bottom screen presentation. Integrate renderer cache invalidation with
CPU/DSP memory writes rather than copying the NX GPU frontend.
Gate: deterministic software-rendered golden scenes, GPU command/memory bounds,
transfer/coherency tests, two-screen layouts, then Vulkan comparisons.

### 6. HID and remaining peripherals

Reference `src/core/hle/service/{hid,ir,cam,mic,nfc,ptm,mcu,qtm}/` and the Azahar
frontend/input interfaces. Wire host input to 3DS shared-memory sample rings,
pad/circle-pad/touch, accelerometer/gyro, IR/New 3DS inputs and events with correct
timestamps and screen-coordinate transforms. Camera/microphone/NFC and lid/power
policies require explicit support or documented unsupported results; never claim
that every peripheral is wired when only button input works.
Gate: injected input/touch tests, ring wrap/events, disconnect/reconnect, New/Old
model differences, camera/mic bounds and end-to-end homebrew interaction.
