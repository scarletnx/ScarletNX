# Switch 2 bring-up

This page records what the bring-up of the first Switch 2 game found so far. It names one game's driver offsets, but
not the game.

## The GPU path

The game does not drive the GPU with Maxwell command buffers. Its 3D engine is class `0xC797` (Ampere B, subchannel 0
on channel 21), and it uploads Turing macros (methods 0x45 to 0x48). The Maxwell path reads `PROGRAM_REGION`
(0x582 and 0x583), which the game never writes, so that path can't render it.

The game draws through NVN2, API version 231.16 (GPU code 201.5 to 201.11). The `sdk` module exports
`nvnBootstrapLoader`, the Switch 1 functions and about 170 new ones: mesh draws, ray tracing, DLSS, device command
buffers, device messages, variable rate shading and HDR window metadata. ScarletNX takes these calls with
`nvn_direct=1`.

The game must be compiled ahead of time (`build/scarletnx -r ... --cpu_extensions=1`). With the JIT alone, the
dispatch slot of `nvnBootstrapLoader` is still empty when `nvn_direct_init` runs, because that runs before `jit_init`.

`tools/nvn_missing.py work/Game` lists the NVN functions of a game's driver that `nvn.hook` doesn't implement for its
API version.

## Shaders

The shader code is SM70+ SASS (the Volta to Ada encoding): 128-bit instructions, with the opcode in bits 0 to 11 of
the low word. Each program ends with `EXIT` (`0x794d`, control word `0x000fea0003800000`), then a `BRA` to itself
(`0xfffffff000007947`, `0x000fc0000383ffff`). The Maxwell decoder can't read it. An SM70+ frontend is in progress, with
Mesa's NAK (MIT, `src/nouveau/compiler/nak/sm70_encode.rs`) as the reference for the encoding.

The program starts at the shader data's GPU address, with no wrapper (the older formats have one of 0x30 bytes). A
compute program starts with its first instruction. A graphics program starts with a 128-byte program header of
version 4: word 0 is `0x020c0481` for a vertex program and `0x000c5482` for a fragment program.

The control section is format 11: `98761234 000000c9 0000000b`, with `0xC9` at 0x04 where the older formats have 1.
`stage_of` in `gpu/nvn/nvn_object.c` doesn't read its stage, constants or sizes yet. The driver's
`nvnProgramSetShaders` (sdk offset `0x5bf8b0`) reads them, and it checks the version and the format at `0x5bfde8`.
One control section also holds SPIR-V 1.6 at `+0x128`.

## Driver offsets

The `sdk` module's `.text` loads at `0xbd0c000` with no offset, so a game address `A` is sdk offset `A - 0xbd0c000`.

| Function | sdk offset |
| --- | --- |
| `nvnProgramSetShaders` | `0x5bf8a0` (a thunk that sets x3 to 0, then `0x5bf8b0`) |
| `nvnProgramSetShadersExt` | `0x5c03f0` |
| `nvnProgramBuilderSetShaders` | `0x5c09d0` |
| `nvnProgramInitializeFromBuilder` | `0x5c1730` |
| `nvnCommandBufferPushGraphicsConstants` | `0x5d34b0` |

A shader data entry is `{data, control}` (16 bytes) for `SetShaders` and for the builder alike: the program's GPU
address, then the control section's CPU address. `nvnProgramInitializeFromBuilder` reads the builder's array as
`SetShaders` reads its own. A program's control sections lie 0xd8 bytes apart.

Push constants write their root table and word offset to method 0x141 of the 3D or compute engine, and the words to
0x142. Variable rate shading writes the shading rate image and palette, which the runtime leaves at the full rate.
`SetGeometryProcessingMode` writes method 0xe26.

## Compute QMDs

The compute QMD is version 3.0, which NVIDIA's `open-gpu-doc` documents (`classes/compute/clc7c0qmd.h`). The
dependence counter is word 4, bits 14 to 29. The PCAS action goes to method 0xB0 (`SEND_SIGNALING_PCAS2_B`, byte
0x2C0): 0x2 schedules, 0x7 decrements the dependence and 0xC increments it. The game runs a chain of 7 empty QMDs on
channel 22 and a ring of 16 on channel 23. Their increments and decrements don't balance in the runtime yet.
