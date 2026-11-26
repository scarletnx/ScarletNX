<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# PS Vita driver

## Index

* [Layers](#layers)
* [How it fits together](#how-it-fits-together)
* [The system's functions by NID](#the-systems-functions-by-nid)
* [The shader corpus](#the-shader-corpus)
* [Code compiled at run time](#code-compiled-at-run-time)
* [Save states](#save-states)
* [Checking the CPU backend](#checking-the-cpu-backend)
* [Limits](#limits)
* [Sources](#sources)

`scarletnx-vita` runs a homebrew VPK the way the other drivers run their games. The VPK is extracted once into the
data folder's `recompiled/<title ID>/` (`app/` holds the package, `ux0/` the writable storage), the eboot's code is
compiled ahead of time into `<title ID>_code.so`, and a copy of the runtime named after the title ID plays it; its
`<title ID>.ini` says `system=vita`, which the launcher and a build's deploy pick the driver by. Code
the offline pass didn't find is compiled at run time by the recompiler's executor, and the addresses it compiled are
kept as hints (`<title ID>.hints`) for the next offline compile.

```sh
build/scarletnx-vita game.vpk                    # extract, compile and play
build/scarletnx-vita ux0/app/PCSE00001           # a dumped game: linked, its update from ux0/patch/PCSE00001
build/scarletnx-vita -x game.vpk                 # extract only
build/scarletnx-vita --game=~/.local/share/scarletnx/recompiled/LVBT00001 --seconds=30 --shot=/tmp/frame
```

`scarletnx-vita` takes the [common options](../../user/usage.md#common-options) that fit it: `-x`, `-r`, `--game`, `--update`, `--data`,
`--outdir`, `--output`, `--seconds`, `--shot`, `--script`, `-h` and the recompile's (`-O`, `--jobs`, `--chunk`,
`--jit-cache`, `--jit-only`, `--keep-ir`, `-q`), each value as `--option VALUE` or `--option=VALUE`.

A dumped game's folder (decrypted, as a console's `ux0:app/<title ID>`) is linked into the game's folder rather than
copied: `app/` names it and `patch/` its update (`ux0:patch/<title ID>` beside it, or `--update DIR`, which an older version named `--patch`), whose files app0:
shows over the game's and whose eboot.bin is the one compiled. A retail eboot is a fixed-address module (ET_SCE_EXEC)
without relocations, so discovery starts at every word of the image and every MOVW and MOVT pair that makes a Thumb
function's address. The libraries the game brings in `sce_module/` (libc, libfios2, libsmart, libult) are loaded
beside the eboot, from `0x71000000` up, and their `module_start` functions run in turn before the game's. Their
imports of the system's functions are the runtime's, by NID. Variables a module imports (SceLibc's `_Ctype` and
`_Stdout`, the C++ exceptions' vtables) get slots in the runtime's page, numbered after those of the modules loaded
before it, which the loader relocates the code to. A slot is filled from a loaded module's export of the variable
when there is one, and by the runtime when there is none.

The first homebrew it targets is the Lua Vita Tester (lpp-vita with vita2d). It draws its screen, reads the buttons
and plays its sound test.

## Layers

| Layer | Code | Documents |
| --- | --- | --- |
| Loader | VPK, SELF and ELF (`src/vita/loader/`) | this page |
| CPU | ARMv7 code compiled ahead of time through the block compiler (`src/vita/aot/`, `src/common/cpu/arm/`), and the code that the offline pass missed at run time (`src/vita/runtime/jit.c`) | [Code compiled at run time](#code-compiled-at-run-time) |
| Kernel and HLE | the process, threads and memory (`src/vita/runtime/`), the system libraries by NID (`src/vita/hle/`) | [How it fits together](#how-it-fits-together) |
| GPU | sceGxm on Vulkan, USSE programs to SPIR-V (`src/vita/gpu/`), each scene drawn at the resolution setting's whole scale and its surfaces' memory at the console's size | [The shader corpus](#the-shader-corpus) |
| Audio | the audio ports, the input port recording the host's microphone (`src/common/utilities/mic.h`), NGS and the audio decoders (`src/vita/hle/audio.c`, `ngs.c`, `audiodec.c`) | this page |
| Input | SceCtrl and touch from the keyboard, the mouse, fingers, the gamepads and the on-screen controls (`src/vita/platform/input.c`) | this page |
| Platform | the host layer that every driver shares (`src/common/platform/`) | [Build](../../dev/build.md) |

## How it fits together

| Part | Files |
| --- | --- |
| VPK, SELF and ELF loading (zip and zlib, relocations, imports patched to SVCs by index) | `src/vita/loader/` |
| The offline compiler and the module it makes: the host's native back end's blocks (the A9's, chained: `src/common/cpu/arm/codegen.h`), as discovery finds them, and LLVM's where it refuses one, or all of them on a host without one or with `--keep-ir` (the recompile says how many each made, and why) | `src/vita/aot/` |
| Process, threads (cooperative, one host thread), waits, memory, the dispatcher, run-time compiles | `src/vita/runtime/` |
| The system libraries, answered by NID: each module's functions declared in a `.hook` beside its `.c` ([The system's functions by NID](#the-systems-functions-by-nid)) | `src/vita/hle/` (`kernel.c`, `io.c`, `ctrl.c`, `display.c`, `audio.c`, `misc.c`, `gxm.c`, `fiber.c`, `ngs.c`) |
| The game's own C and C++ libraries (SceLibc, SceLibm, SceLibstdcxx) and the Clib, malloc on a heap in guest memory | `src/vita/hle/libc.c`, `heap.c` |
| sceGxm on Vulkan: guest memory imported with `VK_EXT_external_memory_host` (a driver without it, as Mali's, works on a host visible copy of each memory block the game maps, which takes the pages the game wrote before the GPU reads them and goes back to guest memory, the rows the GPU wrote alone, when its submission is done), USSE programs translated to SPIR-V through `src/common/utilities/spirv_emit.c`, decoded by the decoder `tools/decodetree` makes from `usse.decode` | `src/vita/gpu/` (`render.c`, `usse.c`, `usse.decode`, `vk.c`, `present.c`) |
| Movies: SceMp4 (MP4 demuxed from memory with libavformat, the file read through the game's callbacks), sceAvcdec (H.264 with libavcodec, the picture laid out in the guest's buffer on the GPU, `picture.comp`), sceAudiodec (AAC and ATRAC9) and SceCodecEngine's memory | `src/vita/hle/` (`mp4.c`, `videodec.c`, `audiodec.c`) |
| Guest memory's writes: the pages the renderer copied to the GPU (textures, color surfaces, the framebuffer; every block the GPU uses where it works on copies) write-protected, a fault marking a page written and letting the write through, so unchanged memory isn't copied again (`mprotect` or `VirtualProtect`, and the fault handler) | `src/vita/runtime/watch.c` |
| Keyboard, mouse and fingers (touch), the on-screen controls, gamepads and scripted presses (`--script=ms:hex,...`, SceCtrl's button bits, each held for `--press-len=` ms) | `src/vita/platform/input.c` |

## The system's functions by NID

A game imports each system function by its NID, the 32-bit number that names it wherever it is imported from. The
functions the runtime answers are declared as the Switch's hooks are ([hooks](../nx/hooks.md)), in the language of
`tools/hookgen`, one spec for each module of `src/vita/hle/` beside its `.c` (`kernel.hook`, `kernel.c`). A declaration
gives the NID in brackets, then the function's name and signature, and the handler after `=`:

```text
family io
[0x99ba173e] hook sceIoLseek(u32 fd, i64 offset, i32 whence) -> u64 = vita_io_lseek
[0x8cba03d5] hook sceClibSnprintf(u32 buffer, u32 size, u32 fmt, ...) -> u32 = vita_libc_clib_snprintf
[0x27eab8c1] hook acosf(f32 x) -> f32 = vita_libc_lm_acosf
```

`hookgen --vita` (`snx_vita_hookgen` in `cmake/hookgen.cmake`) reads every spec into one table, `vita_hle_table`,
sorted by NID, which `hle.c` searches at an import's first call; two declarations of one NID are refused. Each entry's
adapter reads the arguments where AAPCS32 with VFP arguments puts them: r0 to r3, a 64-bit one in an even pair of them,
floats in the S and D registers, the rest on the caller's stack. A variadic function's `...` ends its arguments, which
are the base standard's (a float in core registers), and its handler reads the others itself (`vita_arg`). The adapter
calls the handler, `ret handler(VitaProcess* p, VitaThread* t, args...)`, which `vita/hle/hle.hook.h` declares, and
returns its result in r0 (a 64-bit one's high word in r1, a float in S0 or D0). An addr is the guest's 4-byte address.
A handler that waits (`vita_wait`) returns all the same, and its result is dropped: the thread's r0 is set when the
wait is met.

`tests/vita_hle_adapters.c` checks the adapters of `tests/hook/vita.hook`, and the `hookgen_refuses_vita_*` tests the
declarations that `--vita` refuses.

The twelve keys and the two sticks are on the screen too ([On-screen controls](../../user/usage.md#on-screen-controls)),
for fingers and the mouse's left button: shown on a touch screen while no gamepad is connected, or as `--controls=`
says, their layout edited with F2. A finger or that button off them touches the front panel where it is on the picture,
six at most (`--touch=1` the fingers alone, `--touch=0` neither), and the mouse's right button the rear panel, which no
finger touches. Esc twice within 3 seconds ends the game, a first press warning on the terminal and in a line over the
game, as a Switch game's notices show, and F11 or Alt+Return switches fullscreen: the runtime's keys, as a Switch game
takes them ([On-screen controls](../../user/usage.md#on-screen-controls)).

The driver's settings (`--key=value`) are the `g_vita_debug_ini` table of `src/nx/runtime/settings.txt`: `vita_hle_trace` prints each import call,
`vita_jit_trace` each block compiled at run time, `vita_gxm_trace` each scene and draw (and why a draw was
skipped). `--shot-every=N` (`vita_shot_every`, with `--shot=PATH`) saves a screenshot every N vblanks, as
`PATH_0000.ppm` on. `--vulkan-video=1` (the Switch runtime's setting) stops the game when it makes an AVC decoder: decoding the Vita's movies with Vulkan Video is
not implemented yet, and FFmpeg decodes them on the CPU.

## The shader corpus

`--vita-shader-corpus=DIR` writes each program the game registers or draws with to `DIR` (`<hash>.gxp`: the program
and the hints it was translated with). `build/snx-vita-usse DIR` translates them again as the runtime does and groups
the reasons the others don't translate; `--survey` counts the instructions the translator lacks over every program,
`--validate` runs `spirv-val` on each translation and `--out DIR` keeps them (two builds' outputs, diffed, show a
translator change kept them). The programs are the game's: the folder stays on this machine.

## Code compiled at run time

The run-time compiler's blocks are kept in the game's `.hints`, each once. When the game starts, the hints the module
lacks make it compile the module again with them, so the next runs have them ahead of time; a hint discovery refuses
(not code after all) is dropped then.

## Save states

A state is the whole game at one moment, in the Switch's state file (`src/nx/runtime/state_file.h`): the header and
the 320 by 180 thumbnail the launcher and the game's menu read, then a section for each part, each with a hash of the
names and types of what it holds (`src/vita/runtime/state.c`):

| Section | What it holds |
| --- | --- |
| PROCESS | the threads with their registers, waits and fibers, the objects, the memory blocks, the display, the dialogs |
| LINKS | the threads running and pacing the frames, and each thread's place in the module_start chain, as indices |
| TIME | the times kept by the host's clock (the start, the next vblank, the threads' deadlines, the sound ports' play), counted from the save: a load counts them from its own |
| MEMORY | the image's segments, the runtime's page with the threads' TLS, the imported variables' slots, every memory block |
| IO | each file and folder open, as its host path, sceIoOpen's flags and its place, and the FIOS2 archives mounted (`hle/io.c`) |
| GXM | sceGxm's contexts, programs, targets, mappings and display queue (`hle/gxm.c`) |
| AUDIO, NGS, TROPHY | the sound ports, SceNgs's racks and voices, the trophy contexts (`hle/audio.c`, `ngs.c`, `trophy.c`) |
| AUDIODEC | each audio decoder (AAC, ATRAC9) as its configuration, an ATRAC9 superframe not all handed out yet, the codec engine's blocks (`hle/audiodec.c`) |

A save waits for the GPU, so what it wrote is in guest memory and what sceGxm notes is written; a load makes the
renderer again, which draws from the GXPs and surfaces in memory as at the game's start. A file open is opened again
at its place, never truncated: the game's save data and what it wrote since stay, as a Switch game's files do. An
audio decoder is made again from its configuration: what FFmpeg's decoder keeps of the frames before (the overlap of
their transforms) starts empty, so the first frame it decodes after a load differs from the console's, a click of one
frame at most, and the frames after it are the same. The game is its title ID and its image's segments. A load reads
and checks every section before it changes anything, and refuses another game's state (both named), another
`SNX_VITA_STATE_VERSION` (both versions), a section of another layout (both hashes), a damaged file and one naming a
file that is gone. A save is refused, saying what, while a thread runs, while one is inside a call of the runtime's
into the game (a sort's comparison, a callback), while a scene is being drawn, and while the game plays a movie (an
mp4 file or a video decoder: the demuxer's place and the pictures H.264 decodes the next ones from can't be put back)
or has sockets open, which a state can't keep. The threads run by the host's clock, so `vita_state`
(`tests/vita_state.c`) checks what a load puts back of a process the test drives through the imports, and that a save
right after it is the same, rather than frames; `vita_state_gpu` (`tests/vita_state_gpu.c`) that a save holds the
GPU's work it waited for (a scene, a copy) and that the renderer a load makes draws from the state's memory, on a
device that imports guest memory and on one that copies it.

## Checking the CPU backend

Configure with `-DSNX_VITA_LOCKSTEP=ON` (and `SNX_UNICORN_INCLUDE`, `SNX_UNICORN_LIBRARY` pointing at Unicorn 2) and
run with `--vita-lockstep=N`: from the Nth import call on, every instruction group runs in Unicorn too, and the first
difference in registers, flags or written memory stops the run with both states (`--vita-lockstep-steps=M` stops
comparing after M groups). This is how the Thumb backend's bugs this driver ran into were found.

## Limits

- A stop shows the error's panel ([When a game stops](../../user/usage.md#when-a-game-stops)), unless the run is a
  recompile or timed: a window with no Vulkan device and a GPU that stops answering are stops.

- Color surfaces are U8U8U8U8, U8U8U8, U16 and F16F16F16F16 (half floats), linear, or swizzled or tiled (laid out on
  the GPU both ways, `swizzle.comp`), and stencil is drawn (S8D24). Textures are U8U8U8U8, U8U8U8, U8,
  F16F16F16F16, BC1 to BC3 (UBC1 to UBC3) and linear YUV420 (a movie's
  frames, made RGBA on the GPU, `yuv.comp`), linear, swizzled or tiled (a swizzled or tiled one made linear on the GPU,
  `swizzle.comp`), with their mipmaps laid out as Vita3K does. Other formats, cube textures and MSAA (a scene drawn
  at one sample a pixel) are not yet.
- A scene's notifications are written once the GPU drew it, and the next scene is recorded meanwhile, as on the
  console: a game waits for them (sceGxmNotificationWait) before it writes what a scene reads.
- A fragment program that reads its pixel's color (`GXP_FLAG_FRAGCOLOR_USED`, a program that blends itself) reads
  the color target as an input attachment in place, which needs `VK_KHR_dynamic_rendering_local_read`. With
  `VK_EXT_rasterization_order_attachment_access` the primitives of one draw see each other's colors in order as on
  the console; without it only earlier draws' colors are ordered (a by-region barrier before each such draw).
- The shader patcher keeps up to 4096 vertex and 4096 fragment programs, one per GXP and state, counted by its
  creations and released with the last.
- The USSE translator covers the vector and scalar ALU (`VMAD`, `VMAD2`, `VNMAD`, `VDP`, `VCOMP`, `VMOV`, `VPCK`,
  `VBW`, `SOP2`, `SOP2M`, the integer multiply-adds), the tests that write predicates and masks (`VTST`, `VTSTMSK`),
  loads from uniform buffers and the literal buffer (`VLDST`), dependent texture reads and gathers (`SMP`), `KILL`,
  `LIMM`, `DEPTHF`, `SMLSI`, `SMBO` and branches. A program with branches is a loop over its basic blocks, a switch on
  the next one's first instruction. Not yet: `VDUAL`, `SOP3`, `I8MAD`, stores, calls, texture queries and cube
  reads. Any of these makes the program fail to translate with its name in the log.
- One guest thread runs at a time.
- PGF fonts and networking answer as unavailable. The save data dialog answers as finished and draws no screen, and
  the other common dialogs answer as unavailable.
- SceNgs plays 16-bit PCM and HE-ADPCM players through their patches into the master buss. Its equalizer, envelope,
  pauser and filter modules pass the sound unchanged. The player's callbacks are made (a buffer to refill, the end
  of the data), and those of the other modules aren't. A C++ exception the game throws ends it.

## Sources

The GXM semantics, the GXP layout and the USSE translator are ported from Vita3K (GPL-2.0-or-later). The NIDs and
error codes come from vita-headers (MIT). vios was read as a quick reference. See [Credits](../../CREDITS.md).
