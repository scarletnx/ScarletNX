# Parallel draw batches (design, not built)

Status: a design for review. Nothing here is built. The numbers come from one game's open world on Windows (RTX 4090,
`gpu_split=2`, fps unlocked), from probes in the uncommitted work of `parallel-draw-prep.md` and `draw-fast-path.md`.

## The problem

The renderer thread sets the frame rate. It runs about 11,800 records a present in about 11 ms. The draws take 10.1 ms
of that: about 6,840 a present at about 1.47 us each. Records that are not draws take about 1.3 ms, constant buffer
writes 0.5 ms of it. The path around a draw (the record's state, `vk_lock`, the call) takes about 100 ns of a draw.

`render_draw_main` takes about 1.37 us a draw, in many steps:

| Phase | ns a draw |
| --- | --- |
| The resolve (draw table, targets, fragment key, textures, pipeline key) | 360 |
| Vertex buffers | 133 |
| Storage | 87 |
| Stage sets: constants 96, storage 19, textures 33, images and the set 43, two a draw | 414 |
| Indices and the pass | 117 |
| Binds 68, vertex input and dynamic state 96, push constants 42, the draw 19, commits 30 | 255 |

Six small changes measured no gain, and part D (buffers by address, device-generated commands, textures on the GPU)
measured or estimated 2% to 4% net. The per-draw work stays. The only change that removes the bottleneck is to set up
draws on more than one thread.

A per-draw pipeline (`draw_prep`, two threads in lock step) lost 15%: a hand-off and a wait at each draw (about 400 ns),
and the memos' cache lines that moved between two cores (the renderer's own draws got about twice as slow).

## Prior art

No emulator that we know sets up the draws of one frame on more than one thread (a review of Ryujinx, Eden and the yuzu
family, Cemu, Dolphin, Citra, RPCS3 and PCSX2):

- Ryujinx (`Ryujinx.Graphics.GAL/Multithreading/ThreadedRenderer.cs`, MIT upstream): the GPU thread sets up the draws.
  A backend thread makes the Vulkan calls from a ring of fixed-size commands. A resource gets a handle at once and its
  object later ("handle first, resource later"). A wake-up goes only at a ring's change from empty.
- Eden and yuzu (`vk_scheduler.cpp`, GPL-3.0-or-later and GPL-2.0-or-later): the GPU thread sets up the draws and records
  lambdas into command chunks. One worker runs the chunks in order. `PrepareDraw` locks the buffer and texture caches
  against the CPU threads, not for setup on more than one thread. ASTC decodes and pipeline compiles go to workers.
- Cemu (MPL-2.0): one thread does the command processor, the setup and the recording. Only pipeline compiles go to
  other threads. No secondary command buffers.
- PCSX2's software renderer (GPL-3.0-or-later) divides a frame by screen bands: each worker sees all draws in order and
  owns its bands. It divides pixels, not draws.

So the design below has no prior art. Two ideas apply: deferred commands in order (yuzu's chunks, our record ring), and
generation stamps to check work done out of order (as `vk_submit_number()` already does in a draw's setup).

## Two forms

### Form A: lookups serial, encoding on workers (the Ryujinx and Eden split)

The renderer thread does each batch's lookups (phase 1): the memos, the surface table, the texture and buffer caches,
the pipeline, the staging copies. Workers do phase 2: the hashes, the descriptor writes, the binds and dynamic state,
the push constants and the record into a list of their own.

Phase 2 is about the images and sets (86 ns), the binds and record (255 ns) and part of the constants: about 25% of a
draw. That is the ceiling of form A, the same share that the per-draw pipeline overlapped. Not recommended as the
target. It is the safe first step of form B.

### Form B: whole batches on workers

A worker sets up whole draws: the resolve, the lookups and the encoding. The renderer thread forms the batches, keeps
their order, and does what a worker must not do. This form can remove the bottleneck. It needs caches that more than one
thread can use.

## Form B in detail

### Batches

The renderer thread cuts the records into batches. A batch ends at a new pass and at a record that is not a draw,
except constant buffer writes, which stay inside it in their order. Measured over 600 presents: 480,000 batches, 8.6
draws each, 88% of the draws in batches of 16 or more. A batch of fewer than 16 draws (12% of the draws) runs on the
renderer thread, as today.

### Workers and the order

- A pool of workers (2 or 3), each with a single-producer ring of batches and a slot for each batch's result. A
  worker wakes once for each batch.
- A worker writes its batch's commands into a command list of its own, in the format of the record ring
  (`record.c`). The recording thread replays the lists in batch order. No secondary command buffers.
- The commits of a batch (targets written, results added, images, queries, transform feedback) go to the renderer
  thread with the list, which applies them in batch order.

### The state each thread owns

- Per worker: the per-draw memos (`g_draw.last`, `g_targets`, `g_fs_last`, `g_tex_recent`, `g_dyn`, the stage-set
  caches `g_cb_windows`, `g_storage_recent`, `g_table_words`, `g_dset`), a staging arena, the command list, the
  statistics. A batch starts with the memos of the last batch the worker ran. The draws of one pass are mostly the
  same state, so the memos stay warm on that core.
- Shared, read on the hit path by all: the draw table (`g_draw.e`), the surface table and the surfaces, the texture
  memo (`g_tex_memo`), the buffer cache, the pipeline cache, the descriptor set cache. Today none of these has a lock
  of its own: `vk_lock` guards them all (`texture.c`, `surface.c` and `descriptors.c` take no lock of their own).
- Renderer thread only: submits, landings of results, waits for the GPU, presents, the records that are not draws,
  render pass begins and ends.

### Shared caches

43% of the draws are in batches with a miss of the draw table, the targets or the texture cache. So a worker must handle
misses, not only hits. Each shared cache gets a rule:

1. Reads on the hit path without a lock: entries that are written whole and published with a release store (a
   generation word last), and entries freed only when no batch of an older generation runs (epoch reclamation, as the
   frames in flight already keep descriptor sets and views alive).
2. Misses under a lock of that cache. A miss that needs the GPU (an upload, a reload of a target, a decode) records
   its copy into the worker's list, before the draw, as the commands of an upload do today.
3. A miss that needs a submit, a wait or a landing stops the batch. The renderer thread runs the rest of the batch.

The LRU lists and the statistics become per worker, merged at a present.

### Batches that depend on others

19% of the draws are in batches that sample a target that an earlier batch of the present wrote. The command order of
the replay keeps the GPU's order correct. The CPU's view must also be correct: a texture memo that checks a target's
`wgen`, a copy of a target that a draw samples, the layout and the barriers.

- Each batch notes the surfaces it samples and writes, with the generation it saw.
- Before the renderer thread replays a batch's list, it compares those with the commits of the earlier batches. A
  mismatch discards the batch's list, and the renderer thread sets the batch up again, in order.
- A batch that is known to depend on an earlier one (the same pass's targets sampled, a pass that follows a target's
  pass) waits for it instead of running at the same time.

## Gain

With 81% of the draws in batches that depend on no earlier batch, two to three workers, and the shared caches' cost
unknown: the renderer thread's 10 to 11 ms a present could go to about 4 to 6 ms. The frame then waits for the next
limit: the front end at about 4 ms a present, or the recording thread at about 2.4 ms, or the GPU. This estimate holds
only if the hit paths stay free of locks and the memos stay on their worker's core.

## Build steps, each with a gate

1. Measure what form A would leave serial: a timer around the lookups of each phase. Gate: if the lookups are more
   than about 75% of a draw, form A can't pass about 25%, and only form B is worth building. Measured (all timers on,
   about 1,460 ns a draw): the lookups are about 910 ns (62%): the resolve 386, vertex buffers 138, storage 94,
   `cb_get` 104, the texture lookups 33, the stage sets' storage 38, the indices about 94, the pass's begin 21. The
   encoding is about 510 ns (35%): the binds and record 263, `desc_set_cached` 56, the constants' windows and writes
   about 90, the textures' descriptors and target checks about 51, the images and the rest of the set about 50. The
   gate passes: form A's ceiling is about a third of a draw, less the copy of each draw's lookups to its worker.
2. One worker, form A: the encoding of whole batches into a list, the replay in order. Gate: lists equal to the
   renderer's own (a check mode, as `draw_state_check`), and no loss against today.
3. The per-worker state: the memos, the staging arena, the statistics, moved out of globals into a context that each
   draw gets (today they are globals of `render.c`).
4. The shared caches, one at a time, with their rules and a stress test in `tests/`: the draw table, the surface table,
   the texture memo, the buffer cache, the descriptor set cache, the pipeline cache.
5. Form B with one worker, then two and three. Gate at each: `draw_state_check=1` reports no difference, and an A/B
   of at least three runs of 100 s a side shows the renderer thread's time a present.
6. The dependency check, and the stats line: batches given, run, discarded and set up again.

## Risks

1. The size: `render.c` is about 8,300 lines of code written for one thread, with its state in globals, and the
   callees in `surface.c`, `texture.c`, `buffer.c` and `pipeline.c` rely on `vk_lock`. Step 3 alone touches most of it.
2. Locks on the hit path, or cache lines shared by the workers, can take what the threads give, as the per-draw
   pipeline showed. Each shared cache needs a measurement under two workers.
3. A dependency that the check misses gives a wrong frame, not a crash: a stale texture over a target, a missing
   barrier. The check mode must compare each batch with a serial setup.
4. A small CPU: the workers compete with the game's threads. The setting must choose by measurement, as `gpu_split=2`
   does.

## Built so far (uncommitted)

- Step 3, the per-thread state: the per-draw memos of `render.c` (the draw table, the targets, the fragment key, the
  recent textures, the texture memo, the constant buffer memos and windows, the storage and set caches, the index and
  quad buffers, the per-draw block, and the command buffer's bind state `g_dyn`) hold an entry for each slot
  (`RC_SLOTS`), which a thread takes by `t_rc`. The functions that forget them clear every slot. Cost, three runs of 100
  s a side with a shader cache for each build: the renderer thread 11.54 ms a present at 79.2 fps without, 11.79 ms at
  77.2 fps with (about 2%).
- The command lists (`record.c`, `VkRecList`): a thread's packets go to its list, and `vk_rec_list_splice` puts a list
  into the ring. Each thread has its own batch of barriers and its own draw tags. Descriptor set ids come from an
  atomic count. A command that works on the ring itself (a command buffer's begin, a submit, a drain) first puts the
  thread's list into the ring.
- Batches in turn (`draw_batches=1`): after a draw, the draw records that follow it go to the worker thread
  (`prep.c`), which sets them up into its list with slot 1 and its own copies of the draw state
  (`gpu_ds_batch_draw`), while the renderer thread waits. The renderer thread then splices the list and passes over
  those draws (`gpu_ds_batch_done`). The picture is right. A first run showed wrong draws: both threads record into one
  command buffer, so the bind state of the slot that did not record last was stale. Each side now binds everything
  again after the other recorded. Speed: 72 fps, the renderer thread 12.6 to 13.7 ms a present, slower than without (as
  expected: the renderer thread waits for each batch).

Next is step 4: the shared state that a batch and the renderer thread would touch at the same time. Besides the caches
above, that is the render pass (`g_rp`), the surfaces' layouts and write counts, the staging ring, the buffer cache, the
queries, the statistics, and the descriptor pools' counts.

### Shared calls of a batch's draws (measured)

A probe counted the calls of shared functions from the worker's draws in serial batches (600 presents, 155,000
batches of about 20 draws, 3.14 million draws, 76% of all draws). For each draw in a batch: the buffer cache about 14
calls (`vk_buf_prefetch` 11.8, `vk_buf_get_input` 2.3, `vk_buf_draw_prepare` 0.24), the surfaces about 7
(`vk_surf_touch` 2.2, `vk_rt_note_read` 2.2, `vk_surf_wrote` 1.5, `vk_surf_use` 1.0), the page order tracking 2.0
(`vk_pages_read`), the queries 1.0 (`vk_query_draw`), the staging ring 0.38 (`vk_stage_upload`), the texture checks,
views and samplers about 0.09, the pipelines and descriptor sets under 0.01.

So step 4 starts with the four structures that every draw uses: the buffer cache, the surfaces, the page tracking and
the queries. Each must take two threads at once, or keep a part for each thread that the renderer thread merges in
batch order (the page tracking and the queries record an order, and the surfaces' write counts and layouts decide
barriers and the texture memos' freshness).

### The buffer cache's hit path (mapped)

A map of `vk_buf_prefetch`, `vk_buf_get_input`, `vk_buf_get_uniform_at` and `vk_buf_draw_prepare` (`buffer.c`, with
`uma.c` and `pagespace.c`) found that a hit is not only a lookup. Where guest memory is read in place (the unified
memory hybrid, the default on a discrete GPU with resizable BAR on Windows), a hit also:

1. writes memos, counters and statistics of the last lookup (`g_buffer.got`, `g_buffer.lk`, `g_buf_kind`,
   `g_buf_sources`, `g_uma.memo`, `g_uma.rmemo`, `g_uma.hits`) and LRU stamps (`Vb.used`, the own buffers' list,
   `g_uma.imp[].used`);
2. records a barrier, and ends the render pass for it, where a page was written in place since the last barrier
   (`order` in `uma.c`, `order_use` in `pagespace.c`). That choice depends on the order of all writes and barriers
   before the draw;
3. takes `vk_lock` in `vk_release_pending` on the page cache's path, and where image or compute results wait, in
   `vk_gfx_image_read_need` and `vk_compute_mem_need`, which can land them.

So the buffer cache needs three things: a part for each thread for (1), a lock for each draw's buffer phase for the
entries and their LRU, and for (2) a change of where barriers are chosen. A worker notes the pages its batch reads and
writes, and the renderer thread chooses the barriers when it splices the batch, in batch order. The surfaces' layouts and
write counts choose barriers the same way. That change is the core of form B, and larger than the build steps above
said. Results land only on miss paths and in rare checks, so an asynchronous download of results does not change the
renderer thread's time in this scene (its waits are about 0.2 ms a present).

## Barriers at the splice: predict and check

The calls that choose barriers and passes by the order of all earlier work (`vk_surf_use`, `render_begin`, the unified
memory's `order`, the page cache's `order_use`) do not run on a worker. Each becomes a deferred call in the worker's
list (`vk_rec_defer`), which the renderer thread runs when it splices the list, at the call's place, with the state
of all earlier batches applied. Its barriers then go into the ring there, in batch order.

A worker's later draws in the batch read the state that those calls change (a surface's layout, stage and access).
So the worker keeps a view of its batch's changes (an overlay of the surfaces it used), and the readers ask the
overlay first (`vk_surf_layout` and the like).

A deferred call can also end the render pass (a barrier needs the pass ended), and the commands after it in the list
assume the pass the worker saw. Dropping the rest of a list is not safe: it can hold uploads that the CPU's caches
already count as done. So a deferred call corrects in place. It records the barriers that the real state needs at the
splice, and where that ended the pass, the splice opens the same pass again (with its loads) before the draws that
follow. For that, the worker records with each draw a deferred pass call with the draw's targets. A batch still ends
where the worker sees a new pass: it hands back the draws from there, and their uploads so far stay in the list. In
batches in turn (serial), the splice never corrects. In batches in parallel, a correction is a batch that depends on
an earlier one, and the checks of its sampled surfaces (above) decide whether it is set up again.

The steps: deferred calls in the lists, then `vk_surf_use` with the overlay and the check, then `render_begin` (a
batch ends at a new pass, and the worker hands back the draws from there), then the unified memory's and the page
cache's order.

### The deferral, one funnel

Every change of a surface's layout, stage and access in use goes through `vk_surf_use` (about 78 calls in 13 files:
the pass's begin, uploads, copies, the copies of targets, blits). The other writes set a surface to UNDEFINED where it
is made, loaded again, swapped or moved. So the deferral does not need the paths one by one:

1. On a worker, `vk_surf_use` changes the worker's overlay and defers the real call to the splice. The commands that
   follow it in the list (an upload's copy, the draw) stay after it, so the order holds.
2. The readers of a surface's layout, stage and access on the draw's path ask the overlay.
3. A surface made, loaded again, swapped or moved on a worker, and a new pass, stop the batch at that draw. The renderer
   thread sets up that draw and the rest. The commands that the worker recorded for it so far stay in the list.

Built so far: the deferred calls in the lists (`vk_rec_defer`, run by `vk_rec_list_splice`), not yet used.

### Rewrite progress (serial batches, the picture right after each)

1. Deferred calls in the lists (`vk_rec_defer`), run at the splice.
2. `vk_surf_use` on a worker: the worker's view of the surface (layout, stage, access), the pass ended where its
   prediction needs a barrier, the real use at the splice (`surf_use_splice`, which counts mispredictions: none in
   the runs). 24 readers of a surface's layout on the draw's path ask `vk_surf_layout`. A surface in the view is never
   made over, loaded again, moved, swapped or freed before the list's calls run (`vk_surf_before_reset`).
3. The pass state `g_rp` for each slot: a worker starts from the renderer thread's, and the renderer thread takes the
   worker's after the splice.
4. The buffer cache's lookup state for each thread (`t_lookup`: the last lookup, its kind and sources, the patch
   scratch, the staged map of a draw with nothing aliased). The unified memory's memos (`g_uma.memo`, `g_uma.rmemo`)
   and the descriptor set cache for each slot (`vk_render_slot`).

Left: the surfaces' other state that draws change (write counts `wgen`, `gw`, recency, discards, `pred_undefined`),
the buffer cache's entries and recency under a lock, the page tracking and the queries for each batch, the memory
order's barriers deferred with the same prediction, the staging ring and the statistics for each thread, then batches
in parallel with the check of the dependencies.

### Split runs (`draw_batches=2`): what the validation runs showed

- The worker applies the records before its half (`gpu_ds_batch_apply`) while the renderer thread runs the same
  records. The draw queue frees a record when the renderer thread is done with it, so the front end could put new
  records over bytes that the worker still reads. The renderer thread now keeps those records in the queue from the
  split to the join (`gq_keep`, `gq_keep_end`).
- The validation layer reports `VUID-vkCmdBindVertexBuffers2-pStrides-06209` (stride 6, attribute extent 8) only in
  runs whose shader objects come from SPIR-V. This occurs in the first run after each build, because the binaries of
  the shader objects are not used then. The base build draws the same layouts: a 6-byte stream with an 8-byte
  attribute is valid, and `vkCmdBindVertexBuffers2` gets no strides. With no pipeline bound, the layer (SDK 1.4.341)
  takes the binding stride as dynamic and compares the stride of `vkCmdSetVertexInputEXT`. With binaries, it has no
  SPIR-V to reflect and skips the check. A split run with a warm cache shows no error of this kind. Compare
  validation runs only at the same cache state.

### Split runs: the measured cost (one build, one scene of about 6,860 draws a present)

| | The renderer thread a present | fps | Its own draws a present | ns a draw (resolve, setup, binds) |
| --- | --- | --- | --- | --- |
| `draw_batches=0` | 12.70 ms | 72.6 | 6,851 | 394, 874, 300 |
| `draw_batches=2` | 12.73 ms | 72.6 | 4,448 | 542, 1,303, 471 |

The worker draws about 2,400 draws a present, at about 2.1 µs each. But each draw that stays on the renderer thread
costs about 750 ns more, and that cancels the gain.

- The share lock: at first, 90,000 takes a present on each side, and each side waited 3.9 ms. The lock's cache line
  moves between the two cores at each take. One take for each group of calls, no lock for an atomic getter, no
  prefetch hint during a split, and a `vk_cmd` with no lock during a split brought it to 41,000 takes and 1.5 ms
  of wait on each side. That is still about 360 ns of each draw of the renderer thread.
- The rest of the 750 ns (about 390 ns) is not measured yet. The likely cause is shared data that both threads write
  in each draw: statistics counters, the recency stamps of cache entries, and per-slot arrays with small entries
  whose slots share a cache line.

To gain, a draw must not write a cache line that the other thread also writes. The hit paths of the buffer cache,
the surfaces and the unified memory tables must then read with no lock and write nothing that is shared. That is a
larger change than the steps so far.

### Split runs: the renderer thread is the limit of this scene

An earlier version of this section said that the guest's threads limit the depths scene of game-11ccd5f3. That was wrong. It read the
line "the GPU thread over the last 600 presents: busy", which is the front end's GPU thread (`gpu.c`,
`gpu_thread_busy_ns`). The renderer thread is the GPU renderer thread of `gpu_queue.c`, and its line is "the renderer
thread ran N records a present in X ms". In base, that is 11.5 to 12.4 ms of a 12.6 to 13.4 ms present (about 91%).
The guest's threads together ran about half of one core.

### Split runs: the changes that made the split gain

Each change was measured in base and split runs in turn, on the same build and the same route, with no other load.
The numbers are for the presents with about 4,900 KB of records (the same scene in each run).

| Build | Change | Base fps | Split fps | The renderer thread, base and split (ms a present) |
| --- | --- | --- | --- | --- |
| before | | 79 | 69 | 11.5, 13.4 |
| c3 | the share lock's counts on each side's own cache lines; the wrappers call through while no run is split; the worker's copies of the draw state taken by group | 74.3 to 74.9 | 76.0 to 79.9 | 12.2 to 12.5, 11.4 to 11.8 |
| c5 | the batch probe and the draw marks on the renderer thread alone; the worker's statistics counters apart, folded in at the join | 73.8 to 75.2 | 76.9 to 81.0 | 12.2 to 12.5, 11.4 |

(The first row came from an older build and from runs with the game read from another disk, so compare its two
columns only with each other.)

- The draw marks (`g_draw_marks`) were one array that both threads wrote. The worker's marks went into the renderer
  thread's steps: "to render_draw" showed 400 ns a draw that did not exist. With the marks on the renderer thread
  alone, it is 43 ns.
- The steps of a split on the renderer thread are small: the peeks 0.07 ms a present, the copies 0.05 ms, the kicks
  0.01 ms, the joins 0.39 ms.
- What remains: a draw on the renderer thread costs about 30% more in split runs than in base (resolve 467 ns
  against 373, setup 1,105 against 849, binds 381 against 280). The share lock explains about 160 ns of it (28,000
  takes and 0.7 ms of waits a present on each side). The rest is probably the shared caches that both threads read
  and write (the buffer cache, the surfaces, the page tables). The next step is to measure those with no lock.

### Split runs: fewer takes of the share lock

The scene and the presses are those of the table above. Each row is one or two runs of 180 s. The renderer thread's
time is from its busy line, "ran N records a present (K KB) in X ms", in runs at about 4,900 KB a present.

| Build | Mode | fps | Renderer thread, ms a present | Share lock takes a present, renderer thread |
| --- | --- | --- | --- | --- |
| c6: the commit of the split | base | 74.0 to 74.8 | 12.1 to 12.4 | |
| c7: `draw_timing=0` | base | 76.4 to 76.7 | 11.95 | |
| c7 | split | 81.6 to 81.7 | 11.16 | 28,086 |
| c8: the takes grouped | split | 83.5 to 83.7 | 10.9 | 22,211 |
| c9: the vertex and index memo | split | 84.0 to 87.1 | 10.5 to 10.8 | 19,666 to 19,750 |

- c7: the clock reads of the draw phases, the draw marks, the batch probe and the times of the share lock are now
  read only with the `draw_timing` setting (default 0). They cost about 3% in base.
- c8: the vertex streams of a draw and the miss path of `cb_get` (the pending checks and the staging upload) take
  the lock one time, not one time for each call. A call inside a held group does not set the label of the next take
  (`vk_share_lock`). Before this, the counts by function gave takes to the wrong function.
- c9: `vin_get` keeps the vertex and index lookups of each thread by address, with the rules of `cb_kept`. A hit
  takes no lock. 89% of the 16,300 lookups a present hit.
- With c9 the split is about 14% faster than c6 in base. A run of a main build in the same scene is not done yet.
- Prior art (from memory, not checked against the sources): Ryujinx and the yuzu family resolve the cache state on
  one thread. The yuzu family takes its cache locks one time for each draw, not for each lookup.
- What remains: `vk_surf_use` (3,700 takes a present, both sides) is the next largest. After the lock, the renderer
  thread spends about 4.7 ms a present in records that are not draws. The GPU time of this scene is not measured.

### The search for a scene that the renderer thread limits

Of the logs on this machine, only those of game-11ccd5f3 and of the Ounce game have the busy line. An older Ounce log (from a build of
another branch) showed the renderer thread busy 18.8 ms of a 19.3 ms present, at about 1,830 draws and 10 µs a draw.
This build gives 1.6 to 2.2 ms of busy time a present, at 1,260 to 1,730 draws, 1.1 µs a draw, and 59 to 77 fps.
So with this branch, neither game has a scene that the renderer thread limits. The split cannot gain in them.

The Ounce runs found a bug of this work. The ring's batch of barriers was made per thread for the worker's lists.
But with the split off, other threads also record into the ring, under `vk_lock`. A thread's batch then stayed
pending after another thread submitted its command buffer. It went into the ring later, into a command buffer that
was not recording (`VUID-vkCmdPipelineBarrier2-commandBuffer-recording`), and the driver crashed on the recording
thread. The ring has one batch again. Only a thread that records into a list batches apart, and `list_pause` puts
that batch into the list before the list goes into the ring.
