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
numbers are assigned by the SDK's macro upload, so a slot number alone identifies nothing), and an unknown hash keeps
the interpreter. `maxwell/mme.c` hashes a macro's code from its start to the end of the upload batch that wrote it
(NVN uploads each macro on its own), with Eden's hash, so Eden's table of macro hashes names the same code, and
`maxwell/mme_hle.c` has native versions of the ten macros TotK runs most (its slots 7, 8, 9, 12, 16, 27, 30, 35, 37
and 38), of every macro in Eden's table, and of the busiest ones Catherine and Super Mario Odyssey interpret. They send
the same methods as the interpreter.
`tests/mme_hle.c` checks each against the interpreter running a macro written in the test. A macro's parameters are
pushbuffer words the GPU thread fetched before the call (`gpu.c`), so the indirect draws read their records and count
as the interpreter does: Eden's indirect path (the rasterizer drawing from the buffer's GPU address, with the shaders'
constant buffer reads of the base vertex, base instance and draw index replaced) has no counterpart here yet.
`mme_verify=1` (the `[Debug]` setting, or `--mme_verify=1`) runs both for every call of a known
macro and logs where they differ, `mme_hle=0` interprets everything, and the log names each macro's hash when it
is first called and counts native and interpreted calls every million. `nvn_control.c` decodes the arguments of two
macros and `nvn_command_gap.c` classifies what methods write, both checked against packets captured in their tests.

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

### GPU compatibility coverage

Shader instruction invalidation reaches the Vulkan shader address caches from the 3D engine's `0x87` and `0x369`
methods and the compute engine's `0x87` and `0x5a6` methods. Bit 0 advances a shader generation independent of the
memory epoch. The next lookup reloads code, while content-addressed modules remain reusable. This does not expire
texture caches or force a host wait. Repeated invalidations flush any held draw before advancing the generation.
The method encodings are documented in NVIDIA's public [3D class](https://github.com/NVIDIA/open-gpu-doc/blob/master/classes/3d/clb197.h)
and [compute class](https://github.com/NVIDIA/open-gpu-doc/blob/master/classes/compute/clb1c0.h) headers.

| File | What it is | Built into |
| --- | --- | --- |
| `src/nx/gpu/nvn_trace.c` | `nvn_trace=1` inventory of resolved and called NVN functions | the runtime |
| `tests/support/nvn_control.c` | decoders for the 12-word macro 38 indexed draw and the macro 12 program bind | `tests/nvn_control.c` |
| `tests/support/nvn_command_gap.c` | classifier of Maxwell method write effects in a span of command words | `tests/nvn_command_gap.c` |

Every `SNX_NVN_*` variable is gone: these are `[Debug]` settings now. `gpu_trace_target=address[:bytes]` (`vk/render.c`) logs the clears and
draws that write a render target starting in that range, and replaced the older GPU trace variables.

## Verify mode: nvn_direct against the game's own driver

`nvn_verify=1` (the `[Debug]` setting, or `--nvn_verify=1`, with `nvn_direct`) checks nvn_direct draw by draw against
the Maxwell path while the game runs (`src/nx/gpu/nvn/nvn_verify.c`). The two paths meet at `DsState`
(`gpu/draw_state.h`): nvn_direct's replay fills it from its records, the Maxwell path from the 3D engine's registers
(`maxwell/draw_state.c`). Verify mode makes both for each draw and compares them. Off, it costs a test of the
setting at each replayed draw and submission and a compare of the channel's id in the 3D engine's draw, constant
upload and acting methods, and the slots hold nvn_direct's handlers as before.

### What runs

The game's driver is still in its module, and nvn_direct keeps the address of the driver's code for each function it
takes over (`nvn_direct.c` `take`). In verify mode the slot holds `verify_fn`, which runs nvn_direct's handler on
the game's objects first, as always, and then the driver's code on shadows, on the game's thread and stack. The
driver's results go nowhere but the shadows: the game sees nvn_direct's.

| Shadow | What it holds | When it changes |
| --- | --- | --- |
| Object (device, queue, pool, buffer, texture, sampler, texture and sampler pool, program, sync, event, command buffer) | the driver's object, in guest memory of the runtime's outside every region the game maps (`shadow_map`) | made at the live object's `Initialize`, gone at its `Finalize`; keyed by the live address |
| Builder or state object | the driver's bytes | keyed by the live address and nvn_direct's bytes there: the state the calls at that address made, while the game leaves its bytes as they were. Else by nvn_direct's bytes alone, for a copy the game made by value (nvn_direct keeps less than the driver, such as the device, so such a copy is ambiguous). Each setter runs the driver on that state and records the result for both keys |
| Memory the driver writes (pool storage, queue memory, control memory) | a range of the same size (queue memory: at least 8 MiB) | made at its first use, keyed by the live address |
| Command memory | the game's chunks through the shadow pools, and 256 KiB pieces of a 1 GiB ring of the runtime's when less than a quarter is left, so the driver never calls the game's memory callback | each command buffer call |
| Command handle | the handle the driver's `EndRecording` answered | keyed by nvn_direct's handle |

`tools/nvn_verify_kinds.py` writes each function's argument kinds from `nvn.h`'s prototypes into
`nvn_verify_kinds.inc` (the ctest `nvn_verify_kinds` checks it is current). A call whose argument has no shadow is
skipped, and the log names the function and the reason once. Never made in the shadow: waits, the window and its
presents (a present flushes the shadow queue instead), callbacks, virtual pool mappings, and for now
`CallCommands` and `CopyCommands` (below). The driver's own calls into NVN go through the same slots, and run the
driver's code while the thread is in a shadow call. The driver's lookup with a device sets `nvnPrivate0`: after
each shadow call the word is put back, since it turns on the game's inlined command writes.

### How the shadow pushbuffer gets to the decoder

The driver's queue opens its GPU channel inside a shadow call, so nvdrv marks the fd (`nvn_verify_fd`). A
`SUBMIT_GPFIFO` on a marked fd goes to `nvn_verify_gpfifo` instead of the GPU thread: its words are copied at once
(command memory is reused) into the queue's batches, each tagged with the number of the game's submission it
follows, and the fence's increments are made at once, so the driver's waits on its own channel return. After each
`QueueSubmitCommands` the shadow flushes its queue, so each live submission has a batch of its own.

On the GPU thread, nvn_direct's replay of submission n and each of its draws pull the batches through a verify
channel (`gpu_verify_channel_new`, ids from `GPU_VERIFY_ID`): the Maxwell decoder with its registers, macros and
held draw, and no draw, clear, write, release, barrier or syncpoint (`verify_inert` in `engines.c`). A verify
channel's draw gives its `DsState` and `DrawCall` to `nvn_verify_shadow_draw`, and its constant buffer data goes to
a table beside the decode, never to guest memory. The shadow draws of submission n pair in order with nvn_direct's;
an extra draw on either side is counted, and the next submission starts the pairing again.

### What the compare reports

Each group of `DsState`, the bound targets' blending, and the draw's own arguments, field by field. A GPU address is
compared as the guest bytes it names, an address in a shadow range as the live memory it shadows, so the two paths'
different mappings of one buffer are the same. nvn_direct's inline banks 0 and 2 are compared word by word with the
words the driver's stream wrote into its bank buffers. Each differing field is logged once, with the queue,
submission and draw, and the two values (`nvn verify: queue Q submission S draw D: GROUP.field[i] nvn_direct X,
driver Y`), then its count at 1024 and each power of 4. Also checked: the results of the calls that return a value
(not an address or a time), and each texture descriptor `TexturePoolRegisterTexture` writes, word by word with the
address as guest bytes. A `svcBreak` in a shadow call names the driver function it ran.

### Texture storage answers

Games place textures in their pools by `nvnTextureBuilderGetStorageSize`, `GetStorageAlignment`, `GetStorageClass`
and `GetZCullStorageSize`. `nvn_tex_storage` (`gpu/nvn/nvn_tex.c`) answers them with the rules read from two
drivers' code (NVN 12.3 and 55.15, the same rules):

- The kind (the class is 0x100 with it): 0 for a buffer or a linear texture. A depth or stencil format has a depth
  kind. A COMPRESSIBLE color format of one-texel blocks and 4 bytes or more has a compression kind. Else 0xfe.
- The alignment: 0x80 for LINEAR_RENDER_TARGET, 0x20 for LINEAR, a buffer's element size, else 0x200. Every kind but 0
  and 0xfe takes the big page, 64 KiB, and its size is rounded up to it.
- The ZCull area: 0 for a format without depth bits (stencil only counts with ZCULL_SUPPORT_STENCIL). Else 0x20 bytes
  for each 32x32 tile of each layer, plus 0xE0 of headers (nvdrv's ZCULL_GET_INFO answer).
- A multisampled or rectangle texture has one level, whatever the builder says.

The layout (`nvn_tex_layout`) stays the texture's extent, which `tests/nvn_tex_storage.c` keeps inside the answered
size over targets, formats, sizes, levels, samples and flags. Verify mode's storage sweep runs the driver's getters
and nvn_direct's on the same 7,986 builder states at the game's first query: sizes, alignments and ZCull areas agree.
The compression kinds of some formats still differ (RG16F: the driver's 0xdb, nvn_direct's 0xcc): the driver picks
them from a kind class in its format table, which nvn_direct doesn't have yet. The size and alignment don't depend
on which compression kind it is.

### What the first runs found (one game, SDK NVN 55.15)

The shadow device, pools, programs and queue initialize, and the game runs through its title and save load with the
driver beside nvn_direct. The results the driver answers otherwise, which the game sizes its memory by:

| Call | nvn_direct | Driver |
| --- | --- | --- |
| `nvnQueueBuilderGetQueueMemorySize` (defaults, compute memory set) | 0x100000 | 0x109000 |
| `nvnTextureBuilderGetStorageClass` | 0 | 0x100, 0x1fe |
| `nvnTextureBuilderGetStorageSize` | 0x200, 0x18000 | 0x10000, 0x20000 |
| `nvnTextureBuilderGetStorageAlignment` | 0x200 | 0x10000 |
| `nvnTextureBuilderGetZCullStorageSize` | 0x100, 0x200, 0x2c00 | 0x460, 0x860, 0xafe0 |
| `nvnDeviceGetTextureHandle` | 0x10300107 | 0x110300107 (bit 32 set) |
| TIC word 6 of registered textures | 0 | 0x03000000 |

The texture rows are fixed (above). The queue memory size is certain: the driver's `QueueInitialize` fails with the
size nvn_direct answers. The driver's rule adds to the command and control sizes (defaults 0x10000 and 0x4000 when the
game set none) an area of its own, which grows with the compute memory size (default 0x40000) and with six words of
the device's GPU characteristics, not yet matched to nvdrv's answers.

### Open problems (why draws don't compare yet)

- **The driver's `CallCommands` aborts** about 7 to 25 seconds in: "CommandBuffer has run out of control memory with
  no out-of-memory callback". Each call copies the called recordings' command ranges into the calling buffer's
  control memory, 0x18 bytes a range, and one game's calls took about 0x80000 bytes each. The shadow feeds control
  memory before each call (4 MiB for CallCommands) and the abort still comes, so something else is short. Until it
  is found, verify mode skips both calls, and the game's draws in called recordings are only nvn_direct's: in the
  first run 1,024 draws paired and about 197,000 had no shadow draw.
- **Verify mode makes one game crash.** In 5 runs of 100 seconds without verify mode the game didn't crash, in 5 with
  it the game crashed 3 times, each at the same instruction of the game's code (a null pointer plus an offset).
  Not traced: the shadow driver's allocations through the game's graphics allocator, or its stack copies, are what
  verify mode changes for the game.
- **Arrays of texture views** at `SetRenderTargets` whose state the map doesn't know skip the call, so the shadow's
  targets stay at their defaults.

Not covered even when these are fixed: render-target readbacks (the plan's per-operation pixel compare), indirect
draws' counts (the shadow reads zeros from its shadow pools), virtual pool mappings, the window and present path,
compute dispatches, clears and copies (only draws are paired), and a game whose driver uses another NVN API version
than the ones nvn_direct describes.

## Observing NVN calls

The SDK exports `nvnBootstrapLoader`, which returns a guest function pointer for an NVN function name. With
`nvn_trace=1`, `nvn_trace.c` replaces that function's entry in the recompiler's dispatch table `g_disp`, then the
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
| Bare shader file | magic `0x19866891` at the start of a file one game ships, with its SPH at `0xB50` and no wrapper |
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
| 7 | `0xe0e` | IMMD call with the shader stage, at the start of `BindTexture`: selects the stage's constant buffer (size from scratch `0xd2f + stage`, address from `0xd2a + stage`) for the handle written next |
| 8 | `0xe10` | blend enables of the eight render targets from bits 8-15 of its argument, after a zero to `0x671` |
| 9 | `0xe12` | the eight render targets' color masks (`0x680`-`0x687`), two 13-bit halves per argument |
| 12 | `0xe18` | program bind, five arguments. Selector 1 writes VertexB's code offset, 5 the fragment's, and the third argument is the offset (TotK MME trace) |
| 16 | `0xe20` | sets a constant buffer write offset in words: `0x154` before a storage descriptor upload of three `0x8e4` words landed at bank 0 + `0x550`. In TotK it began at word `0xfe` of the uploaded MME program |
| 25 | `0xe32` | `DrawArrays` (mode, first, count) |
| 27, 37 | `0xe36`, `0xe4a` | instanced draw: topology, vertex count, instance count (masked by scratch `0xd1b`), first vertex, base instance. 37 also writes the base instance into the bound constant buffer at `0x644` |
| 30 | `0xe3c` | instanced indexed draw, TotK's busiest macro: topology, index count, instance count (masked by `0xd1b`), first index, base vertex, base instance |
| 35 | `0xe46` | writes scratch `0xd38`-`0xd39`, then `0x0d8`-`0x0d9`, from two arguments and scratch `0xd3a` |
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
  its cubemap draws, and two other games on every draw. The vertex programs a game ships, translated before its
  draws use them (`prepare_shaders`), take that value, so their keys match those draws.
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
- **`gpu_trace_target`** narrows a wrong target to the clears and draws that wrote it.

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
