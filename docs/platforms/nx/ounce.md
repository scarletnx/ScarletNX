# Ounce bring-up

## Index

* [The GPU path](#the-gpu-path)
* [The platform](#the-platform)
  * [GameChat's images and the camera](#gamechats-images-and-the-camera)
* [Built-in fonts](#built-in-fonts)
* [Shaders](#shaders)
* [Surfaces](#surfaces)
* [Driver offsets](#driver-offsets)
* [Compute QMDs](#compute-qmds)
* [Mesh shaders](#mesh-shaders)
  * [Stages](#stages)
  * [The programs' I/O](#the-programs-io)
  * [Draws](#draws)
  * [Design](#design)
  * [Open questions](#open-questions)

This page records what the bring-up of the first Ounce game found so far. It names one game's driver offsets, but
not the game.

## The GPU path

The game does not drive the GPU with Maxwell command buffers. Its 3D engine is class `0xC797` (Ampere B, subchannel 0
on channel 21), and it uploads Turing macros (methods 0x45 to 0x48). The Maxwell path reads `PROGRAM_REGION`
(0x582 and 0x583), which the game never writes, so that path can't render it.

The game draws through NVN2, API version 231.16 (GPU code 201.5 to 201.11). The `sdk` module exports
`nvnBootstrapLoader`, the Switch functions and about 170 new ones: mesh draws, ray tracing, DLSS, device command
buffers, device messages, variable rate shading and HDR window metadata. ScarletNX always takes these calls; the
`nvn_direct` setting older versions read for that is deprecated ([Deprecations](../../user/deprecations.md)).

The game must be compiled ahead of time (`build/scarletnx -r ...`). With the JIT alone, the dispatch slot of
`nvnBootstrapLoader` is still empty when `nvn_loader_init` runs, because that runs before `jit_init`.

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

### GameChat's images and the camera

The update of a racing game (a newer SDK than its first version) starts GameChat at boot, and makes its camera video
storage with `nn::chat::gsv`: three NV12 images of 384x216 (kind 1) in each storage. The SDK sends TIPC types 1307,
1227 and 1228, which are gsv commands 1291, 1211 and 1212. `CalculateRequiredBufferMemory` (1291), `CreateImagePool`
(1211) and `DestroyImagePool` (1212) send the request of the decoder's 991, 911 and 912, and read the same answer. The
chat's pools are thus the video service's pools (`native_video.c`).

NVN makes a memory pool of each image with `nvnNativeImageInitialize`. That function asks `QueryNativeImage` (428)
for the image's page span, and gives it to `NvRmMemHandleAllocNativeImage`, which makes an NvRm memory handle for the
image. Then it gives the handle to `nvnMemoryPoolBuilderSetNativeHandleNVX`. The SDK's NvRm numbers its handles from
1, for each handle it makes, so the handle is not a native memory or an image number. The runtime's
`nvnMemoryPoolInitialize` asks the SDK's `NvRmMemGetNativeImageHandle` for the image, as the driver does. Before, it
took the handle for a native memory number, which was right only while the two numbers agreed. Another game, whose
title movie uses the video pools, makes no such pool.

The camera session (gsv 602) answers each command but 21 with result 0xdc4 (module 452, description 6), as a console
with no camera. The game's own camera module accepts it from command 3 (the camera "Default") and from command 29,
which it sends again and again. Command 21 must give an event, which the module attaches as a system event. The module
stops if 21 fails. With no camera, the event is never signaled.

## Built-in fonts

An Ounce game's SDK has `nn::pl::GetBuiltInFontData(BuiltInFontDataOunce*, BuiltInFontTypeOunce, bool)` beside the
shared fonts' calls. The runtime answers it in the game's own SDK function (`src/nx/nn/pl.c`, `pl.hook`), as it does
the shared fonts': it fills the caller's `BuiltInFontDataOunce` as the SDK fills it from the service's answer, two
bytes of flags, the font's address at +8 and its size at +0x10, and answers whether the type has a font.

The fonts are the shared fonts: each is a plain TrueType font after its header, so the same memory, mapped read only
in the game at the first ask, answers both. `tests/pl_fonts.c` checks the structure for each type.

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
a compute program's workgroup size at 0x58, 0x5c and 0x60, and its shared memory bytes at 0x64 (0 in most
programs, up to 0xc000). Programs run to 250 KB. A compute program has no NVN
header with its size before it, so the renderer reads as many bytes as its constants' offset gives, up to 512 KiB
(`cs_read_code`). With the Switch's 64 KiB window, one game's compute program of 256,160 bytes had no end in it, and
each of its dispatches (one workgroup of 32, about one a frame) was skipped as not translated.

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
every store and atomic of a compute program set a bit for each byte it writes in the dispatch's write log
(`mark_written`, binding `SNX_VKB_WPAGES`, the layout `SH_WLOG_*` in `shader.h`): a part for each buffer it writes,
a flag for each 4 KiB window of the buffer, then the window's bits. A store past the windows sets none. The Maxwell
translator's compute programs log the same way (`write_log.c`, which both translators call), so a Switch game's
results land from the log too. They don't take the mirror below: in one Switch game, props that a compute program
culls vanished for single frames with it where the host traps guest memory (`mirror_plan` has the counts).

Where the device has sparse residency of buffers and write tracking is on, the program reads and writes a mirror
of guest memory in the GPU's memory (`vk/mirror.c`): one sparse buffer as big as the guest's address space, at the
same offsets, with a mask beside it, a bit for each byte, memory bound in granules of 2 MiB where bindings are.
Windows over the same guest memory are the same bytes, and the programs run one after another on the graphics queue,
as on the console. After each dispatch a pass ORs the bits its log gives into the mask at each buffer's place, the
bits past the buffer's length dropped (`mirror_fold.comp`, `vk_mirror_fold`). Only the GPU reads such a log, so it is in
the GPU's memory (`vk_wlog_get_gpu`) and cleared there after the pass (`vk_wlog_clear`). In host memory each store's
log write, a store and an atomic OR, crossed the bus on a discrete GPU: on an RTX 5080 one upscaler program took
47 ms a dispatch for 0.14 ms, and each pass, which read the log, 14 ms for 0.3 ms. A merge of the main branch had
lost the dispatch's side of the mirror for a while: each writing dispatch made its 16 MiB buffers again, its input
copied on the CPU and composed on the GPU over the results before it, hundreds of them a frame at addresses a few KiB
apart, and one game's races ran at about one frame a second.
A host shadow keeps the guest bytes each page was given. When a buffer is bound, its pages the CPU wrote since
(write tracking) are compared with the shadow, and only the bytes the CPU changed are copied in, so the bytes the
programs wrote there and haven't landed stay. At each submit a pass lists the pages written in it, those with mask
bits set, with their masks, and clears the masks (`mirror_harvest.comp`). Once the submit is done, where the host
traps guest memory, each listed page is queued, still pending (`own_entries`), and stays GPU-owned in the mirror from
the next point where nothing is being recorded (`vk_mirror_publish`): before the releases its submits gate let the game
see them, at the end of a GPU job, and before a CPU read of them. There the CPU's changes to it since it was brought up
to date, the bytes its programs didn't log, are copied into the mirror (a byte both wrote keeps the program's), and
once that copy is submitted the page is published over guest memory with the mirror as its owner
(`docs/layers/gpu/ownership.md`). Nothing of this is recorded where the results land: that runs inside a frame's
recycling, a submission and a draw's setup, and recording there went into a command buffer being reset or submitted,
which crashed in the driver. The pass for the pages past the harvest's room runs there, in a command buffer of its own,
and takes its descriptor sets from a pool of its own: the frame's sets (`vk_desc_alloc`) started the frame's command
buffer inside the landing after a present's submission. It stayed open, and when its slot came round again the draws
recorded into it past the recording thread, with sets only that thread can resolve. The CPU's first touch of a page
lands it whole, which is exact as none of its bytes can change in between, and the mirror lets go of its shadow then
(`vk_mirror_landed`). Pages the CPU never reads never come back. Without traps the pass copies the pages' bytes too, and
they land in guest memory and in the shadow once the submit is done, the bytes logged and no others. Landing every
written page at each submit took most of the GPU thread in one game's menus. A read of guest memory under pending writes
lands them first, and a draw reading them takes the mirror instead (`buffer.c`'s `buf_get`), as does a draw reading a
range part of which the mirror owns (`vk_mirror_holds`): read elsewhere, the CPU's bytes between the owned pages were
staged again for each draw.
Before, every writing dispatch copied the 16 MiB of each range twice (its original bytes, and its input), compared
all of it after the dispatch and copied it back from the GPU, and the per-dispatch buffers took gigabytes of rings.

Without the mirror (no sparse residency of buffers, memory shared with the CPU, no write tracking) each writer's
result is made for the dispatch as before and lands from the dispatch's write log (`vk/writelog.c`), which the host
reads once the submit is done. A writer that logs nothing (a draw, a buffer image) is compared.

A byte the CPU writes while the programs' writes are pending, which a program writes too, takes the program's value:
on the console both reach memory in an order the program left open. Once the submit is done and the page is the
mirror's, the CPU's write traps and lands the page first, so the CPU's value comes last. A byte the CPU writes and the
program doesn't is kept. The comparison took the CPU's value instead; over 80 seconds of one game, 14,000 results
landed with no byte written by both.

The translator declares the global buffers `Aliased` and `Coherent`: one game binds a range and a part of it to the
same program, writes through one and waits for the value through the other.

A program may also reach a buffer through a pointer in memory. One game's material programs load the addresses of
five tables from a table whose own address is in cbuf 0 (`LDG.E.64 R, [UR + 8]`), then index them (`IMAD.WIDE`), and
its compute programs pick one of two such pointers with a predicate. The tables are gigabytes away from the table
that points to them, outside its 16 MiB. The translator names such a buffer by its pointer's cell (`SH_GBUF_INDIRECT`
in `shader.h`, the cell's offset in the buffer of a constant buffer word): the renderer reads the pointer from guest
memory when it binds the draw or dispatch, a pending write there landed first (`gbuf_pointer` in `vk/render.c`), and
binds the memory mapped from it as a bare address's, and the program finds the buffer's base by loading the cell
itself. An address that may be either of two pointers is compared with each buffer when it runs
(`global_dynamic`). A cell that the GPU writes later in the stream than the bind would be read too early: the program
would then index the window bound from the older pointer. Each such buffer is another window of up to 16 MiB bound for
the draw, with the in-place bookkeeping of its pages. A general path (physical storage buffers,
`VK_KHR_buffer_device_address`) would need every guest page a pointer can reach imported and tracked, which the
import limits and the ownership model above don't allow.

`RED` and `ATOMG` of `F32` add (`.FTZ.RN`) are one `OpAtomicFAddEXT` where the device has
`shaderBufferFloat32AtomicAdd`, else a loop of compare and exchange on the word's bits.

A texture's handle may come from memory too. One game's material programs take each texture's from a table of
materials in a storage buffer: an `LDG.E.64` at a record that the draw's constant buffers pick (in one program
`c[0][0x520] + ((c[4][0x88] & 0xfffff) + c[5][0x2ec]) * 32 + 0x18`, with a `SEL` on an `ISETP` of the index's top
bits), then `R2UR` into the uniform register pair of a `TEX`, `TLD`, `TLD4` or `TXQ` whose handle is in a uniform
register (0xf60, 0xf66, 0xf63, 0xf6f: the texture's word, then the sampler's index, the word `>> 20`), or into a
`TEX.B`'s handle register. The translator refused these programs: about 320 draws a frame of the first 3D scene, which
was then a white screen. It now keeps the instructions that make each such handle in `ShaderInfo.memh`, where every
value they read is the draw's (constant buffer words, immediates, and words loaded from addresses made of them), and
the slot names the value that is the handle (`SNX_TEX_MEMORY`, `SH_PROBE` in `shader.h`): a register as one of those
instructions leaves it. Each value one of them reads has one unguarded writer before it on every path, and no other
kept instruction between the two writes it, so the list run in program order gives what the program computes. The
renderer runs it on the CPU for each draw (`sm86_eval` in `gpu/shader/sm86_eval.c`, the translation's arithmetic,
which a test checks against the translated program), on the draw's constant buffers and guest memory, a pending write
there landed first, and binds the handle as a constant buffer's (`memh_handle` in `vk/render.c`, `vk_memory_handles`
for dispatches). As with `gbuf_pointer`, a word the GPU writes later in the stream than the draw is recorded is read
as it was then: the material tables are the CPU's, but a table a program wrote in the same frame would give the
older handles. A handle whose address differs from invocation to invocation (one game's lights and decals: a cell of
a grid in world space, then the cell's list, in a loop) is bound as no texture, which samples zeros
(`SH_PROBE_NONE`), and the log names the program once: the draw goes on without it. An array of the pool's textures
indexed on the GPU would sample them, but the renderer would have to make each texture such a handle can name, which
no draw tells it.

## Surfaces

An Ounce game's block linear surfaces use another GOB: bits 5 and 6 of a byte's offset in it are swapped, so x
bit 4 goes to bit 6 and y bit 1 to bit 5, for every format (`g_gob_native` in `gpu/texture/blocklinear.h`). Eden's
`native_tiling` does the same. With the Maxwell GOB, the game's ASTC and BC textures came out as a weave of 4x4
blocks, alternate block columns one block row off.

What found it: a RenderDoc capture (the runtime's `renderdoc` setting, run under XWayland with
`SDL_VIDEO_DRIVER=x11`, because RenderDoc has no Wayland surfaces) and `qrenderdoc --python` to save each pass's
targets and inputs. That showed the texture itself wrong before any draw. Then the texture's guest bytes, decoded
off line with each order of the GOB's address bits, gave one order with clean block edges.

A game's program that makes a texture's levels from its first has seven image slots, one for each level. The game
leaves the slots past the texture's last level empty: their handle is 0, and the header there is all zeros. The
renderer binds an empty storage image there (`vk_null_storage`). Before, it skipped the whole dispatch. One game draws
its kart's contact shadow as a box that samples a 128x128 texture array with trilinear filtering. Layer 0 is the kart's
footprint, drawn into level 0 from a 256x256 texture, and this program writes levels 1 to 3. Without them the box read
zeros at the title camera's distance and drew a dark square under the kart.

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

## Mesh shaders

The game draws part of its frames with mesh shaders: about 14 `nvnCommandBufferDrawMeshTasks` calls a frame in the
screens after the title, all from the programs of `Shader/Drive.Nin_Ounce_NVN2.release.sarc.zs`
(`DrivePassShaderDefault.sharcb`, the 3D scene's passes). The renderer can draw them with `VK_EXT_mesh_shader`
behind the experimental `mesh_draws` setting, off by default (see [Draws](#draws)). No font or UI program
(`Lib/sead/nvn2_font`, the programs in `main`) is a mesh program, so the missing menu text has another cause. Of the
programs that the shipped pass translates, about 500 fragment and vertex programs are refused (most with "BRX with no
jump table"), which is a better lead for the text.

### Stages

An Ounce control section names the program's stage in its halfword at 0x56: 0 vertex, 1 fragment, 5 compute, 6
task, 7 mesh (`NVN_SHADER_STAGE_TASK` and `NVN_SHADER_STAGE_MESH`). The SPH cannot tell them, as NVIDIA's hardware
runs a task program in the vertex slot and a mesh program in the tessellation slot when a task program launches it,
else in the vertex slot (Mesa's NAK, `nak/sph.rs`, and NVK's `mesa_to_nv9097_shader_type`):

| Program | SPH word 0 | Section 0x56 |
| --- | --- | --- |
| vertex | `0x020c0481` (bit 25, ISBE space sharing, set) | 0 |
| task | `0x000c0481`, or `0x000d0481` with global stores | 6 |
| mesh without a task program | `0x000c0481` | 7 |
| mesh after a task program | `0x020c0c81` (type 1, shader 3) | 7 |
| per-primitive outputs of a mesh program | `0x010c1081` (geometry, passthrough bit 24) | none |

`sm86_sph_stage` thus gives ST_VS or ST_TES for these programs, never -1. `stage11` (`gpu/nvn/nvn_object.c`) now takes
the section's stage, so a mesh program no longer replaces the vertex or tessellation stage of its program. The
section's stage and the SPH agree for every other program of two runs.

A mesh or task program's section also gives (`gpu/nvn/nvn_section.h`):

| Offset | Field | Values seen |
| --- | --- | --- |
| 0x58 (u16) | invocations of the workgroup | 8, 32 |
| 0x5a (u16) | task payload bytes, in 128-byte lines; a mesh program alone has 0x80 | 0x80, 0x100, 0x580 |
| 0x5e (u8) | 1 when a task program launches the mesh program | |
| 0x5f (u8) | vertices of a primitive | 3 |
| 0x60, 0x62 (u16) | most vertices, most primitives | 32/16, 128/64, 256/192 |
| 0x64, 0x68 | offset from the start of the program's code area (its own SPH included) and bytes of the per-primitive SPH (0 for none) | 0xd00, 0x80 |

The offset counts from the start of the code area, the program's own SPH included. A code area of 0xe00 bytes (0x44)
has the per-primitive SPH at 0xd00, and its last 128 bytes are zero. One of 0x4900 bytes has it at 0x4800. The dumps
of 11 mesh programs show the SPH word `010c1081` (or `010d1081`) there in each program whose offset is not 0. The
renderer read it at 0x80 + the offset before, where the code area holds zeros, and so it dropped every per-primitive
output. Its OMAP gives the per-primitive output words, and the mesh SPH's OMAP the per-vertex ones.

The fields do not go in the program object. The Ounce's program object is 0xa0 bytes: the driver's
`nvnProgramInitialize` (sdk offset `0x5bf2c0`) zeroes that many and uses offsets up to 0x98. The runtime keeps them,
and a compute program's c[0] record, in a table by the program's SetShaders serial (`nvn_program_mesh`,
`nvn_program_c0`). Before, the c[0] record was at bytes 0x98 to 0xab of the object, and each SetShaders wrote 12 bytes
of the game's memory after it. The game's own bytes there (0xc2 in one run) then took the place of the mesh
programs' per-primitive SPH offset.

### The programs' I/O

Every output, input and count goes through the ISBE (the "internal stage buffer entry"), with `ISBERD` (0x923) and
`ISBEWR` (0x927), as NAK lowers them (`nak_nir_lower_mesh_intrinsics.c`). The fields (NAK's `sm70_encode.rs`): 24-31
the offset register, 32-39 the data (ISBEWR), 40-55 an immediate offset, 74-75 the size (U8, U16, B32), 76-77 the
access (MAP, PATCH, PRIM, ATTR), 78 SKEW, 79 output. The game's programs use no LDS, STS or BAR, and every workgroup
is one warp (at most 32 invocations, `S2R SR_LANEID` as the invocation).

| What | Instruction |
| --- | --- |
| workgroup index (task, mesh alone) | `ISBERD.ATTR.SKEW [RZ+0x0]` (the vertex ID of the point draw) |
| workgroup index (mesh after a task) | `ISBERD.ATTR [RZ+0x0]` |
| task payload, task side | `ISBEWR.ATTR.O [Rn+0x10+...]`, from byte 0x10 |
| mesh groups to launch | `ISBEWR.ATTR.O [RZ+0x4]` (one count, x only; NAK also writes x, y, z at 0x8 to 0x10) |
| task payload, mesh side | `ISBERD.ATTR [Rn+0x10+...]`, the same offsets |
| per-vertex output | `ISBEWR.ATTR.SKEW.O` at `(v / 32) * G + (v % 32) * 4 + k * 0x80`, word k of the used words in OMAP order, G = 0x80 times their count |
| per-primitive output | `ISBEWR.ATTR.SKEW.O` at `V + (p / 32) * P + (p % 32) * 4 + k * 0x80`, V the vertex area (G times the vertex groups), P from the per-primitive SPH |
| primitive indices | `ISBEWR.MAP.O.U8 [4 + 3 p + i]` |
| primitive count | `ISBEWR.MAP.O.B32 [RZ+0x3]`; no vertex count is written |
| draw parameters, buffers | constant banks as in vertex programs (`c[0][0x670]`, `c[0][0x680]`, banks 3 to 5), `LDG`/`STG` |

A mesh program after a task program, 8 invocations, 32 vertices of position and generic 1.xy, 16 primitives with
generic 0.x per primitive:

```
ISBERD.ATTR.B32 R3, [RZ+0x0]                 ; this group's index in its task's launch
IMAD R3, R3, 0x28, RZ                        ; its payload record
ISBERD.ATTR.B32 R19, [R3+0x20]               ; payload words
ISBEWR.ATTR.SKEW.O.B32 [R7+0x0], R16         ; position.x of vertex R7/4 (G = 6 words * 0x80 = 0x300)
ISBEWR.ATTR.SKEW.O.B32 [R7+0x180], R6        ; position.w
ISBEWR.ATTR.SKEW.O.B32 [R7+0x200], R2        ; generic 1.x
ISBEWR.MAP.O.U8 [R4+0x4], R9                 ; R4 = lane * 6: two triangles a lane
ISBEWR.ATTR.SKEW.O.B32 [R0+0x0], R3          ; R0 = 2 * lane * 4 + 0x300: generic 0.x per primitive
MOV R0, 0x10 ; ISBEWR.MAP.O.B32 [RZ+0x3], R0 ; 16 primitives (lane 0)
```

Its task program, 32 invocations, one payload record of 0x28 bytes for each meshlet that passes the culling:

```
ISBERD.ATTR.SKEW.B32 R4, [RZ+0x0]            ; workgroup index
S2R R15, SR_LANEID ; IMAD R4, R4, 0x20, R15  ; global invocation
VOTEU.ANY ; POPC R6 (lanes below)            ; compacted slot
IMAD R1, R6, 0x28, RZ
ISBEWR.ATTR.O.B32 [R1+0x10], R2              ; payload, ten words
POPC R0, UR5 ; ISBEWR.ATTR.O.B32 [RZ+0x4], R0 ; mesh groups to launch
```

### Draws

The game calls `DrawMeshTasks(first 0, count 1)`, and `(0, 6)` and `(0, 30)`. It calls no indirect mesh draw in the
runs so far, and it binds its mesh programs with stages `0xff`. `SetGeometryProcessingMode(2)` comes before them.

The driver binds the task and mesh stages apart from the others (sdk offsets of the functions:
`BindUniformBuffer` `0x5db4c0`, `BindStorageBuffer` `0x5dd080`, `BindTexture` `0x5de120`, `BindProgram` `0x5daa80`).
Every function takes stages 6 and 7 through the same path as the graphics stages, with a table for each stage:

| Stage | `BindProgram` bit | uniform buffer group (method 0xe02) | storage buffers in c[0] | handle bank (method 0xe1c) |
| --- | --- | --- | --- | --- |
| vertex 0, fragment 1, geometry 2, control 3, evaluation 4 | bits 0 to 4 | 8, 4, 3, 1, 0xa | 0x110, 0x510, 0x410, 0x210, 0x310 | 0, 4, 3, 1, 2 |
| task 6 | 0x40 | 9 | 0x670 + 16 i | 5 |
| mesh 7 | 0x80 | 0xb | 0x770 + 16 i | 6 |

The handles have the offsets of the other stages in their bank: textures at 0x20 + 8 r, images at 0x120 + 8 r,
separate textures at 0x168 + 8 r, separate samplers at 0x568 + 8 r. One mesh program reads its textures' handles at
`c[2][0x20]` and `c[2][0x28]`. The task programs read their storage buffer addresses at `c[0][0x670]` and
`c[0][0x680]`, the mesh programs at `c[0][0x770]` to `c[0][0x7b0]`. A mesh stage's bindings thus never replace the
vertex stage's: the draw state keeps them apart (`DS_TASK` and `DS_MESH` in `gpu/draw_state.h`), and bank 0 of each
stage is 0x870 bytes.

`DrawMeshTasksIndirect` gives the GPU two words of the indirect buffer (a GPFIFO entry of 2 words after macro 0x31),
`{count, first}` as `NV_mesh_shader`'s `DrawMeshTasksIndirectCommandNV` has them. A compute program writes them in the
same frame. Vulkan's indirect mesh draw reads three, x, y and z, and has no first workgroup, so the renderer copies
the record on the GPU into one of the frame's staging, `{count, 1, 1}` and the first workgroup a uniform buffer's
alignment after it (`mesh_indirect` in `vk/render.c`), which it binds at `SNX_VKB_RS` of the task or mesh stage: where
the push constant's z has `SH_PUSH_GPU_MESH_FIRST`, the task program, or the mesh program without one, takes its first
workgroup from there. The copy ends the rendering pass. In the first 3D scene of one game about 30 mesh draws a frame
are indirect; the 3D form's and the multi-draws, which no run calls, are still skipped.

With `mesh_draws` on, an RTX 2080 faulted in most menu runs: Xid 31 (`GPCCLIENT_GCC`, reads of addresses that no
memory backs), then Xid 109 and a lost device. NV diagnostic checkpoints put each hang in a draw with a task program,
and the device fault data put the faulting instruction in the task program, at the same address in each run. A task
program that does nothing launched no fault. A task program that reads no constant buffer still faulted. Every
program's outputs are within `maxMeshOutputMemorySize`. The faults stopped when the two mesh stages saw the
descriptor sets of both, the vertex stage's (the mesh program's) and the tessellation control stage's (the task
program's): `vk_set_stages` in `gpu/vk/pipeline.c`. The mechanism inside the driver is not known. After that change,
no run had Xid 31, but 2 of 7 menu runs still hung with Xid 109 alone. That hang also came once with the task
programs emptied, and the checkpoints of that run put it in a draw without a mesh program, so it has another cause.

No mesh draw of 16 menu runs (184,239 draws) goes to a cascade of the shadow map (depth only, 3072 by 3072). Each has
one color target. The programs after a task program draw into the scene's color and depth targets (832 by 466 to 1280
by 720), and the others into a color target alone. The mesh program of 128 vertices after a task program samples a
depth array texture with a compare (`TEX.DC`), as a program that receives shadows does.

The driver writes Turing macro calls (method 0xe00 + 2 n is `CALL_MME_MACRO(n)`), not 3D methods:

| Function (sdk offset from 0xbd0c000) | Push buffer |
| --- | --- |
| `SetGeometryProcessingMode` (`0x5e2e10`) | `0x8000_0e26 \| mode << 16` (IMMD, macro 0x13), `0x8000_0e3e` (macro 0x1f, 0) |
| `DrawMeshTasks` (`0x5e2e80`) | `0xa002_0ea2`, count, first (macro 0x51) |
| `DrawMeshTasks3D` (`0x5e3320`) | `0xa003_0ec8`, x, y, z (macro 0x64) |
| `DrawMeshTasksIndirect` (`0x5e2f00`) | `0x91120002`, `0x81900e36`, `0xa002_0e62` (macro 0x31), then a GPFIFO entry of the indirect buffer's words |
| `MultiDrawMeshTasks*Indirect*` | a helper (`0x5e2830`) with macro methods 0x3b40 and 0x3990 |

On the hardware the macros end in NVK's sequence (`nvk_mme_build_draw_mesh`): `SET_DRAW_CONTROL_A` with points,
then `DRAW_VERTEX_ARRAY_BEGIN_END_A` (0x9c) with 0 and the task count x * y * z. Mesh state is `SET_MESH_CONTROL`
(0x453), `SET_MESH_SHADER_A` (0x454: topology, most vertices, most primitives), `SET_MESH_SHADER_B` (0x455: shared
lines, invocations) and `SET_MESH_INIT_SHADER` (0x456: invocations, local lines, payload lines) (`clc597.h`). The
runtime takes the API calls and needs none of the macros.

### Design

The host (an RTX 2080) has `VK_EXT_mesh_shader` with 256 vertices and primitives and 128 task invocations, more than
the game asks for.

1. NVN (done). `NvnProgram` keeps stages 6 and 7 (`NVN_STAGES`) last, in an Ounce game's program object only: another
   game's keeps its own fields from byte 0x80 (`NVN_PROGRAM_SWITCH_BYTES`). SetShaders keeps the sections' fields
   (`DsMeshKey`) by the program's serial (`nvn_program_mesh`). `OP_DRAW_MESH` records {first, count}, a 3D draw as
   x y z workgroups in a row from 0, and an indirect one with the buffer's address, count address and stride. The
   replay (`nvn_draw.c`) makes it a `DrawCall` with `mesh` set, of `DsPrograms`' task and mesh programs and the
   fragment program.
2. Translator (done, `sm86_translate.c`, with its interface in `sm86.h`: `ShaderInfo.mesh` is the key, a task program
   takes descriptor set 2 and a mesh program set 0). New stages ST_TS and ST_MS, given by NVN, not by the SPH. Model the
   ISBE as Workgroup arrays: the output area (`u32 [V + primitive area]`), the map (`u32 [4 + 3 * most primitives]`, a
   byte to a word) and, for a task program, the payload as `TaskPayloadWorkgroupEXT`. `ISBEWR` and `ISBERD` become
   stores and loads at their offsets, so the program's own addressing needs no decode. `EXIT` returns from the program's
   function. An epilogue in `main` after a workgroup barrier then does the outputs:
   - mesh: `OpSetMeshOutputsEXT(most vertices, count)`, the count from map word 3 (0 at the start). Then a loop over
     vertices and primitives that writes `gl_Position`, the generic outputs (`Location` = generic n, per-primitive ones
     `PerPrimitiveEXT`), and `PrimitiveTriangleIndicesEXT` from the map. The skew offsets come from the OMAPs and
     the most vertices, at translation.
   - task: `OpEmitMeshTasksEXT(word 1, 1, 1)` with the payload.
   The workgroup index is `WorkgroupId.x`, from either kind of read at offset 0. `SR_LANEID` is the subgroup
   invocation. Constant banks, textures and global buffers are as in the vertex stage. The 7 task and 12 mesh programs
   of one run translate and pass `spirv-val` (`snx-sm86 --task` and `--mesh`).
3. Fragment linkage. A fragment program reads per-primitive generic n as a plain input. Its module must declare
   them `PerPrimitiveEXT`, so the fragment shader key gets a mask of per-primitive locations from the mesh program
   (`pipeline.c`'s key, `fs_variant` in `render.c`). The translator's half is done: a fragment program's
   `ShaderInfo.prim_mask` names those inputs, the mesh program's `prim_mask`.
4. Vulkan (done but the indirect forms). `device.c` turns on `meshShader` and `taskShader` with
   `subgroupSizeControl` and `computeFullSubgroups`, when the device has a required subgroup size of 32 in the mesh
   and task stages. Else the log says why once, and the mesh draws are skipped. The mesh and task bits join the
   push constants' stages and the visibility of sets 0 and 2. A mesh pipeline (`PipeKey.mesh`) has its mesh shader
   in the vertex slot and its task shader in the tessellation control slot, so the pipeline file needs no new
   fields. It has no vertex input, input assembly or tessellation state, and no topology, restart or vertex input
   dynamic state. Its stages require a subgroup of 32, and whole subgroups when the workgroup is 32 invocations (8
   cannot be). `render.c` draws with `vkCmdDrawMeshTasksEXT(count, 1, 1)` and the first workgroup in push constant
   x. The periodic `vulkan:` stats line counts the mesh draws and those skipped. The storage writes of the two
   stages are in the barriers before their results land (`buffer.c`'s `out_stages`).

On a Radeon 890M (RADV, Mesa 26.2.5) the mesh draws, direct and indirect, ran without a fault in every run so far,
the menus and the first 3D scene. With `RADV_DEBUG=hang`, which waits for each draw (`syncshaders`), the first mesh
draws of the menus hung the GPU twice, once with every task draw of 0 workgroups (the gfx ring, then a compute ring
timed out; the dump's compute IB held an `ACQUIRE_MEM` and `NOP`s, with the shader engines idle): a task program's
draw is a gang submission, its task stage on an ACE queue, which that option's waits stall. Don't take it to a run
with mesh draws.

With `mesh_draws=1` the draws translate and draw, with no `not translated` line and no validation message of their
own, but 6 of 7 runs through the menus on an RTX 2080 (driver 615.78.08) ended in a GPU fault: Xid 109 (context switch
timeout) and Xid 31 (MMU faults of `GPCCLIENT_GCC`, reads at addresses such as `0xfffff079_ad3f2000`), then
`VK_ERROR_DEVICE_LOST`. Runs without mesh draws had none. GPU-assisted validation reported nothing about them. What
did not remove the faults: the per-primitive SPH at the right offset, the fragment inputs no mesh output writes read
as 0, 0, 0, 1, and storage buffers of the two stages bound to all the memory mapped from their addresses. A run that
drew only the mesh programs alone without per-primitive outputs had no fault in 319 frames, which is too few to be
sure. So the setting stays off.

### Open questions

- Which mode values `SetGeometryProcessingMode` takes (2 before mesh draws) and what macro 0x1f does.
- Whether a task program's payload header (bytes 0 to 0xf) holds more than the count at 0x4, and what the hardware
  does when no lane writes the count. The design starts it at 0.
- Whether `first` reaches the index as the first vertex of the point draw does (the game passes 0, so the runs
  cannot show it).
- The cause of the GPU faults with `mesh_draws=1`. Next checks: the per-primitive outputs and the fragment
  program's `PerPrimitiveEXT` inputs (with their interpolation decorations), the primitive indices against the vertex
  count, and a RenderDoc capture of the first faulting draw.
- The 3D form's indirect record (x, y, z), and whether `DrawMeshTasksIndirect`'s is `{count, first}`, as the
  renderer takes it, or the other way round (a GPU-written record, which the CPU can't see when it records the draw;
  the pictures with it look right).
- The texture keys of task and mesh programs: their integer, rectangle and queried textures do not key a variant yet,
  as the vertex program's do (`vs_tex_variant`).
