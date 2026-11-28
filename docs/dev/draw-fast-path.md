# A draw-level fast path (design, not built)

## Index

* [The problem](#the-problem)
* [What repeats from draw to draw](#what-repeats-from-draw-to-draw)
* [What can be kept, and what runs on each draw](#what-can-be-kept-and-what-runs-on-each-draw)
* [The design](#the-design)
* [The gain, revised](#the-gain-revised)
* [Risks](#risks)
* [Measurement plan](#measurement-plan)
* [Recommendation](#recommendation)
* [Measured since: the phases of a draw, and runs for device-generated commands](#measured-since-the-phases-of-a-draw-and-runs-for-device-generated-commands)
  * [The offsets in push constants (window epochs) and D2's ceiling](#the-offsets-in-push-constants-window-epochs-and-d2s-ceiling)

Status: a design for review. Nothing here is built. The numbers come from one game's open world on Windows (RTX 4090,
`gpu_split=2`, fps unlocked), from timestamp marks in `render_draw_main` and from the runtime's sampler.

## The problem

In that world the renderer thread sets the frame rate. At about 84 fps a present takes 11.95 ms, and the renderer
thread runs its records for 10.8 ms of it. The front end is busy 4.0 ms, the recording thread about 2.4 ms, and the
busiest game thread 1.6 ms. The renderer prepares about 6,850 draws a present at about 1.6 us each, almost all of it in
`render_draw_main` (`src/nx/gpu/vk/render.c`).

The time of a draw is spread over many steps:

| Phase of `render_draw_main` | Share |
| --- | --- |
| Stage sets (`stage_set_`) | 34% |
| Binds and push constants | 15% |
| Targets, capture and fragment key | 12% |
| Buffer prefetch | 9% |
| `draw_find` and shaders | 8% |
| Storage and images | 6% |
| Start, formats and draw key, pipeline and setup, record | 3% to 4% each |

Two smaller changes were measured and dropped. A stage set kept from the stage's last draw kept 67% of the stage sets,
but its key cost about what it saved (net about 2% of the thread). Static buffers for the large stack frames gave no
gain.

## What repeats from draw to draw

A probe compared each draw with the draw before it. In the world:

- 67% of the draws have the same generations as the last draw for `DS_PROGRAMS`, `DS_VFORMAT`, `DS_BLEND`,
  `DS_RASTER_KEY`, `DS_XFB`, `DS_TARGETS` and `DS_TARGET_FORMATS`, and the same texture pools.
- 28.6% also have the same global stamps. Of the stamps, only `gpu_mem_generation()` differs on many of the other
  draws. The results added, the storage images, the targets taken, the write epoch, the surface table, the submission
  and the present stay the same on almost all of them.

Such a draw changes its constant buffers and inline banks, its vertex and index buffers, its dynamic state and
viewports, and the values of its `DrawCall`.

## What can be kept, and what runs on each draw

A map of `render_draw_main` (lines 6501 to 8167 at the time) puts its steps in three classes. KEPT steps depend only on
the kept groups, the pools and stamps. EACH steps must run for each draw. MIXED steps have parts of both.

- KEPT: the shaders of `draw_find` (S4, without its constant buffer fold and viewport checks), the targets (S6),
  `alpha_integer` (S7), the predraw choice (S12), the target drops of S14, the "nothing drawn" test (S15), the pipeline
  key `k` and `pl` of `draw_key` (S18, with `pl.empty` checked against the vertex buffers), and the pipeline (S29).
- MIXED: the fragment key (S11: the texture handles come from a constant buffer, so they change with it), the coverage,
  sample location and `fs_spec` keys (S16: they read `DrawCall` fields and dynamic state), the blend and coverage
  choices (S9, S10).
- EACH: everything from the vertex buffers on: S19 vertex buffers, S21 storage, S23 images, S24 the stage sets, S26
  indices, S27 transform feedback, and S32 to S37 the pass, the binds, the dynamic state, the push constants and the
  record.

Most KEPT work is already kept by memos: `g_draw` (`draw_find`, `draw_keep`), `g_targets` (`read_targets`),
`g_fs_last` (`fs_key`) and the texture memos (`g_tex_recent`, `g_tex_memo`). Each memo checks its own stamps on each
draw. So a fast path does not skip whole phases. It replaces several memo checks with one check.

## The design

One record, `DrawFast`, holds the last full draw's kept outputs and its key. A draw takes it when the key holds, and
then runs only the EACH steps.

The key:

- the generations of the seven kept groups, and the pools;
- the texture handles the fragment and vertex programs read, read again from their constant buffer with one
  `gmem_read` and compared, because the constant buffer changes with the draw;
- the fields of `DsDynamic`, `DsViewports` and `DrawCall` that the kept part reads: the viewport transform and
  swizzle, the viewport 0 depth, cull and bias, alpha test, window and sprite origin, clip height, the fixed-function
  fields (raster samples, coverage, multisample flags, sample locations, subpixel bias, advanced blend, state flags,
  shading rate), the topology, mesh and indirect, and whether the vertex buffers are empty;
- the owner's predicate and the query state;
- the stamps: results added (with unified memory's writes), storage images, targets taken, the surface table, the write
  epoch, the present, the shader generation, the fold generation and the descriptor heap mode;
- for `gpu_mem_generation()`, a range check, not a stamp. The record keeps the generation `gw0` and a list of ranges:
  each slot's texture and sampler headers, each non-target texture's memory, the memory of a texture kept over a
  target, and the targets. On a draw where the generation moved, each range is tested with
  `gpu_mem_dirty_since(gw0, address, size)`. Where all are clean, the record takes the new generation.

What a hit copies: the shaders and `last`, `edges` and `points`, the targets (`rt`, `zt`, `n`, `bound`), the blend and
coverage choices, `fs`, `fk` and the per-slot `TexRes` and `TexOver`, `tf`, the coverage and sample location keys,
`fs_spec`, the pipeline key and `pl`, `ke` and the pipeline.

What a hit does again:

- each `vk_rt_note_read` of the kept part (the targets, a target sampled whole, the source of a copy of a target). A
  note can reload a target and move the surface table's epoch, so the notes come before the stamp checks, and a moved
  epoch ends the hit;
- the merge with the open pass (S8);
- the `shadows=0` test (S15) and the asynchronous pipeline test (S17);
- the bookkeeping of S31 (`ke->used`, `g_draw.last`, `g_draw.fs`) and the statistics.

Never kept:

- feedback draws (a feedback slot, `predraw_slots`, a predraw probe), capture draws, texture draws and the predraw
  phases;
- a draw with `draw_state_check=1`. There each draw runs in full, and a draw the fast path would have taken is
  compared with it;
- a record past a present, a retry (each `goto retry` drops the record), or a draw after `predraw_run` or
  `draw_forget`.

## The gain, revised

The first estimate assumed that a hit skips about 60% of a draw. The map shows that the stage sets, the buffers, the
binds and the record (about 61% of a draw) run on each draw anyway, and that the KEPT part (about 25%) is already
kept by memos. A fast path saves the checks of those memos, not their work.

So the estimate is about half of that 25%, on the draws that hit. With a hit rate between 28% (the global stamps) and
60% (a range check that works), that is 3% to 8% of the renderer thread, about 0.3 to 0.9 ms a present. That is two
to four times the dropped stage set split, and it is still within the run-to-run noise of a single run.

## Risks

1. The kept part reads more than the kept groups (risk 1 of the map: viewports, dynamic state, vertex buffers, the
   fixed-function fields, the predicate). Each one left out of the key gives a wrong draw.
2. The texture handles come from a constant buffer. A hit that does not compare them binds the wrong textures. The
   existing `draw_holds` may already miss a change of the vertex texture and image header variants, which come from
   the same handles. That needs a check before this design.
3. A draw writes its targets. A texture kept over a target, or a sampled target, can be stale at the next draw
   (`tex_over_holds`, the layout).
4. Read notes and lookups can reload targets and make surfaces in the middle of a hit.
5. A submit during the setup lands results and voids the stamps.

## Measurement plan

1. Fix the data drive of the test copy first: the runs crash in the game's loader while that drive loses writes.
2. Count the hits of the key above in a probe, with the range check, in the world.
3. Build it behind a setting, off by default, with `draw_state_check=1` comparing each hit with a full draw.
4. A/B with at least four runs of 60 s a side, because the expected gain is near the noise of fewer runs.

## Recommendation

The gain is smaller than the first estimate: 3% to 8% of the renderer thread, for a change with the risks above in
the core of the draw path. Measure the hit rate with the range check first (step 2). Build it only if that rate is
near 60%.

The larger share is the stage sets (34%) and the binds (15%), which run on each draw. Making those cheaper, or moving
them off the renderer thread, is the bigger lever, and it needs its own design.

## Measured since: the phases of a draw, and runs for device-generated commands

Timer marks on the renderer thread (`render.c`, `g_ph`) in the same world, 4.1 million draws in 600 presents:

| Phase | ns a draw |
| --- | --- |
| The resolve (start to the pipeline key) | 360 |
| Vertex buffers | 130 |
| Storage | 90 |
| Stage sets (2 a draw, about 180 ns each: constants 85, storage 18, textures 32, images and the set 44) | 390 |
| Indices and the pass | 120 |
| Binds, dynamic state, push constants and the record | 245 |

Runs of draws in a row (`vk_draw_runs=1`, `record.c`), with two keys added for device-generated commands (D2):

- With only push constants, vertex and index buffers and the draws' own numbers changing (what one
  `vkCmdExecuteGeneratedCommandsEXT` can take): 3.42 draws a run. The descriptor sets' dynamic offsets move on
  almost every draw, as each draw's constants are copied to a new place in the ring.
- The same with those offsets changing too, as they would once they are push constants (part of D1): 9.94 draws a
  run, the longest 477.
- Keeping the same pipeline and the same set handles, 93% of the draws are in runs of 8 or more.

So D2 needs D1's offsets in push constants first. Even then it removes at most the binds and the record (245 ns, about
18% of a draw) and writes a sequence of tokens for each draw in their place. The resolve, the buffer lookups and the
stage sets stay.

### The offsets in push constants (window epochs) and D2's ceiling

The dynamic offsets that end the runs are the constant buffer windows' bases: each bank moved its 64 KiB window of the
staging ring at its own time (1.20 million moves in 600 presents, of 16.9 million window binds). The setting
`vk_cb_window_epochs=1` gives all windows in the ring one base, which moves for all of them at once; a kept copy below
the base is copied again. Window moves fell to 0.27 million, and the runs one device-generated execute could take grew
from 3.42 to 10.08 draws.

The epochs alone cost time (100 s a side, one build): the renderer thread 10.9 ms a present at 83 fps off, 11.2 to 11.4
ms at 80 fps on. The constants part of a stage set rose from 96 to 111 ns (the copies made again).

The binds and record (256 ns a draw) in parts: pipeline, blend and sets 68, vertex input, buffers and dynamic state 96,
push constants 42, the draw command 19, the commits after it 30. Inside a D2 run, the push constants, the vertex and
index buffers and the draw still go as tokens, and the dynamic state must still be computed and compared to find where a
run ends. So D2 would save the ring's commands and a few binds, about 50 to 80 ns a draw (4% to 6%), against the
epochs' 30 ns: 2% to 4% net, for the largest change of part D. Not built.

Part D stopped here (2026-11-24). In this world no change of part D measured or estimated removes more than about 4% of
the renderer thread net: the per-draw time is the resolve, the buffer lookups and the stage sets, which each draw needs.
The probes stay in the worktree for later measurements: `vk_draw_runs`' device-generated keys (`record.c`), the phase
timers (`g_ph`, `render.c`), the texture handle probe and the offsets probe.
