<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# The CPU cost of a draw

## Index

* [Where the time goes](#where-the-time-goes)
* [Measure first](#measure-first)
* [The work](#the-work)
  * [What the items are worth after item 8, on the desktop](#what-the-items-are-worth-after-item-8-on-the-desktop)
* [The course load](#the-course-load)
* [A target refused whole for a write over part of it](#a-target-refused-whole-for-a-write-over-part-of-it)
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

In an Ounce game's race on the desktop of 2026-11-23, a counter in `stage_set` found 4,107 stages set a present.
Only 5.0% of them had the bindings, programs and inline constants of the last draw's same stage (`DsState`'s
`DS_BIND` and `DS_PROGRAMS` generations, the stage's inline generations and the root's), all of those with no
GPU-side write to memory since. `stage_set_` was 17.8% of the GPU thread, so a skip of the unchanged stages could
save under 1% of it there. Most of `stage_set_` is the constant buffer lookups of item 1 (`cb_get` 5.7%,
`cb_window_place` 1.9%). The item waits for a game whose stages repeat.

**7. Bindless textures.** A Maxwell or SM86 shader reads a texture handle from a constant buffer and indexes the
texture and sampler pools. Let the translated shader index a descriptor heap that mirrors those pools and lasts
across draws, so that the renderer resolves no texture handle for each stage of each draw. The risk is in what
`tex_resolve` picks for each handle: render targets, scaled targets and converted formats. A heap entry must change
when its surface changes. This gives the most and risks the most, so it comes after items 1 to 6.

The design of 2026-11-23 builds on the texture table (`vk/table.c`), a heap of views and samplers that lasts across
frames, with update-after-bind. An Ounce game's race already reads its SM86 textures through it ("SM86 textures
through the texture table, one word a texture"), so what each draw still pays is the resolve that finds those words:
`fs_key`'s `tex_resolve` (5.9% of the GPU thread) and the texture part of `stage_set_`. The constant buffers that
`stage_set_` binds (`cb_get` 5.7%, `cb_window_place` 1.9%) stay, so the gain is about 10% of the GPU thread, not
most of `stage_set_`. (`descriptors.c` does not write the pool registrations of NVN into descriptor memory: only its
comment said so.)

1. A pool map in front of the heap: a heap slot for each TIC index and a sampler slot for each TSC index, in pages
   that a change copies, in an arena for each frame slot that resets at its fence. The translated shader reads the
   handle from its constant buffer as now, takes the TIC and TSC indices, checks them against the pools' limits from
   push constants, and reads the map. The record has a view for each class the TIC type allows, a view of a scaled
   target at the guest's size, and flags (integer, unnormalized, scaled) that replace `fs_key`'s texture bits with
   uniform branches.
2. For each draw the CPU reads the handle words and tests a valid bit for each slot. Events clear the bits: a write
   to the pools' headers (the write tracking, then a compare as `tex_memo_unwritten` makes), a new surface at an
   address (`surf_changed`), a new draw to a target a record crops, a new scale, an image swapped (`tex_image_swap`:
   texconv, ASTC to BC7) and a view destroyed. The next draw that names a cleared index resolves it once, the old way.
3. An upload changes no view. The textures of the live records are checked against the write tracking at each new
   epoch, and the GPU-side writes through item 3's generations for each address, not at each draw.
4. No draw names its textures, so the barriers move to the writers: each surface a pass, copy, upload or storage
   write changes gets one barrier, batched at the next pass's start, with unified image layouts. A draw that samples
   a target bound to it (feedback, read-only depth) takes the path of each draw, as do indexed tables, memory handles
   and the depth compare of a colour texture.

The steps, each one landed and measured alone, behind a setting that is off until the race and a Maxwell game run
clean with it: the measure, item 3, the pool map in a shadow mode that `draw_state_check` compares with the full
resolve, the barriers at the writers, the SM86 translator, the renderer's skip for plain slots, the Maxwell
translator, the indexed tables, then the setting on. Each has its test in `tests/`: a header written between two
draws, a target redrawn under a crop, an ASTC texture swapped to BC7, integer and signed formats through one slot
under GPU-assisted validation, a feedback draw, and the shader corpus under `spirv-val`.

Steps 1 to 6 are in, behind `pool_map_shadow`, `writer_barriers` and `pool_map`, all off. On the desktop the race
found every texture slot plain, about 1,300 TIC indices resolved about ten times each a present, and no record the
full resolve contradicted. Where guest memory is shared with the CPU, the map needs a storage buffer in place of
device addresses, and a draw that skips the resolve needs each record's own check of the writes since: the branch
`bindless-uma` has both. There it took 57% of the slots with no resolve and made the race slower. An A/B/B/A with a
pipeline cache warmed for each side gave 75.5 and 82.0 fps off and 64.1 and 69.4 on, about 2 ms more of the GPU
thread a present. The map's own work for each draw (its pages, and the checks by address after each submission and
dispatch moves the stamps) costs more than the resolves it skips.

The profile, on 2026-11-24 after the merge of main (the branch `bindless-merged`, with each side's pipeline cache
warm): 79.3 and 82.4 fps off, 75.6 and 72.6 on. The renderer thread rose from 25.0% to 28.5% of the samples:

- `vk_pool_gpu_draw` took 2.0%. At each new frame it encodes again, entry by entry, every page of the map that has
  records (about 405 of the texture pool's 514 pages, 3.4 MB a present), where 13 pages changed.
- The checks of each slot (`vk_pool_gpu_skip`, `map_skip`) took 0.85%.
- What the map left out (`tex_bind_view`, `vk_sampler`, `vk_table_words`) was about 0.9%. Inclusive `tex_resolve`
  stayed at 2.72%: the slots the map skips are the ones the memo answered already.

Pages kept encoded, and written only where they change (versions freed once their submission is done), would remove
the 2%, and leave the map near even. A map that wins must check nothing for each draw: each entry of the texture table
kept valid by the writes that change it (uploads, the pool's header writes, drawn targets), so a draw binds the table
alone. That is a new validity model for the texture cache, not a change to this map.

**7b. A texture table kept valid by writes.** The design that replaces the map's checks.

What it can save, from the profile above with the map off: inclusive `tex_resolve` is 2.7% of the samples, about 11% of
the renderer thread. Some of `stage_set_` (5.7% inclusive) and `fs_key` (3.0%) is texture work too. The ceiling is
about 15% of the renderer thread, and less where the memo already answers.

What a draw must still do: read its handle words from the constant buffers. Nothing else says which textures the
draw reads, and a texture that no draw resolved has no image yet. The model does not load every texture that the
game registered: the pools of an Ounce game name about 131,000 headers, and the game uses about 1,300 of them.

The model:

1. **An entry for each TIC index that a draw resolved**: the texture, its view and sampler slots, and a valid bit.
   This is the pool map's record (`pool_map.c`), with the stamps and the checks by address removed. A draw reads
   each handle and tests the bit. A set bit binds the slots with no other read. A clear bit takes today's full path
   (`tex_resolve`, `vk_tex_recheck`, take of drawn targets) and sets the bit again.
2. **Events clear the bits.** Each of the memo's checks becomes the event that makes it fail:

   | Today's check for each draw | The event that clears the bits |
   | --- | --- |
   | The surface table at the texture's address (`vk_surf_epoch_at`) | A surface made, freed or moved there: the entries of the textures at that address |
   | CPU writes to the headers (`tex_memo_unwritten`) | A write-tracking intake of a pool page: the headers that changed, compared with the record |
   | GPU-side writes to the headers | `gpu_mem_written_by` over a pool page: the same compare |
   | Pending results over the headers or the texture (`tex_memo_unpended`) | A result added over a texture's memory: its entries |
   | A pending storage image over the texture (`tex_memo_image_pending`) | The same, when the image is added |
   | A drawn target under the texture (`over`, `wgen`) | `vk_rt_drawn` over a texture's memory: its entries |
   | The texels (`vk_tex_recheck`) | A CPU write to a texture's pages at the intake, or a GPU-side write over them: its entries |

3. **An index from guest address to the textures that have entries.** These are the textures that drawing uses,
   about 1,300. The index is buckets of 64 KiB with a list of textures each, so an event with a range finds them with
   no search of the surface table. Each texture keeps the list of its entries. A first test of a bitmap of the pages
   in the index lets most events (most DMA copies, most publishes) return at once.
4. **Pages on the GPU written only where they change.** An entry's words go into its page in CPU memory when it
   changes. A changed page is written into a new block of the buffer, and the old block is freed when the
   submission that last used it is done. This is how drivers version buffers that the GPU reads. A frame no longer
   writes every page again.
5. **The order of events.** An event and the draws are on the renderer thread, in stream order. The front end's
   writes come to the renderer in that order already (`gpu_mem_written_by`, the intake of the scans at a submission).
   A bit that a write clears is clear before the next draw that the write comes before.

What stays per draw is the handle reads, a bit test for each slot, and the bind of the table. The fs_key and the
descriptor set of the texture slots go when the bits are all set and the handles did not change since the last draw
of the same program (item 6's signature, for the texture slots alone).

The checks, as for the map:

- `draw_state_check=1` resolves each slot in full and counts each set bit that a full resolve contradicts. Each
  contradiction names the event that the model missed.
- A warm A/B/B/A on the race, and the route of a Switch game, before the setting goes on by default.

The steps:

1. The index and the events, with the bits only counted against today's memo (a shadow, as `pool_map_shadow` was).
   This shows that the events are complete before any draw uses them.
2. The draws use the bits (no stamps, no checks by address).
3. The pages written only where they change.
4. The texture slots' set and fs_key left out when nothing changed.

Step 1 is in (`tex_events_shadow`). The events go into `addr_gens`, with a new space, `ADDR_GENS_WRITE`, for the
GPU side's writes, the writes to targets, surfaces made or freed and pending storage images. A kept texture's
generations are summed over every level and layer. Over the race route for 200 s, about 333 kept textures a present
were claimed current, and no check contradicted a claim. About 13 a present were checked with nothing changed, from
slots that ranges share. Two things it found:

- Level 0's range alone (`rt_bytes`, which `vk_surf_guest_size` gives) missed the draws to the smaller levels of a
  downsample chain. The texture memo asks its pending results and pending storage images over that same range
  (`tex_memo_unpended`, `tex_memo_image_pending`). Whether a result pending over a texture's other levels reaches a
  draw is not checked yet.
- The streamed arrays of over 128 MiB have more granules than a read adds up (`ADDR_GENS_READ_MAX`). Step 2's
  index of textures keeps a stamp for each texture, so the size of a texture costs nothing.

A profile of the texture resolves before step 2 found two costs that the design did not need, now gone:

- `vk_uma_import_in` took `g_uma.lk` at each take of the write tracking, 4.8% of the renderer thread (`52201bebe`).
- `gpu_mem_granules_generation` walked each 2 MiB granule of a texture at each quiet check, 3.4% (`585d3bbeb`).

After them, the resolves are 8.5% of the renderer thread. The memo itself is about 3%. What step 2 could take out (the
memo's header and pending checks, the quiet check, the take's search) is about 5.3%, about 0.4 ms of a present. The
write tracking finds the CPU's writes when a check takes in the scans of its own range for the epoch (`vk_ww_take`),
and the epoch moves at each submission and wait. So no event is not no write until every watched page is taken in.
That is a wait for the scanner in the frame, or the intake that each check does now, which is most of what is left of
`tex_memo_unwritten` and `quiet_own`.

**8. Render targets published only when memory needs them.** A profile of 2026-11-23 of an Ounce game's race, on a
discrete GPU whose driver imports guest memory as dma-bufs (udmabuf), found `vk_surf_publish` at 21.5% of the GPU
thread, its largest phase. 18.1% came from one path: the import branch of `rt_watch` publishes every target drawn
over imported memory at each release. A counter of publishes by why (the `render targets published a present` line)
found 2 such publishes a present. One is a 6144x6144 RGBA8 surface of 151 MB that compute writes as a storage image,
published whole at each present. It costs the GPU thread 2.5 ms a present in the CPU's work for each page in
`own_publish`: `gpu_mem_written_cpu` stamps about 36,900 pages written, and `vk_rt_before_publish_cpu` looks up the
targets that alias them, under the page locks. It also copies 151 MB on the GPU a present, and its stamps make a
3072x3072x2 texture beside it load 36 MB of pieces again in each present. The CPU never asked for the surface.

A driver keeps a target's pixels in the image while only the GPU uses them. Memory gets them when something outside
the GPU reads it, and a GPU reader of that address takes the image. yuzu and Ryujinx mark the range as modified by
the GPU and download it when the CPU's access faults. The change:

1. While only the GPU uses a target, GPU readers of its address take the image, as `tex_resolve` already does, and
   guest memory under it is not written.
2. Memory gets the pixels on a CPU access (a watch's fault, `vk_rt_watch_hit`), or for an engine that reads guest
   memory directly. A target that the CPU read in the last few presents (`cpu_asked`) stays eager, so a game that
   reads a small target each frame takes no fault each frame.
3. Over a dma-buf region, the watch traps the CPU's mapping only. The driver pinned the pages when it imported them,
   so the GPU keeps them (`uma.c`). This needs a watch that does not move the pages into the alias pool. Over a host
   pointer import, amdgpu takes the pages again at each submission and fails on a trapped one, so the publish stays
   eager and in place there.
4. A publish that does run covers only the part of the surface written since the last one, and costs one step for
   each run of pages, not each page: one range stamp in the write record, one alias lookup for each target and one
   hold of each lock.
5. The counter of publishes a present by why (a CPU read lately, an import, a CPU access, no traps, other callers),
   with their bytes and CPU time. It is in place.

The test `vulkan_uma_region_trap` shows that a dma-buf region takes a trap of the CPU's mapping with the driver's
submissions unaffected: on RADV, its mapping stayed with no access through 100 submissions that wrote it on the GPU,
and the CPU then read the last write. `vk_uma_region_held` tells such a region from a host pointer import. The
change is done when the GPU thread's time for each frame drops in the race, the texture memo misses for memory
written fall, the race's screenshots match at fixed frames, and the save thumbnail of commit 12f8977cd still holds
its picture. On that host this is the largest single item, so it comes before items 6 and 7.

Points 1 to 3 and 5 are in place for dma-buf regions (commit 73e87dc43). In the race, A/B/B/A against the commit
before it, the frame rate went from 52 to 54 fps to 64 to 66, the GPU thread from 15.7 to 16.5 ms a present to 13.3
to 13.5, and the staging of bindings from 147 MB a present on the GPU and 36 MB on the CPU to under 17 MB and 0.2 MB.
What is left:

- Point 4. A publish that runs still does the work for each page.
- A host pointer import still gets a publish at each release. A driver that imports guest memory that way (where
  dma-bufs are not cheaper) gains nothing yet.
- The save thumbnail of commit 12f8977cd, in its own game.

### What the items are worth after item 8, on the desktop

A perf profile of the race of 2026-11-23 (the GPU thread, 30 s, after commits 73e87dc43 to 7332d0e99), the share of
the GPU thread that each item could take at most:

| Item | Its part of the profile | Share |
| --- | --- | --- |
| 1 | `vk_uma_read`'s stamps of each binding in place (bindings already bind in place there) | 6.4% |
| 2 | `cb_get` and `cb_window_place` | 7.1% |
| 3 | the texture memo's checks: `tex_memo_unwritten`, `tex_memo_unpended`, `vk_tex_recheck` | about 3% |
| 4 | the program key (the shader hashes) and the texture handles | about 1% each |
| 5 | the GPU mirror's logs, folds and harvest | under 1% |
| 7 | `fs_key` (7.9%) and most of `stage_set_` (20.1%) | up to about 25% |

The lock calls are 13.5% across them all. The largest named one is `vk_uma_import_in` from the write tracking
(`ww_scan_free`, 2.0 to 2.5%): it takes `g_uma.lk` for each piece a scan reads, where on that host every import is a
dma-buf region that the scans watch anyway. So on that host item 7 is the one large item left, and items 3, 4 and 5
are small.

## The course load

The plan's last step was the course load of an Ounce game, measured on 2026-11-24 after the merge of main. With a
warm pipeline cache it is now two frames of 300 to 350 ms, each with about 200 MB of texture pieces. It was 3 to 4 s
before items 5b and 8 and before ASTC Auto followed the memory.

The pieces are three sparse 8192x8192 arrays of 68 layers with 12 levels each. Two are ASTC 8x8, held as BC7. One is
held as it is, at a byte a texel. The game streams them a layer at a time. It maps about 1,344 brick pages a layer and
fills them with NVN buffer-to-texture copies. Those copies are DMA launches whose results are GPU-owned, and the
texture's next check loads the pieces from those owners on the GPU.

In a Tracy capture the load is one draw of 1.3 s on the renderer thread, and in it:

- **0.76 s** waits in the BC7 encoder's pace (`astc.c`, `vk_submit_upload_wait`). The GPU timestamps give about
  0.54 s of BC7 encodes for the load: 4.7 gigatexels at about 10 gigatexels a second, in dispatches of 1.3 megatexels
  on average. That is the cost of the data with the quick build, which these arrays already take. It is not a slow
  encoder.
- **0.27 s** in `block_find` and `space_take`, about 25,000 runs for the sparse pages' binds. That is Tracy's cost for
  each zone, not the allocator's. A perf profile of the normal build over the same load gives `block_find` 0.02% of
  the samples. A remembered pick of the allocator's walk, exact against the full walk over 65,000 picks, changed
  nothing measurable, and it was not kept.

What would make the load shorter is less to encode: a BC7 cache key for a sparse texture's pieces. Today the pieces
have no key, because their pages are GPU-owned (`astc_content`). A key from the bytes of the copy's source would let
the persistent cache serve the next load of the same course. `vk_tex_dma_rows` could also take copies into any layer
and level of a texture that needs no conversion, which would leave out the owner and the piece load for the third
array. Neither changes the frame rate, only the time to load.

## A target refused whole for a write over part of it

In the race of an Ounce game, textures load about 15 MB of pieces in each present, and no DMA copy causes them. Most
are the 3072x3072 D16 shadow array of 3 layers. Its first 128 KB are shared with a storage image that a compute
program writes after the shadow draw. The write makes the layer-0 target stale as a whole (`vk_rt_drawn_fresh_in`
asks one generation, `draw_gw`, over the target's span), so the texture loads 15 MB of that layer from memory. That
in turn needs the target published there first.

A take of the target with those pages loaded from memory over it was tried, and it was slower (64 fps against 82). To
load the pages, the runtime publishes the target, and the publish stamps the whole span as written after the draw.
So the next lookup found the whole layer stale, and took and loaded it again, about 350 times a present. Keeping a
target fresh past its own publish does not help here, because the compute write made it stale before the publish.

The fix needs the freshness of a target by range: which runs of its memory changed since its draw, and which of
those its own publish wrote. Then a take can copy the target and load only the runs that something else wrote.

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
