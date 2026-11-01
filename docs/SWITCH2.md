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

A Switch 2 control section has `0xC9` at 0x04, where the older formats have 1. Formats 7 and 11 share one layout:
0x30 the header's bytes (0xd8), 0x38 the code's bytes, 0x3c the constants' bytes, 0x40 the constants' offset (the
code's bytes rounded up to 256; the constants are bank 1), 0x4c the registers, the stage in the halfword at 0x56, and
a compute program's workgroup size at 0x58, 0x5c and 0x60. Programs run to 250 KB.

Bytes 0xa8 to 0xbb are the driver's record of a compute program's `c[0]`. Byte 0xa8 is the kind. A kind 0 program
reads the driver's table as on the Switch 1: the grid at 0x200, uniform buffers from 0x210 and storage buffers from
0x310, 16 bytes each, and its handles in bank 2. A kind 1 program reads a packed `c[0]`, which the driver's macro
`0xeac` writes: its bytes rounded up to 16, the grid, the root tables (push constants), the uniform and storage
buffers, then the low words of its texture, separate texture, separate sampler and image handles, as many of each
as bytes 0xa9 to 0xb6 count. `c0_packed` in `gpu/nvn/nvn_draw.c` builds it. The byte counts add up to the size in the
halfword at 0xba for every program checked. The driver's
`nvnProgramSetShaders` (sdk offset `0x5bf8b0`) reads them, and it checks the version and the format at `0x5bfde8`.
One control section also holds SPIR-V 1.6 at `+0x128`.

## Surfaces

A Switch 2 game's block linear surfaces use another GOB: bits 5 and 6 of a byte's offset in it are swapped, so x
bit 4 goes to bit 6 and y bit 1 to bit 5, for every format (`g_gob_native` in `gpu/texture/blocklinear.h`). Eden's
`native_tiling` does the same. With the Maxwell GOB, the game's ASTC and BC textures came out as a weave of 4x4
blocks, alternate block columns one block row off.

What found it: a RenderDoc capture (the runtime's `renderdoc` setting, run under XWayland with
`SDL_VIDEO_DRIVER=x11`, because RenderDoc has no Wayland surfaces) and `qrenderdoc --python` to save each pass's
targets and inputs. That showed the texture itself wrong before any draw. Then the texture's guest bytes, decoded
off line with each order of the GOB's address bits, gave one order with clean block edges.

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
