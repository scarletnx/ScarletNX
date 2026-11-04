# Ounce bring-up

## Index

* [The GPU path](#the-gpu-path)
* [The platform](#the-platform)
* [Shaders](#shaders)
* [Surfaces](#surfaces)
* [Driver offsets](#driver-offsets)
* [Compute QMDs](#compute-qmds)

This page records what the bring-up of the first Ounce game found so far. It names one game's driver offsets, but
not the game.

## The GPU path

The game does not drive the GPU with Maxwell command buffers. Its 3D engine is class `0xC797` (Ampere B, subchannel 0
on channel 21), and it uploads Turing macros (methods 0x45 to 0x48). The Maxwell path reads `PROGRAM_REGION`
(0x582 and 0x583), which the game never writes, so that path can't render it.

The game draws through NVN2, API version 231.16 (GPU code 201.5 to 201.11). The `sdk` module exports
`nvnBootstrapLoader`, the Switch functions and about 170 new ones: mesh draws, ray tracing, DLSS, device command
buffers, device messages, variable rate shading and HDR window metadata. ScarletNX takes these calls with
`nvn_direct=1`.

The game must be compiled ahead of time (`build/scarletnx -r ...`). With the JIT alone, the dispatch slot of
`nvnBootstrapLoader` is still empty when `nvn_direct_init` runs, because that runs before `jit_init`.

## The platform

The NPDM tells an Ounce program (`snx_npdm_ounce` in `loader/ounce.h`). An Ounce ACID's signature and key are 0x180
bytes each, where a Switch's are 0x100, so its magic is at 0x300 of the ACID, not at 0x200. The recompiler and the
runtime read the same NPDM, and `g_ounce` is the one flag. It gives the game these, with no setting:

- the CPU extensions of `cpu_extensions=1` (LSE, RCpc and PAuth), in the recompiler and the runtime JIT alike
- the 10 GiB application pool and the 12 GiB heap region of `memory_gb=12`
- SM86 shaders and the T239's GOB
- a 31.25 MHz system tick. The SDK's `nn::os::GetSystemTickFrequency` returns 31,250,000 as a constant, and the SDK
  never reads `CNTFRQ_EL0`. With the Switch's 19.2 MHz, the game's time ran at 0.61 of real time. The GPU's
  timestamps keep their 19.2 MHz counter (`guest_gpu_ticks`).

`tools/nvn_missing.py work/Game` lists the NVN functions of a game's driver that `nvn.hook` doesn't implement for its
API version.

## Shaders

The shader code is Ampere SASS, which the runtime calls SM86 (`gpu/shader/sm86.h`). Its format is the one Volta
(SM 7.0) brought and Ada (SM 8.9) still has: 128-bit instructions, with the opcode in bits 0 to 11 of the low word. Each program ends with `EXIT` (`0x794d`, control word `0x000fea0003800000`), then a `BRA` to itself
(`0xfffffff000007947`, `0x000fc0000383ffff`). The Maxwell decoder can't read it. The SM86 frontend has
Mesa's NAK (MIT, `src/nouveau/compiler/nak/sm70_encode.rs`) as the reference for the encoding.

The program starts at the shader data's GPU address, with no wrapper (the older formats have one of 0x30 bytes). A
compute program starts with its first instruction. A graphics program starts with a 128-byte program header of
version 4: word 0 is `0x020c0481` for a vertex program and `0x000c5482` for a fragment program.

An Ounce control section has `0xC9` at 0x04, where the older formats have 1. Formats 7 and 11 share one layout:
0x30 the header's bytes (0xd8), 0x38 the code's bytes, 0x3c the constants' bytes, 0x40 the constants' offset (the
code's bytes rounded up to 256; the constants are bank 1), 0x4c the registers, the stage in the halfword at 0x56, and
a compute program's workgroup size at 0x58, 0x5c and 0x60. Programs run to 250 KB.

Bytes 0xa8 to 0xbb are the driver's record of a compute program's `c[0]`. Byte 0xa8 is the kind. A kind 0 program
reads the driver's table as on the Switch: the grid at 0x200, uniform buffers from 0x210 and storage buffers from
0x310, 16 bytes each, and its handles in bank 2. A kind 1 program reads a packed `c[0]`, which the driver's macro
`0xeac` writes: its bytes rounded up to 16, the grid, the root tables (push constants), the uniform and storage
buffers, then the low words of its texture, separate texture, separate sampler and image handles, as many of each
as bytes 0xa9 to 0xb6 count. `c0_packed` in `gpu/nvn/nvn_draw.c` builds it. The byte counts add up to the size in the
halfword at 0xba for every program checked. The driver's
`nvnProgramSetShaders` (sdk offset `0x5bf8b0`) reads them, and it checks the version and the format at `0x5bfde8`.
One control section also holds SPIR-V 1.6 at `+0x128`.

A compute program reaches its storage buffers through 64-bit addresses (`LDG`, `STG`), with no check against a
size. Of about 400 storage buffers of one game's programs, 5 read the size word after their address, so the size
the game binds doesn't bound the accesses: binding exactly that size hung the GPU, with accesses past it. The
renderer binds all of the memory mapped from the address, up to 16 MiB (`vk_gbuf_desc`).

So such a program's storage is not made for each dispatch. Its stores log the bytes they write: the translator has
every store and atomic of a compute program OR a bit for each byte it writes into a mask (`mark_written`, binding
`SNX_VKB_WPAGES`), at the buffer's base plus the store's offset, the bases and the buffers' lengths in a uniform
(`SNX_VKB_MBASE`). A store at or past its buffer's length, which the binding drops, sets none.

Where the device has sparse residency of buffers and write tracking is on, the program reads and writes a mirror
of guest memory in the GPU's memory (`vk/mirror.c`): one sparse buffer as big as the guest's address space, at the
same offsets, with the mask beside it, memory bound in granules of 2 MiB where bindings are. Windows over the same
guest memory are the same bytes, and the programs run one after another on the graphics queue, as on the console.
A host shadow keeps the guest bytes each page was given. When a buffer is bound, its pages the CPU wrote since
(write tracking) are compared with the shadow, and only the bytes the CPU changed are copied in, so the bytes the
programs wrote there and haven't landed stay. At each submit a pass lists the pages written in it, those with mask
bits set, with their masks, and clears the masks (`mirror_harvest.comp`). Once the submit is done, where the host
traps guest memory, each listed page stays GPU-owned in the mirror (`own_entries`): the CPU's changes to it since it
was brought up to date, the bytes its programs didn't log, are copied into the mirror first (a byte both wrote keeps
the program's), then the page is published over guest memory with the mirror as its owner (`docs/GPU_OWNERSHIP.md`).
The CPU's first touch of it lands it whole, which is exact as none of its bytes can change in between, and the mirror
lets go of its shadow then (`vk_mirror_landed`). Pages the CPU never reads never come back. Without traps the pass
copies the pages' bytes too, and they land in guest memory and in the shadow once the submit is done, the bytes logged
and no others. Landing every written page at each submit took most of the GPU thread in one game's menus. A read of guest memory under
pending writes lands them first, and a draw reading them takes the mirror instead (`buffer.c`'s `buf_get`).
Before, every writing dispatch copied the 16 MiB of each range twice (its original bytes, and its input), compared
all of it after the dispatch and copied it back from the GPU, and the per-dispatch buffers took gigabytes of rings.

Without the mirror (no sparse residency of buffers, memory shared with the CPU, no write tracking) each writer's
result is made for the dispatch as before and lands from a log of the dispatch's own (`vk/writelog.c`), a mask
for each buffer's 16 MiB. A writer that logs nothing (a draw, a Switch program, a buffer image) is compared.

A byte the CPU writes while the programs' writes are pending, which a program writes too, takes the program's value:
on the console both reach memory in an order the program left open. Once the submit is done and the page is the
mirror's, the CPU's write traps and lands the page first, so the CPU's value comes last. A byte the CPU writes and the
program doesn't is kept. The comparison took the CPU's value instead; over 80 seconds of one game, 14,000 results
landed with no byte written by both.

The translator declares the global buffers `Aliased` and `Coherent`: one game binds a range and a part of it to the
same program, writes through one and waits for the value through the other.

## Surfaces

An Ounce game's block linear surfaces use another GOB: bits 5 and 6 of a byte's offset in it are swapped, so x
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

Push constants write `table | offset << 8` to method 0x141 of the 3D or compute engine (`SET_ROOT_TABLE_SELECTOR`),
and the words to 0x142 (`LOAD_ROOT_TABLE`). The driver passes the API's offset, in bytes, and it cuts a push at 256
bytes, a table's size. Programs read the eight tables as banks 24 to 31, which the renderer binds as one uniform
buffer (`SNX_VKB_ROOT`). Variable rate shading writes the shading rate image and palette, which the runtime leaves at the full rate.
`SetGeometryProcessingMode` writes method 0xe26.

## Compute QMDs

The compute QMD is version 3.0, which NVIDIA's `open-gpu-doc` documents (`classes/compute/clc7c0qmd.h`). The
dependence counter is word 4, bits 14 to 29. The PCAS action goes to method 0xB0 (`SEND_SIGNALING_PCAS2_B`, byte
0x2C0): 0x2 schedules, 0x7 decrements the dependence and 0xC increments it. The game runs a chain of 7 empty QMDs on
channel 22 and a ring of 16 on channel 23. Their increments and decrements don't balance in the runtime yet.
