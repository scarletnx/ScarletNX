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
* [Parity roadmap (Azahar)](#parity-roadmap-azahar)
* [Staged roadmap (stages 2–6, superseded by the parity roadmap)](#staged-roadmap-stages-26-superseded-by-the-parity-roadmap)
  * [2. CIA extraction, memory controller and static execution](#2-cia-extraction-memory-controller-and-static-execution)
  * [3. Basic HLE OS and a SwIPC-derived generator](#3-basic-hle-os-and-a-swipc-derived-generator)
  * [4. Audio DSP](#4-audio-dsp)
  * [5. Video graphics](#5-video-graphics)
  * [6. HID and remaining peripherals](#6-hid-and-remaining-peripherals)

## Status: stage 1 + stage 2a + stages 2b1–2b4 + initial 3a/3b/FS

`scarletnx-3ds` runs **application games** — a `.cia`, a cartridge image
(`.cci`, `.3ds`), a bare NCCH (`.cxi`, an `.app`, homebrew's `.hbb`), a bundle of
one (`.bcci`, `.bcxi`, `.bcia`), or a z3ds one (`.zcci`, `.zcxi`, `.zcia`) — through
**offline static AOT -> native module -> checked ARM/Thumb execution**,
with the compiler built into the driver. A launch reuses a finished extraction
and module and runs until the game exits or its window closes (or for `--seconds N`,
after which it reports where each thread is).

A game's folder is a folder of the data folder's `recompiled/`, named after its title
ID as a Switch game's is named (`recompiled/0004000000123400/`). It holds the compiled
module (`<title ID>_code.so`, `_code.dll` on Windows), the blocks runs compiled at
run time (`<title ID>.hints`), the game's state (`sdmc/`, `savedata/`, frames,
audio), `<title ID>.ini` and a copy of the runtime named `<title ID>`. The `.ini`
names the game (`source=`), the console (`model=`) and the system (`system=3ds`),
which the launcher and a build's deploy pick the driver by. Running the copy plays the
game, as a Switch game's copy does: `recompiled/<title ID>/<title ID> --seconds=60
--script=15000:1`, or `scarletnx-3ds --game=recompiled/<title ID>`. Where scarletnx is
also the 3DS driver (`SNX_MULTICALL`), the copy is scarletnx, which runs a 3DS game
under a 3DS title ID's name. A build copies its runtime beside every game again
(`cmake/deploy.cmake`): a 3DS title ID's folder gets the 3DS driver. `--outdir DIR`
names another folder of games, and `--output DIR` the game's folder itself. A game
an older version kept in `work/3ds/<the game's name>` moves into its new folder the
first time it is played, its module and hints renamed.

What a file holds is read from its own header, not its name: a CIA's magic at 0, an
NCSD's at 0x100, a bare NCCH's at 0x100 (`src/3ds/loader/loader.c`). A bundle is a
tar whose game is its first entry named as a game's, read where it lies in the
archive (`src/3ds/loader/tar.c`), each header held to its checksum and no entry's
data read past the archive's end. A z3ds game is Azahar's launcher's compressed one:
its frames are decompressed once into the plain container beside the title's folder
(`<folder>.unpacked.cci`, named by the input's extension without its z) and read from
then on as that container is (`src/3ds/loader/z3ds.c`), the stream held to the size
its header declares. A cartridge
image's game is its NCSD partition 0, and the whole image is read where it lies (the
loader takes a card-sized image, up to 8 GiB, since nothing of it is written). A
cartridge whose partitions carry the game's update title (the base title ID with the
update high bits 0x0004000e00000000, in partition 7 or a New 3DS's 6) is played as
that update: the update NCCH's ExHeader, code, ExeFS sections and RomFS are the
game's, as Azahar's `overlay_ncch` is, so the process the game runs in is the update
title's, as the console runs it once an update is applied, while the game's folder
stays named by the base NCCH's title ID, as the card is one game. A CIA's content
table, ticket and TMD are parsed as before;
a cartridge and a bare NCCH have none, and their NCCH is checked as a CIA's boot
content is.

An encrypted CIA, as a console dump is, is decrypted once with the user's own keys
into a copy beside the title's folder (`<folder>.decrypted.cia`, published only
complete), which is read from then on (`src/3ds/loader/decrypt.c`). The keys are an `aes_keys.txt` in Azahar's
format, looked for in `~/.3ds/aes_keys.txt` (`%USERPROFILE%\.3ds` on Windows) as
the Switch driver looks in `~/.switch`, or named with `--keys FILE`. They are
never part of ScarletNX. Both layers come off: each content's AES-CBC under the
ticket's title key (itself under the common key), and the NCCH's AES-CTR under
the primary key (slot 0x2C) and the secondary one its crypto method names (0x2C,
0x25, 0x18, 0x1B). The decrypted CIA says NoCrypto and carries the decrypted
contents' hashes, and it is checked as any game is before it is published, so keys
that are not the console's never leave a game behind. Seed crypto (seeddb) is not
supported yet. An encrypted cartridge image or bare NCCH has no CIA layer: its
NCCHs' own layer is taken off where each NCCH lies (the game partition and an update
partition both), and the whole image copied through into the copy beside the title's
folder (`<folder>.decrypted.cci`), named by the input's extension, then checked as
any image is (a bundle's copy is the bare game, read from then on by its content). A copy of a card-sized image takes its size in disk, and an
already-decrypted image is read as it is.

Code the static discovery misses (an indirect call's target, a return into it)
compiles when the dispatch first reaches it (`src/3ds/aot/jit.c`, through the ARM
backend's executor), and goes in the game's `<title ID>.hints`, which the next
recompile (`-r`) seeds discovery with, so it is static from then on. A
commercial title's 7 MiB of text compiles to about 340000 blocks in about six
and a half minutes on six cores (the chunks compile in parallel).

Since 2026-10-21 a commercial title, decrypted this way, runs its startup
(cfg, FS with its save data, APT, and the system services the runtime answers
generically), creates its save data and stops at its first prompt for the A
button, drawn through the PICA renderer. Input is the next gap.

The GPU's work runs on one Vulkan device (`src/3ds/gpu/vk/`), which needs Vulkan
1.3, 8-bit storage buffers and `VK_EXT_external_memory_host`: the board's RAM
regions are imported as buffers, so the GPU reads and writes the guest's memory
in place and nothing of it is copied by the CPU. Memory fills, display transfers
and texture copies are compute shaders (`gpu/vk/shaders/`), held byte for byte to
the CPU reference `gpu/transfer.c` by `tests/3ds_vk_transfer.c`. The Y2R engine (`y2r:u`, `services/y2r.c`), which
turns a video's YUV frames to RGB, converts on the GPU too (`gpu/vk/y2r.c`). Its streams
are the application's virtual memory, so each page goes through a table to its
physical page. `tests/3ds_vk_y2r.c` holds it to the CPU reference `gpu/y2r.c`, ported
from Azahar's. PICA draws run
on the GPU too (`gpu/vk/render.c`). The vertex program is decompiled (`gpu/vk/vsgen.c`)
and the fragment pipeline generated (`gpu/vk/fsgen.c`), both ported from Azahar's
shader generators and both written as SPIR-V words directly (`src/common/utilities/spirv_emit.c`, shared with NX), with no
shader compiler at run time. The output merger is the pipeline's fixed state. Vertex and
index data are read in place. `tests/3ds_vk_draw.c` holds the draws to the software
renderer they replaced, which stays only as that reference. The shown
framebuffers are decoded and turned to the landscape screen by a compute shader
straight from guest memory into the window's swapchain; the frame dumps are the
one readback.

What the GPU makes stays on it, and guest memory catches up only when something
reads it. A render target's images are kept, and loaded from guest memory by a compute
shader only when memory changed under them. The pages a draw makes newer stay the
GPU's until the target is stored, on the GPU, for a GPU engine that reads that memory
(a display transfer, a texture over it, the presenter), or for the CPU when it touches
them. Textures are decoded once and kept while their memory is unchanged. Work goes to
the GPU in submissions that nothing waits for, a few in flight at once
(`gpu/vk/device.c`), and each GX queue run's interrupts are raised when its work is
submitted. Not when it is done, as the console raises them: the DSP and the vblanks keep
the host's time, so a GPU slower than the console's would leave a frame without its
audio.

Write tracking keeps the guest from seeing any of this unfinished
(`snx_ctr_track_*` in `memory/ctr_memory.h`). Each physical page has a level:
the CPU's; watched, when the GPU keeps something made from it or a submission not done
reads it, so a write traps; or the GPU's, when a target there is newer or a submission
not done writes it, so every access traps. Every guest view of the page is protected
to its level, and so are the memory API's accesses and a bus master's by physical
address. A trap is resolved by the renderer, on the faulting thread:
- the GPU's page has its targets stored and its writes waited for, and is watched
  after;
- a watched page has the GPU's reads of it waited for and what was made of it dropped,
  and is the CPU's after.

A guest fault (`memory/host.h`) is taken by SIGSEGV or SIGBUS on Linux, FreeBSD and the
other POSIX hosts, on an alternate stack large enough to wait for the GPU, and by a
vectored exception handler on Windows. `tests/3ds_memory_track.c` checks the levels
through the flat space and the API with a stand-in renderer.

Compiled code addresses the guest flat: a 4 GiB host reservation where guest
address `va` is `base + va`, each mapping a view of the board's RAM held in one
host memory object (`src/common/memory/host.c`, the GBA window's too: a memfd on
Linux and Android, an unlinked `shm_open` object on the other POSIX systems, a
pagefile section mapped into placeholders on Windows 10 1803 and later). Every guest address of a physical
page (a heap page and its MAP mirror, shared memory, the linear heap) is the same
memory, and the HLE and the GPU read and write that object directly. Blocks read and
write `cpu->mem + va` with no callback; a host fault in the reservation is the
guest's data abort, reported with the block it happened in. A host whose pages are
not 4 KiB (Apple silicon, some AArch64 and PowerPC64 kernels) keeps the checked
bus, and its modules are compiled for it.

The cores take turns on the one host thread, each scheduling its own threads (a
slice ends at an SVC or after `SNX_CTR_POLL_BLOCKS` blocks), so a busy thread on the
system core leaves the application core's threads their time, as on the console.
TLS pages come from the BASE region, outside the application's pool, as Azahar
allocates them: a game that commits all of its pool still starts threads.

Debug switches are settings of `g_ctr_debug_ini` in `src/nx/runtime/settings.txt`,
given as `--key=value`: `--ctr_ipc_trace=1` logs each service request with the
thread and its answer, `--ctr_svc_trace=1` each SVC that fails (2: every SVC),
`--ctr_gpu_trace=1` each GX command and framebuffer swap, `--ctr_jit_trace=1` each
block compiled at run time. A run ended by `--seconds` (or a stop) reports each
thread's core, priority, pc, what it waits on (whose event it is) and the return
addresses on its stack, how long the process sat idle, and its most frequent SVCs.

Since 2026-10-16, SuperTux runs past `main` and its RomFS reads. It draws its title and loading
screens through a software port of Azahar's PICA pipeline (command processor,
shader interpreter, rasterizer, texturing, combiners, lighting) and plays its
title music through a port of Azahar's HLE DSP. A run of about 2 billion native
blocks presented frames until it was ended. It is **not playable** yet:

- Input is the pad alone: the touch screen is not read, and the sensors are at
  rest (`hid:USER`, below).
- The music has a gap after every rendered frame, because rendering and presenting
  run inside the guest's GSP call.
- Geometry shaders, procedural textures, shadows and the DSP's AAC pipe still stop
  the run explicitly.

The [parity roadmap](#parity-roadmap-azahar) lists the remaining work, and the
[SuperTux integration target](3DS-SUPERTUX.md) holds the pinned input.

Text is bounded at **16 MiB**, using conservative static CFG discovery for larger
images. This is not complete indirect-target coverage or a general 3DS runtime.
Scalar VFPv2 now supports F32/F64 arithmetic/conversions and bounded atomic RAM
transfers; short vectors, enabled exceptions and legacy extended-save forms
remain unsupported. Stage 2b4 adds serialized ARM11 checked exclusives and
a bounded kernel-bootstrap subset. Stage 2 still needs broader discovery/ISA
validation and cache/resume. Initial stage 3a adds a SwIPC-derived CTR generator,
checked transport and bounded service sessions. Initial 3b adds the minimal
APT/thread/event/mutex/PTM/HID startup personality and ARM11 UADD8/SEL.
The HLE DSP, the software PICA renderer and the F1 kernel work since then are
described below. Full HLE/IPC, input and the other peripherals remain pending. No
firmware, keys, title assets or Azahar CPU engine are included.

Implemented:

- Stage 1: `SNX_BUILD_3DS`, independent driver, Old/New 3DS board profiles,
  physical-range bounds checks, lifecycle/base ticks, GUI CIA drops/browser/
  recents and strict routing to the 3DS driver without Switch-only flags.
- `src/3ds/loader/`: the container by the file's own header (a CIA, an NCSD
  cartridge image, a bare NCCH); streaming CIA sections; signature-dependent
  ticket/TMD offsets; title, boot-index, content bitmap, unique ID/index,
  content-info and SHA-256 validation. Boot content is selected by its TMD index,
  not table order. All present contents and container metadata are materialized on
  disk. A cartridge image's game is its NCSD partition 0, its update taken by title
  ID from the later partitions, both read where they lie (up to a card's 8 GiB);
  a bare NCCH fills the file. A proto image (its content size in bytes, as an early
  tool's is) is read as Azahar's `is_proto` reads it, with the usual priority given
  to an application whose ExHeader names none.
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
  The link is the LLVM backend's own (`src/common/cpu/recompiler/llvm/link.c`),
  with the helpers the blocks call (`armlib.c`), as the Switch's 32-bit modules
  are: an ELF shared object, a PE DLL or a Mach-O dylib, without a C compiler.
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
  run, and dispatch alone never completes a pending call. Since F1 waits can have
  finite deadlines that time out with `0x09401BFE`, and timers exist (SVC
  `0x1a`–`0x1d`, one-shot, sticky and pulse).
- SVC `0x23` closes handles and `0x27` duplicates them. SVC `0x2a` (GetSystemInfo)
  answers type 0 with the application's allocated FCRAM, type 26 with the kernel's
  process count, and Luma's type `0x10000` queries for the application region's size
  and the model. Other queries answer zero, as Azahar does for the ones it lacks.
  ExitProcess (`0x03`) and ExitThread (`0x09`) end the process, the latter at its
  last thread.
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
  rolled back. MAP and UNMAP alias the application's own heap (an UNMAP that does
  not match a MAP returns `0xD8E007F5`), the region bits of the operation are
  ignored as the application's region is the only one, and executable heap
  permissions are refused. FREE of a range a memory block or a mapping holds is
  refused. This is not full ControlMemory behavior for static image/shared mappings.
- ARM11 checked LDREX/STREX B/H/word/double and CLREX use optional checked-ABI-1
  reservation transactions. The bus owns a saturating board-wide write/mapping
  epoch, so kernel writes, aliases and other address spaces invalidate reservations
  even after an ABA write. Unrelated writes may fail exclusives conservatively.
  MMIO is rejected. All execution and mutations remain serialized, with no host
  concurrent/multicore scheduler claim. A57 defaults and both CPU ABIs are unchanged.
- The scheduler (F1) runs the ready thread with the best effective priority. A
  thread waiting on a mutex lends its priority to the holder, as Azahar's
  UpdatePriority does. On a tie the running thread keeps the core unless it
  yielded (SleepThread 0), and otherwise the thread ready longest runs. Waiters
  are woken by effective priority, then by how long they have waited. Threads
  start with FPSCR `0x03C00000` (default NaN, flush to zero, round toward zero)
  and the main thread with `0x03C00010`, as Azahar sets them. The main thread's
  priority comes from the ExHeader. CreateThread accepts processors -2 to 3, all
  run by the same serialized scheduler.
- The APPLICATION resource limit is enforced: 32 threads, events, mutexes, 8
  semaphores and timers, 16 shared memory blocks and 2 address arbiters, each
  failing with its own OutOf result when exhausted.
- QueryMemory (SVC `0x02`) returns the base, size, permissions and the memory
  state of the page's region in r1 to r4 (Free, IO, Code, Private, Shared,
  Continuous, Aliased, Alias, Locked), as Azahar's VMManager classifies them.

### Initial CTR IPC/services (3a)

SVC `0x2d` connects to the actual `srv:` endpoint; `0x32` dispatches checked
TLS command words; `0x35` identifies the actual process root. A separate SwIPC
`--ctr` mode emits typed handlers without HIPC/CMIF or successful default stubs.
`srv:` supports client registration and service lookup with real session
ownership. Initial 3b adds a one-application APT startup lifecycle, real recursive
lock/events, serialized thread contexts/waits/wakeups, PTM base/model queries
and HID handles/read-only mapping. HID sampling is not implemented. FS supports
PID/SDK initialization, read-only SelfNCCH RomFS OpenFileDirectly, and the
read-write SDMC archive (9) on the title's folder: OpenArchive, OpenFile,
CloseArchive, CreateFile, DeleteFile, CreateDirectory, DeleteDirectory, Rename,
OpenDirectory, and FSFILE Read, Write, GetSize, SetSize and Close. Archive tokens
are FS-private monotonic 64-bit identifiers, not kernel handles. 32 live bindings
are allowed and stale IDs never revive. NX and CTR share `common/filesystem/`
indexing, immutable file mappings and fallback opens, not their IPC protocols or
RomFS metadata. Only extracted `romfs/` is exposed, with no original container reads.
Save data (4) and ExtSaveData (6, in `sdmc/Nintendo 3DS/<id0>/<id1>/extdata/`, and the
shared 7, in `nand/data/<id0>/extdata/`, as the console lays them out) are on the
title's folder too. SelfNCCH also serves the
ExeFS icon, logo and banner. The NCCH archive (0x2345678A) serves the title's own
RomFS, and in place of the system titles no console here has, the open stand-ins
Azahar carries (`cmake/ctr_system_archives.cmake`, generated by
B3n30/citra_system_archives): the Mii data, the country list, the shared font
(also for the Chinese, Korean and Taiwanese fonts) and the bad word list. Another
system title is NotFound. Directory sessions read their entries, and FSFILE has
Flush, priorities and OpenLinkFile. Reads preflight
the complete guest destination and stage host IO before publication.
Host-paced GetSystemTick and the shared page's date and time (the host's local
time, rewritten every hour into the entry the counter does not select, as
Azahar does) serve time queries, without cycle accuracy. FSUSER ControlArchive
GET_TIMESTAMP (`0x80d`) resolves a bounded UTF-16 archive path and returns the real
backing file's host modification time as a 3DS datetime (milliseconds since 1900
UTC). Missing paths return `0xc8804478` and stale tokens `0xc8804465`.
COMMIT_SAVE_DATA on the SDMC area succeeds, since every write has already reached
the host, and other actions stop. Unknown commands answer success with one word, as
Azahar's ReportUnimplementedFunction does and a Switch game's do (`auto_stub_ipc`),
each logged the first time. A service the runtime does not know at all is a stand-in
whose commands answer so, as a Switch game's is (`sm.c`). With
`--ctr_auto_stub_ipc=0` both stop instead: an unknown command stops the run unless
`--ipc-stubs` names its service, and a service the runtime lacks makes the thread
asking for it wait forever, as no other process can register it, with a message
that names it.

`hid:USER` and `hid:SPVR` (`services/hid.c`, ported from Azahar's `hid.cpp`) write
the pad's samples into HID's shared memory 234 times a second from the first
GetIPCHandles, each with both pad events signalled, and the accelerometer's (104 a
second) and the gyroscope's (101) between their enable and disable. Each pad entry
has the buttons, what was pressed and released since the one before, and the circle
pad's position, averaged over three samples, with its direction bits. The input
(`src/3ds/input/input.c`) is a Switch game's: X or Space A, Z B, C X, V Y, Q L, E R,
Enter Start, Backspace Select, the arrows the D-pad and WASD the circle pad, or a
gamepad's buttons by position (`gamepad_swap_ab`) with its left stick, and the
scripted presses of `--script=ms:hex,...` (`ctr_script`), in the pad's bits (1 A,
2 B, 4 Select, 8 Start, 10 Right, 20 Left, 40 Up, 80 Down, 100 R, 200 L, 400 X,
800 Y), each held for `press_len` ms. The touch entries say no touch, and the
sensors give a console lying still. ZL, ZR and the C-stick are the New 3DS's
`ir:rst`, which the runtime does not have yet.

`gsp::Gpu` is modeled with real bounded resources: RegisterInterruptRelayQueue
returns the genuine `0x2a07` first-initialization result, one writable 0x1000
GX/interrupt shared block and up to four relay threads with real interrupt-queue
slots and guest-created events. AcquireRight/TryAcquireRight/ReleaseRight own a
single right, and WriteHWRegs, WriteHWRegsWithMask and SetBufferSwap fail with
`0xD8202A06` without it, as in Azahar. WriteHWRegs and WithMask take the forms
libctru sends (data in static buffer 0, mask in 1) and check them as Azahar does:
an address below `0x420000`, at most `0x80` bytes in whole words. No register
they could change is modeled (framebuffers come from SetBufferSwap and the shared
FrameBufferUpdate blocks, transfers from GX commands), so the values are not kept.
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
start to end rounded down to 8 bytes, the end being an open bound that may be its
memory region's end. Each command raises its own interrupt (DMA, PSC0 or
PSC1, PPF) on the right holder's relay queue, a failing command stores its error and the
FAILED status as GSP does, and the queue goes on. SubmitCmdList writes the list's
address and size to the first command buffer channel and runs it on the PICA command
processor (`src/3ds/gpu/pica.c`, ported from Azahar's `PicaCore`): masked register
writes, batched and sequential extra data, shader program, swizzle and uniform uploads,
the lighting, fog and procedural-texture LUTs, default attributes and immediate mode,
command buffer jumps and the P3D interrupt request with autostop. A list outside memory
raises P3D without running. Draws go to the GPU (see above). The software renderer
ported from Azahar's (`gpu/shader.c`, `gpu/draw.c`, `gpu/raster.c`, `gpu/lighting.c`,
`gpu/texture.c`) is only built into the tests, as the reference the GPU's draws are
compared with. It samples the nearest texel only, as Azahar's does, so texture
filtering has no reference. Geometry shaders and shadow textures and rendering are not
drawn yet: a draw that needs one stops the run and names it. The ExHeader's address-mapping capabilities
are applied at process creation (VRAM, DSP RAM, New 3DS extra RAM), as Azahar's
`HandleSpecialMapping` does; MMIO areas stay unmapped. SetBufferSwap and the shared
FrameBufferUpdate blocks latch real framebuffer descriptors in the five formats
the presenter converts (RGBA8, RGB8, RGB565, RGB5A1, RGBA4). A swap through
SetBufferSwap in another format stops rather than being shown with invented
colors. The LCD shows the left eye of the pair `shown_fb` (one bit) selects. A
swap whose shown pair has no address, a stride too short for a row or memory the
guest cannot read is dropped and the screen keeps its last frame. Display timing
is host-paced 60 Hz: latched frames present at vblank waits and overdue swaps, and
waits on the relay event complete only at real vblank deliveries. MapMemoryBlock
maps a block another process owns (an HLE service's) into the application's
range, or an application's own block as a mirror within the permissions it was
created with. Each mapping holds a reference to the block. HLE shared pages are
reserved from board FCRAM past the application pool.

Presentation is **Vulkan only** (volk, with no linked Vulkan library):
the device converts the guest framebuffers to RGBA8 and shows the landscape-
rotated screens live in an SDL3 Vulkan window through a real swapchain, the top
screen above the bottom one as on the console, the bottom one centred under it.
The pair keeps its shape in a window of any size. Bounded
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

See [CTR IPC](CTR-IPC.md) for the strict generator subset, descriptor/permission
checks, quotas, malformed-request policy, tests and unimplemented operations.
Unknown SVCs still stop explicitly. No service or device success is fabricated,
except for unknown commands and services, which answer success as a Switch game's
do unless `--ctr_auto_stub_ipc=0` asks for the console's answers.

By default nothing of the game is extracted. `scarletnx-3ds game` maps the game read-only and checks it as the
extraction would (a CIA's content table and its every content's SHA-256, the NCCH and ExHeader hashes, each ExeFS file's
hash, the RomFS superblock and its metadata). The game's ExeFS files and its RomFS (level 3, the image the game reads)
are then read where they lie in the mapping, and only the decompressed `.code` is copied into memory. The game's
folder keeps its module, its copy of the runtime and its state alone: `sdmc/` and the frames and audio it writes. An extraction an earlier
`--extract-container` left there is taken away, its compiled module and saves kept. The image's identity is the same
either way, so a module compiled in one mode loads in the other. The manifest records which container the game came
from (`container=`, and `overlay=1` when it is a cartridge's update), which the image loader ignores.

`--extract-container` (the launcher's *Extract dump*) extracts the game into its folder first and plays from
the extraction, as before. `-x` extracts only:

```sh
build/3ds/scarletnx-3ds -x --output work/my-title game.cia
```

The output is a directory containing `contents/*.app`, `exefs/` (with
expanded/page-padded `.code`), `romfs/`, raw container/NCCH/ExHeader metadata and
`manifest.ini` with source/code hashes and segment layout (a CIA's `contents/` and
container metadata; a cartridge image's, a bare NCCH's and a bundle's without them,
a bundle's game read from its entry). The default directory
is the game's folder, `recompiled/<title ID>`, in the data folder the Switch driver uses
(`--data DIR`, else a portable copy's folder, else the user's data folder, from a
build tree too). On Linux the user's data folder keeps `work/` (the decrypted CIAs)
in `~/.cache/scarletnx/work/3ds/`. No source game
needs to be mounted by the future guest FS. Extraction is privately staged, then
published only after all checks succeed, and a failure removes the staging files.
As in the Switch driver, a folder already at the output path is taken as a
finished extraction and used as it is, never extracted again or hashed. Keep the input and output parent trusted and
not concurrently modified; extraction is not a sandbox against hostile host
processes racing parent directory changes.

`-x`/`--extract` stops after extraction and `-r`/`--recompile` compiles without
playing, as the Switch driver's do. Otherwise the driver loads
`<title ID>_code.so` and compiles it first only when needed: when it is
missing, or no longer loads or matches the build's module ABI or the extracted
image's identity (a changed `.code`). `-r` always compiles again. A new module
replaces the old one only once it is complete, by a rename over it; the objects
it was linked from stay in memory. A failed compile leaves the old module and the
extracted tree. Once the module is ready the driver copies itself into the folder as the
game's runtime, unless the copy there is the same program. The GUI uses this same
driver path, not a separate engine.

A launch runs until the game exits or its window is closed, as a Switch game does.
`--max-blocks N` stops it after N native blocks instead. A larger-image block may
execute up to 32 instructions, so this is not a cycle or exact instruction budget.
Each instruction still checks fetch, identity and permissions.
`--model old|new` selects RAM backing (default Old), not a different ISA or
compiler. `--board-info` prints physical regions without allocating RAM.
`--ipc-stubs L`, with `--ctr_auto_stub_ipc=0`, still answers the unknown commands of
the services in L (`all`, or a comma list of `srv`, `fs`, `apt`, `ptm`, `hid`, `gsp`,
`cfg`, `dsp`, `system`, `am`) with success instead of stopping. Given more than once,
the lists add up. It only changes a
run, so it is refused with `--board-info`, `-x` and `-r`.
`Guest ExitProcess: r0=...` prints a register diagnostic, **not a POSIX guest exit
status**. When the last thread ends with ExitThread the driver prints
`Guest's last thread exited` instead. Exit codes:

- 0: help/info, extract-only or compile-only (`-r`) success, a guest that exited
  or a window that was closed.
- 1: malformed data, IO or limit failure, ABI mismatch or guest fault.
- 2: invalid CLI arguments or a missing/non-game file path.
- 3: unsupported format, ISA or SVC, a failed compile, the text-size cap, a
  missing static target, an exhausted block budget or a guest with no thread that
  can ever run again.

### Current limits and trust

Only application content is supported: a CIA (encrypted ones decrypted with the
user's keys as above, without seed crypto), a cartridge image's game partition and
update partition, and a bare NCCH — not a cartridge's manual or DLP partitions as
content of their own, DLC, system titles or nonstandard NCCH media units/platforms. A
cartridge image may be as large as a card (8 GiB), since it is read where it lies; a
CIA's input, the total materialized output and a RomFS view are each capped at 4 GiB
(the runtime's policy, not a format limit); metadata tables at 16 MiB,
code at 64 MiB, entries at 100000, and RomFS nesting at 64. Host filenames must
be portable safe components (maximum 255 UTF-8 bytes); unsafe/reserved names
are rejected rather than silently renamed. Host paths with control characters
are refused to prevent log/banner injection. Larger/unsupported inputs fail closed.

**Hashes are integrity checks, not signature authentication.** RSA/ECDSA
signatures, certificate trust, ticket rights and ExHeader access-descriptor
signatures are not authenticated. The headers' and superblocks' hashes (NCCH
ExHeader, ExeFS and RomFS superblocks, the TMD's tables) are checked every time.
The whole-data ones (each CIA content, each ExeFS file, the input itself) are checked
by an extraction only, not by the launch that reads a game where it lies, where they
cost seconds on a retail title; an extraction to a folder given hashes no input. The
internal IVFC hash tree is not separately verified. The NCSD header's RSA signature
is read and ignored, as a CIA's sections' are.
AOT execution is **not a host security sandbox**. Run only images you trust:
compiler correctness is not isolation, and native loading can execute host
constructors before descriptor validation. A launch loads the `<title ID>_code.so` an
earlier compile published in the game's folder when it still matches the build's
module ABI and the image's identity, so the extraction folder must be as trusted
as the program itself. Do not treat ABI/hash checks, host native modules or an
editable extraction manifest as authenticated permissions.

The image loader derives layout from raw ExHeader and requires exact page-packed
expanded `.code`; it ignores `manifest.ini` for execution. Its compatibility hash
covers a versioned domain, full ExHeader and full expanded code. It is not yet a
complete cache key (CIA/resources, compiler/build/configuration versions and
resume policy still need a separate design). Initial AOT also requires a
word-multiple declared text length. Text-page padding beyond declared text has
no static entry, even if its page is executable: it compiles at run time, as any
code the static graph lacks does. The static graph is bounded by the text itself
(each instruction slot is visited once in each instruction set). Objects together
are capped at 1 GiB and the linked module at 1 GiB, and only the module is kept in
`native/`. Tiny-image code uses O2, and larger-image chunks use O1, with 512
entries per LLVM module, compiled by a worker per host core.

### Build and test

`scarletnx-3ds` holds the AOT compiler, so it links LLVM, and its presenter and
audio output use SDL3 and Vulkan headers, which CMake fetches as for the Switch
runtime. With the normal Clang, CMake and LLVM SDK prerequisites:

```sh
cmake -S . -B build/3ds -DSNX_BUILD_NX=OFF -DSNX_BUILD_GNUABI=OFF -DSNX_BUILD_GUI=OFF
cmake --build build/3ds
ctest --test-dir build/3ds --output-on-failure
build/3ds/scarletnx-3ds --model new --board-info
build/3ds/scarletnx-3ds --output work/small-title --max-blocks 100000 small.cia
```

The 3DS tests are named `3ds_*` and `ctr_*` (`ctest -R "3ds|ctr"`, `3ds_cci` the
cartridge images' and bare NCCHs'), and the ARM
CPU tests the 3DS shares with the Switch's 32-bit games `arm_*`. Use `-DSNX_BUILD_GUI=ON` to include the launcher and its
regression tests. `-DSNX_BUILD_3DS=OFF` omits the driver with its compiler and
runtime (the GUI diagnoses its absence). The profile decoder test needs no LLVM.
Additional static AOT tests are explicitly opt-in:

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
The compiler links its objects with the LLVM backend's linker, so a game compiles
on a machine without a C compiler. The compiled modules need no LLVM library. On Windows `scarletnx-3ds` carries the runtime's
application manifest (long paths, UTF-8 code page), and the module is a DLL linked
with DEP and ASLR compatibility, exporting only its descriptor.
Add `SNX_TEST_ARM_AOT=ON` for the CPU tests as well as the pipeline's.
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
and the archive timestamp request answered with success, then runs a fixed number
of blocks more one at a time and a few of the driver's polled slices.
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
The z3ds support (`src/3ds/loader/z3ds.c`) reads Azahar's `Z3DSWriteIOFile` and
`Z3DSReadIOFile` (`common/file_derived.{h,cpp}`, GPL-3.0-or-later) for the container's
header and its zstd seekable stream, whose frames a streaming decompressor takes in
order and whose seek-table footer is the skippable frame it skips; the unpacker is new
C code over zstd's own streaming API.
The bundle support (`src/3ds/loader/tar.c`) reads Azahar's `TarArchive`
(`common/tar_file.cpp`, GPL-3.0-or-later) for what a bundle is taken to be: a ustar
archive, the game its first entry named as a game's, hidden and non-file entries
skipped, and duplicate names refused; the reader is new C code over the POSIX
header layout.
The cartridge-image support (`src/3ds/loader/ncsd.c`, the container
dispatch, the update overlay) reads `ncch_container.cpp`'s `AutoOpenNCCHNCSD`,
`LoadHeader` and `is_proto`, and `loader/ncch.cpp`'s `OpenUpdateNCCH`,
`UPDATE_TID_HIGH` and `overlay_ncch` at revision
`68202bc4ff7edf9d7280ec0dcd6572b7cd1ae26e` (GPL-3.0-or-later) for the NCSD
partition layout and the base/update semantics; the C implementation is new, as
the CIA one is. Initial 2b CPU/profile and bus code is new project C code using ARMv6K/legacy
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
The parity roadmap's F1 and its audit read Azahar at `987f18405073960622e1043bb44005989135c6e1`
(master on 2026-10-17) for kernel and service semantics, without porting code:
`core/hle/kernel/{svc,thread,timer,mutex,semaphore,address_arbiter,resource_limit,shared_memory,shared_page,process,vm_manager}.cpp`
and `errors.h` (GPL-2.0-or-later, except `svc.cpp`, which is GPL-3.0-or-later at that revision),
`service/service.cpp`'s ReportUnimplementedFunction and `service/gsp/gsp_gpu.cpp` (GPL-3.0-or-later) for the
GPU right and the register writes. SVC register layouts and IPC descriptor forms are checked against libctru's
`source/svc.s`, `include/3ds/ipc.h` and `source/services/gspgpu.c` (zlib).
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
in Azahar. New 3DS CPU scaling is a separate future scheduler concern. Interrupts
are delivered at the scheduler's deadlines (vblank, DSP frames, timers), paced by
the host clock rather than by guest cycles. There is no ARM9 emulation or boot ROM. Synthetic CIAs and
the supplied SuperTux startup execute natively, not complete games/retail titles.

## Parity roadmap (Azahar)

The goal is general parity with Azahar, area by area, on Linux x86-64 and Windows.
The software renderer is finished first and a Vulkan one follows. Each phase names the Azahar
files it ports or reads. Each ported file keeps its notice, and its origin and revision are
recorded under [Source provenance and licensing](#source-provenance-and-licensing).

| Phase | Work | Azahar references |
| --- | --- | --- |
| F0 Base (done 2026-10-17) | The 3DS driver builds, compiles and runs SuperTux on Windows. The module links by file name in its staging folder (a full-path command line passed Windows' 32767 characters), without ELF's `-z` options, and exports only its descriptor (COFF has no hidden visibility). The GSP, transfer, PICA, DSP and SuperTux tests run there too. The SuperTux baseline is in [its page](3DS-SUPERTUX.md). GSP shows the left eye of the pair `shown_fb` selects and takes RGB5A1 and RGBA4 framebuffers. These docs were brought up to date | `video_core/gpu.cpp`, `service/gsp/gsp_gpu.cpp` |
| F1 Kernel and time (done 2026-10-17, audited 2026-10-19) | The scheduler delivers every deadline (vblank, DSP frame, timers, wait timeouts, sleeps, the shared page's hourly time). The dispatcher reaches it every 4096 blocks, so a thread a deadline wakes with a better priority preempts one that makes no SVC. Priorities are effective ones, with mutex priority inheritance. WaitSynchronization1 and N and the arbiter take finite timeouts. SleepThread 0 yields and -1 sleeps forever. ExitThread ends the process at its last thread. Timers and pulse events exist. CreateThread runs threads on processors -2 to 3 within the APPLICATION limit's priority, and the resource limit's counts are enforced. Also implemented: GetHandleInfo, GetSystemInfo (Luma's queries too), GetProcessInfo, GetThreadInfo, GetProcessIdOfThread, the resource limit SVCs, Break, the cache SVCs and Luma's 0x93 and 0x94, QueryMemory with the console's register layout and memory states, CreateMemoryBlock, UnmapMemoryBlock, MapMemoryBlock of the application's own blocks and ControlMemory MAP and UNMAP. Any well-formed IPC descriptor is translated, buffers are checked against the caller's memory and moved handles are closed. `--ipc-stubs` answers the unknown commands of the services it names. The shared page has its retail, battery, Wi-Fi and hourly local time fields. Guest time stays host time. Subtracting slow HLE work from it waits for the GPU thread (F2), since the audio device plays in host time | `core_timing.*`, `kernel/{svc,thread,timer,mutex,semaphore,address_arbiter,resource_limit,shared_memory,process,shared_page,vm_manager,hle_ipc}.cpp`, `service/service.cpp`, `service/gsp/gsp_gpu.cpp` |
| F2 SuperTux playable | Presenter without readback or queue waits, on its own thread; both screens and the Default, Single, Large, Side-by-side and Hybrid layouts; keyboard, SDL3 gamepad, touch and motion input; full `hid:USER`/`hid:SPVR` rings and `ir:rst`; GX work on a GPU thread with interrupts delivered by the poll; banded raster threads; audio prebuffer and hold-last-frame | `frontend/framebuffer_layout.cpp`, `frontend/emu_window.cpp`, `service/hid/hid.*`, `service/ir/ir_rst.*`, `renderer_software/sw_rasterizer.cpp`, `audio_core/dsp_interface.cpp` |
| F3 OS for retail titles | cfg blocks; APT:U/APT:A and the `aptMainLoop` lifecycle; shared font; HLE applets (swkbd first); FS save data, extdata, system save data, the NCCH archive and the SDMC write-only archive; ptm:u, ac:u and registered stubs for the small services; err:f | `service/{cfg,apt,fs,ptm,ac,err}/`, `hle/applets/`, `file_sys/archive_*` |
| F4 Software GPU | Geometry shaders, procedural textures, shadow textures and rendering, VRAM save and restore, the stereo right eye | `pica/geometry_pipeline.cpp`, `pica/shader_unit.cpp`, `renderer_software/{sw_proctex,sw_framebuffer,sw_rasterizer}.cpp` |
| F5 Dynamic code | ldr:ro CROs compiled ahead of time, with a design for their run-time base and relocations; a log of missed targets for a second discovery pass; an explicit result for self-modifying code | `service/ldr_ro/` |
| F6 Audio | The DSP's AAC pipe on FFmpeg; CSND; time stretching if gaps remain | `audio_core/hle/aac_decoder.cpp`, `audio_core/csnd.cpp`, `service/csnd/`, `audio_core/time_stretch.cpp` |
| F7 Peripherals and network | y2r, mic, cam (no camera), ir:USER, soc:U, then ssl and http | `service/{cam,mic,ir,soc,ssl,http}/`, `hw/y2r.cpp` |
| F8 Loader | Encrypted content with the user's keys; CCI, CXI, 3DSX and ELF; updates and DLC; a complete module cache key | `file_sys/`, `loader/` |
| F9 Vulkan renderer | A Vulkan device shared with NX; the PICA fragment shader generator on `spirv_emit.c`; the surface cache with page-level coherence in the memory controller; host vertex and geometry shaders. The software renderer stays as the reference and fallback | `renderer_vulkan/`, `rasterizer_cache/`, `shader/generator/` |
| F10 Performance | Shader JIT if profiles call for it; the syscore CPU limiter; GUI settings for layouts and controls | `shader/shader_jit_*`, `core.cpp` |

F3 and F4 can run in parallel after F2. Every phase is checked by asset-free tests
(`ctest -R "3ds|ctr"`) and by a run of the title it affects.

### Gaps the F0/F1 audit left to later phases

The audit of 2026-10-19 fixed what F0 and F1 introduced. It also found places where
code older than F0 differs from Azahar or from the console. Each belongs to the
phase that reworks its area:

- F2 (gsp::Gpu, with the GPU thread):
  - FlushDataCache and InvalidateDataCache take only the process pseudo-handle, and
    do not check the range as Azahar's IsValidCacheRange does (`0xE0E02BF5`). DSP's
    FlushDataCache and InvalidateDCache have the same limits.
  - RegisterInterruptRelayQueue returns the shared memory as a moved handle where
    Azahar copies it. It does not reset the queue header, a second registration
    keeps the old event, and a failure while adding the shared handle leaves the
    registration's side effects.
  - The GPU right: AcquireRight takes it from its holder and TryAcquireRight acts
    as AcquireRight with that command's wire form, where Azahar blocks or fails busy
    (`0xC8402BF0`). ReleaseRight from a session that does not hold it fails, where
    Azahar succeeds. The flags check of AcquireRight is invented, and granting the
    right does not clear the holder's FrameBufferUpdate flags.
  - The GSP thread index is assigned at registration rather than when the session
    connects, so a right holder that never registered stops TriggerCmdReqQueue.
  - Relay events are signaled when nothing was queued, and interrupts are queued
    for a holder with no event. The latch walks every registered thread rather
    than only the right holder's.
  - SetLcdForceBlack invents an error above 1, and the presenter ignores it.
  - `relay_flags` and `frame_seq` are written and never read.
  - A memory fill with its start at or past its end fails without raising PSC.
    Azahar may still signal it, which needs checking.
  - Framebuffers are kept as virtual addresses. Azahar translates them to physical
    ones, so a framebuffer outside the linear heap may show differently.
  - The session limits per service are invented (Azahar's default is 10, and GSP's
    must not exceed its four threads).
  - Without a Vulkan device the whole GSP test is skipped, not only its
    presentation checks, and it keeps two inline copies of `frame_pixel`.
- F3 (kernel and IPC, with the services retail titles use):
  - Every reply writes back all 64 command words, where the console copies only
    the reply's length (to be checked on hardware).
  - GetProcessInfo type 20 always gives the old linear heap's offset. Azahar picks
    `0x30000000` for kernels from `0x22C` by the ExHeader's descriptor, while the
    configuration page reports `0x23A`.
  - A wait-all that succeeds at once writes -1 to r1 where Azahar leaves it, and a
    timed arbiter wait that is signaled leaves the timeout result in r0 in Azahar.
    What the console does in both cases is unknown.
- F6 (audio): ReadPipe consumes the pipe's data before it checks that the receive
  buffer is writable and uses a 64 KiB stack buffer per call, `semaphore_mask` is
  kept but never read, `SNX_CTR_DSP_RAM` is both a macro and an enumerator, and
  ReadPipe (`0x0e`) and Flush/Invalidate with a real process handle are untested.
- F8 (loader and module cache):
  - No default test covers the Windows link and export, and the static pipeline
    test parses only ELF modules, not the Windows DLL.
  - The staging folder's ACL, meant for keys, may become `native/`'s in an
    elevated run (to be checked).

## Staged roadmap (stages 2–6, superseded by the parity roadmap)

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
fallthrough pass `gettimeofday`, and the archive timestamp control was answered
later, before the parity roadmap took over.
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
remains unfinished.

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

### Experimental local wireless over rooms

`nwm::UDS` supports two ScarletNX instances using an explicitly selected Eden-compatible
room in Network settings. Enable Room wireless on both players and choose the same room.
The selected profile supplies the console username, limited to the 3DS's ten characters.

The backend implements discovery, host/join, node status, channel bindings, packet send/pull
and disconnect. It uses a private protocol inside the room's datagram transport. It does
not connect to physical consoles, Citra/Azahar rooms or Switch LDN sessions. LAN mode is
not implemented by this backend and does not silently join a public room.

No game image or synchronization window is patched. NSMB2 internet co-op still has
input-delay and synchronization limits. The development runs with title-specific
buffering experiments do not establish stock-game WAN compatibility for this backend.

Synthetic IPC tests cover discovery, a duplicate join, packet bounds, FIFO bursts and
disconnect through a local relay and a relay with 120 ms delay in each forwarding leg.
They also check that discovery beacons omit the passphrase hash, a beacon-only join
is rejected, and pending joins leave other guest threads, display and DSP scheduling
running. Both connect commands finish asynchronously; timeout, CLOSE before SYNC,
disconnect, shutdown and caller exit exercise their cleanup and reply ownership.
`ldn_room` covers a 512-packet socket burst and the 4 MiB low-latency queue bound.
Optional `snx-test-3ds-uds --public` uses `SNX_TEST_UDS_ROOM=host:port` rather than a
hardcoded public server. No game data is needed for these tests.
