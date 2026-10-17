# ASTC compute uploads

ASTC textures are decoded on the GPU by a compute shader, `src/nx/gpu/texture/astc.comp`. It is a GLSL port of
Eden's `src/video_core/host_shaders/astc_decoder.comp` (GPL-3.0-or-later, with yuzu's GPL-2.0-or-later parts). The
CPU decoder in `src/nx/gpu/texture/astc.c` is a C90 port of Eden's `src/video_core/textures/astc.cpp`, which
carries the University of North Carolina at Chapel Hill's Apache-2.0 decoder (FasTC). The runtime does not link
the CPU decoder. The tests use it as the reference the shader must match byte for byte.

## The shader

Each invocation decodes one 128 bit block, in workgroups of 32. The shader uses only storage buffers, push
constants and 32 bit integer arithmetic, with no floating point, so its output does not depend on the driver's
rounding.

- Binding 0 is the input: rows of whole blocks, 16 bytes each, already deswizzled from block-linear guest memory
  by `texture.c`. Eden's shader deswizzles in the shader instead. That part was left out.
- Binding 1 is the output: one packed RGBA8 word per texel, red in the low byte, in rows of `width` words.
- The push constants are `width` and `height` (the strip's size in texels), the block footprint, and `srgb`,
  nonzero for an SRGB image (the texture header's sRGB bit, which `vk/astc.c` reads from the surface's format).
  `vk/astc.c` dispatches `ceil(blocks per row / 32)` by the strip's block rows. Edge texels beyond the image are
  not stored.

Eden's shader stores floats into a storage image. Here the conversion to 8 bits is explicit:

- LDR channels and the void-extent color are UNORM16 values rounded to nearest, `(c * 255 + 32767) / 65535`.
  This is what storing `c / 65535` into an RGBA8 image gives.
- HDR channels are clamped to [0, 1] and rounded to nearest (halfway cases up) from the half-float bits.
- Invalid blocks are transparent black, Eden's error color. The specification's magenta is not used.
- SRGB images use the specification's sRGB decode mode (Khronos Data Format Specification, ASTC chapter,
  "Weight Application"), which Eden does not implement. For R, G and B, LDR endpoints expand to 16 bits as
  `(e << 8) | 0x80` instead of by bit replication (`e * 257`), and the decoded byte is the top 8 bits of the
  interpolated value, which the specification hands to the sRGB conversion. Alpha decodes as in UNORM. HDR
  endpoints are not affected (the sRGB profile is LDR only, and this decoder keeps decoding them as HDR). An LDR
  void extent's R, G and B are the top 8 bits of its UNORM16 color, its alpha rounds as in UNORM. The bytes are
  still copied without a gamma step: the image's SRGB format applies the gamma when the texture is sampled.
  The two modes differ by at most one step, mostly in void extents and at rounding boundaries.

### Where it differs from Eden

Eden's two decoders do not agree with each other on every block, and both accept blocks that the ASTC
specification treats as errors. Both ports make the same choices, checked against ARM's astcenc (see below):

- Weights stored as plain bits are replicated to 6 bits, as the specification and Eden's CPU decoder do. Eden's
  shader rounds `bits * 64 / (2^n - 1)` instead, which differs for 4 and 5 bit weights.
- Bits past the color endpoint sequence's length read as zero, as Eden already does for the weights. Eden decodes
  the spare values of the last trit or quint block from them, which can change the values that are used.
- HDR endpoint mode 11 (and the RGB of modes 14 and 15) sign-extends only the low bits of its two delta values.
  Eden keeps the bits above them, which this mode shares with other fields.
- The following are errors, as in the specification: a block whose weights and configuration leave no room for
  color data (Eden's CPU decoder decodes it anyway), more than 18 color values, color data too short for 6 levels,
  and a void extent whose coordinates are not all ones but have a minimum not below its maximum.
- The infill factors `Ds` and `Dt` are computed in integers, as in Eden's CPU decoder. Eden's shader uses floats,
  with the same results for the block sizes ASTC has.

## The upload path

- `texture.c` deswizzles guest block-linear memory on the CPU, and only the compressed blocks enter the staging
  ring.
- `vk/astc.c` decodes into a device-local scratch buffer of at most 8 MiB, in strips of block rows that keep both
  descriptor ranges within `maxStorageBufferRange`. Every mip level, array layer and depth slice is uploaded.
- The compute work runs on the graphics queue, outside rendering. A barrier orders the shader's writes before the
  copy into the image, and another orders the copy's reads before the scratch buffer is reused, including across
  submissions. Descriptors come from the frame's pools. Staging stays alive until the frame's fence.
- The bytes are copied into the existing UNORM or SRGB image unchanged. The surface's own barriers make the image
  visible to later sampling.

The pipeline, its layouts and the scratch buffer live as long as the device, like the backend's other internal
pipelines. A failure to create them is fatal: there is no CPU fallback.

## Rebuilding the shader

`src/nx/gpu/texture/astc_spv.h` is generated and checked in, so builds need no shader compiler:

```sh
python3 scripts/build-astc-shader.py            # rebuild the header
python3 scripts/build-astc-shader.py --check    # what ctest runs (astc_shader_current): no compiler needed
python3 scripts/build-astc-shader.py --spv astc.spv
```

The script compiles with `glslangValidator -V --target-env vulkan1.1`, from the official KhronosGroup glslang
16.6.0 release unpacked into `.cache/tools/glslang-16.6.0/`, from `PATH`, or from `--glslang`. It validates with
`spirv-val` when it finds one in `.cache/tools/spirv-tools-*/bin` or on `PATH`. SPIRV-Tools publishes no
binaries, so build `spirv-val` from its source or take it from the Vulkan SDK. The header records the glslang
version and a SHA-256 of the script and `astc.comp`. `--check` compares that hash, so an edit to the shader that
was not rebuilt fails the test.

## Tests

- `astc_decode` (ctest) checks the CPU decoder on void-extent blocks: the color in both decode modes, the
  reserved bits, HDR void extents and empty extents.
- `tests/run_astc_vulkan.py` builds `tests/astc_vulkan.c` with the production `vk/astc.c` and `vk/cmd.c`, volk and
  VMA, and compares raw image readbacks with the CPU decoder byte for byte. It covers all 14 footprints with UNORM
  and SRGB images, each against the CPU decoder in its decode mode, and decodes the same blocks in both modes
  for every footprint, checking that alpha is the same, that R, G and B differ by at most one step, and that
  some do. It also covers images whose size is not a multiple of the block size, dispatch padding, five mip
  levels down to 1x1, six layers, 3D slices, scratch chunking, frame reuse and staging ring growth. The random
  blocks go through all 2048 block modes, so they cover void extents, dual planes, two to four partitions, every
  color endpoint mode, trit and quint weights and invalid blocks. No game files or window are needed:

  ```sh
  VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json \
  VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation \
  VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT \
  python3 tests/run_astc_vulkan.py \
    --vulkan-include .cache/cpm/vulkan-headers/<hash>/include \
    --volk .cache/cpm/volk/<hash> \
    --vma-include .cache/cpm/vulkanmemoryallocator/<hash>/include
  ```

  Without the validation layer installed, the "zero validation errors" line only means that nothing reported an
  error.

The CPU decoder was also compared with ARM's astcenc 5.7.0 (`astcenc -dl` and `-dH`, Apache-2.0, a release binary
used only as a tool), on the same kind of random blocks for every footprint. Every texel agreed within one step of
rounding (astcenc's 8 bit output rounds differently), and every block one rejects the other rejects too. In the LDR
profile astcenc also rejects HDR endpoint modes, which this decoder decodes. The sRGB decode mode was checked
the same way against `astcenc -cs` and `-ds` on five footprints: every byte agreed, alpha included.
