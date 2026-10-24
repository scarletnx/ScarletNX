# The AGB board

`scarletnx-agb` is the Game Boy Advance system type. It loads a GBA ROM image
(`.gba`) onto an emulated AGB board, the codename of the console's hardware, and
runs it with the same static recompiler the other system types use. The board
library under `src/agb/` is CPU-only: it links `scarletnx-common` and nothing of
`src/nx/` or `src/gnuabi/`, so the board builds without the Switch runtime's
graphics and media packages.

```sh
cmake -S . -B build/agb -DSNX_BUILD_NX=OFF -DSNX_BUILD_AGB=ON
cmake --build build/agb
build/agb/scarletnx-agb Game.gba
```

`scarletnx-agb` is also a menu entry of `scarletnx-gui` ("Load GBA ROM"), next
to the Switch, Linux and 3DS loaders.

## Playing

`scarletnx-agb Game.gba` plays the ROM in a window four times the console's
screen, scaled by whole steps when it is resized, with the console's sound and
its 59.73 frames a second. `--fullscreen=1` starts it fullscreen, and F11
switches. The keyboard is the Switch runtime's: the arrows are the D-pad, X or
Space A, Z B, Q L, E R, Enter Start and Backspace Select. A gamepad's buttons are
where the console has them (A east, B south), its shoulders and triggers L and
R, its left stick the D-pad.

The game's folder is `recompiled/<name>/` of the data folder (`--data`, else the
user's), named after the ROM's file, or `<name>/` of `--outdir`. It holds the
module and the save, `<name>.sav`, and the ROM's folder stays as it is. A
`<ROM>.sav` beside the ROM, which another emulator keeps, is read once, when the
folder has none. What the game writes to its SRAM or flash reaches the disk a
second after it writes, and when the window closes.

## Status

The board is under construction, piece by piece:

| Part | State |
| --- | --- |
| Cartridge and ROM header | done: header parsing and validation |
| Memory map | done: every region and mirror, the I/O window as plain storage |
| HLE BIOS interface | mostly done: the software interrupts run as board routines, the waits and sound driver stubs |
| PPU and display | done: the text, affine and bitmap layers, sprites, windows and blending |
| APU (audio) | done: the four PSG channels, the two FIFOs and the stereo mixer |
| Keypad (HID) | done: KEYINPUT and the KEYCNT interrupt, from the keyboard and gamepads |
| Save memory | SRAM and flash, kept in the game's folder; EEPROM not yet |
| CPU, DMA, timers, interrupts | done |

A ROM is recompiled when it is loaded, the same process the other system
types use, and runs on the board from its native module.

## The display device

`ppu/ppu.c` owns the LCD controller's registers at 0x04000000 and its timing,
308 dots of four cycles a scanline and 228 scanlines a frame, with the blanking
flags in DISPSTAT and the vblank, hblank and vcount interrupts. Each visible
line is drawn into the 240 by 160 RGBA frame the host presents: the text,
affine and bitmap background layers, the sprites with their affine forms, the
windows, and the colour special effects (alpha blending and brightness). Mosaic
is the one display feature still missing.

## The sound hardware

`apu/apu.c` owns the sound registers and the mixer: the two square channels with
their envelopes, length counters and sweep, the wave channel with its two banks
of four bit digits, the noise channel with its feedback register, and the two
direct sound FIFOs the audio DMA fills. The mixer samples at 32768 Hz and
produces the interleaved stereo stream the host plays, with the SOUNDCNT_L and
SOUNDCNT_H volume and panning settings applied.

## The memory map

`board/memory.c` owns the address space the CPU will run on: work RAM (256 KiB
at 0x02000000 and 32 KiB at 0x03000000), the palette, VRAM and OAM, the save
memory at 0x0E000000 and the cartridge window at 0x08000000, each with the
mirrors the console decodes. The I/O window at 0x04000000 is plain register
storage so far. The devices replace their registers with real behaviour as they
land.

Some reads see the bus rather than memory. The BIOS can only be read by the
BIOS itself, so a read from anywhere else returns the opcode the BIOS fetched
last. That opcode is fixed for the four ways control leaves the BIOS (after the
boot, after a software interrupt, inside an interrupt handler, after its
return), and the board keeps it in `AgbBoard.bios_prefetch`. Each of the three
cartridge windows holds the image once. Past the image the cartridge drives no
data and a read returns the halfword address the cartridge latched, address / 2.
A 32 KiB SRAM has no 16th address line, so it repeats twice in the 64 KiB save
window.

## The HLE BIOS

The console's BIOS is not shipped with the emulator. The software interrupts run
as board routines (`bios/bios.c`), the way the Switch runtime turns SVC calls
into HLE and the GNU ABI runtime turns `SVC #0` into Linux system calls. Div,
Sqrt, ArcTan and ArcTan2 compute in fixed point, CpuSet and CpuFastSet move
memory, the LZ77, run length and difference calls decompress, BitUnPack widens
packed fields, and BgAffineSet and ObjAffineSet build the rotation matrices. The
calls that need the interrupt controller or the sound hardware (Halt, Stop,
IntrWait, VBlankIntrWait, SoundBias and the sound driver) are known stubs that
land with those devices.

Angles are turns in 16 bits, the way the BIOS counts them, and the affine matrix
drops the low byte of the rotation angle, giving 256 steps around the circle.
The trigonometry rounds to 1.14 fixed point like the BIOS's own tables.

## The recompiled ROM

`cpu/recomp.c` takes the ROM's ARM and Thumb code through the existing ARM
recompiler of `src/common/cpu/arm`, block by block, and links the emitted
objects into a native module with the host compiler. Loading a ROM does this
automatically and caches the module in the game's folder as `<name>.code.so`,
the way the NX system type caches `<Game>_code.so`; `-r` rebuilds it. Blocks the
discovery step does not reach, a branch through a register or the instruction
after one the board interpreted, are compiled when they are first dispatched.
The miss starts a discovery walk of its own; a build with the LLVM backend
compiles each block it finds in the process at its first dispatch
(`src/common/cpu/arm/exec.c`), and one without it puts them in a module of their
own (`<name>.code.<n>.so`), so a miss costs those blocks and not the whole ROM.
When the run ends the blocks it ran are folded into the cached module, so the
next run starts with every block the last one reached. A block built from RAM
runs while RAM holds the words it was built from, and is built again once the
game writes over them; the map names the ROM's hash, and a map of another ROM is
refused with both.

The CPU state is the recompiler's `SnxArmCPU` on a flat window (`cpu/memmap.c`)
where the board's regions live at their canonical addresses with the console's
mirrors mapped to the same pages, so generated code reaches everything with
one pointer add. Each region is a host memory object (`src/common/memory/host.c`,
as the 3DS's RAM) mapped at every mirror at the host's page size; a host whose
pages are past 32 KiB keeps each mirror as RAM of its own, and the runner says so. The blocks are compiled with the backend's ARM7TDMI profile
(`src/common/cpu/arm/README.md`), which follows the console's CPU where ARMv8-A
differs (rotated misaligned loads, PC + 12 stores, POP {pc} without
interworking) and sends the BIOS, I/O window, palette, VRAM, OAM and save memory
accesses to the board's bus hook. The privileged instructions the recompiler refuses, the mode
switching MSR/MRS forms, SWP and the exception return, are interpreted against
the banked register state. Software interrupts go to the HLE BIOS and
interrupts follow the entry and return the BIOS stub performs. Recompiled
blocks are charged two cycles per instruction, and the waits, the save memory
width rules and the timer driven FIFO rates are approximations documented
divergences.

## The ROM header

Every GBA ROM image starts with a header the cartridge presents to the console:
an ARM branch at offset 0, the boot logo, the title at 0xA0, the game code at
0xAC, the maker code at 0xB0, the fixed byte 0x96 at 0xB2 and the complement
check at 0xBD over the title block. `cartridge/cartridge.c` validates the branch,
the fixed byte and the complement check, and keeps the parsed fields beside the
image. A ROM the console would reject is refused with the failing byte named.

## Tests

The board's unit tests are `tests/agb_*.c`. A test build also downloads
[jsmolka's gba-tests](https://github.com/jsmolka/gba-tests) (`cmake/dependencies.cmake`)
and runs each ROM as a test of its own, `agb_gba_<rom>` (`tests/agb_gba_tests.py`).
The ROMs check the ARM and Thumb instruction sets, the BIOS's open bus, the
memory map, the save memory and the display. A self-checking ROM leaves the
number of its first failed test in a register, which `scarletnx-agb --registers`
prints at the end of the run. The display ROMs are checked against the picture
their source draws.

```sh
ctest --test-dir build/agb -R agb_gba
```
