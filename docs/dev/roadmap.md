<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# Status + roadmap

Recorded run, 2026-10-11: a game's New Game cutscene, Vulkan, 60 FPS, no audio.

Implemented: Clang/CMake, Linux/Windows CI, SDL3/Vulkan, shader caches, component libs,
`audout` PCM playback, viewport Y swizzles and framebuffer H/V flips.

## Next work

- **Regression:** ARM ISA, IPC, VM, GPU-cache tests. Headless smoke runs + reference images.
- **Diagnostics:** stricter warnings, shader vector-boundary tests at R254/RZ. Import thread-dump
  reader and Maxwell instruction scanner.
- **Audio:** `audren` splitters, voice biquads, delay/reverb/biquad effects. Validate hwopus playback.
- **GPU:** the NVN functions games call that the loader does not implement yet (the game stops and names each).
  Programs that write their own GPU command buffers (OpenGL games, homebrew) go through the Maxwell front end,
  which interprets every macro and has no macro HLE or JIT. 90-degree framebuffer rotation, attachment
  feedback. Check the BCn fallback (decoded on the GPU) on a Mali device.
- **CPU:** more SIMD/FP throughput with exact result bits and FPSR. Stateful helpers already implement NaN
  propagation, default NaNs and directed rounding, with guarded host fast paths. Thread suspension at WFE.
  32-bit games run (A32 and Thumb, `src/nx/runtime/arm.c`), checked only with the synthetic program
  (`tests/nx_arm32_smoke.py`): a real one, ldr:ro's NROs in a 32-bit process, the Thumb32 DSP and
  saturating instructions (QADD16, SMULxy, SSAT), and TBB/TBH targets ahead of time.
- **Updates and DLC:** checked with synthetic dumps and a homebrew game laid out as an update with DLCs, not yet with
  a real game's update or DLC: an update's Patch RomFS read at run time, and a game's own aoc:u calls.
- **Host:** vsync during Win32 window drags, native Windows/gamepad coverage.
- **Performance:** runtime/guest profiling, Vulkan measurements, broader title coverage. The CPU cost of each draw
  on the GPU thread sets the frame rate now: [the plan](draw-cost.md).

Commands: [development](development.md).

## Design

The runtime has two GPU front ends that draw through one Vulkan renderer. Games that use NVN take the NVN path:
the loader (`src/nx/gpu/nvn/nvn_loader.c`) takes every NVN function a game resolves, so the game's NVN driver never
writes command memory, and a queue's recordings replay on the GPU thread. Programs that write their own GPU command
buffers (OpenGL games, homebrew) submit them with `SUBMIT_GPFIFO`, and the Maxwell front end (`src/nx/gpu/gpu.c` and
`src/nx/gpu/maxwell/`) runs them: the GPFIFO puller, the 3D, compute, 2D, DMA and inline-to-memory engines, and the
macro engine, which interprets every macro. The [NVN reference](../layers/gpu/nvn.md) records what the SDK does.
