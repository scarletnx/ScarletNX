<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# The NVN investigation

*Snapshot: this page records what the investigation that led to the NVN loader found, as it was found. The code has
changed since, and the page is not kept up to date. The current design is in [NVN](../layers/gpu/nvn.md).*

## Why the first replay was abandoned

This section describes a replay that ran beside the Maxwell decoder, after the driver had written the
command memory. The loader avoids its problems, because it takes each NVN function before the driver can write
anything. The investigation built about 14,000 lines of opt-in probes: a recorder of NVN calls, a replay of those calls on the
GPU thread, a second Vulkan renderer with its own image cache, and pixel comparisons against the Maxwell path. It
reached byte-exact intermediate targets in all four titles, but the approach could not become a renderer:

- **Not every command is in a recorded list.** Catherine's `nvnQueuePresentTexture` submits queue-side packets
  around the recorded command segments, and the tail sets the window-origin register (`0x4eb`) for the next
  submission. TotK submits 65 depth-writing macro 38 draws in GPFIFO entries that lie outside every recorded command
  span, inside the same `nvnQueueSubmitCommands`. A list of NVN calls cannot reproduce such a submit.
- **TotK inlines NVN.** `BeginRecording` is inlined in game code at `0x8714394` and `0x87164ac`, which write the same
  fields as the exported function at `0xd15a480`. Draws, constant-buffer binds and vertex ranges also appear in
  cursor gaps between exported calls. Even `--hook-direct-calls` sees none of this.
- **Post-submit observation is too late.** Replay ran after `QueueFlush` or the present. By then the 64×64 depth
  surface's address named a 1280×720 surface, generated texture handles had changed, the window-origin tail had run,
  and Maxwell had already rewritten guest memory the replay meant to read.
- **It duplicated the decoder.** To close each gap the probes grew a parser of Maxwell methods, MME argument
  decoders, register tracking and write provenance. That is `gpu.c`, `maxwell/` and `vk/` written a second time, and
  running both at once made coherence a problem of duplicate writes.

## What the first runs found (one game, SDK NVN 55.15)

With the driver beside nvn, the game ran through its title and save load. The results the driver answered
otherwise, which the game sizes its memory by:

| Call | nvn | Driver |
| --- | --- | --- |
| `nvnQueueBuilderGetQueueMemorySize` (defaults, compute memory set) | 0x100000 | 0x109000 |
| `nvnTextureBuilderGetStorageClass` | 0 | 0x100, 0x1fe |
| `nvnTextureBuilderGetStorageSize` | 0x200, 0x18000 | 0x10000, 0x20000 |
| `nvnTextureBuilderGetStorageAlignment` | 0x200 | 0x10000 |
| `nvnTextureBuilderGetZCullStorageSize` | 0x100, 0x200, 0x2c00 | 0x460, 0x860, 0xafe0 |
| `nvnDeviceGetTextureHandle` | 0x10300107 | 0x110300107 (bit 32 set) |
| TIC word 6 of registered textures | 0 | 0x03000000 |

The texture rows are fixed ([Texture storage answers](../layers/gpu/nvn.md#texture-storage-answers)). The queue memory size is fixed too (`nvn_queue.c`): the driver's
`QueueInitialize` fails with less. Its rule (`sdk`'s `GetQueueMemorySize` of NVN 55.15) adds to the command and control
sizes (defaults 0x10000 and 0x4000) an area of its own, rounded up to 4 KiB:

- the fragment interlock buffer, unless the queue flags have `NO_FRAGMENT_INTERLOCK`: 1 KiB for each GPC and 4 bytes
  for each warp of each SM, rounded up to 0x100;
- 0x7B00 bytes of the driver's own (0x9400 with `ENABLE_SEPARATE_SAMPLER_TEXTURE_SUPPORT` on the device);
- the ZCULL context, unless the flags have `NO_ZCULL`: the area and the context size each rounded up to 4 KiB;
- the compute memory (default 0x40000), in whole blocks of 0x800 bytes (0xD00 with separate samplers).

The six device words the rule reads are nvdrv's answers, which the driver keeps in its device: 1 GPC
(`num_gpc`), 2 SMs (`NUM_VSMS`), 0x80 warps (`sm_arch_warp_count`), the ZCULL context size 1 (`ZCULL_GET_CTX_SIZE`)
and its 4 KiB alignment, and the device flags.

## Emulator fixes kept from the investigation

| Fix | Where |
| --- | --- |
| A render-target alias of an 8×8 cubemap mip is padded to a 16×8 surface (64-byte GOB rows). The importer accepts that shape and copies the logical extent | `vk/texture.c` `tex_take_drawn` |
| Depth-only D16 and D32F textures import from render targets through the depth aspect | `vk/texture.c` |
| Four-byte packed depth/stencil uploads: G24R8 (`0x0e`) and Z24S8 (`0x29`) keep stencil in the low byte, X8Z24 (`0x2a`) and S8Z24 (`0x2b`) depth in the low 24 bits | `vk/texture.c` |
| A BRX resolves its jump table when the LDC is up to 64 instructions back, with only a short list of instructions that don't write the index register in between and no control flow before it. TotK has the LDC at instruction 93 and the BRX at 125 | `shader/shader.c` `brx_far_ldc` |
| Compute code is bounded by the NVN header's declared size (magic `0x12345679` at code - 0x100), so an unresolved BRX cannot scan into neighboring programs | `vk/compute.c` |
| Storage results are read back from host-cached memory | `vk/buffer.c`, `vk/cmd.c` |
