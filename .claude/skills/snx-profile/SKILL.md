---
name: snx-profile
description: "Find where a frame's time actually goes, before changing code, on Windows, Linux, macOS and iOS. It covers the runtime's own counters (--stats: finishes by call site, dispatches, texture uploads, waits by caller), plat_ns timers around a phase, the GPU's own timestamps (vk_gpu_profile) and what replaces them on Metal, and each host's sampler. Use it before any change made for speed, and to prove that one worked."
---

# Profile

A mechanism that sounds right is wrong often enough to cost whole sessions: five changes here were written from a
plausible cause, and each was disproved by its own instrumentation. Measure first, then cut.

## 1. The runtime's counters, on every platform, with no rebuild

`--stats` logs every 600 presents:

- `N finishes (P%, M ms) at file:line` — each submit-and-wait, attributed to the call site that made it. Read this
  first: a frame that waits for the GPU names its own file and line here.
- `N dispatches in N batches (M ms waiting, K a network's pass stood in for), N skipped`.
- `N texture uploads (M MB)` — bytes moved into images, the usual cost of a scene that reloads its pictures.
- `... waits for it`, then `those waits by who asked: ...` — the same waits split by caller.
- `a N ms frame: ... M ms in finishes` — the slow frames, with what they held.

## 2. A timer around the phase you suspect

`plat_ns()` is the clock (`nx/runtime/platform/plat.h`). Sum the phase into a global and print it in the stats block
of `gpu/vk/renderer.c`, under `g_vk_present % 600 == 0`. Wrap the function rather than editing its body, so every
return is covered:

```c
static int tex_rehash_inner(Surf* t) { ... }
static int tex_rehash(Surf* t) {
    u64 a = plat_ns();
    int r = tex_rehash_inner(t);
    g_tx_ns[0] += plat_ns() - a; g_tx_n[0]++;
    return r;
}
```

A timer variable must be set before every path that reads it: an uninitialised one printed 121 million milliseconds
and cost a round. A counter declared in a header must not take a name another module already has (`g_sub_ns`, `g_bg`
and `g_tx_ns` were each taken once).

## 3. The GPU's time

`vk_gpu_profile=1` (`gpuprof.c`) timestamps each rendering pass and dispatch, and logs the frame's span, its passes
and its targets every 10 s.

**It is forced off on a portability driver, by design.** Each timestamp there takes a Metal encoder of its own that
waits for the work before it, and reads unreliably (MoltenVK issues 1585 and 2378). The log says which host it is.

| host | GPU time |
|---|---|
| Windows, Linux | `vk_gpu_profile=1`, where the driver has `timestampComputeAndGraphics` |
| macOS, iOS | `MTL_HUD_ENABLED=1` for the live HUD, or Instruments' **Metal System Trace**. Both are Metal hosts, so neither has the runtime's own profiler |

With no GPU profiler, take the GPU's cost by difference: the same scene at two `--resolution` scales, and at
`--compute=0`. A frame time that does not move with resolution is not the GPU's.

## 4. The CPU's time

- **Windows**: `profiler=1` (the `--profiler` setting) writes `profile.txt` beside the game every 10 s, by thread,
  with waiting threads counted as waiting (`platform/profiler_win32.c`).
- **Linux**: `perf record -g --call-graph dwarf -p <pid>`, then `perf report`.
- **macOS**: `sample <pid> 5 -file out.txt`. It samples every thread, so a busy process can look idle; check
  `ps -o time` as well. Read CPU **seconds per frame**, not samples.
- **iOS**: Instruments, Time Profiler, on a device.

If a sampler disagrees with `ps` about the total, trust `ps` and use the sampler only for the split.

## 5. What a measurement must look like

- One protocol for both builds: same `--seconds`, `--docked`, `--vsync=0`, `--fps_limit=0`, same scene.
- Interleave them A/B/B/A. A fanless host throttles 15% within a minute, and a single window at the cap is not a
  sustained rate: quote the whole per-5-second series.
- When the rate swings, quote CPU seconds per frame; it does not follow the clock.
- Keep the picture: `--shot=<stem>` writes the framebuffer. Faster and wrong is not faster.
- A change is ready only when its mechanism shows in a counter, not only in the rate.
