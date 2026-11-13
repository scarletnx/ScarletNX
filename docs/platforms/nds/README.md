<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# The Nintendo DS

## Index

* [Layers](#layers)
* [Playing](#playing)
* [Status](#status)
* [What runs and what is answered](#what-runs-and-what-is-answered)
* [The compiled module](#the-compiled-module)
* [The memory map](#the-memory-map)
* [The ARM7's services](#the-arm7s-services)
* [The HLE BIOS](#the-hle-bios)
* [The display on the GPU](#the-display-on-the-gpu)
* [Sound](#sound)
* [Known limits](#known-limits)
* [Source provenance and licensing](#source-provenance-and-licensing)

`scarletnx-nds` is the Nintendo DS system type. It takes a card image (`.nds`), compiles the game's ARM9 code ahead
of time into a native module, as the 3DS driver compiles a title's, and runs it on a board whose every other part is
answered at a high level: there is no ARM7 processor, no BIOS and no firmware in the emulator, and none is read. The
picture is drawn on the GPU with Vulkan. Nothing of it is drawn or copied a pixel at a time on the CPU.

```sh
cmake -S . -B build/nds -DSNX_BUILD_NX=OFF -DSNX_BUILD_NDS=ON
cmake --build build/nds
build/nds/scarletnx-nds Game.nds
```

The driver uses melonDS as its reference and ports parts of it ([Source provenance and licensing](#source-provenance-and-licensing)).

## Layers

| Layer | Code | Documents |
| --- | --- | --- |
| Loader | the card image, its header, the ARM9 program and the overlays decompressed (`src/nds/cartridge/`) | [The compiled module](#the-compiled-module) |
| CPU | the ARM9's ARM and Thumb code compiled ahead of time through the block compiler (`src/nds/aot/`, `src/common/cpu/arm/`), and the blocks the offline pass missed at run time (`src/nds/cpu/`) | [The compiled module](#the-compiled-module) |
| Kernel and HLE | the BIOS calls as board routines (`src/nds/bios/`), the ARM7's services (`src/nds/hle/`), the board's memory, registers, DMA, timers and card (`src/nds/board/`) | [The ARM7's services](#the-arm7s-services), [The HLE BIOS](#the-hle-bios), [The memory map](#the-memory-map) |
| GPU | the geometry engine (`src/nds/gpu/gpu3d.c`), and on Vulkan the 3D engine's picture, the 2D engines' frame and the window (`src/nds/gpu/vk/`) | [The display on the GPU](#the-display-on-the-gpu) |
| Audio | the sound driver (`src/nds/hle/sound.c`) and the sound hardware's channels (`src/nds/audio/`) | [Sound](#sound) |
| Input | the keys, the gamepads and the pen from the window (`src/nds/runtime/host.c`) | [Playing](#playing) |
| Platform | the run (`src/nds/runtime/`), and the host layer that every driver shares (`src/common/platform/`) | [Build](../../dev/build.md) |

## Playing

`scarletnx-nds` takes the [common options](../../user/usage.md#common-options) that fit it: `-r`, `--game`, `--data`,
`--outdir`, `--output`, `--frames`, `--seconds`, `--shot`, `--script`, `-f`, `-H` and `-h`, each value as
`--option VALUE` or `--option=VALUE`.

`scarletnx-nds Game.nds` plays the image in a window with both screens, the upper one above the lower one, scaled to
the window at the console's shape. The keyboard is the other consoles': the arrows are the D-pad, X or Space A, Z B,
S X, A Y, Q L, E R, Enter Start and Backspace Select. A gamepad's buttons are where the console has them. The pointer
held on the lower screen is the pen. F11 switches fullscreen, and Esc twice within 3 seconds closes the window.

`scarletnx-nds` is also a menu entry of `scarletnx-gui` ("Load NDS ROM"), and an image in a games folder shows in
Games.

The first run of a game compiles it, which takes a few minutes on a few cores (a game of 3 MiB of code makes about
200000 blocks). Later runs load the module.

`--frames N` and `--seconds N` end the run after that much of the console's time, and `-H` (`--headless`) opens no
window. `--shot PATH` writes the last frame, both screens, as a PPM image, and `--shot_every=N` one every N frames
beside it. `--script MS:KEYS,...` presses keys unattended, each at its time in ms from the start of the run, by the
console's frames: KEYS in hex are KEYINPUT's bits (1 A, 2 B, 4 Select, 8 Start, 10 Right, 20 Left, 40 Up, 80 Down,
100 R, 200 L), 400 X, 800 Y, and 1000 the pen, at the lower screen's pixel the top 16 bits give (`XXYY1000`:
`60601000` touches x 96, y 96). `--press_len=MS` is how long each is held (300). `--wav=PATH` writes the run's sound.

`--aot=0` plays without the module: every block is compiled at run time, which starts at once and is slower until the
game's code has been seen. `--aot_threads=N` sets the compile's threads. For work on the driver, `--trace` prints the
services' requests and what the board does not know (twice, each interrupt too), and `--dump_ram=PATH` writes the
ITCM and main memory's range, 0x01FF8000 to 0x02FFFFFF, when the run ends.

The game's folder is `recompiled/<name>/` of the data folder (`--data`, else the user's), named after the image's
file, or `<name>/` of `--outdir`, or the folder `--output` names. It holds the module (`<name>_code.so`), the blocks
runs compiled at run time (`<name>.hints`), the save (`<name>.sav`), a copy of the runtime named after the game and
`<name>.ini`, whose `source=` names the image and `system=nds` the console. The image stays where it is and is read
there. The copy plays its game alone, and `scarletnx-nds --game=recompiled/<name>` plays it the same way. `-r` compiles
the module again without playing, starting from the hints. A `<image>.sav` beside the image, which another emulator
keeps, is read once, when the folder has none.

## Status

| Part | State |
| --- | --- |
| Card image | done: the header, the ARM9 program and the overlays, each decompressed. An image with its secure area still encrypted is refused |
| ARM9 | done: ARMv5TE through the block compiler's ARM946E-S profile, ahead of time and at run time, CP15, the modes and their banked registers |
| Memory, DMA, timers, divider, square root, interrupts | done |
| BIOS | done but for the calls that read through the game's own functions, which read memory instead |
| ARM7's services | the handshake, the firmware's settings, the clock, the touch screen, the power chip, the save memory, the sound driver. No wireless, no microphone |
| 2D engines | done on the GPU: text, affine, extended and large backgrounds, objects (affine and bitmap ones too), windows, the color effects, mosaic, the master brightness, the display modes, the main memory display |
| 3D engine | geometry done on the CPU. Drawn on the GPU: polygons, lines and wireframes, textures in all seven formats, toon and highlight shading, translucency by polygon ID, shadows, the clear plane and the clear bitmap, edge marking, fog, anti-aliasing |
| Display capture | done on the GPU: the compute pass writes the captured picture, which lands in the VRAM bank the capture names |
| Sound | the SDK's sequence player and the sixteen channels. No sound capture |
| Save memory | kept in the game's folder |

Super Mario 64 DS (USA, revision 1) is the game the driver was brought up on: it runs from its title screen through
its menus and its opening into play, with sound.

## What runs and what is answered

A DS has two processors. The ARM9 runs the game. The ARM7 runs a program that is the SDK's own, the same in every
game but for its version: it serves the sound hardware, the touch screen, the clock, the power chip, the firmware's
flash, the card's save memory and the wireless hardware, and the game's ARM9 side asks it for each over a FIFO between
the processors. The ARM9 also calls the console's BIOS, and it starts in the state the firmware leaves.

`scarletnx-nds` runs the ARM9's code alone, compiled. The ARM7 is a set of answers
([The ARM7's services](#the-arm7s-services)), the BIOS a set of routines ([The HLE BIOS](#the-hle-bios)), and the
firmware a starting state: the header and the program in main memory, the user's settings, CP15's registers and the
stacks, as melonDS's direct boot sets them. The hardware the ARM9 reaches itself has its registers
(`src/nds/board/io.c`): the display's, the DMA channels, the timers, the divider and the square root, the keys, the
card's slot, the interrupt controller, and the 3D engine's.

The board keeps the console's time in bus cycles (33.51 MHz). A compiled block is charged a cycle for every two
instructions. Events come at their cycle: each line's horizontal blank and end, a timer's overflow, the sound driver's
tick and its alarms. A DMA transfer and a geometry command take no time: a transfer runs whole when it starts, and a
command runs when it is written. So does a read of the card: a block is all there when its command is written, a DMA
channel that waits for the card takes it at once, and the card's interrupt follows. The commands after SWAP_BUFFERS are the exception, which wait for the vertical blank
in a queue, as the console holds them, and the geometry engine says it is busy until then.

## The compiled module

The game's code is in RAM on this console, and a game replaces parts of it as it plays: its overlays, which the SDK
loads from the card over one another at the same addresses. So an address alone names no code. The loader reads from
the image every piece of code whose address is known (`src/nds/cartridge/rom.c`): the ARM9 program, decompressed as
its start code would decompress it (the field the start code decompresses by is cleared, so the game does not do it
again), the blocks the start code copies elsewhere (the ITCM's code among them), and each overlay, decompressed. These
are the image's regions.

The offline compiler (`src/nds/aot/`) discovers the code of each region from the entry point, the overlays' static
initializers and the hints, along every direct branch and call. A function reached only through its address is found
from the words that hold the address, where a function can start: at code that reads as code, right after the end of
code already found or at a region's start. The blocks are compiled in chunks on the threads `--aot_threads` gives and
linked by the backend's own linker, with a table of blocks for each region and instruction set and each block's
length.

At run time (`src/nds/cpu/recomp.c`) a block runs only while memory holds the bytes it was compiled from, which every
dispatch compares. The first time an address is dispatched, the module's regions that hold it are looked through for
a block whose bytes are in memory. When none is, the block is compiled in the process (`src/common/cpu/arm/exec.c`)
from memory as it is. Blocks of other code at the same address stay, and are found again when their code comes back.
A block compiled at run time whose bytes are a region's is a hint: the run adds it to `<name>.hints`, and the next
`-r` starts its discovery there too.

The blocks are compiled with the backend's ARM946E-S profile (`src/common/cpu/arm/README.md`): ARMv5TE on the flat
window, with the I/O window's accesses sent to the board's bus hook. The instructions the profile leaves to the
system run in the runtime against the banked registers (`src/nds/cpu/cpu.c`): MRS and MSR, SWP, the exception
returns, the LDM and STM of the user bank, and CP15, whose registers the SDK's start code sets (the caches, the
protection unit's regions, the two TCMs) and whose wait for an interrupt halts the machine.

## The memory map

`src/nds/board/memory.c` owns the flat window the compiled blocks run on, as the GBA's does: each of the console's
memories is a host memory object mapped at its address and its mirrors (main memory four times, the shared work RAM
and the ITCM through their ranges), and the window's rest is memory of zeros. Three things move while the game runs,
and their pages are mapped again where they go:

- The nine VRAM banks, as the game writes VRAMCNT. A bank is seen at the address its mode gives (a background's or an
  object's space, or its own place), and a page with no bank shows a scratch page.
- The DTCM, as the game writes CP15's register 9: its 16 KiB lie over whatever its range held.
- Nothing else: the palettes and the OAM are a page each.

The mirrors of the palettes, the OAM and the VRAM spaces past each one's first copy are plain memory, not mirrors.

The two TCMs are the processor's own. A DMA channel is on the bus and does not see them: below main memory it
writes nothing and reads zeros, and under the DTCM it has main memory (`src/nds/board/dma.c`).

## The ARM7's services

`src/nds/hle/pxi.c` is the ARM7's side of the FIFO, ported from melonDS's high-level ARM7. A word the game sends
names a service in its low five bits, and each service answers with a word of its own:

| Service | Answered with |
| --- | --- |
| The handshake over IPCSYNC | each count with the next, as the two sides start |
| Firmware | a flash image the board makes, with the user's settings in it: reads, the write-enable latch and writes, kept for the run |
| Clock | the host's local date and time |
| Touch screen | the pen from the window, a sample at a time or by the line, at the place in main memory the SDK reads |
| Sound | [Sound](#sound) |
| Power | the power chip's registers, which light nothing |
| Microphone | done, with no samples |
| Save memory | the save's file: reads, writes and verifies of the block the request names |
| Wireless | no answer, as with the radio off |

The keys the ARM9 has no register for (X, Y and the hinge) are kept where the ARM7 keeps them in main memory, a line
at a time.

## The HLE BIOS

The software interrupts run as board routines (`src/nds/bios/bios.c`): Div, Sqrt, CpuSet and CpuFastSet, GetCRC16,
BitUnPack, the LZ77, Huffman and run-length decompressors, the two difference filters, WaitByLoop, IsDebugger and
CustomPost. Halt, IntrWait and VBlankIntrWait leave on the stacks and in the registers what the console's BIOS leaves
there while they wait, and an interrupt is entered and left as the BIOS enters and leaves it (`src/nds/cpu/cpu.c`):
the SDK's interrupt handler switches threads by rewriting the frame the BIOS pushed, and a thread that waits inside
the BIOS is resumed there.

The decompressors that read through the game's own functions (LZ77, Huffman and run-length "by callback") read the
memory at their source instead, which is what the SDK's functions read. The run says so once.

## The display on the GPU

Everything the console shows is drawn with Vulkan (`src/nds/gpu/vk/`), and a run without a Vulkan 1.3 device shows
nothing. The CPU's part is three copies a frame, in whole ranges: the video memory as the 2D engines address it, the
video memory as the 3D engine addresses it, and the registers each line was drawn with.

**The 2D engines.** At each visible line's horizontal blank the board keeps the registers the line was drawn with
(`src/nds/board/lcd.c`): the display control, the backgrounds' controls and scrolls, the affine parameters with the
reference points as the line counts them on, the windows, the color effects and the master brightness. So a register
the game changes between lines takes effect on the line it would on the console. When line 191 is done, one compute
pass (`shaders/compose.comp`) makes every pixel of both screens: it reads its line's registers, fetches the four
backgrounds' pixels and the objects' from the copy of video memory (tiles, maps, bitmaps, palettes and extended
palettes decoded there), and composes them by priority, window and color effect, over the 3D engine's picture. The
video memory, the palettes and the OAM are the ones the frame's end left: a change to them in the middle of a frame
is seen for the whole frame.

Mosaic is the pixel's own arithmetic: a background takes its block's first pixel (an affine one with the reference
point of the block's first line), and an object with the mosaic bit shows in each block what it has at the block's
first pixel. The main memory display (display mode 3) is a frame the board keeps: a DMA channel with the display's
timing sends a line of 256 pixels to DISP_MMEM_FIFO at each visible line, and the shader reads the frame from the
copy.

**The 3D engine.** The geometry engine is on the CPU (`src/nds/gpu/gpu3d.c`, ported from melonDS): the matrix stacks,
lighting, clipping and the viewport transform are the console's fixed point, and the game reads their results back
(the clip matrix, the box, position and vector tests, the polygon and vertex counts). What it leaves is a frame's
polygons, their vertices already on the screen. At line 215 they are drawn (`render3d.c`, after melonDS's OpenGL
renderer): opaque pixels first, then the translucent polygons in their order, the stencil buffer holding each pixel's
polygon ID so a translucent polygon does not blend with itself and a shadow falls only where its mask says, then edge
marking and fog as passes over the picture, and anti-aliasing as the last. The fragment shader decodes each texel where it lies in the copy of
texture memory (`shaders/texture.glsl`), in all seven formats, so there is no texture cache and no decoding on the
CPU. The picture is the console's 256 by 192.

Four things are drawn that melonDS's OpenGL renderer leaves out or draws another way, each in a shader. Every
triangle has its own three vertices, which say which of its sides are its polygon's, so the fragment shader knows how
many pixels a pixel is inside its polygon (`shaders/poly.frag`):

- A wireframe polygon (alpha 0) keeps only its outline's pixels.
- An opaque pixel carries an edge flag, and edge marking colors only edge pixels, as the software renderer does.
- With anti-aliasing on, an edge pixel carries a coverage and the way across its edge. The last pass
  (`shaders/aa.frag`) blends it with the picture's pixel across the edge where that pixel is farther away, which is
  the surface the polygon hides. The console blends an edge pixel with a second pixel it keeps under it. Where two
  polygons of a surface meet, that is the neighbor polygon at the same pixel, so the blend changes nothing there, and
  here such a pixel is left alone: a mesh's inner edges do not smear its texture. The coverage is from the pixel's
  distance to the edge, not the console's own count along the edge's slope, so the smoothing is close to the
  console's and not equal to it.
- A polygon that is a line is drawn with a line pipeline.

With DISP3DCNT's bit 14 the picture starts from the clear bitmap, not from a plain plane: a pass
(`shaders/clear.frag`) reads each pixel's color, depth and fog bit from texture slots 2 and 3.

**Display capture.** When DISPCAPCNT asks for a frame, the compute pass also writes the captured picture (the
engine's frame or the 3D picture, blended with a VRAM bank's picture or the main memory display's as the register
says) to a second image. It is read
back in one range into the VRAM bank the register names, so the game, or the other engine, finds it there as a bitmap.
A game that draws 3D on both screens does it this way.

**The window.** The frame, the upper screen above the lower one, is scaled into the window's swapchain image at its
shape (`present.c`).

## Sound

The SDK's sound library has two sides. The game's side sends lists of commands over the FIFO and shares a block of
main memory. The ARM7's side is the driver: it plays sequences (SSEQ) with their banks and waves through the sound
hardware. `src/nds/hle/sound.c` is that driver, ported from melonDS's high-level ARM7: the commands, the sequence
player with its tracks and note operations, the channels' envelopes, sweeps and modulation, and the alarms. It runs
every 5.2 ms of the board's time, as the driver's timer has it.

`src/nds/audio/spu.c` is the sound hardware the driver plays through: sixteen channels (PCM, ADPCM, the square waves
and the noise channels) mixed to stereo at the console's rate, 32728.5 Hz. SDK 2.0's sound library numbers its
commands in two other ways (its early builds and its later ones), which the program's SDK version tells.

## Known limits

- The ARM7 is answered, never run: a game whose ARM7 program is its own (some homebrew) gets the SDK's answers.
- Time is by the block and the line. A game that counts cycles finer than that sees them coarser.
- The geometry engine takes no time, and neither does a DMA transfer.
- A game that changes video memory, a palette or the OAM in the middle of a frame shows the change for the whole frame.
- A captured picture is in its VRAM bank one frame after the console would have it there.
- An anti-aliased edge's coverage is from the pixel's distance to the edge, and an object's mosaic is taken object by
  object, where the console takes it from the line of objects already drawn. Both are close to the console's
  picture and not equal to it.
- No wireless, no microphone, no GBA slot.
- An image that is shorter than its header says (a copy that stopped early) is played with a warning, and the game
  stops where it reads the part that is not there.
- Games on a later sound library than the SDK's first two (melonDS's "Peach" driver) have no sound.

## Source provenance and licensing

melonDS is GPL-3.0-or-later. The files ported from it keep its copyright line beside the project's, and name the
file and the revision they came from:

- `src/nds/gpu/gpu3d.c` and `gpu3d_state.h`: `GPU3D.cpp` and `GPU3D.h` at `497aa6c` (the geometry engine).
- `src/nds/gpu/vk/render3d.c` and `shaders/poly.vert`, `poly.frag`, `post.frag`: `GPU3D_OpenGL.cpp` and its shaders
  at `497aa6c` (how a frame is drawn: the passes, the stencil's polygon IDs, the blends, edge marking and fog). The
  Vulkan code is new.
- `shaders/aa.frag`, `shaders/layers.glsl` and the edge rules of `shaders/poly.frag`: the anti-aliasing blend, the
  edge marking test and what an edge pixel is, as `GPU3D_Soft.cpp` has them. `shaders/clear.frag`: the clear bitmap
  as `GPU3D_OpenGL.cpp` reads it.
- `shaders/texture.glsl`: the texture formats as `GPU3D_Soft.cpp`'s `TextureLookup` reads them.
- `shaders/compose.comp`: the 2D engines' rules as `GPU2D_Soft.cpp` and `GPU_ColorOp.h` have them (the layers' order,
  the objects, the windows, the blends). The shader is new: melonDS draws lines on the CPU.
- `src/nds/hle/pxi.c`: `HLE_Retail/IPC.cpp` of the `melonHLE` branch at `b42e2f4`.
- `src/nds/hle/sound.c`: `HLE_Retail/Sound_Nitro.cpp` of the same branch.
- `src/nds/audio/spu.c`: `SPU.cpp` at `497aa6c` (the channels' formats and the mixer's arithmetic).
- `src/nds/board/memory.c` (where each VRAM bank goes), `board.c` (the state a game starts in), `io.c` (the divider's
  corner cases) and `lcd.c` (the reference points, the windows' flags) follow `GPU.cpp`, `GPU2D.cpp` and `NDS.cpp`.

The registers, the card's commands, the BIOS's calls and the header's layout are GBATEK's. The machine loop and the
flat window follow the project's own GBA driver. melonDS is not a build dependency.
