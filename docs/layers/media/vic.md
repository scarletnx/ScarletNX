# VIC composition on the GPU

VIC composes up to eight NV12 or NV21 slots onto an RGBA, NV12 or NV21 surface. `src/nx/gpu/video/vic.c` reads the
config and resolves the slots. Once the renderer is up, `src/nx/gpu/vk/vic.c` runs the composition as a compute
shader and the CPU path (`compose_cpu`) only composes what the shader declines. The shader is
`src/nx/gpu/video/vic.comp`, which declares the buffers, and `vic_compose.glsl`, the body, written in the part of
GLSL that is also C++ so the tests run it on the CPU. The parameters are declared once, in `vic_params.glsl`, for
GLSL, C and C++. Both are ported from Eden's VIC (GPL-3.0-or-later, with yuzu's GPL-2.0-or-later parts).

## The shader

A workgroup is 16x4 invocations and composes a 64x8 tile, which is one GOB of 8 bit luma. Each invocation owns a
block of 4x2 pixels: two luma words and one chroma word of NV12, or eight RGBA words. Every output word has one
owner, so the shader has no shared memory and no barrier. Each input word a pixel needs is loaded once, straight
from its plane, and the neighbors' loads of the same lines coalesce. The dispatch covers the target rectangle
only, since nothing outside it is written.

The previous version staged each tile's input window in shared memory behind two barriers. It composed every
slot that covered a pixel, overwriting the last one, and ran the colour matrix in full for every pixel. The
rewrite removes repeated work in these ways:

- The parameters are a uniform buffer, and `vic_gpu_params` moves every per-job invariant out of the shader. The
  clamp, the alpha and the background are output bytes already. The coefficients are scaled by 4, so 8 bit
  samples need no shift to 10 bit. The rectangles are ends and offsets, and the block linear constants are
  shifts and masks.
- The slots are walked from the last one down, each claiming the pixels that are still unclaimed (a 4 bit mask
  per row), and the walk ends when none are left. One slot over the picture costs one pass.
- Without the matrix a sample goes to 10 bit and back unchanged. NV12 out is then the input words as they are,
  with each chroma pair's bytes swapped when the input and output orders differ, and RGBA out is bytes moved into
  place.
- With the matrix, `((v >> shift) + c3) >> 8` clamped to the 10 bit range and then shifted down by 2 equals
  `((v >> shift) + c3) >> 10` clamped to the range shifted down by 2. Floor division nests, and a monotone map
  commutes with a clamp when `lo <= hi`. A slot whose clamp has `lo` above `hi` is composed by the CPU. The sums
  stay within 31 bits for 20 bit coefficients and 8 bit samples.
- A pair's chroma terms (`U * c1 + V * c2` for each row) are computed once and serve both its pixels and both rows
  of the block when they share a chroma row. RGBA out takes 3 multiplications a pixel. NV12 out computes the R row
  for every pixel, and G and B only for the two pixels a chroma word comes from.
- The branches depend on the slot (source alignment, chroma row parity, matrix), the plane (layout) or the job
  (format), so no subgroup diverges on them. The per-pixel choices are masks.
- A source window that isn't word aligned is two loads funneled into one word. A 4 bit pixel mask becomes a byte
  mask with one multiplication, a partial word is merged with `a ^ ((a ^ b) & mask)`, and a word the invocation
  owns entirely is stored without being read.
- An input word that starts inside a plane and ends past it isn't masked. Those bytes are columns past a pitch
  linear row (block linear planes end on whole GOBs), and no slot covers them.

`vic_gpu_params` refuses a job with a slot outside the target rectangle, which `slot_resolve` never produces,
because the shader writes the target alone.

The shader is built twice from the same source (`cmake/glsl.cmake`). The default build needs every plane's rows to
keep word aligned columns word aligned (block linear planes, or pitch linear ones with a pitch that is a multiple
of 4), which `vic_gpu_aligned` checks. The `-DVIC_UNALIGNED` build funnels its loads and inserts its stores with
atomics, since invocations can then share a word.

## Speed

Measured on lavapipe (LLVM 19, on the CPU) with the real renderer, host copies included, median of 25 jobs. The
old shader is shown as it shipped and recompiled with glslang's `-O`, which made it slower:

| Job | Old | Old with `-O` | New |
| --- | --- | --- | --- |
| 1920x1080 NV12 block linear to A8B8G8R8, BT.709 matrix | 21.4 ms | 30.3 ms | 7.6 ms |
| 1920x1080 NV12 block linear to A8B8G8R8, no matrix | 32.0 ms | 28.7 ms | 6.3 ms |
| 1920x1080 NV12 block linear to NV12 block linear | 19.8 ms | 31.1 ms | 5.4 ms |
| 1280x720 NV12 pitch linear to A8B8G8R8, BT.709 matrix | 14.2 ms | 19.9 ms | 3.8 ms |

Old and new wrote the same bytes in every job. Lavapipe counts operations and shows nothing of coalescing or
divergence, which the new layout also improves on a GPU.

On an RX 6700 XT (RADV), the dispatch alone from GPU timestamps around it, median of 100 jobs per run, runs of both
builds taken in turn:

| Job | Old | New |
| --- | --- | --- |
| 1920x1080 NV12 block linear to A8B8G8R8, BT.709 matrix | 90 us | 49 us |
| 1920x1080 NV12 block linear to NV12 block linear | 108-160 us | 16 us |
| 1920x1080 NV12 block linear to NV12 block linear, BT.709 matrix | 120-167 us | 41-53 us |
| 1918x1080 NV12 pitch linear to NV21 pitch linear (the unaligned build) | 155-166 us | 24-25 us |

The idle card switches between two power states, and both builds slow down together in the slow one, so these are
compared within the fast one. The whole `vic_compose` call is mostly the host copies there (1.4 ms for 1080p to
A8B8G8R8): it gains about 3% with A8B8G8R8 out and 13-15% with NV12 or NV21 out.

## Tests

- `vic_glsl` runs `vic_compose.glsl` on the CPU in both builds (`tests/glsl/vic_glsl.cpp`, `vic_jobs.c`). Random
  jobs go through `vic_execute` from the config and registers a game writes, and each one is compared byte for
  byte with `compose_cpu`. The jobs cover every output format, NV12 and NV21 inputs, pitch linear (word aligned
  and not) and block linear planes of every block height, up to eight overlapping slots, odd source columns and
  rows, crops, planes cut short by their mapping or unmapped, and colour matrices. The test also checks that
  nothing around the output planes was written. Each optimization above was broken on purpose once to see the
  test fail.
- `vic_compose` checks `compose_cpu` itself on hand-made jobs.
- `vulkan_headless` (`-DSNX_TEST_VULKAN=ON`) runs the real SPIR-V through the renderer on a few jobs, a colour
  matrix and NV12 rows that aren't word aligned among them.
