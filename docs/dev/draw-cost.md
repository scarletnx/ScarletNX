<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# The CPU cost of a draw

## Index

* [Where the time goes](#where-the-time-goes)
* [Measure first](#measure-first)
* [The work](#the-work)
* [Memory on the GPU](#memory-on-the-gpu)
* [Out of scope](#out-of-scope)

The GPU thread sets the frame rate of the games we profile, not the shaders on the GPU. The work it does on the CPU
for each draw and each dispatch is what this plan cuts. Each item here applies to every NVN game, and none of them
looks at a title ID.

## Where the time goes

A profile of 2026-11-01 (before the GPU mirror and the merge of the Ounce branch) measured about 23 us of GPU thread
time for each draw, with about 600 draws in a frame:

| Game | Largest parts of the GPU thread |
| --- | --- |
| A Switch game, open world | `stage_set` 22%, `tex_resolve` 11%, `buf_get` 10%, the publish of linear targets 6% |
| A Switch game, city street | libc copies 31%, `gmem_read` 16%, `vk_shader_hash` at each dispatch 8% |

An Ounce game races at 11 to 13 frames a second on an RTX 2080 at the console's resolution. A static recompiler of
that game (MIT) spends about 3.3 us for each draw on its replay thread, and races in 31 to 33 ms a frame at 720p on an
integrated Radeon 660M. Its design is close to ours: the SDK replaced by host code, and NVN taken at the API and played
back into Vulkan. The difference is in what each draw costs. The items below take its methods where they fit our
design, with ideas of our own.

## Measure first

1. Profile the GPU thread in an Ounce game's race and in a Switch game's street on the current branch, with
   `cycles:u` and frame pointer call graphs. Look at `buf_get` and `vk_buf_*`, `cb_get`, `tex_resolve`, `stage_set`,
   `vk_shader_hash` and `sm86_hash`, and `vk_mirror_harvest`.
2. Count the hits and misses of the memos of `tex_resolve` and `cb_get`, and the descriptor sets that `stage_set`
   builds against the sets it finds in its cache.
3. Compare frame rates at the same internal resolution as other projects (the `resolution` setting).

The order below is a first guess. The profile sets the real order.

## The work

**1. Every buffer binding from the GPU mirror.** Vertex, index, uniform, storage and indirect bindings take a
(buffer, offset) in the GPU mirror (`mirror.c`), which takes the pages the CPU wrote one time at each submit. Today
`buf_get` looks up a cache of guest ranges with content hashes (`buffer.c`), copies the changed ranges into staging,
and `cb_get` checks the pending results of each binding. The other project makes each NVN memory pool a buffer, syncs
the pages that write-watch marked in a command buffer before the first use in a submission, and finds a binding by a
search of a sorted index. Expected: the largest gain for each draw. Buffer device addresses are a later step that lets
a shader read a buffer by its guest address with no binding at all.

**2. Uniform blocks copied one time for each submission.** When the recording of a submission starts, copy every
uniform range it binds into an arena, merged and aligned to 256 bytes. Draws bind by an offset in the arena. A GPU
write in the same submission drops the copies it covers. This also gives each draw the bytes it had when the game
submitted it, where a game writes its uniform buffers again after the submit. Item 1 needs it.

**3. Generations for each address in the memos.** `tex_resolve` drops its memo on `gpu_mem_generation()` and on the
counts of pending results and of images (`g_pend_any.adds`, `g_gfx_img_any`), which are global. `cb_kept` compares
`vk_compute_adds()` and `vk_buf_adds()`. A compute result anywhere in memory makes every memo miss, and an Ounce game
mixes many dispatches with its draws. Key each memo to generations of the addresses it read, held in a small hashed
table of the storage address.

**4. The key of a program made one time.** Make the program key when the game sets the program
(`nvnProgramSetShaders` and its Ounce form), keep it in the program object, and make it again only when a GPU or CPU
write reaches the code pages (`writes.c`). Today graphics programs and the compute programs that miss the fast
cache of `compute.c` hash their code at the draw or dispatch (`vk_shader_hash`, and `sm86_program` with `sm86_hash`
for SM86). Check first how much of this the fast cache of `compute.c` already does.

**5. The flags of NVN memory pools.** A pool with `NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS` never needs traps, write
logs, masks or landings, because the CPU cannot read it. A pool with GPU no access needs no GPU memory. Storage
bindings come from the bound ranges, so only the bound ranges that a shader writes come back. `nvn.h` defines the
flags and no code reads them. Expected: GPU time for the logs and folds, CPU time for the harvest, and VRAM for the
masks.

**6. A draw whose bindings did not change skips its descriptor build.** Compare a signature of the inputs of the
descriptors of a draw, and compare the bytes of the bound slots only when a bind ran since the last draw. Skip a push
equal to the last under the same layout. Reuse a set whose descriptors pack to the same bytes, through update
templates. The other project skips about a quarter of its draws this way. `stage_set` builds the writes of each
stage and then hashes them (`desc_set_cached`).

**7. Bindless textures.** A Maxwell or SM86 shader reads a texture handle from a constant buffer and indexes the
texture and sampler pools. Let the translated shader index a descriptor heap that mirrors those pools and lasts
across draws, so that the renderer writes no descriptor set for each stage of each draw. This removes most of
`stage_set` and `tex_resolve`. `descriptors.c` already writes the pool registrations of NVN into indexed descriptor
memory. The risk is in what `tex_resolve` picks for each handle: render targets, scaled targets and converted formats.
A heap entry must change when its surface changes. This gives the most and risks the most, so it comes after items
1 to 6.

## Memory on the GPU

- Give physical pools their memory when the GPU first uses them, in chunks of 4 MiB.
- Give no memory to pools that the GPU cannot access.
- Mirror only the pools that the GPU uses as buffers, within the budget of the device.
- Evict mirrored textures that no draw used for a time.
- A limit on the tiles of sparse textures, and a setting for textures at half or a quarter of their size. Both cost
  image quality, so both are settings that are off by default.

Measure the VRAM of the GPU mirror, its masks and its host shadow first.

## Out of scope

Patches of one game's code at fixed addresses, which the other project uses for its internal resolution, its shadow
maps and its spin waits. A general form of one of them can come later: find the zstd decoder of a game by its bytes
and hook it with the host's libzstd. That helps the time to load, not the frame rate.
