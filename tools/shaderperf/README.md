<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# snx-shaderperf

Times SPIR-V shaders (compute, vertex, geometry, fragment) on the GPU and compares them. It is made to be driven by an agent:
every input is an option, defaults come from the shader's reflection, and `--json` gives every number.

The tool stands alone. It opens the Vulkan loader at run time and needs only Vulkan's headers to build. It does not
use the runtime's Vulkan code or SPIR-V writer. `cmake --build build --target snx-shaderperf` puts it beside the other
tools. `snx-shaderperf --help` lists every option.

## Workflow

1. `snx-shaderperf --reflect a.spv` shows the bindings (`S.B` targets), the push constant members, the specialization
   constants, the stage inputs and outputs and the workgroup size.
2. Run it. Give only what the defaults get wrong:

   ```sh
   snx-shaderperf a.spv b.spv --size 1920x1080 \
       --bind 0.0=raw=albedo.bc7,format=bc7_unorm,size=2048x2048 \
       --bind 0.1=file=noise.ppm,format=rgba16_sfloat,filter=nearest \
       --push inv_size=f32:0.00052,0.00093 --spec 0=16 \
       --check --target-rse 0.005 --json=out.json
   ```

3. Read `comparisons[].verdict`, `ratio_of_medians` and its interval, and `outputs_match` in the JSON.

## Graphics stages

The measured shaders are one stage; `--vs`, `--gs` and `--fs` give the others of the pipeline, each one shader for
all the measured ones or one for each in order (when their interfaces differ).

- A fragment shader alone gets a built-in full-screen triangle with float `vec4` varyings at locations 0 to 7.
  Throughput is per pixel.
- A vertex shader draws `--vertices` (65536 primitives' worth) with noise in each attribute's format, into
  rasterizer discard unless `--fs` gives a fragment shader. Throughput is per vertex.
- A geometry shader gets the built-in vertex shader (the same varyings, with a full-screen triangle every 3
  vertices) unless `--vs` gives one, and its input primitive picks the topology unless `--topology` does
  (`lines_adj`, `triangles_adj` for adjacency). Shaders of one run must read the same primitive. Throughput is per
  primitive invocation (primitives times the `invocations` mode); the pipeline statistics count its invocations and
  the primitives it emits.

A fragment or geometry shader with integer or other inputs needs a vertex shader: `--print-vs a.spv > a.vert`,
`glslc a.vert -o a.vert.spv`, then `--vs a.vert.spv`.

What the other stages are changes the measured one's time: the render target formats (`--rt`, bytes a pixel and
blending), the vertex attribute formats and the varyings the stages pass, the primitives a geometry shader emits and
whether anything is rasterized. Keep them the same between shaders you compare.

## Inputs

- Images: `raw=` takes the bytes as the GPU reads them, level 0 of all layers first. Block formats (BCn, ETC2/EAC,
  ASTC) come only this way, or as random blocks. `file=` reads PPM, PGM or PFM and transcodes it to a plain or packed
  format, and makes the mips with a box filter. `pattern=` makes one (`noise`, `gradient`, `checker`, `solid:R,G,B,A`).
  A format that the device cannot sample is an error, with no fallback. `--list-formats` shows the support.
- Buffers: `size=`, `elements=` for runtime arrays, `fill=`, `value=TYPE:V,...`, `file=`.
- Push constants by offset or member name, a typed list of values. Specialization constants by id or name, and the
  workgroup size follows them.

## Measurement

- GPU timestamps around `reps` dispatches or draws in one submission. The default `--reps auto` aims at
  `--sample-ms` (2 ms) of GPU time a sample. By default there is a barrier between repetitions, and `--overlap` drops
  it. Before sampling, a warmup of `--warmup` submissions and `--warmup-ms` lets the clocks rise.
- The submissions of several shaders are interleaved (or `--order shuffle`, `sequential`), so drift affects them
  alike. The tool rejects outliers (IQR by default) and reports the median with an order-statistic 95% interval, the
  mean with Student's t, the percentiles, CV and MAD. `--target-rse` samples until the median's interval is tight.
- A comparison gives the ratio of medians with a bootstrap interval, Mann-Whitney and Welch p-values, and a verdict.
  The verdict needs p < 0.01 and an interval that excludes 1.
- Queries: pipeline statistics (invocations a repetition), `VK_KHR_pipeline_executable_properties` (on NVIDIA:
  register count, binary size, stack, shared memory a stage), `VK_KHR_performance_query` counters (`--counters`,
  where the driver has them; NVIDIA's has none), and calibrated timestamps for the latency from submit to GPU start.
  On NVIDIA, `nvidia-smi` gives the clocks and temperature before and after.
- The drivers' shader disk caches are off unless you give `--driver-cache`, so compile times are real.
- `--check` runs each shader once on fresh inputs and hashes all that it writes. `--dump` saves any of it.

The GPU's clocks move between runs. On a GPU that drives a display, the compositor shares the GPU. To compare two
shaders, put them in one run. Do not compare the numbers of two runs.
