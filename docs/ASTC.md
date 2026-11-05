# ASTC compute uploads

## Index

* [The shader](#the-shader)
  * [Written for the machine](#written-for-the-machine)
  * [Interface](#interface)
  * [Where it differs from Eden](#where-it-differs-from-eden)
* [The upload path](#the-upload-path)
* [Encoded again as BC7](#encoded-again-as-bc7)
  * [The BC7 cache](#the-bc7-cache)
* [Textures bigger than one allocation](#textures-bigger-than-one-allocation)
* [Building the shader](#building-the-shader)
* [Tests](#tests)

ASTC textures are decoded on the GPU by a compute shader, `src/nx/gpu/texture/astc.comp` with its body in
`astc_decode.glsl`. It is a GLSL port of Eden's `src/video_core/host_shaders/astc_decoder.comp` (GPL-3.0-or-later, with yuzu's GPL-2.0-or-later parts). The
CPU decoder in `tests/support/astc.c` is a C port of Eden's `src/video_core/textures/astc.cpp`, which
carries the University of North Carolina at Chapel Hill's Apache-2.0 decoder (FasTC). The runtime does not link
the CPU decoder. The tests use it as the reference the shader must match byte for byte.

## The shader

A workgroup of 64 invocations decodes a tile of 64 blocks in two phases. First each invocation decodes one 128 bit
block into the block's slot of the tile in shared memory: the weights, the partitions' endpoints and the numbers
that place a texel in the weight grid and the partitions. After a barrier the 64 invocations write the tile's
texels together, one texel each at a time, in rows. The shader uses only storage buffers, push constants, shared
memory and 32 bit integer arithmetic, with no floating point, so its output does not depend on the driver's
rounding.

The block decoding is serial by nature (a sequence of fields of variable length), and one block per invocation
runs it once per block. The texels are not: an invocation that also wrote its own block's texels, as Eden's port
does, ran a loop of up to 144 texels whose stores land a footprint apart in each store of a subgroup, whose
branches differ from invocation to invocation with the blocks' modes, and which leaves most of the GPU idle on a
small texture or mip level. In the second phase each store of the workgroup is 64 neighbors in a row, the texel
code has no branch but the image's edge, and a tile of few blocks still keeps all 64 invocations busy. The cost is
that a texel finds again what the loop kept in registers: its block and coordinates, its partition, its endpoints'
interpolation terms and the second plane's weight. That is about 110 operations and 13 shared memory reads a texel
where the loop took about 45.

`astc.comp` holds only the declarations: the workgroup size, the footprint, the push constants, the two buffers and
the tile in shared memory (39 words a block, 9984 bytes). The decoder is `astc_decode.glsl`, which it includes, written in the part
of GLSL that is also C++ so that the tests run the same file on the CPU (see Tests). The rules are at the top of
`tests/glsl/glsl.hpp`.

### Written for the machine

Some mobile drivers neither inline across functions nor fold constants, unroll loops or strength-reduce, so the
shader does that work in its source, and glslang's optimizer (`-O`) inlines everything into one function with no
local variables: every value is an SSA register, and memory is touched only through the two buffers and shared
memory (53 access chains, where Eden's port had 383 and 88 function-local variables).

- **No private arrays.** A private array read at a computed index goes to scratch memory on many GPUs. The decoded
  values (color values, then weights) are bytes in 24 words of the block's slot, each field of the 64 slots side
  by side so that the first phase's invocations keep to their own banks. The partitions' modes are nibbles of one
  word, and the integer-sequence encodings 2 and 4 bit fields of constants.
- **Tables in constants.** What varies per block is read from constants with a shift and a mask: an encoding's
  kind and bit count, the block mode's layout (a nibble table on the bits that tell the layouts apart) and its
  weight grid size (each layout's field shift, width and addend), the HDR endpoint modes (the bits of `0xC88C`), and
  the unquantization constants per bit count. A sequence's unquantization is chosen once per block, never per value.
- **Multiplication instead of loops.** Replicating `b` bits to 8 or 6 is `(v * m) >> s` with `m` and `s` from a
  table, as is the trit and quint formula's `B` term. The color classes of all partitions are summed with one
  multiplication by `0x1111`, and a mode shared by all partitions is replicated by one.
- **No loops that only walked fixed fields.** The search for the largest color encoding that fits (21 steps, each
  with two divisions' worth of arithmetic) is a sum of 21 comparisons against `b * n + extra`, which equals the
  search because the lengths only grow with the encoding. Trits and quints are decoded a block at a time from
  constant field offsets: 3 quints from one word, 5 trits from two. Bits are decoded as many to a word as 31 bits
  hold. The stream is shifted down once per such group.
- **Nothing cleared or read back.** A word of decoded values is written whole each time a value joins it, so the
  storage is never cleared and never read before it is written.
- **The footprint is a specialization constant.** `vk/astc.c` makes one pipeline per footprint, so the weight infill
  steps (`Ds`, `Dt`, each a division) and everything else that depends on the footprint are constants. Dividing a
  tile's texel coordinates by the footprint is a multiplication by a 17 bit reciprocal, exact for every footprint
  and coordinate (checked).
- **Two channels per operation for each texel.** In both decode modes an interpolated channel is exactly
  `(e0 * (64 - w) + e1 * w + 32) >> 6`, which was checked for every endpoint pair and weight: the UNORM16
  expansion and rounding and the sRGB mode's `(e << 8) | 0x80` expansion both reduce to it, so the two modes differ
  only in void extents. R and B share one word, G and A another, in 16 bit lanes: each word is one multiply-add,
  `e0 * 64 + 32 + (e1 - e0) * w`, whose lanes never carry into each other, the negative differences being exact
  modulo 2^32. The second plane's channel adds its own difference times the difference of the weights, which is 0
  with one plane.
- **No branch per texel.** Whatever would make a texel take another path is folded into the block's slot. An HDR
  partition's endpoints are both the error color, and a solid block (a void extent, or an error) is one partition
  whose endpoints are both its color: each interpolates to itself whatever the weight. The partition scores of
  partitions a block doesn't have are 0, which never wins, so one partition needs no test. A weight grid the size
  of the block infills to its own weights exactly (checked for every footprint), so it needs no path of its own.
  The 4 partition scores are two words of 16 bit lanes, two multiply-adds each. Weight grid neighbors past the grid
  always weigh 0 (checked for every footprint, grid size and texel), so they are read without bounds checks, two
  neighbors from one funneled pair of words.
- The UNORM16 to 8 bit conversion of void extents is `(c * 255 + 32895) >> 16`, which equals
  `(c * 255 + 32767) / 65535` for every `c` up to 65535 (`astc_glsl` checks all of them), without the division.

On lavapipe, decoding a 4096x4096 picture encoded by astcenc (`-fast`) takes these milliseconds, including the copy
into staging and into the image. Lavapipe runs on the CPU: it counts operations, and none of what the tiles are
for (coalesced stores, invocations that don't diverge, a busy GPU on small textures) shows there, while its
barriers cost extra (it switches between a workgroup's invocations at each). The tiled version measured 107, 68,
66 and 67 milliseconds with its barrier removed:

| Shader | 4x4 | 6x6 | 8x8 | 12x12 |
| --- | --- | --- | --- | --- |
| Eden's port | 161 to 172 | 110 to 128 | 97 to 110 | 78 to 92 |
| Block per invocation, texel loop (the previous version) | 63 to 76 | 45 to 49 | 42 to 48 | 40 to 43 |
| Tiles of 64 blocks (this one) | 111 to 133 | 87 to 110 | 79 to 97 | 77 to 105 |

Which of the last two is faster on a GPU is not measured: the tiles do more arithmetic per texel, and win back
what scattered stores, divergence and idle invocations cost there.

### Interface

- Binding 0 is the input: rows of whole blocks, 16 bytes each, already deswizzled from block-linear guest memory
  on the GPU by `blocklinear.comp` (docs/BLOCKLINEAR.md). Eden's shader deswizzles in the shader instead. That part
  was left out: the deswizzle is one shader for every format.
- Binding 1 is the output: one packed RGBA8 word per texel, red in the low byte, in rows of `width` words.
- The push constants are `width` and `height` (the size of each slice of the strip in texels), `blocks_x` and
  `blocks_y` (the same in blocks), `srgb`, nonzero for an SRGB image (the texture header's sRGB bit, which
  `vk/astc.c` reads from the surface's format), and `tile_shift`: a tile is `1 << tile_shift` blocks wide and
  `64 >> tile_shift` high. `vk/astc.c` picks the shape that leaves the fewest slots of the strip empty, the widest
  of equal ones, and dispatches the strip's tiles across, down and its slices: a workgroup's z is its slice in both
  buffers. The footprint is the specialization constants 0 and 1. Texels beyond the image are not stored.

Eden's shader stores floats into a storage image. Here the conversion to 8 bits is explicit:

- LDR channels and the void-extent color are UNORM16 values rounded to nearest, `(c * 255 + 32767) / 65535`.
  This is what storing `c / 65535` into an RGBA8 image gives.
- Invalid blocks are the LDR profile's error color, opaque magenta, where Eden's is transparent black.
- SRGB images use the specification's sRGB decode mode (Khronos Data Format Specification, ASTC chapter,
  "Weight Application"), which Eden does not implement. For R, G and B, LDR endpoints expand to 16 bits as
  `(e << 8) | 0x80` instead of by bit replication (`e * 257`), and the decoded byte is the top 8 bits of the
  interpolated value, which the specification hands to the sRGB conversion. Alpha decodes as in UNORM. An LDR
  void extent's R, G and B are the top 8 bits of its UNORM16 color, its alpha rounds as in UNORM. The bytes are
  still copied without a gamma step: the image's SRGB format applies the gamma when the texture is sampled.
  The two modes differ only in void extents, by at most one step: for interpolated texels both expansions give the
  same byte (see above).

### Where it differs from Eden

Eden's two decoders do not agree with each other on every block, and both accept blocks that the ASTC
specification treats as errors. Both ports make the same choices, checked against ARM's astcenc (see below):

- Weights stored as plain bits are replicated to 6 bits, as the specification and Eden's CPU decoder do. Eden's
  shader rounds `bits * 64 / (2^n - 1)` instead, which differs for 4 and 5 bit weights.
- Bits past the color endpoint sequence's length read as zero, as Eden already does for the weights. Eden decodes
  the spare values of the last trit or quint block from them, which can change the values that are used.
- Both decoders follow the LDR profile, as edenprobe's astc cases hold them to: an HDR void extent is an error,
  and a partition with an HDR endpoint mode (2, 3, 7, 11, 14 or 15) is the error color on its own texels while
  the block's other partitions decode. Eden decodes HDR endpoints. Its HDR endpoint decoding was left out.
- The following are errors, as in the specification: a block whose weights and configuration leave no room for
  color data (Eden's CPU decoder decodes it anyway), more than 18 color values, color data too short for 6 levels,
  and a void extent whose coordinates are not all ones but have a minimum not below its maximum.
- The infill factors `Ds` and `Dt` are computed in integers, as in Eden's CPU decoder. Eden's shader uses floats,
  with the same results for the block sizes ASTC has.

## The upload path

- `vk/astc.c` decodes into a device-local scratch buffer of at most 16 MiB, a strip at a time. A strip is block rows
  of one slice, or whole slices when one fits, so a 3D texture's thin slices share a dispatch and a copy. Strips keep
  both descriptor ranges within `maxStorageBufferRange` and the staging within the allocation cap. Every mip level,
  array layer and depth slice is uploaded.
- `texture.c` stages a strip's guest bytes as they are, whole rows of blocks, as `vk_astc_upload` asks for them, and
  `vk/blocklinear.c` deswizzles them into a device-local scratch buffer the decoder reads, so a level is never
  copied whole into memory of its own and the CPU never takes GOBs apart.
- The compute work runs on the graphics queue, outside rendering. A barrier orders the shader's writes before the
  copy into the image, and another orders the copy's reads before the scratch buffer is reused, including across
  submissions. Descriptors come from the frame's pools. Staging stays alive until the frame's fence.
- The bytes are copied into the existing UNORM or SRGB image unchanged. The surface's own barriers make the image
  visible to later sampling.

The pipeline, its layouts and the scratch buffer live as long as the device, like the backend's other internal
pipelines. A failure to create them is fatal: there is no CPU fallback. Running out of staging is not: the level
is left out, the texture keeps its old hash, and the next check uploads it again.

## Encoded again as BC7

Most desktop GPUs sample BCn but not ASTC, and a decoded ASTC texture is four bytes a texel where the game's blocks
take one (4x4) or less. With the `astc_bc` setting (a `startup` one, off by default: BC7 is close but not exact),
`format.c`'s `TA()` gives such a texture a `VK_FORMAT_BC7_UNORM_BLOCK` or `_SRGB_BLOCK` image in place of RGBA8, and
`vk_astc_upload` runs a second dispatch after the decode: `src/nx/gpu/texture/bc7.comp`, with its body in
`bc7_encode.glsl`, encodes the decoded texels into BC7 blocks, an invocation a block, into a second scratch buffer of
the same size, and the copy writes those blocks into the image. A BC7 strip starts on a row of whole 4x4 blocks, so
its ASTC block rows are a multiple of 4 / gcd(block height, 4), and whole slices are counted by the blocks' bytes
too, which for a slice smaller than a block are more than its texels'. `device.c` turns `textureCompressionBC` on for
it whatever `native_bc` says, and `format.c` then samples BCn as it is only when `native_bc` asks too. A 3D texture
keeps RGBA8 where the device has no 3D BC7 images, as native block formats do. Every 600 frames the log says how many
levels went up as BC7 and their megabytes against RGBA8's.

Without `astc_bc` the runtime chooses (`vk_astc_as_bc7`): an ASTC texture is exact RGBA8 while the memory budget holds
the working set, and from the first time it does not, BC7 for the rest of the run for every texture of
`VK_BC7_AUTO_TEXELS` (256x256 texels, all layers) or more, 3D ones aside. "Does not hold" is `vk_mem_trim` having to free
what the last second used (any step past its first): one game's textures then took 5.7 GB of a 5.7 GB budget as RGBA8
and were evicted and uploaded again every frame (600 to 1000 ms frames), where as BC7 they took 2.5 GB. The textures
made before keep RGBA8 until the trimmer evicts them, and come back as BC7. In a sample of 1116 of another game's ASTC
textures, those under 256x256 were a quarter of them and 1.5% of their RGBA8 bytes: small textures (often UI) stay
exact for almost no memory. BC7 is not exact: on one game's 6x6 textures the full build gives 53.8 dB over RGBA, the
fast build 50.7, both with a worst channel error of 50 (see the tables below).

A texture with holes in its mapping (a game's sparse texture, `texture.c`) is BC7 whatever `astc_bc` says, where the
GPU samples BCn: one game's are 68 layers of 8192x8192 ASTC 8x8 with seven of them mapped, 1.7 GB each as RGBA8 (16
times the game's bytes), which took the GPU past its memory. `device.c` turns `textureCompressionBC` on wherever the
device has it for this. A sparse texture's levels are decoded only in the strips that have memory bound
(`vk_image_sparse_bound`), in strips as high as the device's sparse blocks.

A level of `VK_BC7_FAST_TEXELS` (2048x2048) or more takes the encoder's fast build, `bc7_fast_spv` (`SNX_BC7_FAST`
1): a block whose mode 6 error is 256 or less (squared, summed over its 16 texels and 4 channels: 2 a channel on
average) keeps mode 6, and only the others try every mode, where the full build keeps mode 6 only when it is exact.
A level of `VK_BC7_MODE6_TEXELS` (8192x8192) or more takes mode 6 alone, `bc7_mode6_spv` (`SNX_BC7_FAST` 2). On a
GeForce RTX 2080 an 8192x8192 level of random 8x8 blocks (`snx-test-astc-vulkan --bench`) decoded in 15 ms and
encoded in 860 ms with every mode and in 48 ms with mode 6 alone; the game above needed about a thousand million
texels at once.

The fast build was mode 6 alone, which holds gradients but not edges: on the picture below gradients at 43 dB, edges
at 26, noise at 25 and alpha at 40, and on astcenc's normal maps (luminance and alpha apart) errors of up to 239 a
channel. The threshold keeps 96% of the blocks in mode 6 and most of the full build's quality:

| One game's ASTC 6x6 textures (25), and astcenc's 4x4, 8x8 and 12x12 encodings of 7 of them (CPU model) | Full | Fast build | Mode 6 alone |
| --- | --- | --- | --- |
| 6x6, PSNR over RGBA / worst channel error | 53.8 dB / 50 | 50.7 dB / 50 | 46.5 dB / 147 |
| 4x4 | 46.6 dB / 65 | 45.9 dB / 65 | 32.9 dB / 239 |
| 8x8 | 47.6 dB / 50 | 46.7 dB / 50 | 36.2 dB / 231 |
| 12x12 | 46.4 dB / 68 | 45.7 dB / 68 | 38.0 dB / 196 |
| The picture below: gradients, edges, noise, alpha | 51.1, 49.0, 29.0, 48.0 | 43.4, 46.8, 29.0, 47.7 | 43.2, 26.2, 25.2, 40.0 |

What it costs depends on the content, as the blocks that try every mode are as slow as in the full build and hold up
the invocations beside them. On the RTX 2080, beside a running game, a 4096x4096 level took (decoding included):

| Content | Mode 6 alone | Fast build | Full build (2040x2040, scaled) |
| --- | --- | --- | --- |
| One game's ASTC 6x6 albedo | 7 to 11 ms | 12 to 14 ms | 190 ms |
| One game's ASTC 6x6 packed masks (the worst of its textures tried) | 7 to 8 ms | 85 to 88 ms | 250 ms |
| astcenc 8x8 normal map | not measured | 256 to 267 ms | not measured |
| Random 8x8 blocks | 21 to 29 ms | 189 to 196 ms | 260 ms |

The encoder tries each block in the modes that suit it and writes the one with the least squared error. Every
block takes mode 6, one line through RGBA with 4-bit weights, which holds a block of one gradient closely. An opaque
block (every alpha 255) also takes the modes that store no alpha and so keep it 255: modes 1 and 3, two subsets of
one of the format's 64 partitions, each with a line of its own (6-bit ends and 3-bit weights, or 7-bit and 2-bit),
for an edge between two colours, and mode 2, three subsets, for three colours meeting. Any other block takes mode 7,
two subsets with alpha. The partitions tried are the two of 64 two-subset ones, and the one of 64 three-subset ones,
whose subsets spread least around their means, a pass over the block each. Last, every block takes modes 4 and 5,
one subset whose colour and alpha are two lines, each with weights of its own, in each of the format's four
rotations: alpha apart, or red, green or blue apart in alpha's place, as ASTC's second plane keeps one channel. Mode 5
stores the colour in 7 bits and alpha in 8, both with 2-bit weights; mode 4 the colour in 5 and alpha in 6, one of
the two with 3-bit weights (the index selection bit says which). The colour's line is fitted first, and the alpha's
only when the colour's error alone is still under the best block so far.

A subset's line starts from its bounding box, each channel turned to run with the channel the subset spans most (the
sign of their covariance), pulled in by a sixteenth from each end and tried as it is too; then a least squares fit
to the weights the texels took, twice, solved from whole sums, so a channel of one value keeps that value. The ends'
p-bits are tried in every combination the mode allows, but a subset whose every alpha is 255 (or 0) takes the one
bit that keeps it exactly: a nearly opaque texel blends, and a nearly clear one passes an alpha test of more than
nought. Modes 4 and 5 have no p-bits and hold 255 and 0 as they are. Each texel takes the best of the three weights
around its projection. All of it is integer arithmetic in the part of GLSL that is also C++, so the tests run the
same file on the CPU and get the GPU's bytes.

The encoder is written for the code the GPU runs, not only for what it computes. Each partition is read from its
table once a shape (`SetShape`), into registers: the tables are arrays indexed by a value that differs between
invocations, which the GPU keeps in local memory, and reading them for each texel of each fit made a block with
subsets cost hundreds of times one without. Each body is called from one place, in a loop: the shapes and modes 4 and
5 are jobs of one loop in `Encode`, a subset's four candidate lines one loop in `FitSubset`, and the least squares
fit sums every channel in one pass. glslang inlines every call, and the copies came to 1.2 MB of machine code for
NVIDIA's driver; it is 145 KB now, and an 8192x8192 level that took 10.9 seconds takes 0.86. The blocks are the same,
byte for byte: the old and new files, run on the CPU over 65,536 blocks of noise, opaque noise, edges and two
colours with alpha, wrote the same bytes, and a broken anchor in the new one showed.

What it costs, as PSNR against what it encodes, on edenprobe's astcimage picture (gradients, hard edges where three
colours meet in a block, noise, and alpha) decoded from ASTC: 39.9 dB over the picture at 4x4 (gradients 53.1,
edges 48.9, noise 34.2, alpha 50.9), 40.4 at 6x6, 46.2 at 8x8 and 45.2 at 12x12. Mode 6 alone gave 27.6 at 4x4,
its edges 23.5. Noise is what BC7 holds worst: a block of independent texels is a line or two of them at best.

### The BC7 cache

The memory budget's trim evicts textures that the game uses again, and each comes back through the encoder: in one
game 6,000 to 14,000 level encodes in a 150 second run, each 3 to 5 ms of GPU time (up to 149 ms in one frame). The
same ASTC bytes always become the same BC7 blocks, so the blocks of each encode are kept, in memory as an index and on
disk as `shader_cache/<game>.vkbc7`, next to the shader files (`gpu/vk/bc7_cache.c`). The file stays from run to run,
so a later run hits from its start.

- **The key** is XXH3-128 (`vk_hash128`) of the level's guest bytes and the words that place its blocks in them
  (block linear or pitch, the GOB block's height and depth, GOBs across, the row's bytes and rows, the layers:
  `texture.c`'s `astc_content`), then of that hash with the level's size, slices or layers, footprint, sRGB decode
  mode, the encoder's build (full, fast or mode 6 alone) and `VK_BC7_ENCODER_VERSION` (`vk_bc7_key`). The bytes are
  hashed as the CPU has them. A level of which the GPU holds any byte (an engine's result, `memory/own.c`) is not
  keyed: `vk_guest_peek` gives a pattern that names the owner, not the bytes. A level that the page table does not
  map whole is not keyed, and neither is a sparse texture's level (only its bound strips are decoded).
- **The file's version** is XXH3-128 of the SPIR-V of the deswizzle, the decoder and the encoder's three builds (a
  change to any shader changes it), `VK_BC7_ENCODER_VERSION`, the GPU's vendor and device, the driver's version and
  its pipeline cache UUID (`vk_astc_cache_open`). A file of another version starts empty.
- **A hit** (`vk_astc_upload` with the key): the record's blocks are read from the file 1 MiB at a time, checked
  (XXH3 of each piece, folded) while they are copied into staging, and one copy writes the whole level, every layer or
  slice, into the image. There is no decode, no encode and no read of the ASTC blocks. A record that does not check
  out is dropped, and the level is encoded again.
- **A miss** encodes as before, and also copies each strip's blocks into their place in the frame's readback ring
  (`vk_stage_back`, host-cached memory), with a barrier to the host after the last strip. When that submission is
  done, `cmd.c`'s landing (`vk_astc_frame_done`) appends the blocks to the file. The GPU thread never waits for
  them. Blocks in flight are bounded to `VK_BC7_CAPTURE_BYTES` (32 MiB), which bounds the ring's growth. That is also
  the largest level the cache takes (a 4096x4096 level is 16 MiB). A larger level is encoded each time, as before
  (from 8192x8192, it is mode 6 alone).
- **Memory:** only the index stays in RAM, 32 bytes a level, and one 1 MiB buffer for the reads. The blocks stay in
  the file (and the system's page cache).
- **Disk:** the file has a bound of `VK_BC7_CACHE_MAX` (4 GiB). A record that would go past it starts the file over.
  A record that a crash cut short ends the reading, and the file is cut there. While the file is open, it is locked
  (`snx_lock_file`). A second process of the same game runs without the cache, so it does not write the same file.

RGBA8 levels are not cached. Their decode is the cheap part: an 8192x8192 level decoded in 15 ms on the RTX 2080,
about 1 ms per 4096x4096 level. As RGBA8, a cached 4096x4096 level would be 64 MiB to read from the file and to
copy through staging and across PCIe. That is several times what the decode costs.

## Textures bigger than one allocation

Some drivers fail device memory allocations past 256 MiB even where `maxMemoryAllocationSize` is larger (NVIDIA's on
FreeBSD), and a texture can be bigger once decoded: a 2D ASTC texture of 11584x11584, or a 3D one of 512^3, is
512 MiB of RGBA8. `vk/memory.c` caps every allocation at 256 MiB (or `maxMemoryAllocationSize`, when smaller):

- An image whose memory is bigger (a texture, or a render target at a high resolution scale) is created with sparse
  binding and bound to several dedicated allocations of at most the cap, on the graphics queue, which the host
  waits for before the image is used. Shaders and views see one ordinary image. The device needs the
  `sparseBinding` feature on the graphics queue's family; without it the image gets one allocation, which the
  driver may still accept.
- The staging and readback rings stop growing at the cap: a full ring at the cap is followed by another one, and a
  request past it fails, so large uploads stage in parts. `texture.c` stages a level bigger than 64 MiB (or the
  cap) in bands of block rows of one slice, converted band by band.
- VMA's own blocks stay within the cap.

The `[Debug]` setting `vk_max_allocation_mb` (`--vk_max_allocation_mb=4` for a run) lowers the cap, to test the pieces on a driver that accepts anything (lavapipe).

## Building the shader

Nothing compiled is checked in. `cmake/glsl.cmake` compiles `astc.comp` at build time with
`glslangValidator -V --target-env vulkan1.1 -O -g0 --spirv-val`, into `build/generated/spirv/astc_spv.c`, which
holds the words. `src/nx/gpu/texture/astc_spv.h` declares them (`astc_spv` and `astc_spv_size`, in bytes). glslang's
depfile makes an edit to `astc_decode.glsl` rebuild it too.

glslangValidator is KhronosGroup's glslang 16.6.0 release for the host, a CPM package pinned by its SHA-256, on Linux
x86-64, Windows x86-64 and macOS. Other hosts (FreeBSD, Linux on ARM) use the one on `PATH`, from the system's glslang
package, and `-DSNX_GLSLANG=path` picks any. The release has the SPIR-V validator and optimizer built in, so no
separate SPIRV-Tools is needed.

## Tests

- `astc_decode` (ctest) checks the CPU decoder on void-extent blocks (the color in both decode modes, the
  reserved bits, HDR void extents and empty extents), and on edenprobe's illegal block and HDR partition block
  against its reference decoder's texels.
- `astc_blocks` (ctest) holds the CPU decoder to single blocks from rEden's decoder tests, texel for texel: an
  illegal block at each error exit, HDR partitions, color data cut short inside a trit or quint group under every
  value of the unallocated bits, the color value limits, void extents with their bounds in and out of order, and
  astcenc's blocks.
- `astc_glsl` (ctest, no GPU) runs `astc_decode.glsl` on the CPU through `tests/glsl/glsl.hpp`, compiled as C++, and
  compares it byte for byte with the CPU decoder: all 14 footprints in both decode modes, random blocks through all
  2048 block modes, sizes that are not a multiple of the footprint, strips of several slices, and all 7 tile
  shapes. Each invocation of a workgroup runs on a thread of its own and they meet at a `std::barrier`, so a race
  on the tile is a race in the test: built with `-fsanitize=thread` it reports none, and reports the race when the
  barrier is taken out. It also checks the
  shader's `Unorm16To8` for every input and the layer's built-ins against values from the GLSL specification.
- `astc_vulkan` and `astc_vulkan_pieces` (ctest with `-DSNX_TEST_VULKAN=ON`) build `tests/astc_vulkan.c` with the
  production `vk/astc.c`, `vk/cmd.c` and `vk/memory.c`, volk, VMA and the SPIR-V the build makes, and compare raw image
  readbacks with the CPU decoder byte for byte. They cover all 14 footprints with UNORM and SRGB images, each against
  the CPU decoder in its decode mode, and decode the same blocks in both modes for every footprint, checking that
  alpha is the same, that R, G and B differ by at most one step, and that some do. They also cover images whose size
  is not a multiple of the block size, dispatch padding, five mip levels down to 1x1, six layers, 3D slices batched
  into strips, scratch chunking, frame reuse and staging ring growth. `astc_vulkan_pieces` runs it under a 4 MiB
  allocation cap, so the bigger images are bound in pieces and read back in bands. No game files or window are
  needed:

  ```sh
  cmake -B build -DSNX_TEST_VULKAN=ON && cmake --build build --target snx-test-astc-vulkan
  VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json \
  VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation \
  VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT \
  ctest --test-dir build -R astc_vulkan
  ```

  Without the validation layer installed, the "zero validation errors" line only means that nothing reported an
  error. `build/src/nx/snx-test-astc-vulkan --large` decodes a 2D texture of 11584x11584 (4x4 blocks) and a 3D one of
  512^3 (8x8 blocks), 512 MiB each, past the 256 MiB cap, and compares every byte.
- `bc7_glsl` (ctest, no GPU) runs `bc7_encode.glsl` on the CPU (`tests/glsl/bc7_cpu.cpp`) and decodes its blocks
  with the CPU reference, `tests/support/bcn.c`: the encoder's 16 weights are the format's, its least squares fit
  gives a line's ends back and a constant alpha its value in every subset of every partition, blocks of one colour,
  blocks that lie on a line mode 6 holds and blocks each other mode holds (modes 4 and 5 in each rotation and index
  selection) come back exact, blocks with one channel apart come back closer than the same encoder built without
  modes 4 and 5 brings them (`SNX_BC7_MODES_4_5` 0, `bc7_encode_cpu_without_45`), edenprobe's astcimage picture comes
  back no worse than the PSNR each region is held to (its fast build, `bc7_encode_cpu_fast`, to floors of its own),
  and a 13x7 image of three slices encodes slice by slice as each alone.
  `astc_vulkan` checks `vk_astc_as_bc7`: RGBA8 without pressure, BC7 from 256x256 texels under it but never 3D, BC7
  with holes always. `astc_vulkan` and `astc_vulkan_pieces` also upload random blocks into BC7 images, footprints whose strips step
  4, 2 and 1 block rows, with mips, layers, slices and many strips, and a level of `VK_BC7_FAST_TEXELS` that the
  fast build encodes, and compare the GPU's blocks with the same encoder (or its fast build) run on the CPU over the
  CPU decoder's texels, byte for byte. `--bench [file.astc]` times a 4096x4096 level (the fast build) and a
  2040x2040 one (the full build) as RGBA8 and as BC7, of random 8x8 blocks or a file's repeated over the level.
- `tex_upload_rows` checks what `texture.c` hands `vk_astc_upload`: an ASTC level's block rows, strip by strip,
  against the block-linear layout walked byte by byte, and the bands other formats take past the cap. It also checks
  the BC7 cache's key: the key is made only where `vk_astc_cached` says so, a byte of any layer changes it, and no key
  is made where the GPU holds bytes.
- `bc7_cache` (ctest, no GPU) checks `bc7_cache.c`. The key changes with each field of the level, its build, every
  bit of its bytes and every word of its layout, and stays the same for the same inputs. Records come back as
  written (one read piece or several), also after the file is opened again. A file of another version starts empty.
  A changed byte on disk drops its record on the read. A record cut short is cut off, and the records before it stay.
  The bound starts the file over. The index grows past 3,000 records, and the file stays locked while it is open.
- `astc_vulkan` and `astc_vulkan_pieces` upload a level into a new BC7 image three times, with the cache's key: the
  full and fast builds, strips of 4, 2 and 1 block rows, layers, slices and many strips. The first upload encodes. The
  second, and the third after the file is opened again, encode nothing (`vk_astc_stats` counts the levels and the
  hits), and their image bytes are the same as the first's, byte for byte. A changed block misses. `--bench` also
  times the cache: an encode that stores, then hits.

The CPU decoder was also compared with ARM's astcenc 5.7.0 (`astcenc -dl` and `-dH`, Apache-2.0, a release binary
used only as a tool), on the same kind of random blocks for every footprint. Every texel agreed within one step of
rounding (astcenc's 8 bit output rounds differently), and every block one rejects the other rejects too. In the LDR
profile astcenc also rejects HDR endpoint modes, which this decoder decoded then. After HDR endpoints became the
error color, the CPU decoder was compared with edenprobe's LDR profile reference decoder (`source/astcref.c`, held
to astcenc by edenprobe's reference suite) on 20,000 random blocks for each of the 14 footprints: every texel was
one of the two bytes the reference allows, error blocks included. The sRGB decode mode was checked
the same way against `astcenc -cs` and `-ds` on five footprints: every byte agreed, alpha included.
