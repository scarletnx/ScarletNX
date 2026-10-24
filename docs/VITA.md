<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# PS Vita driver

`scarletnx-vita` runs a homebrew VPK the way the other drivers run their games. The VPK is extracted once into the
data folder's `recompiled/<title ID>/` (`app/` holds the package, `ux0/` the writable storage), the eboot's code is
compiled ahead of time into `<title ID>_code.so`, and a copy of the runtime named after the title ID plays it; its
`<title ID>.ini` says `system=vita`, which the launcher and a build's deploy pick the driver by. Code
the offline pass didn't find is compiled at run time by the LLVM backend's executor, and the addresses it compiled are
kept as hints (`<title ID>.hints`) for the next offline compile.

```sh
build/scarletnx-vita game.vpk                    # extract, compile and play
build/scarletnx-vita ux0/app/PCSE00001           # a dumped game: linked, its update from ux0/patch/PCSE00001
build/scarletnx-vita -x game.vpk                 # extract only
build/scarletnx-vita --game=~/.local/share/scarletnx/recompiled/LVBT00001 --seconds=30 --shot=/tmp/frame
```

A dumped game's folder (decrypted, as a console's `ux0:app/<title ID>`) is linked into the game's folder rather than
copied: `app/` names it and `patch/` its update (`ux0:patch/<title ID>` beside it, or `--patch DIR`), whose files app0:
shows over the game's and whose eboot.bin is the one compiled. A retail eboot is a fixed-address module (ET_SCE_EXEC)
without relocations, so discovery starts at every word of the image and every MOVW and MOVT pair that makes a Thumb
function's address. The libraries the game brings in `sce_module/` (libc, libfios2, libult) aren't loaded: like the
system's, their functions are the runtime's, by NID. Variables the eboot imports (SceLibc's `_Ctype` and `_Stdout`,
the C++ exceptions' vtables) get slots in the runtime's page, which the loader relocates the code to and the runtime
fills.

The first homebrew it targets is the Lua Vita Tester (lpp-vita with vita2d). It draws its screen, reads the buttons
and plays its sound test.

## How it fits together

| Part | Files |
| --- | --- |
| VPK, SELF and ELF loading (zip and zlib, relocations, imports patched to SVCs by index) | `src/vita/loader/` |
| The offline compiler and the module it makes (the A57-profile AArch32 backend, `src/common/cpu/arm/`) | `src/vita/aot/` |
| Process, threads (cooperative, one host thread), waits, memory, the dispatcher, run-time compiles | `src/vita/runtime/` |
| The system libraries, answered by NID | `src/vita/hle/` (`kernel.c`, `io.c`, `ctrl.c`, `display.c`, `audio.c`, `misc.c`, `gxm.c`, `fiber.c`, `ngs.c`) |
| The game's own C and C++ libraries (SceLibc, SceLibm, SceLibstdcxx) and the Clib, malloc on a heap in guest memory | `src/vita/hle/libc.c`, `heap.c` |
| sceGxm on Vulkan: guest memory imported with `VK_EXT_external_memory_host`, USSE programs translated to SPIR-V through `src/common/utilities/spirv_emit.c` | `src/vita/gpu/` (`render.c`, `usse.c`, `vk.c`, `present.c`) |
| Movies: SceMp4 (MP4 demuxed from memory with libavformat, the file read through the game's callbacks), sceAvcdec (H.264 with libavcodec, the picture laid out in the guest's buffer on the GPU, `picture.comp`), sceAudiodec (AAC and ATRAC9) and SceCodecEngine's memory | `src/vita/hle/` (`mp4.c`, `videodec.c`, `audiodec.c`) |
| Keyboard, mouse (touch), gamepads and scripted presses (`--script=ms:hex,...`, SceCtrl's button bits, each held for `--press_len=` ms) | `src/vita/platform/input.c` |

The driver's settings (`--key=value`) are the `g_vita_debug_ini` table of `src/nx/runtime/settings.txt`: `vita_hle_trace` prints each import call,
`vita_jit_trace` each block compiled at run time, `vita_gxm_trace` each scene and draw. `--vulkan_video=1` (the
Switch runtime's setting) stops the game when it makes an AVC decoder: decoding the Vita's movies with Vulkan Video is
not implemented yet, and FFmpeg decodes them on the CPU.

## The shader corpus

`--vita_shader_corpus=DIR` writes each program the game registers or draws with to `DIR` (`<hash>.gxp`: the program
and the hints it was translated with). `build/snx-vita-usse DIR` translates them again as the runtime does and groups
the reasons the others don't translate; `--survey` counts the instructions the translator lacks over every program,
`--validate` runs `spirv-val` on each translation and `--out DIR` keeps them (two builds' outputs, diffed, show a
translator change kept them). The programs are the game's: the folder stays on this machine.

## Code compiled at run time

The run-time compiler's blocks are kept in the game's `.hints`, each once. When the game starts, the hints the module
lacks make it compile the module again with them, so the next runs have them ahead of time; a hint discovery refuses
(not code after all) is dropped then.

## Checking the CPU backend

Configure with `-DSNX_VITA_LOCKSTEP=ON` (and `SNX_UNICORN_INCLUDE`, `SNX_UNICORN_LIBRARY` pointing at Unicorn 2) and
run with `VITA_LOCKSTEP=N`: from the Nth import call on, every instruction group runs in Unicorn too, and the first
difference in registers, flags or written memory stops the run with both states. This is how the Thumb backend's
bugs this driver ran into were found.

## Limits

- Color surfaces are A8B8G8R8. Textures are U8U8U8U8, U8, BC1 to BC3 (UBC1 to UBC3) and linear YUV420 (a movie's
  frames, made RGBA on the GPU, `yuv.comp`), linear, swizzled or tiled (a swizzled or tiled one made linear on the GPU,
  `swizzle.comp`), with their mipmaps laid out as Vita3K does. Other formats, cube textures, MSAA and stencil are not
  yet.
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
- PGF fonts, networking and the common dialogs answer as unavailable.
- SceNgs is silent: its voices finish at once. A C++ exception the game throws ends it.

## Sources

The GXM semantics, the GXP layout and the USSE translator are ported from Vita3K (GPL-2.0-or-later). The NIDs and
error codes come from vita-headers (MIT). vios was read as a quick reference. See [CREDITS](CREDITS.md).
