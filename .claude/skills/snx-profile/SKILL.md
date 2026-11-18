---
name: snx-profile
description: "Find where a frame's time actually goes, before you change code, on Windows, Linux, macOS and iOS. It covers the runtime's own counters (--stats: finishes by call site, dispatches, texture uploads, waits by caller) and plat_ns timers around a phase. It also covers the GPU's own timestamps (vk_gpu_profile), what replaces them on Metal, and each host's sampler. Use it before any change made for speed, and to prove that one worked."
---

# Profile

A mechanism that sounds right is wrong often enough to cost whole sessions. Five changes here came from a plausible
cause, and their own instrumentation disproved each one. Measure first, then cut.

## 1. The runtime's counters, on every platform, with no rebuild

`--stats` logs every 600 presents:

- `N finishes (P%, M ms) at file:line` — each submit-and-wait, attributed to the call site that made it. Read this
  first: a frame that waits for the GPU names its own file and line here.
- `N dispatches in N batches (M ms waiting), N skipped <why>` — the dispatches that the runtime skipped, with a count
  for each reason (`past the GPU's limits`, `a storage image not bound`, and the rest of the `g_cs_skip_names` list in
  `gpu/vk/compute.c`).
- `N texture uploads (M MB)` — bytes moved into images, the usual cost of a scene that reloads its pictures.
- `... waits for it`, then `those waits by who asked: ...` — the same waits split by caller.
- `a N ms frame: ... M ms in finishes` — the slow frames, with what they held.

## 2. A timer around the phase you suspect

`plat_ns()` is the clock (`nx/runtime/platform/plat.h`). Sum the phase into a global and print it in the stats block
of `gpu/vk/renderer.c`, under `g_vk_present % 600 == 0`. Wrap the function and do not edit its body, so that the timer
covers every return:

```c
static int tex_rehash_inner(Surf* t) { ... }
static int tex_rehash(Surf* t) {
    u64 a = plat_ns();
    int r = tex_rehash_inner(t);
    g_tx_ns[0] += plat_ns() - a; g_tx_n[0]++;
    return r;
}
```

Set a timer variable before every path that reads it. An uninitialised one printed 121 million milliseconds and
cost a round. A counter declared in a header must not take a name another module already has (`g_sub_ns`, `g_bg`
and `g_tx_ns` were each taken once).

## 3. The GPU's time

`vk_gpu_profile=1` (`gpuprof.c`) timestamps each rendering pass and dispatch, and logs the frame's span, its passes
and its targets every 10 s.

**The runtime forces it off on a portability driver, by design.** Each timestamp there takes a Metal encoder of its
own that waits for the work before it, and reads unreliably (MoltenVK issues 1585 and 2378). The log says which host
it is.

| host | GPU time |
|---|---|
| Windows, Linux | `vk_gpu_profile=1`, where the driver has `timestampComputeAndGraphics` |
| macOS, iOS | `MTL_HUD_ENABLED=1` for the live HUD, or Instruments' **Metal System Trace**. Both are Metal hosts, so neither has the runtime's own profiler |

With no GPU profiler, take the GPU's cost by difference: the same scene at two `--resolution` scales, and at
`--compute=0`. A frame time that does not move with resolution is not the GPU's.

## 4. The CPU's time

- **Windows**: `profile=1` (the `--profile` setting) writes `profile.txt` beside the game every 10 s, by thread,
  and a thread that waits gives no sample (`platform/profiler_win32.c`).
- **Linux**: launch the game under perf with its events off, and turn them on for the scene: `mkfifo ctl ack`,
  `perf record -F 199 --call-graph dwarf,8192 -D -1 --control fifo:ctl,ack -- <the game's command>`, then
  `echo enable > ctl` and `echo disable > ctl`, and `perf report`. Attached with `-p <pid>`, perf runs out of time
  while it reads the game's memory maps, and the game's own frames come out `[unknown]`. The `dwarf,65528` setting
  slowed a game three times over.
- **macOS**: `sample <pid> 5 -file out.txt`. It samples every thread, so a busy process can look idle. Check
  `ps -o time` as well. Read CPU **seconds per frame**, not samples.
- **iOS**: Instruments, Time Profiler, on a device.

If a sampler disagrees with `ps` about the total, trust `ps` and use the sampler only for the split.

## 5. What a measurement must look like

- One protocol for both builds: same `--seconds`, `--docked`, `--vsync=0`, `--fps_limit=0`, same scene.
- Interleave them A/B/B/A. A fanless host throttles 15% within a minute, and a single window at the cap is not a
  sustained rate: quote the whole per-5-second series.
- When the rate swings, quote CPU seconds per frame, which does not follow the clock.
- Keep the picture: `--shot=<stem>` writes the framebuffer. Faster and wrong is not faster.
- A change is ready only when its mechanism shows in a counter, not only in the rate.
