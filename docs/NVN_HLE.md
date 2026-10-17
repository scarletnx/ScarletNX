<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# NVN reference

Switch games draw through NVN, NVIDIA's graphics API, which runs as guest code. It writes Maxwell pushbuffers,
submits them through nvdrv, and the GPU thread decodes them (`src/nx/gpu/gpu.c`, `maxwell/`) before calling the Vulkan
renderer (`vk/`). This page records what an investigation into handling NVN directly found about the SDK, and what it
decided. Facts marked as observations come from one or two titles and are not rules for every SDK version.

Titles referred to: Catherine: Full Body (USA v2.0, "Catherine"), Tears of the Kingdom (v1.0.0, "TotK"), Super Mario
Odyssey ("Odyssey") and Pokémon Scarlet ("Scarlet").

## Status and direction

**The Maxwell decoder renders, and any faster path builds on it: the MME is what to replace (below).** NVN command memory already
is a Maxwell pushbuffer, so a second front end that reinterprets NVN calls would decode the same words twice. A
disassembly study of the command-buffer functions found how little each call writes:

| Call | Words written |
| --- | --- |
| `nvnCommandBufferDrawArrays` | 4: one MME macro call, header `0xa0030e32`, then mode, first, count |
| `nvnCommandBufferDrawElements` | 6: header `0xa0050e38`, then mode, index address high and low, index type, count |
| `nvnCommandBufferBindTexture` | 4 or 5 (Catherine): an IMMD macro call `0x80000e0e \| table[stage] << 16` (a per-stage value from an SDK table), `0x8000068b` when the command buffer's byte `+0x6c` is set, then `0x200208e3`, `id*8 + 0x20`, handle |

Replacing command-buffer calls with host code therefore saves almost no guest CPU time. The hot path worth replacing
is the MME. A native implementation of a known macro would be chosen by a hash of the macro's uploaded code (slot
numbers are assigned by the SDK's macro upload, so a slot number alone identifies nothing), and an unknown hash would
keep the interpreter. None exists yet. `nvn_control.c` decodes the arguments of two macros and `nvn_command_gap.c`
classifies what methods write, both checked against packets captured in their tests.

### Why semantic replay was abandoned

The investigation built about 14,000 lines of opt-in probes: a recorder of NVN calls, a replay of those calls on the
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

## What remains in the tree

| File | What it is | Built into |
| --- | --- | --- |
| `src/nx/gpu/nvn_trace.c` | `SNX_NVN_TRACE=1` inventory of resolved and called NVN functions | the runtime |
| `src/nx/gpu/nvn_control.c` | decoders for the 12-word macro 38 indexed draw and the macro 12 program bind | `tests/nvn_control.c` |
| `src/nx/gpu/nvn_command_gap.c` | classifier of Maxwell method write effects in a span of command words | `tests/nvn_command_gap.c` |

Every other `SNX_NVN_*` variable is gone. `SNX_GPU_TRACE_TARGET=address[:bytes]` (`vk/render.c`) logs the clears and
draws that write a render target starting in that range, and replaced the older GPU trace variables.

## Observing NVN calls

The SDK exports `nvnBootstrapLoader`, which returns a guest function pointer for an NVN function name. With
`SNX_NVN_TRACE=1`, `nvn_trace.c` replaces that function's entry in the recompiler's dispatch table `g_disp`, then the
entry of the `nvnDeviceGetProcAddress` pointer it returns, then the entry of every address either of them resolves. The
wrappers count calls and always call the original, so rendering is unchanged.

| Log line | Meaning |
| --- | --- |
| `nvn: NAME -> ADDRESS` | a name resolved through either loader |
| `nvn-submit: queue=... count=... lists=... first=...` | an `nvnQueueSubmitCommands` call and its first list handle |
| `nvn-nvnQueue...: GPFIFO submissions during call=N` | GPFIFO submissions, by any thread, while one queue call ran (`SubmitCommands`, `Flush`, `FenceSync`, `PresentTexture`, `WaitSync`) |
| `nvn-used: NAME COUNT` | at exit, the calls to each address that was called |
| `nvn: N resolved addresses, M called` | at exit, the totals |

Limits:

- The wrappers see calls through `g_disp`, which is how NVN function pointers are called. A statically resolved
  branch calls its generated C function directly and is only seen in a module recompiled with
  `--hook-direct-calls`. Counts are lower bounds otherwise.
- Names that fold to one address share a counter, labeled with the first name resolved there.
- Functions the game inlines are never seen (see the TotK example above).
- A title that resolves NVN some other way than `nvnBootstrapLoader` needs another discovery path.

Boot samples resolve about 470 (Catherine) and 530 (TotK) names and call about a third of them. The frequently called
shared functions are buffer-address and texture-handle lookups, texture binds, blend and vertex state, and
command-buffer operations. Counts vary between runs with the same frame limit.

## Queue and submission observations

| Queue call | Catherine | TotK |
| --- | --- | --- |
| `nvnQueueSubmitCommands` | no GPFIFO submission | no GPFIFO submission |
| `nvnQueueFenceSync` | none | none |
| `nvnQueueWaitSync` | none | not recorded |
| `nvnQueueFlush` | not recorded | one per call |
| `nvnQueuePresentTexture` | one per call | one per call |

These are per-call observations, not a guarantee of where the SDK flushes. The queue stages list handles, then walks a
tagged control stream of GPU work and sync operations, so submission is deferred to a flush or a present.

Catherine's first present issues one six-entry GPFIFO submission: two queue packets, the two recorded command
segments, then a 58-word queue tail that writes the window origin. The queue packets come from a separate queue-side
backing, not from the command pool the game supplied. The recorded segments' GPU addresses follow from the command
buffer's chunk fields (next section).

## Command buffer layout (Catherine USA v2.0 and TotK v1.0.0 SDK observations)

`NVNcommandBuffer` fields, from disassembly of the two SDKs:

| Field | Catherine | TotK |
| --- | --- | --- |
| device | `+0x00` | `+0x60` |
| memory callback, userdata | `+0x08`, `+0x10` | `+0x68`, `+0x70` |
| command cursor | `+0x18` | `+0x00` |
| command limit | `+0x20` | `+0x08` |
| chunk start | `+0x28` | `+0x40` |
| control cursor | `+0x30` | `+0x20` |
| control limit | `+0x38` | `+0x28` |
| control stream start | `+0x40` | `+0x48` |
| current range-record header | `+0x48` | `+0x50` |
| chunk CPU base | `+0x50` | `+0x30` |
| chunk GPU base | `+0x60` | `+0x58` |
| recording flag (byte) | `+0x68` | `+0x11` |
| space check enabled, BindTexture's `0x68b` word (bytes) | `+0x6d`, `+0x6c` | not observed |
| reserve margin in words | `+0x70` (u32) | `+0x16` (u8) |

The GPU address of a cursor is chunk GPU base + (cursor - chunk CPU base). TotK caps each range at `0x7ffffc` bytes.

Command functions are all inlined copies with no shared reserve helper. Each checks whether cursor + n*4 exceeds the
limit (Catherine checks only when the command buffer's byte `+0x6d` is set) and if so calls a slow path (Catherine `0xd141f20`, TotK `0xd15a754`). The slow path calls the memory callback
(event 0 for command memory, 1 for control memory), expects it to call `AddCommandMemory` or `AddControlMemory`, and
asserts if the space is still short.

The control stream:

- `BeginRecording` writes `{0xffffffffffffff01, 0}`.
- A range record is a header qword (low dword type 3, high dword entry count) followed by 24-byte entries
  `{u64 gpu_va, u64 cpu_va, u32 nwords, u8 flags}`, with flags 1 for mergeable and 3 for normal. A range contiguous
  with the previous one merges by adding to its `nwords`.
- `EndRecording` writes a type-2 terminator and returns control start + 0x10 as the command handle.
- Draws and binds do not touch the control stream. Only flushes (`AddCommandMemory`, `EndRecording`) and compute-stage
  binds (16-byte control items) do. A compute-stage (stage 5) `BindTexture` writes a control item instead of methods.

| Function | Catherine | TotK |
| --- | --- | --- |
| `nvnCommandBufferInitialize` | `0xd141860` | `0xd15a0a0` |
| `nvnCommandBufferAddCommandMemory` | `0xd141920` | `0xd15a140` |
| `nvnCommandBufferAddControlMemory` | `0xd141af0` | `0xd15a320` |
| `nvnCommandBufferBeginRecording` | `0xd141c40` | `0xd15a480` |
| `nvnCommandBufferEndRecording` | `0xd141cf0` | `0xd15a530` |
| `nvnCommandBufferDrawArrays` | `0xd163b00` | `0xd183880` |
| `nvnCommandBufferDrawElements` | `0xd163b80` | `0xd1838f0` |
| `nvnCommandBufferBindTexture` | `0xd1627c0` | `0xd181680` |

`nvnDeviceGetTextureHandle` is `orr w8,w1,w2,lsl #20; sxtw x8,w8; orr x0,x8,#0x100000000; ret` in both (TotK
`0xd165530`): the texture ID in the low 20 bits, the sampler ID in the next 12, and bit 32 set.

## Other SDK object layouts

Layouts differ between SDK versions, so a getter is safer than an offset. TotK exports builder and view getters
(`nvnMemoryPoolBuilderGet*`, `nvnTextureBuilderGet*`, `nvnTextureViewGet*`). Pure getters such as
`nvnTextureGetStorageSize`, `GetTextureAddress`, `GetStride` and `GetStorageClass` were also called successfully on a
copy of the CPU state after `nvnTextureInitialize`. The layouts seen:

- **Memory pool builder:** newer SDKs keep flags, storage and size at `+4`, `+16`, `+24`. Odyssey's older SDK keeps
  storage, size and flags at `+8`, `+16`, `+24`.
- **Texture builder:** Odyssey's older layout keeps the pool pointer and storage offset eight bytes earlier than the
  newer one. Odyssey also passes copies of initialized texture and sampler objects to `SetRenderTargets` and the
  register calls, so object identity cannot be keyed on the pointer.
- **Texture view:** TotK's `TextureViewSetDefaults` clears 16 bytes and packs selection and override flags into it
  (six selection flags, target, format, swizzle with identity `2,3,4,5`, depth mode). Catherine's clears 36 bytes with
  separate flag and value fields. Odyssey also uses 36 bytes, with a different constructor and the target at `+32`.
- **Programs:** `nvnProgramSetShaders` passes 16-byte (code, control) pairs. `SetShadersExt` passes 24-byte entries,
  which the SDK splits into those pairs plus an array of third words. Reading the plain pairs as 24-byte entries
  shifts every reference after the first.
- **Texture handle:** low 20 bits texture ID, next 12 bits sampler ID. `RegisterTexture` writes a 32-byte TIC and
  `RegisterSampler` a 32-byte TSC into the pool's memory. `RegisterImage` writes an image descriptor into the texture
  pool. Neither Catherine nor TotK exposed a GPU base for the descriptor pools, so the descriptors were read through
  the pool's CPU backing.
- **Vertex state (the same in Catherine and TotK):** an attribute is one 32-bit word with an enable bit, a 5-bit
  stream index, a 14-bit byte offset and a 10-bit format. A stream is two 32-bit words, stride and divisor.
- **Samplers:** the builder layouts differ between Catherine and TotK, so only explicit setter arguments are reliable.
- **`BindProgram`** applies to a stage mask. A later partial bind replaces only its stages.

`ClearColor` is not a no-op when repeated: each call advances the cursor by 48 bytes. A clear to an integer target
passes float bits (Scarlet passes `0x3f800000`), which reach the Maxwell clear registers unchanged.

## Shader blobs

| Kind | Layout |
| --- | --- |
| Graphics | a 0x30-byte wrapper with magic `0x12345678`, then the Maxwell shader program header (SPH) and instructions |
| Compute | magic `0x12345679` with the declared code size at `+4`, 0x100 bytes before the code. The QMD's program address is the code, so it is the NVN candidate + 0x100. Local workgroup size and shared memory come from the NVN control object |

- **VertexA and VertexB:** NVN dual vertex programs put the position in VertexA (slot 0) and the varyings in VertexB.
  In Catherine, a program reference named only VertexA, and VertexB was at the next 0x200-byte code slot. Using
  VertexA alone produced alpha zero.
- **Embedded CBUF 1:** compiler-generated constant bank 1 sits in the code section after the program. A header size of
  `0x190` put it at offset `0x200`, and `0x210` at `0x300`. Both matched the Maxwell bank 1 bindings.
  [deko3d's `dk_shader.cpp`](https://github.com/devkitPro/deko3d/blob/master/source/dk_shader.cpp) does the same. For
  compute, the offset comes from the control metadata.
- **Odyssey** has blobs with a zero size field and a valid magic and SPH. Their bank 1 offset was not established.

## Constant buffer banks

| Bank | Contents |
| --- | --- |
| 0 | SDK-generated. Base vertex and base instance at `0x640` and `0x644`. Storage buffer descriptors (TotK fragment at `0x550`, compute at `0x310`). TotK bound bank 0 once for all five graphics stages, with an ordinary bind before its first submission (an observation) |
| 1 | embedded in the shader blob (above) |
| 2 | generated texture handles at word `8 + 2*slot`, image handles at word `72 + 2*slot`. Compute texture slot 0 is word 8 in TotK |
| 3 + N | application uniform slot N, graphics and compute alike |

The bank 2 layout matches `BindTexture`'s `id*8 + 0x20` byte offset. Maxwell rounds uniform binding sizes up to 256
bytes. Binding NVN uniform slots to identically numbered banks is wrong. The generated bank's live contents change
during a submission, so a value read after the GPU ran can differ from the one a draw used.

## Maxwell methods and macros observed

Method numbers are 3D engine (subchannel 0) word offsets, and macro calls are methods `0xe00 + 2*index`.

| Method | Use seen in NVN command memory |
| --- | --- |
| `0x004`-`0x007` | puller semaphore address, payload and operation (release writes memory, acquire reads) |
| `0x3e0`, `0x68b` | written with zero (cache flush and no-op forms, no memory effect) |
| `0x4eb` | window origin. Catherine's queue tail changes it from 0 to `0x10` after the first submission |
| `0x5f2`, `0x5f6` | index buffer address (two words) and index type, before macro 38 |
| `0x6c0`-`0x6c3` | query address, sequence and report operation |
| `0x701`, `0x702`, `0x7c0`, `0x7c1` | vertex stream 0 start address and inclusive end |
| `0x8e0`-`0x8e3` | constant buffer size, address and write offset |
| `0x8e4`-`0x8f3` | constant buffer data |
| `0x904 + 8*stage` | constant buffer bank bind (`0x904` vertex, `0x924` fragment) |

| Macro | Method | Observed meaning |
| --- | --- | --- |
| 7 | `0xe0e` | IMMD call with the shader stage, at the start of `BindTexture` |
| 12 | `0xe18` | program bind, five arguments. Selector 1 writes VertexB's code offset, 5 the fragment's, and the third argument is the offset (TotK MME trace) |
| 16 | `0xe20` | sets a constant buffer write offset in words: `0x154` before a storage descriptor upload of three `0x8e4` words landed at bank 0 + `0x550`. In TotK it began at word `0xfe` of the uploaded MME program |
| 25 | `0xe32` | `DrawArrays` (mode, first, count) |
| 28 | `0xe38` | `DrawElements` (mode, index address high and low, index type, count) |
| 38 | `0xe4c` | the 12-word indexed draw in TotK secondary lists: IMMD `0xe20` (value `0x190`, the base vertex word), index type, index address, then six arguments. Packet word 10 (the fifth argument) is the base vertex |

Slot numbers are TotK's and those written by the studied functions. Another title's upload can number them
differently. Register writes made inside a macro are not visible in the pushbuffer, so shader and constant buffer
state cannot be inferred from direct writes alone.

## NVN formats

Values from the [reconstructed NVN header](https://code.botw.link/uking/uking/lib/NintendoSDK/include/nvn/nvn.h.html),
each confirmed in at least one title:

| NVN | Format | Vulkan |
| --- | --- | --- |
| `0x03` | R8 UINT | `R8_UINT` |
| `0x06` | R16 | `R16_UNORM` |
| `0x0a` | R32F | `R32_SFLOAT` |
| `0x0f` | RG8 UINT | `R8G8_UINT` |
| `0x11` | RG16F | `R16G16_SFLOAT` |
| `0x12` | RG16 | `R16G16_UNORM` |
| `0x13` | RG16 SNORM | `R16G16_SNORM` |
| `0x25` | RGBA8 | `R8G8B8A8_UNORM` |
| `0x26` | RGBA8 SNORM | `R8G8B8A8_SNORM` |
| `0x27` | RGBA8 UINT | `R8G8B8A8_UINT` |
| `0x29` | RGBA16F | `R16G16B16A16_SFLOAT` |
| `0x2e` | RGBA32F | `R32G32B32A32_SFLOAT` |
| `0x38` | RGBA8 sRGB | `R8G8B8A8_SRGB` |
| `0x3d` | RGB10A2 | `A2B10G10R10_UNORM_PACK32` |
| `0x3f` | R11G11B10F | `B10G11R11_UFLOAT_PACK32` |

Also seen, without their enum values recorded: R8, RG8, R16F, D16, D32F, D24S8, BC1, BC2, BC3 and BC4. An sRGB view of
RGBA8 storage (Odyssey) is a format reinterpretation of one image, not a second image.

## Per-device and per-submission state

- **Depth mode** belongs to the device, not the draw: draws use the device's `[0, 1]` or `[-1, 1]` mapping.
- **Window origin** cannot come from the device getter. Catherine's getter reports upper left from startup while the
  register changes between the first and second submission, written by the queue tail. Its value matters at the
  command segment's position in the submission.
- **User clip enables** are a register the vertex shader translation depends on. TotK enabled all eight (`0xff`) on
  its cubemap draws.
- **Render conditions:** `SetRenderEnableConditional` compares query values that puller releases inside the same list
  change between clears. Only an ordered decode of the list can say whether a clear runs. Reading guest memory after
  the list cannot.
- **Unbound attributes:** TotK enables attributes with no bound stream. The Maxwell renderer supplies `(0,0,0,1)`, and
  any NVN path has to match it.

## Emulator fixes kept from the investigation

| Fix | Where |
| --- | --- |
| A render-target alias of an 8×8 cubemap mip is padded to a 16×8 surface (64-byte GOB rows). The importer accepts that shape and copies the logical extent | `vk/texture.c` `tex_take_drawn` |
| Depth-only D16 and D32F textures import from render targets through the depth aspect | `vk/texture.c` |
| Four-byte packed depth/stencil uploads: G24R8 (`0x0e`) and Z24S8 (`0x29`) keep stencil in the low byte, X8Z24 (`0x2a`) and S8Z24 (`0x2b`) depth in the low 24 bits | `vk/texture.c` |
| A BRX resolves its jump table when the LDC is up to 64 instructions back, with only a short list of instructions that don't write the index register in between and no control flow before it. TotK has the LDC at instruction 93 and the BRX at 125 | `shader/shader.c` `brx_far_ldc` |
| Compute code is bounded by the NVN header's declared size (magic `0x12345679` at code - 0x100), so an unresolved BRX cannot scan into neighboring programs | `vk/compute.c` |
| Storage results are read back from host-cached memory | `vk/buffer.c`, `vk/cmd.c` |

## Techniques and pitfalls

Techniques that worked:

- **Verify a replaced function against the original.** Run the SDK function on a copy of the CPU state and compare
  every register with the replacement's result.
- **Byte-compare a GPFIFO entry with command memory** to find which list or pool it came from. Translate GPU addresses
  to CPU addresses before concluding two ranges are separate, since they live in different address spaces.
- **Read back at the operation, not at the present.** A readback at the present describes the frame's last use of an
  address, not the operation in question.
- **Prove coverage with a sentinel.** Clear to an unusual value per component, draw, and accept a target as fully
  written only if every component of every pixel changed.
- **Trace the executed MME** to learn a macro's arguments, rather than guessing from the call site.
- **`SNX_GPU_TRACE_TARGET`** narrows a wrong target to the clears and draws that wrote it.

Pitfalls:

- Screenshot hashes are not deterministic across unmodified fixed-frame runs. Compare readbacks of a chosen target.
- Post-submit comparisons mislead. Surfaces are reused at the same address within a submission (TotK clears and
  reuses one depth surface for each cube face), so a later readback describes a different operation.
- Two uncovered images both read back as zero and compare equal. Check coverage before calling a comparison a match.
- A probe that submits or finishes the queue changes what it measures. A readback after a draw once masked a missing
  descriptor, and a queue finish advanced the frame so that draws were skipped and the result looked better.
- Bounded logs answer "maybe" when they wrap. The Maxwell write log (`maxwell3d.c`, 1,024 entries) answered "assume
  dirty" for any range over 1 MiB once constant buffer updates wrapped it, every frame, and the probes treated that
  as a real write. Writes are kept per page now (`gpu/maxwell/write_pages.c`), but a write of the same data still
  counts as a write.
- A zero texture handle (Scarlet binds many) may be an unused slot or a shader-visible null descriptor. Decide from
  the shader's texture use, not from the bind.
- Run counts vary with timing even at the same frame limit. Treat them as samples, not coverage.

## References

- [deko3d primer](https://github.com/devkitPro/deko3d/blob/master/Primer.md): command buffers, queues and fences on
  Switch. A related API, not a specification of NVN.
- [NVN header reconstruction](https://code.botw.link/uking/uking/lib/NintendoSDK/include/nvn/nvn.h.html) and its
  [inline function table](https://code.botw.link/uking/uking/lib/NintendoSDK/include/nvn/nvn_FuncPtrInline.h.html).
- [Maxwell shader program header](https://download.nvidia.com/open-gpu-doc/Shader-Program-Header/1/Shader-Program-Header.html).
