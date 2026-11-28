# Parallel draw preparation (design, not built)

## Index

* [Why](#why)
* [The design: two stages in a pipeline](#the-design-two-stages-in-a-pipeline)
  * [Correctness: Stage 1 results are proposals](#correctness-stage-1-results-are-proposals)
  * [What Stage 1 must not do](#what-stage-1-must-not-do)
* [Shared state](#shared-state)
* [The gain](#the-gain)
* [The hand-off](#the-hand-off)
* [Risks](#risks)
* [Recommendation: a smaller step first](#recommendation-a-smaller-step-first)
* [Measured since](#measured-since)
* [Build steps](#build-steps)
* [What was built](#what-was-built)
* [Measured: the built form loses time](#measured-the-built-form-loses-time)
* [Measured since: batches of draws for worker threads](#measured-since-batches-of-draws-for-worker-threads)

Status: built behind the setting `draw_prep` (off by default), as the section "What was built" tells. The sections
before it are the design as it was reviewed. "Shared state" comes from a map of the data that each stage reads and
writes. The numbers come from one game's open world on Windows (RTX 4090, `gpu_split=2`, fps unlocked).

## Why

In that world the renderer thread sets the frame rate. A present takes 11.95 ms at about 84 fps, and the renderer
thread runs its records for 10.8 ms of it. The front end is busy 4.0 ms, the recording thread about 2.4 ms, and the
busiest game thread 1.6 ms. The renderer prepares about 6,850 draws a present at about 1.6 us each.

The time of a draw is spread over many steps (see [the fast path design](draw-fast-path.md)). Each change measured
so far acted on 1% to 3% of the thread and did not move the frame rate: a stage set kept from the last draw, static
buffers for the large stack frames, the vertex input compared by plan serial. Neither Eden nor Cemu prepares one
frame's draws on more than one thread. The only lever left that can cut the renderer's time by a large share is to
give part of each draw's preparation to a second thread.

## The design: two stages in a pipeline

The renderer's records stay in order. Each draw goes through two stages on two threads:

- Stage 1, resolve (a new thread): `draw_find` and `draw_shaders`, `read_targets`, `fs_key` and the texture resolve,
  `draw_key`, and the pipeline lookup. Its output is a draw's resolved state: the shaders, the targets, the fragment
  variant and its textures, the pipeline key and the pipeline.
- Stage 2, bind and record (the renderer thread): the vertex and index buffers, storage and images, the stage sets,
  the render pass, the binds, the dynamic state, the push constants, and the record into the ring of the recording
  thread.

Stage 1 works on draw N+1 while Stage 2 works on draw N. Records that are not draws (clears, copies, barriers, writes
of the front end) stop Stage 1 at their place: Stage 1 resolves no draw past a record that Stage 2 has not run.

### Correctness: Stage 1 results are proposals

Stage 2 can change what Stage 1 read: draw N writes its targets, adds results that wait to land, makes surfaces, and
can submit. Stage 1 for draw N+1 may have read the state from before that. So a Stage 1 result carries the stamps it
read (the same stamps the memos ask today: `gpu_mem_generation()`, the results added, the storage images, the targets
taken, the surface table, the write epoch, the submission, the present). Stage 2 compares them before it uses the
result. Where one moved, Stage 2 resolves the draw again itself, as the renderer does today. The pipeline can only
lose time, never correctness.

`draw_state_check=1` turns Stage 1 off, or makes Stage 2 resolve each draw again and compare.

### What Stage 1 must not do

Stage 1 must not submit, wait for the GPU, begin or end a render pass, or record a command. A step of Stage 1 that can
do one of these (for example a texture upload that waits, or a reload of a target) stops Stage 1 for that draw, and
Stage 2 does the step.

## Shared state

A map of `render_draw_main` and its callees (render.c, surface.c, texture.c, pipeline.c, record.c) found two blockers
before any single structure:

- `vk_lock` is a reentrant mutex, and each draw holds it from `vkr_draw`. Stage 1 cannot hold it while Stage 2 does,
  and several Stage 1 callees take it (`mem_need_reason` in buffer.c, `vk_compute_mem_need`).
- `ds` is the owner's live `DsState`, which the front end changes after `vkr_draw` returns. Stage 2 of draw N reads
  its dynamic state, viewports, bindings, index, vertex buffers, blend, raster key and targets. Each draw needs its
  own copy of `ds`, of `dc` and of its inline indices.

Structures both stages touch:

- `g_tex_recent` and `g_tex_memo`: both write them. `stage_set_` resolves the textures of the stages before the
  fragment stage and the slots `fs_key` did not resolve, and `tex_memo_of` writes each entry's view and sampler caches.
  One owner means that all texture resolves move into Stage 1.
- `g_targets` and `g_draw`: Stage 1 writes them on each lookup. Stage 2 writes `g_targets` in a predraw probe, and
  `g_draw.last` and `fs` at the end of each draw. The final key needs the feedback slots and edges, which only Stage 2
  knows.
- The surfaces: Stage 2 writes `layout` (in `render_begin` and `vk_surf_use`) and `wgen` (in `target_written`). Stage 1
  writes `gw`, `layout` and the LRU when it reloads or touches a target.
- The results that wait to land: Stage 2 adds them, and Stage 1 lands them on any guest read over them (`gmem_read`).
- `g_rp` (Stage 1 reads it at the merge with the open pass), `g_vk.has_descriptor_heaps` (the heaps retry), and the
  statistics.

Effects of Stage 2 that Stage 1 must see, and their stamps:

- A target written: `Surf.wgen` and the take generation. A result added: the adds counters. A surface made or
  reloaded: the surface table's epoch and the take generation. A submit or a wait: the submission number and the write
  epoch. A result landed: `gpu_mem_generation()`.
- No stamp exists for: a target's layout leaving or entering `UNDEFINED`, a texture's quiet and upload state, the draw
  memo's last entry, and `g_rp` after `render_begin`. Each needs a new counter, or a direct check in Stage 2.

Stage 1 is free of side effects only on its memo hits. Each miss path can land results, submit, end a render pass or
compile: `vk_frame_check`, the guest reads of `draw_find` and `draw_shaders`, the target reloads of `read_targets`, the
uploads and reloads of the texture resolve, `capture_draw`, and the pipeline wait. These paths need `vk_lock` and the
record ring, which has one producer. So Stage 1 must stop at a miss and leave the draw to Stage 2.

Globals that need to change: a per-draw packet for `g_mesh_first`, `g_mesh_payload`, `g_memh_draw`,
`g_setup_restarts`, `g_vk_drawing`, `g_predraw` and `g_capture_draw`. Thread-local copies of `g_pipeline.sh_ds`,
`sh_bind` and the write epoch's stream state. Per-thread statistics. Atomics or a sequence lock for the take
generation, the surface bucket epochs, `g_vk_present`, `Surf.wgen` and `Surf.layout`. A review of every callee that
takes `vk_lock` or checks that it is held.

## The gain

From the measured shares of a draw's time:

| Stage | Steps | Share |
| --- | --- | --- |
| 1, resolve | `draw_find` and shaders 8%, targets and fragment key 12%, formats and draw key 3%, pipeline and setup 3%, the start 4% | about 30% |
| 2, bind and record | stage sets 34%, binds and push constants 15%, buffer prefetch 9%, constant prefetch 3%, storage and images 6%, record 3% | about 70% |

The map puts the start in Stage 2 too, because `vk_frame_check` can submit. Then Stage 1 is about 25% of a draw (2.7
ms a present) and Stage 2 about 73% (7.9 ms). A pipeline goes as fast as its slower stage. With the copy of `ds` and
the hand-off (50 to 100 ns a draw, 0.35 to 0.7 ms a present), the renderer would take about 8.3 to 8.8 ms a present,
against 10.8 ms: about 20% faster, 84 to about 100 fps. That holds only where Stage 1's results stay valid. The write
epoch moves at each submit and the adds counter at each draw that stores, so the share of stale Stage 1 results can
take much of the 2 to 2.5 ms.

Moving all texture resolves into Stage 1 (needed for one owner of the texture memos) shifts a little more work there.

## The hand-off

The hand-off must cost less than about 0.1 us a draw, or it takes most of the gain. Like Eden's command chunks, Stage
1 writes its results into a ring by value (no pointer into state that changes later), in batches, with one atomic
store of the write position for each batch and no allocation for each draw. Stage 2 reads a batch, and waits only
when the ring is empty.

## Risks

1. The shared state (section above): every structure both stages touch needs a lock, an atomic, or one owner. Locks
   on the memos at each draw cost what the pipeline saves.
2. Redos: where Stage 2 changes a stamp at most draws (the GPU-write generation moved between most otherwise equal
   draws in a probe), Stage 1's results go stale. The stamps may need to be finer (ranges, as in the fast path design)
   before the pipeline pays.
3. Order: a record that is not a draw between two draws must stop Stage 1, or Stage 1 reads memory before a write of
   the front end lands.
4. The work is the largest of the GPU plan, with no prior art in Eden or Cemu.

## Recommendation: a smaller step first

The two-stage pipeline gives about 20% at best, and it needs a copy of each draw's state, Stage 1 outside `vk_lock`,
one owner for the texture memos, four new stamps, and per-thread statistics. A smaller split, with only the fragment
key and the texture resolve on a second thread, gains at most about 1 ms and needs most of the same work: not
recommended.

The better first step moves the binds, the dynamic state and the push constants (about 15% of a draw, render.c
7710 to 8085) to the recording thread, which is busy about 2.4 ms a present:

- `g_dyn`, the state of the command buffer, is used only by that code (and `vertex_input_set` and
  `vk_render_forget_state`). It becomes the recording thread's own.
- The renderer sends one packed record for each draw (the pipeline, the sets and their offsets, the vertex buffers,
  the viewports and scissors, the dynamic values, the push constants) in place of about 20 separate commands. The
  recording thread compares it with its last and records only what changed.
- Its inputs are values of the draw, copied in, so nothing on the renderer's side changes under it.

Expected gain: about 1.2 to 1.6 ms of the renderer's 10.8 ms a present, about 11% to 15% fps, with low risk. The
recording thread goes from about 2.4 to about 4 ms a present, still well under the frame.

## Measured since

- The packed draw record was built and measured: the renderer thread stayed at 11.0 ms a present (10.8 to 11.2 ms
  before). The compares it moved cost what the copy of the draw's state into the ring costs. It was dropped.
- A probe took the global stamps after a draw's key (where Stage 1 ends) and after the draw's last commit (where Stage 2
  ends). In the world (about 4.07 million draws in 600 presents), 99.3% of the draws moved none of them. A target taken
  moved on 0.6%, a result added on 0.1%, and the GPU writes, storage images, surface table, write epoch and submission
  on none. So a Stage 1 result made during the previous draw's Stage 2 stays valid for about 99% of the draws, by the
  global stamps. The written targets (`Surf.wgen`) and the layouts have no global stamp: Stage 2 checks those of the
  draw's own textures and targets directly.

## Build steps

1. A copy of each draw's state (`DsState`, `DrawCall`, the inline indices) that the front end hands over, so that
   Stage 2 never reads the live state.
2. Stage 1 as a function of that copy, with no side effect on a memo hit, which returns "do it in Stage 2" at any miss
   (a guest read that can land results, a reload, an upload, a compile).
3. One owner for each shared structure: the texture memos and all texture resolves in Stage 1, the buffer cache,
   rings, sets, `g_dyn` and `g_rp` in Stage 2. Per-thread statistics.
4. The Stage 1 thread outside `vk_lock`, fed through a ring of draw copies, and its results through a ring by value.
5. In Stage 2, the check of the stamps and of the draw's surfaces, and the full resolve where one moved.
6. A setting to turn Stage 1 off, `draw_state_check=1` to compare each result with a full resolve, and the headless
   tests.
7. A/B with at least three runs of 100 s a side.

## What was built

The build takes a narrower form of the design, chosen so that no structure needs a lock or an atomic:

- Lock step, not a free pipeline. The renderer thread gives the next draw to the preparation thread (`prep.c`,
  `vk_prep_kick`) after the current draw's commits, and it waits for the result (`vk_prep_join`) before it lets go of
  `vk_lock`. Between the two, the renderer thread runs only the binds, the dynamic state, the push constants and the
  draw command (render.c, from `rp_record` to the record). That code touches nothing the resolve reads or writes. So
  the two threads never touch one structure at the same time, and the memos keep one owner at each moment.
- The commits of a plain draw (`draw_commit`: its targets written, its feedback slots, its buffer writes) move before
  its binds, so the next draw's resolve sees them. A draw with commits that must follow its command (transform
  feedback, late depth, writes that land, the pre-draw capture, storage images) gives no draw ahead.
- `vk_lock` is lent: the preparation thread runs under the renderer thread's hold (`vk_lock_lend`, depth 2). A take
  of the lock there fails the resolve.
- Stage 1 is `render_draw_main` itself, from its start to the pipeline key, on the kept path only. Each step that a
  memo hit does not cover fails it (`AHEAD_FAIL`, and `gpu_spec_fail` in the callees): a miss of the draw table, the
  targets' memo or a texture memo, a target to reload, a texture to check in full or to take from a target, a guest
  read that would land results (gpu.c), and the rare paths (mesh, raster samples, coverage to color, advanced blend,
  per-vertex passes, save attachments). The renderer thread then resolves the draw itself, as before.
- The preparation thread has its own copy of each draw state (`g_ds_ahead`, renderer.c). It takes from the renderer's
  copy each group whose generation is not its own, then applies the next queue record (`gq_peek_call`). The memos
  that keyed on the state's pointer key on the renderer's copy (`gpu_ds_canonical`).
- The result goes back by value in one record (`DrawPrep`). The renderer thread takes it where the record is the one
  it runs and none of the stamps moved since the resolve began (`prep_holds`): the GPU writes, the results added, the
  storage images, the targets taken, the surface table, the write epoch, the submission, the present, the frame, the
  shader and fold generations and the heaps mode. A draw that takes it skips to the vertex buffers (`resolved:`).
- With `draw_state_check=1`, the renderer thread resolves each draw in full and compares the result made ahead
  (`prep_differs`). The log line "draw_prep over the last 600 presents" counts the draws given ahead, the results
  taken, those stale, and the failures by reason.

- The setting is off (0), on (1) or auto (2). Auto chooses by measurement, as `gpu_split=2` does for the front end
  and renderer split (`gq_auto`): every 600 presents or more, it measures the renderer thread's time in records a
  present (`gq_run_ns`) over 120 presents in the chosen mode, 120 in the other and 120 in the chosen mode again, and
  it changes the mode after two probes in a row cost under 97% of both. On a CPU with few cores the thread competes
  with the game's threads, and the pipeline can then lose time: auto keeps one thread there. The test for that limits
  the process to 4 cores (an affinity mask) and runs the game with the thread on and off.

The gain of this form is at most the binds' and the record's share of a draw (about 18%), not the resolve's (about
25%): the resolve runs only while the binds run.

## Measured: the built form loses time

One game's open world, Windows, RTX 4090, `gpu_split=2`, fps unlocked, one build:

| `draw_prep` | Renderer thread a present | fps |
| --- | --- | --- |
| 0 | 10.6 ms | 80 to 85 |
| 1 | 12.3 ms | 74 |

With `draw_prep=1`, 84% of the draws took the result made ahead (3.09 million of 3.67 million in 600 presents),
about 5,000 were stale and about 7,900 failed. So the results are valid, and the loss is the cost of the form itself:
a hand-off and a wait on each draw, and the memos that move between two cores, against an overlap of only the binds
and the record. A first run also showed that the write epoch has a view on each thread (`vk_ww_view`), which the
preparation thread must take from the renderer thread: without it, the resolve missed every kept entry.

The setting stays off by default. A form that pays must overlap more of a draw than the binds, without a wait on
each draw.

## Measured since: batches of draws for worker threads

The per-draw form (above) loses its time in the hand-off at each draw. A batch form hands a worker a whole run of
draws. A probe (`bp_draw`, `render.c`) cut the draws of one game's world into batches, by three rules, over 600 presents
(about 4.1 million draws):

| A batch ends at | Batches | Draws each | Draws in batches of 16 or more |
| --- | --- | --- | --- |
| any record that is not a draw, or a new pass | 898,000 | 4.6 | 70% |
| the same, but a constant buffer write stays inside | 480,000 | 8.6 | 88% |
| only a new pass | 96,000 | 42.6 | 97% |

With the second rule, 19% of the draws are in batches that sample a target that an earlier batch of the present wrote
(the probe sees targets, not storage images or buffers), and 43% are in batches with a draw that missed a memo of the
draw table, the targets or the texture cache (a lookup in a shared cache). So batches are long and mostly
independent, and the shared caches must take workers' lookups, not only their hits.
