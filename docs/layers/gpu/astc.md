# ASTC compute uploads

## Index

* [The shader](#the-shader)
  * [Written for the machine](#written-for-the-machine)
  * [Interface](#interface)
  * [Where it differs from Eden](#where-it-differs-from-eden)
* [The upload path](#the-upload-path)
* [Encoded again as BC7](#encoded-again-as-bc7)
  * [Straight from the blocks](#straight-from-the-blocks)
  * [Two passes](#two-passes)
  * [Auto, Exact and BC7](#auto-exact-and-bc7)
  * [Between frames](#between-frames)
  * [The fast build](#the-fast-build)
  * [The BC7 cache](#the-bc7-cache)
  * [Very large levels](#very-large-levels)
* [Textures bigger than one allocation](#textures-bigger-than-one-allocation)
* [Sparse textures](#sparse-textures)
* [Building the shader](#building-the-shader)
* [Tests](#tests)

ASTC textures are decoded on the GPU by a compute shader, `src/nx/gpu/texture/astc.comp` with its body in
`astc_decode.glsl`. It is a GLSL port of Eden's `src/video_core/host_shaders/astc_decoder.comp` (GPL-3.0-or-later, with yuzu's GPL-2.0-or-later parts). The
CPU decoder in `tests/support/astc.c` is a C port of Eden's `src/video_core/textures/astc.cpp`, which
carries the University of North Carolina at Chapel Hill's Apache-2.0 decoder (FasTC). The tests use it as the
reference the shader must match byte for byte, and the runtime runs it only for the background encodes on the CPU
([Very large levels](#very-large-levels)).

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
  on the GPU by `blocklinear.comp` (docs/layers/gpu/blocklinear.md). Eden's shader deswizzles in the shader instead. That part
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
take one (4x4) or less. Held as BC7 (the `astc_mode` setting, below), `format.c`'s `TA()` gives such a texture a
`VK_FORMAT_BC7_UNORM_BLOCK` or `_SRGB_BLOCK` image in place of RGBA8, which texture.c makes between frames
(below), and `vk_astc_upload`, where it uploads straight to BC7, runs another shader in the decoder's place:
`src/nx/gpu/texture/astc_bc7.comp`, with its body in `astc_bc7.glsl`, makes the ASTC blocks BC7 blocks in one
dispatch, an invocation a 4x4 block, into a scratch buffer of their own, and the copy writes those blocks into the
image ([Straight from the blocks](#straight-from-the-blocks)). A BC7 strip starts on a row of whole 4x4 blocks, so
its ASTC block rows are a multiple of 4 / gcd(block height, 4), and whole slices are counted by the blocks' bytes
too, which for a slice smaller than a block are more than its texels'. `device.c` turns `textureCompressionBC` on for
it whatever `native_bc` says, and `format.c` then samples BCn as it is only when `native_bc` asks too. A 3D texture
keeps RGBA8 where the device has no 3D BC7 images, as native block formats do. Every 600 frames the log says how many
levels went up as BC7 and their megabytes against RGBA8's.

A texture with holes in its mapping (a game's sparse texture, `texture.c`) is BC7 whatever `astc_mode` says, where the
GPU samples BCn: one game's are 68 layers of 8192x8192 ASTC 8x8 with seven of them mapped, 1.7 GB each as RGBA8 (16
times the game's bytes), which took the GPU past its memory. So it goes up straight to BC7, not RGBA8 first and BC7
between frames (below). `device.c` turns `textureCompressionBC` on wherever the device has it for this. A sparse
texture's levels are decoded only in the strips that have memory bound (`vk_image_sparse_bound`), in strips as high
as the device's sparse blocks.

An array texture goes up a level at a time over all its layers (`vk_astc_upload_layers`), the layers as a 3D level's
slices: a strip takes as many as the scratch holds, and the encoder runs once for them. A layer and level at a time, a
small level gave the encoder too little to fill the GPU and each layer's dispatch waited for the one before: one
game's arrays of 118 layers of 128x128 in 8 levels took 667 ms of an RX 6700 XT, where a level of one in a dispatch
takes 13.5 ms, and the game stopped 1.4 s for two of them.

`vk_astc_upload` waits for the GPU once 256 thousand blocks are queued. The encoder is slow on a small GPU, and without
the wait a load queued up tens of seconds of it and the device was lost in most runs of one game, on a GT 1030, with
nothing in the validation layer or the kernel's log. Why the depth of the queue does that is not known: it is the
driver's. Splitting the work over submissions with no wait did not help, and neither did smaller dispatches.

### Straight from the blocks

The BC7 blocks were first made in two dispatches: `astc.comp` decoded a strip into RGBA8 texels, four bytes each, and
a second shader read them back and encoded them. `astc_bc7.comp` does both. An invocation takes one 4x4 block, decodes
the ASTC blocks its 16 texels lie in (`astc_decode.glsl`'s `DecodeBlock`, then `SlotTexel` for each texel, the code
`astc.comp` runs), and encodes them (`bc7_encode.glsl`'s `Encode`). The texels lie in one ASTC block where the
footprint is 4x4 or 8 or 12 a side, else in up to two each way, so an ASTC block is decoded once for each 4x4 block
over it, up to nine times (12x12). No texel is written out, there is one dispatch and no barrier between two, and the
blocks are the same bytes the two dispatches made (`astc_bc7_glsl`, below).

- **The footprint is pushed**, where `astc.comp` has it as a specialization constant: the encoder is most of the
  shader's code, and a pipeline for each footprint in each of the encoder's builds would be compiled where the game
  waits for its first texture of each. The divisions by the footprint come as reciprocals: `(v * ceil(2^18 / side))
  >> 18` is `v / side` for every side and every coordinate under 16384 (checked for all of them).
- **No shared memory.** A first version decoded a tile of 64 ASTC blocks into the workgroup's shared memory, as
  `astc.comp` does, each block once. On a GeForce GT 1030 it was 7 to 26% slower than the two dispatches, and the
  cause was the shared memory itself: the encoder alone with those 10 KB declared and unused took 285 ms where it
  had taken 230. Shared memory is what a GPU has least of, so fewer workgroups were in flight through the encoder,
  where nearly all the time goes. The decoded block is the invocation's own 39 words now.
- **What it saves** is the texels' traffic and a dispatch, not the encoder's time: decoding a 2040x2040 level to RGBA8
  takes that GPU 4 ms, encoding it 230 to 390. On random blocks the one dispatch is level with the two (4x4 232 ms
  against 228, 6x6 410 against 393, 8x8 312 against 307, 12x12 290 against 285). The time came out of the encoder
  itself (below).

A texture made BC7 between frames has no texels to hand the shader, and no longer needs any: it keeps its ASTC blocks
(below).

### Two passes

A GPU runs a shader's invocations 32 or 64 abreast, each group at the pace of its slowest. A block mode 6 holds is
done in a hundredth of the time of one that takes the whole search, and with one such block in six nearly every group
has one and waits for it: one game's 8x8 level with 18% of its blocks past mode 6 took a GeForce GT 1030 392 ms, and
a level of the same game that mode 6 holds throughout 18 ms, 21 times, where the operations counted are 5 times.

So the full and the fast build are two dispatches (`SNX_BC7_PASS`, `astc_bc7.glsl`). The first runs over the strip's
blocks, gives each mode 6 alone, and writes those the search would have stopped at there (`Encode`'s `enc_more`). It
lists the others in a work list, a third buffer: their numbers, counted by an atomic add, with the second pass's
workgroups kept beside the count as an indirect dispatch reads them. The second is that indirect dispatch: each
invocation takes a listed block, decodes its ASTC blocks again and gives it the whole search, so every group is at the
same work. The blocks are the bytes one pass writes. Mode 6 alone and the quick build have no search past mode 6, and
stay one dispatch.

On the GT 1030, a 2040x2040 level as BC7 (`snx-test-astc-vulkan --bench FILE`, the fast build), on levels one game
loads (`astc_dump=DIR` keeps them as `.astc` files) repeated over it:

| Level | One pass | Two passes |
| --- | --- | --- |
| 8x8, mode 6 holds every block | 18.5 ms | 20.3 ms |
| 8x8, 18% of the blocks past mode 6 | 392 ms | 94 ms |
| 4x4, 512x512 | 205 ms | 46 ms |
| 8x6, 1000x800, mostly one colour | 89 ms | 13.5 ms |
| Random 4x4 blocks | 164 ms | 30 ms |
| Random 8x8 blocks | 220 ms | 46 ms |

Two cuts of the search are options of the build that no build takes (`snx-test-astc-bc7-glsl --measure FILE...` gives
each one's PSNR against the ASTC blocks' own texels, the modes written and the CPU's seconds):

- `SNX_BC7_HINTS` (`astc_bc7.glsl`): the search takes what the ASTC blocks say of the block. Texels of one ASTC
  partition lie on one line, so a block of one such set takes mode 6 alone; a block of two sets (two partitions, or two
  ASTC blocks' texels) takes the partition of BC7's 64 with the fewest texels on the wrong side, found by counting
  bits where the search sums 128 partitions' spreads; a second plane names the rotation of modes 4 and 5. In the
  second pass it took the two levels above with hard blocks from 95 to 67 ms and 69 to 51. Over 11 of that game's
  levels it gave 51.13 dB where the fast build gives 51.27, and one level of text lost 1.8 dB. The tests' expected
  blocks come from texels, which have no hints, so turning it on means making them from the blocks.
- `SNX_BC7_QUICK` (`bc7_encode.glsl`): mode 6 first takes its box's line with one choice of p-bits and each texel at
  the weight nearest its place, and keeps it under an error. Under 64 it took the first pass of a level mode 6 holds
  from 18.5 to 12.8 ms for 0.3 to 0.8 dB, and made the levels with hard blocks no faster.

### Auto, Exact and BC7

`astc_mode` (a `startup` setting) says how a GPU that samples BCn but not ASTC holds the ASTC textures it decodes:

| Value | Name | Held as |
| --- | --- | --- |
| `0` | Auto, the default | Exact where the budget of the heap images go to is 8 GiB or more as the device starts, else BC7 |
| `1` | Exact | RGBA8, as the game's blocks decode, four bytes a texel |
| `2` | BC7 | RGBA8 at once, then BC7 made between frames, a byte a texel |

Memory is what a desktop GPU runs short of. As RGBA8, one game's textures took 5.7 GB of a 5.7 GB budget and were
evicted and uploaded again every frame (600 to 1000 ms frames); as BC7 they took 2.5 GB. Another took 3.9 GB of an
RX 6700 XT's 11.8 GB budget as RGBA8, 0.4 GB more than as BC7, and its frames were smoother so. BC7 is not exact (its
PSNR is below), mostly seen in UI text. Auto's line (`SNX_ASTC_EXACT_MB` in `gpu/texture/astc_bc.h`) is drawn
between them: the budget is `VK_EXT_memory_budget`'s (VMA's estimate of 80% of the heap without it), or `vk_budget_mb`
where that is lower, as `vk_mem_use` gives it, read once after the device is made (`device.c`'s `astc_choose`). The
log's line says the budget, the line and the choice (an RX 6700 XT's):

```
vulkan: astc_mode=0 (Auto): heap 1's budget is 11834 MB as the device starts, at least 8192 MB: ASTC textures are decoded to RGBA8, exact, in four times BC7's memory (Exact)
```

Held as RGBA8 (Exact, or Auto with the budget for it), the log suggests `astc_mode=2` once a run, with the budget, the
use and the megabytes of ASTC textures decoded to RGBA8, when the images run short (`surface.c`'s `astc_bc_suggest`):
an image allocation finds no memory, a trim leaves the caches past their budget, or a trim takes what the last eight
presents used (the working set, which the next frames load again).

### Between frames

Uploading straight to BC7 held the game for the encoder: the encoder makes about 10 million blocks a second on an
RX 6700 XT, and two arrays coming in one frame cost 30 to 40 ms of it, frames of 50 to 90 ms, where RGBA8 left none
over 50 ms on the same route. So a texture held as BC7 goes up decoded to RGBA8, which the game samples at once, and
texture.c (`vk_tex_later_frame`, after each present, in a submission of its own that the present doesn't wait for)
makes its BC7 image from the texture's ASTC blocks. The texture keeps them on the GPU beside the RGBA8 image
(`vk_astc_keep`): each strip the decoder reads is also copied into its place in a buffer of the texture's own, a
region's rows too, a quarter of the RGBA8 image's bytes at most (4x4 blocks; a ninth for 6x6). Guest memory is not read
again: the game may have written or unmapped it, and the BC7 image has to hold what the RGBA8 one showed.
`vk_bc7_encode_kept` runs `astc_bc7.comp` on a strip of them, as an upload does, so the blocks are the same an upload
straight to BC7 makes. The buffer goes once the BC7 image is whole or the texture is freed; a texture that finds no
memory for it stays RGBA8. A frame encodes `VK_BC7_LATER_BLOCKS` (32768) blocks at most: 3.3 ms of the
RX 6700 XT, a tenth of a frame at 30 fps. Its strips are whole slices of a level (an array's layers, a 3D level's
depth slices, as many as the blocks left take), or whole rows of ASTC blocks of one when a slice is more than that,
from a row that a row of 4x4 blocks starts on too; a frame's first strip goes whatever its size, and one that the
blocks left don't hold waits for the next frame. Textures go in the order they were loaded, one at a time. The step does nothing when
`vk_cmd` would wait for the GPU first, so it never holds the frame after the present.

Once the BC7 image is whole it takes the RGBA8 one's place in the texture, moved to the layout the RGBA8 one was
sampled in, and the RGBA8 image and its views go once the frames that sample them are done (`vk_defer_image`), three
quarters of its memory. Meanwhile the texture takes RGBA8's memory, a quarter more for the BC7 image being made, and
its blocks' bytes.

- Written before its BC7 image is whole (a whole load or pieces, which give it a new write generation): the encoder
  drops what it made and starts over on the new texels, behind the other textures.
- Written whole after: an RGBA8 image in the BC7 one's place, loaded at once, and the encoder makes BC7 again. Where
  no memory is found for it, the texture is left out until a check finds some, as any image that finds none.
- Written in pieces after: pieces of a frame's blocks or fewer go straight to BC7 (they are decoded and encoded as a
  region, as above); more load whole as RGBA8 (`later_pieces_whole`).
- A texture whose memory is bound by tiles (sparse) is BC7 from the start and loaded straight to it, as the tiles
  of an RGBA8 image don't match the guest's (below).
- A texture freed leaves the queue, and the BC7 image being made goes with it.

A texture the game writes often is held Exact instead (`later_wrote`): written again before the encoder could make its
BC7 image and the game sample that a frame, that is no more frames after the write before than the encoder takes for it
(a frame for each 32768 blocks) and one, it leaves the queue and stays RGBA8, and its writes load as RGBA8 alone,
pieces too: as BC7 each write would drop the encoder's work, or encode the pieces where the game waits for them. A
write every frame is held so whatever the texture's size; a 2048x2048 one with its mips, 349,525 BC7 blocks and 11
frames of the encoder, is held when written every 12 frames or more often. Read unwritten for four times those frames and one, and 30
frames at least (`later_cool`), it is queued to be made BC7 again: a texture read much and written little stays BC7.

Every 600 frames the log says how many textures were made BC7 between frames, the RGBA8 memory that freed, the blocks
encoded and the encodes started over, how many textures wait with their RGBA8 memory, and how many are held Exact for
their writes, went so and were made BC7 again.

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

Nearly all of the encoder's time is `Eval`, which every line of every subset of every shape runs over the subset's
texels. A texel's place along a line is `p.q1 - p.q0 - q0.(q1 - q0)`, two dot products of the words' four bytes and a
constant of the line, where it was four multiplications of unpacked channels. Its error at a weight interpolates all
four channels in two multiplications, two channels a word in 16 bit lanes as the decoder's texels are, and is
`I.I - 2 I.p + p.p`. The place's division by the line's squared length is a multiplication by its reciprocal in
floating point, the quotient made exact from the integer remainder, so no byte depends on the driver's rounding. The
blocks are the same bytes as before (the test's digests), and the GT 1030 encodes a quarter faster: 232 to 172 ms for
a 2040x2040 level of random 4x4 blocks, 312 to 232 ms of 8x8. Where the GPU says it has an instruction for the packed
dot product (Vulkan 1.3's `shaderIntegerDotProduct` with `integerDotProduct4x8BitPackedUnsignedAccelerated`),
`astc.c` takes four more builds of the shader that use it (`SNX_BC7_DOT`): 5% less again on that GPU (305 to 291 ms
of 6x6), the same blocks. ucas, asked for cheaper forms of the shader's functions under its model of that GPU, makes
the same sums dot products and finds under 1% elsewhere.

A block of one colour takes no search: `Solid` writes it as mode 5, colour ends `c >> 1` and
`(c >> 1) + (c & 1) - (c >> 7)` at weight 21 of 64, which interpolate to `c` for every value of a channel. Mode 6
holds such a block only when its channels agree in their low bit or the odd ones are neither 0 nor 255 (an end's
p-bit is shared by its four channels, and no weight brings one channel to 0 and another to 255: opaque black), so
the search went through every partition and mode for it and could still end a step off.

A block stops trying modes once its squared error over all its texels and channels is `SNX_BC7_GOOD_ENOUGH` (16) or less,
a mean squared error of a quarter, 54 dB, for that block. A hard block still gets the whole search. Within a fit,
`Eval` leaves a candidate once its partial error proves it cannot win, and a second least squares runs only on
weights that changed. Quality tests compare against the prior unpruned search and require each block to stay
within the 16-error allowance or produce no more error than that reference.

What it costs, as PSNR against what it encodes, on edenprobe's astcimage picture (gradients, hard edges where three
colours meet in a block, noise, and alpha) decoded from ASTC: 39.9 dB over the picture at 4x4 (gradients 53.1,
edges 48.9, noise 34.2, alpha 50.9), 40.4 at 6x6, 46.2 at 8x8 and 45.2 at 12x12. Mode 6 alone gave 27.6 at 4x4,
its edges 23.5. Noise is what BC7 holds worst: a block of independent texels is a line or two of them at best.

### The fast build

A level of `VK_BC7_FAST_TEXELS` or more takes the encoder's fast build, `bc7_fast_spv` (`SNX_BC7_FAST` 1): a block
whose mode 6 error is 256 or less (squared, summed over its 16 texels and 4 channels: 2 a channel as a root mean
square) keeps mode 6, and only the others try every mode, stopping at `SNX_BC7_GOOD_ENOUGH` as the full build does. A
level of `VK_BC7_MODE6_TEXELS` (8192x8192) or more takes mode 6 alone, `bc7_mode6_spv` (`SNX_BC7_FAST` 2): a game's
sparse texture of seven mapped 8192x8192 layers took the full encoder 14 seconds on a GeForce RTX 2080, which mode 6
alone encodes at 1200 megatexels a second. The build goes by the level's size, not the region's (`vk_bc7_build`), so a
level loaded in pieces, or made BC7 between frames, encodes each block as its whole upload does. Each build's
pipeline is made the first time a level asks for it. Both rules count a layer, as a sparse texture's layers are loaded
one at a time. A level held as BC7 at load that is not sparse and has `astc_quick_mtexels` megatexels or more, all
its layers counted, takes the quick build instead and the fast build later ([Very large levels](#very-large-levels)).

Mode 6 alone holds gradients but not edges, a second plane or luminance apart from alpha; the threshold keeps most
of the full build's quality. PSNR over RGBA and the worst channel error:

| Content | Full | Fast build | Mode 6 alone |
| --- | --- | --- | --- |
| The picture below: gradients, edges, noise, alpha (`bc7_glsl`) | 51.1, 49.0, 29.0, 48.0 | 43.4, 46.8, 29.0, 47.7 | 43.2, 26.2, 25.2, 40.0 |
| Random ASTC 4x4, 6x6, 8x8, 12x12 blocks, 256x256 decoded (CPU model) | 48.58, 42.96, 46.23, 44.45 | 48.56, 42.92, 46.19, 44.39 | not measured |
| One game's ASTC 6x6 textures, 25 (fuddlesworth, RTX 2080) | 53.8 dB / 50 | 50.7 dB / 50 | 46.5 dB / 147 |
| astcenc's 4x4, 8x8, 12x12 encodings of 7 of them (same) | 46.6, 47.6, 46.4 | 45.9, 46.7, 45.7 | 32.9, 36.2, 38.0 |

On the random blocks 1.3 to 5.9% of the fast build's blocks differ from the full build's, and 90 to 98% of them are
mode 6.

`VK_BC7_FAST_TEXELS` is 0: every level takes the fast build. The full build had most of a game's textures, those under
2048x2048, and a batch of them held a frame for seconds. On an RX 6700 XT (RADV), a level of random 8x8 blocks decoded
and encoded (`snx-test-astc-vulkan --bench`, two interleaved rounds of four runs after the first; a -O0 test build,
the time the GPU's):

| Level | Full build | Fast build |
| --- | --- | --- |
| 4096x4096 | 103 to 107 ms, 9.8 to 10.2 million BC7 blocks a second | 76 to 80 ms, 13.1 to 13.8 million |
| 2040x2040 | 23.5 to 24.3 ms, 10.7 to 11.1 million | 17.5 to 18.2 ms, 14.3 to 14.8 million |

Random blocks are the fast build's worst case: the blocks that try every mode hold up the invocations beside them.
On a GeForce RTX 2080 an 8192x8192 level of random 8x8 blocks decoded in 15 ms and encoded in 860 ms with every mode
and in 48 ms with mode 6 alone. Beside a running game, a 4096x4096 level took (decoding included):

| Content | Mode 6 alone | Fast build | Full build (2040x2040, scaled) |
| --- | --- | --- | --- |
| One game's ASTC 6x6 albedo | 7 to 11 ms | 12 to 14 ms | 190 ms |
| One game's ASTC 6x6 packed masks (the worst of its textures tried) | 7 to 8 ms | 85 to 88 ms | 250 ms |
| astcenc 8x8 normal map | not measured | 256 to 267 ms | not measured |
| Random 8x8 blocks | 21 to 29 ms | 189 to 196 ms | 260 ms |

### The BC7 cache

The same ASTC bytes always become the same BC7 blocks, and the memory budget's trim evicts textures a game uses again:
each came back through the decoder and the encoder (one game encoded 6,000 to 14,000 levels in a 150 second run, each
3 to 5 ms of an RTX 2080). So the blocks of each whole level encoded are kept, as an index in memory and records on
disk in `shader_cache/<game>.vkbc7`, beside the shader files (`gpu/vk/bc7_cache.c`), from run to run. `pipeline.c`
opens it where ASTC textures are held as BC7 (`vk_astc_bc7`).

- **The key** is XXH3-128 (`vk_hash128`) of the level's guest bytes, as the CPU has them, and the words that place its
  blocks in them (block linear or pitch, the GOB block's height and depth, GOBs across, the row's bytes and rows, the
  layers: `texture.c`'s `astc_content`), then of that hash with the level's size, slices or layers, footprint, sRGB
  decode mode, the encoder's build and `VK_BC7_ENCODER_VERSION` (`vk_bc7_key`). A level of which the GPU holds any byte
  (an engine's result, `memory/own.c`) is not keyed, nor one the page table does not map whole, nor a region, nor a
  sparse texture's (only its bound strips are decoded).
- **The file's version** is XXH3-128 of the SPIR-V of the deswizzle, the decoder and the encoder's three builds whose
  blocks it keeps (not the quick one's, which it never stores), of `VK_BC7_ENCODER_VERSION`, the GPU's vendor and device, the driver's version and its pipeline cache UUID
  (`vk_astc_cache_open`). A file of another version starts empty.
- **Straight to BC7** (`vk_astc_upload` with the key): a hit reads the record's blocks from the file 1 MiB at a time,
  checks them (XXH3 of each piece, folded) as it copies them into staging, and one copy writes the whole level, every
  layer or slice, into the image: no decode, no encode. A miss encodes as before and also copies each strip's blocks
  into their place in the frame's readback ring (`vk_stage_back`), with a barrier to the host after the last strip.
- **Between frames** (`texture.c`'s `later_encode`): the RGBA8 upload keeps each level's key (`Surf.bc7.key`), and a
  level the cache has is copied from it into the BC7 image, counted a sixteenth of its blocks against the frame's
  `VK_BC7_LATER_BLOCKS`; a level the encoder makes whole is copied from the BC7 image into the readback ring
  (`vk_bc7_capture_level`). A texture whose bytes change in pieces loses its keys until it loads whole again.
- **Landing:** once the submission is done, `cmd.c`'s landing (`vk_astc_frame_done`) appends the blocks to the file,
  before the frame's ring is reused. The GPU thread never waits for them. Blocks on their way are bounded to
  `VK_BC7_CAPTURE_BYTES` (32 MiB), which bounds the ring's growth and is the largest whole level the cache takes (a
  4096x4096 level is 16 MiB). A very large level straight to BC7 goes in pieces instead (below), and any other larger
  level is encoded each time (from 8192x8192, it is mode 6 alone).
- **Memory and disk:** only the index stays in RAM, 32 bytes a level, and one 1 MiB buffer for the reads. The file has
  a bound of `VK_BC7_CACHE_MAX` (8 GiB): a record past it starts the file over. A record a crash cut short ends the
  reading, and the file is cut there; one that does not check out is dropped and its level encoded again. While the
  file is open it is locked (`snx_lock_file`): a second process of the same game runs without the cache.

RGBA8 levels are not cached: their decode is the cheap part (about 1 ms a 4096x4096 level on the RTX 2080), and a
cached one would be four times the bytes to read and copy.

### Very large levels

One game's first 3D frame uploads two 1365x1365 ASTC 4x4 arrays of 90 layers: their first levels are 168 MB of BC7
blocks each, 447 megatexels with their mips. Its blocks are the encoder's worst kind, most of them trying every mode,
and the fast build took a Radeon 890M 92 ms a strip of two layers: the frame took 10.1 seconds. (In one submission it
had reset the GPU, amdgpu's job timeout being 2 seconds: the upload's wait once 256 thousand blocks are queued, above,
keeps every submission well under that.) The rules above count one layer (1.9 megatexels), and the levels were past
what the cache took, so every run paid it again. A level held as BC7 at load (`astc_mode=3`, or Auto for an Ounce
game: one made BC7 between frames goes up as RGBA8 first, above) of `astc_quick_mtexels` megatexels or more (8 by
default: a 4096x2048 level, or two layers of 2048x2048), every layer of a 2D array and every slice of a 3D level
counted, now goes another way:

- **Now:** the frame that needs it takes the quick build, `bc7_quick_spv` (`SNX_BC7_FAST` 3): mode 6 from its first
  line alone, the bounding box pulled in, where mode 6 alone also tries the box and two least squares fits. The two
  arrays took 0.46 s of the GPU in all (258 dispatches of 1.8 ms, the smaller levels' fast build included), where mode
  6 alone took 1.1 s and the fast build 10.1; the frames that uploaded them took 0.42 and 0.47 s. Beside each strip's
  decode, its ASTC blocks, as the decoder read them, are copied on the GPU into the level's snapshot, a buffer in host
  memory (`bg_job_new`, at most 1 GiB of them at once).
- **In the background:** the fast build is made from the snapshot, never from guest memory again, so what is stored
  under the key is what the key names even when the game changes, moves or drops the texture meanwhile. The level is
  cut in pieces of at most 256 KiB of blocks (`Bc7Pieces`): whole layers or slices when one is smaller, else bands of
  block rows, whose height is a multiple of the rows that start on a row of ASTC blocks too. The level's size and
  footprint alone place them, and each is a record of the cache under a key of its own (the level's key, with the
  fast build's, and where the piece lies), so the cache takes a level of any size. With `astc_background=1`, the
  default, the GPU encodes one piece after each present, in a submission of its own after the frame's
  (`vk_astc_background`), the next once that one is done. Each piece also goes into the texture, while it holds the
  upload the background started from: another upload of that level, a copy of render targets into it or its end
  (`vk_astc_forget`) stops that, and the pieces then go to the cache alone. This run shows the better blocks as they
  come, and a texture uploaded again before the end (an eviction) finds its piece in the cache, or the background
  goes on into the new image.
- **The next run:** an upload whose pieces are all in the cache copies them into the image, with no decode and no
  encode. One piece missing (a run ended before the background did) encodes the level quickly again, and the
  background makes only the missing pieces. The reads run on the GPU thread, which the frame waits for: 21 ms for a
  168 MB level in the system's page cache, but 4.2 s from that laptop's game disk, an NVMe drive in a USB enclosure
  that reads 40 MB/s (`dd iflag=direct`: it runs at USB 2 speed). So a frame's hits read for 100 ms at most
  (`BG_HIT_NS`): past that a level is encoded quickly, and the pieces the cache holds load into the texture after
  the presents that follow, 2 ms of reads after each (at least a piece: 6.5 ms from that disk), `vk_astc_background`.
  The file is read without the kernel's readahead (`POSIX_FADV_RANDOM`, on Linux): that disk's (64 MiB) streamed the
  loads at its full 40 MB/s, which starved the game's own reads, 2.4 frames a second while the 1890 pieces loaded in
  15 s; without it they take 100 s, a piece a frame, at 4 frames a second for the scene's first 10 s and its usual
  rate after. With the file out of the page cache the scene's worst frame was still 3 s, of which the very large
  levels took 0.1 s; the rest is not measured apart, and is likely the whole levels the cache gives the scene's other
  textures (some 60 MB, read that way since the cache began) and the game's own reads from the same disk.

On the 890M in that game's first 3D scene (the 3D picture is wrong there for other reasons, and its frame rate climbs
over the scene's first 40 seconds as the game streams):

| | Before | GPU background (first run) | Cache hit (next run) | CPU background, 2 threads (first run) |
| --- | --- | --- | --- | --- |
| The frame that uploads the arrays | 10,123 ms | 417 ms, then 471 | under 250 ms (the hits: 56 ms of the GPU thread, the pieces in the page cache) | 422 ms, then 474 |
| Frames a second, 37 to 87 s after the scene starts | 15.4 | 15.0 | 15.9 | 13.8 |
| Frames a second, 87 to 137 s after | 16.0 | 15.2 | 17.9 | 14.0 |
| The background | none | 133 s for 1890 pieces, 2 to 10 ms of the GPU a frame, 13 s of GPU time in all | none | 62% done after 230 s, 416 s of CPU time |

The GPU had room: before, it was busy 27 ms of each 57 ms frame, and a piece of the background made that 36. The frame
rates' differences are within what runs vary by: runs without any background gave 15.4 to 17.9 in the same windows.
Copying each piece into the texture as well (the build after these runs) left the background's cost a frame and its
length (138 s) the same. The CPU column is the worker threads the background once had as well, which ran the CPU
decoder and the encoder's body at idle priority. The game was run capped to four CPUs (a cgroup quota of 400%), as all
our test runs are: its own threads took 2 to 2.3 of them in that scene, so even two workers used up the cap and held
the game back, and six brought it down to 3 to 6 frames a second. Without such a cap the cost stays: on that game's
blocks a Zen 5 core encodes 0.7 megatexels a second, so the two arrays take 10 minutes of CPU time where the GPU spends
13 seconds. The background is the GPU's alone now; the CPU decoder and encoder are only the tests' references
(`tests/support/astc.c`, `tests/glsl/bc7_cpu.cpp`).

Why 8 megatexels: at that game's rate (25 ns a texel, most blocks trying every mode), the fast build of a level under
it costs the frame at most about 200 ms on the 890M, and a tenth of that on blocks mode 6 holds (an albedo map); a game
has many 2048x2048 levels (4 megatexels), which keep the fast build and the whole level's record of the cache, records earlier
runs made included. Each quick level shows the quick build until the background reaches its pieces, one a frame, so
a lower bound (4 takes those in) trades those levels' first frames' hitches for their quick blocks a while longer.
That game's arrays are past either bound: their levels of 160, 40 and 10 megatexels go quick, the next (2.5) takes
the fast build. A level between 8 megatexels and `VK_BC7_CAPTURE_BYTES` that earlier runs cached whole is encoded
quickly once more, and its pieces stored, as its old record is not the pieces'.

The pieces of the two arrays' quick levels, 450 MB, took the cache's file from 181 to 631 MB (1890 records more in
its index, 32 bytes each in memory).

| The quick build on edenprobe's astcimage picture | Gradients | Edges | Noise | Alpha |
| --- | --- | --- | --- | --- |
| PSNR, against mode 6 alone | 42.7 dB (43.2) | 23.1 (26.2) | 24.7 (25.2) | 32.4 (40.0) |

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

## Sparse textures

A sparse texture's memory is mapped a 64 KiB tile at a time; for blocks of 16 bytes a tile is 64x64 of them. Where the
device has sparse residency of 2D images and reads what isn't bound as zeros, `texture.c` gives such a texture's image
memory only for its mapped tiles. An ASTC 4x4 one held as BC7 (`astc_mode`, above) takes part: BC7's sparse block is
256x256 texels, 64x64 blocks of 16 bytes too, so each mapped tile is one block of the image, decoded and encoded on its own as
a texture's changed runs are. Other footprints (an 8x8 tile is 2x2 BC7 blocks) and ASTC held as RGBA8 (blocks of
128x128 texels) keep the whole image, and the log line saying so names the format and the device's block in the
layout's terms.

## Building the shader

Nothing compiled is checked in. `astc_bc7.comp` is built twelve times: the two passes of the full and of the fast
build, mode 6 alone and the quick build, each with its dot products spelled out (Vulkan 1.1) and as SPIR-V's packed
one (Vulkan 1.3).
`cmake/glsl.cmake` compiles `astc.comp` at build time with
`glslangValidator -V --target-env vulkan1.1 -O -g0 --spirv-val`, into `build/generated/spirv/astc_spv.c`, which
holds the words. `nx/gpu/texture/astc_spv.h`, which the build writes beside it (`build/generated/include`), declares
them (`astc_spv` and `astc_spv_size`, in bytes). glslang's
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
- `astc_glsl` (ctest, no GPU) runs `astc_decode.glsl` on the CPU through `glsl.hpp`, compiled as C++, and
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
  `build/src/nx/snx-test-astc-vulkan --bench [file.astc]` times a 4096x4096 level and a 2040x2040 one as RGBA8 and
  as BC7, in milliseconds, texels and BC7 blocks a second, of random 8x8 blocks or a file's (astcenc's `.astc`)
  repeated over the level: the encoder's time depends on what it encodes, and random blocks are no game's.
- `astc_bc7_glsl` (ctest, no GPU) runs `astc_bc7.comp`'s body on the CPU and holds its blocks, byte for byte, to
  the encoder's (`bc7_cpu.cpp`) over the texels the CPU's ASTC decoder makes of the same blocks, which is what the two
  dispatches wrote: all 14 footprints in both decode modes, each of the encoder's builds, random blocks through all
  2048 block modes, sizes on no block of either kind, strips of several slices, the two passes of the full and the
  fast build (the same blocks, some of them the second pass's), and the division by the footprint for every footprint
  and coordinate. `snx-test-astc-bc7-glsl --measure file.astc...` gives the PSNR and the modes of
  each build on a game's own blocks (`astc_dump=DIR` keeps the levels a game loads as such files).
- `bc7_glsl` (ctest, no GPU) runs `bc7_encode.glsl` on the CPU (`tests/glsl/bc7_cpu.cpp`, which gives it pictures
  through `tests/glsl/bc7_main.inc`, as no shader encodes texels any more) and decodes its blocks
  with the CPU reference, `tests/support/bcn.c`: the encoder's 16 weights are the format's, its least squares fit
  gives a line's ends back and a constant alpha its value in every subset of every partition, blocks of one colour,
  blocks that lie on a line mode 6 holds and blocks each other mode holds (modes 4 and 5 in each rotation and index
  selection) come back exact, blocks with one channel apart come back closer than the same encoder built without
  modes 4 and 5 brings them (`SNX_BC7_MODES_4_5` 0, `bc7_encode_cpu_without_45`), edenprobe's astcimage picture comes
  back no worse than the PSNR each region is held to (the fast build, mode 6 alone and the quick build,
  `bc7_encode_cpu_quick`, to theirs, the fast build keeping mode 6 on more blocks than the full one, mode 6 alone and
  the quick build on all), and a 13x7 image of three slices encodes slice
  by slice as each alone.
  `astc_vulkan` and `astc_vulkan_pieces` also upload random blocks into BC7 images, footprints whose strips step
  4, 2 and 1 block rows, with mips, layers, slices and many strips, and compare the GPU's blocks with the same
  encoder run on the CPU over the CPU decoder's texels, byte for byte, in the build the level takes (`vk_bc7_build`).
  They do it also for arrays whose layers go up a level at a time, all of them at once (`vk_astc_upload_layers`), as
  RGBA8 and as BC7.
- `tex_upload_rows` checks what `texture.c` hands `vk_astc_upload`: an ASTC level's block rows, strip by strip,
  against the block-linear layout walked byte by byte, and the bands other formats take past the cap. It also checks
  the BC7 cache's key: made only where `vk_astc_cached` says, the same for the same bytes, another for a byte of any
  layer changed, none where the GPU holds bytes, and kept in the texture for the encoder between frames.
- `bc7_cache` (ctest, no GPU) checks `bc7_cache.c`: the key changes with each field of the level, its build, every bit
  of its bytes and every word of its layout; records come back as written, also after the file is opened again; a file
  of another version starts empty; a changed byte drops its record; a record cut short is cut off; the bound starts the
  file over; the index grows past 3,000 records; and the file stays locked while it is open.
- `astc_vulkan` and `astc_vulkan_pieces` upload a level into a new BC7 image three times with the cache's key: strips
  of 4, 2 and 1 block rows, layers, slices and many strips. The first encodes; the second, and the third after the
  file is opened again, encode nothing (`vk_astc_stats` counts the levels and the hits, `vk_astc_dispatches` the
  encoder's dispatches) and their bytes are the first's; a changed block misses; and 100 encodes in one submission
  are all stored. `vulkan_headless` (`tests/vulkan_astc_later.inc`) stores the levels the encoder makes between frames
  and gives them to a second texture of the same bytes in its first frame, with no encode.
- `vulkan_headless` (`tests/vulkan_astc_later.inc`) holds the BC7 images made between frames to the CPU encoder's
  blocks over the RGBA8 texels the texture showed first, byte for byte: a 1024x768 texture in three frames of 32768,
  32768 and 3 blocks, the RGBA8 image freed after them; a 2D array of sides on no block in one frame and one dispatch
  a level, written whole after and made BC7 again from the new texels; a texture written between its two frames,
  started over; an array of 40 layers and a 3D texture of 40 slices, cut between two frames; pieces of a BC7 one,
  straight to BC7 for a block and whole as RGBA8 for 55296 blocks; a texture written 3 frames after its write before
  queued, 2 after held Exact out of the queue, and queued again once read 30 frames unwritten, not 29; pieces of a BC7
  one, straight to BC7 3 frames after its write before and whole as RGBA8, held Exact, the next frame; and a texture
  freed while it is made. `astc_bc_suggest` (ctest, no GPU) holds Auto's line and the three modes.
- `astc_vulkan` and `astc_vulkan_pieces` also upload very large levels (`astc_quick_mtexels=1` there, a megatexel):
  bands of block rows of 4x4 and 6x6 blocks (whose bands step 3 rows) and pieces of several layers, once with the
  background on the CPU and once on the GPU. The first upload's image is the quick build of the CPU decoder's texels,
  byte for byte, and the level goes to the background; once it is done (`vk_astc_background_drain`), the next upload,
  and one after the file is opened again, copy the pieces, which are the fast build of the whole level byte for byte:
  the CPU's and the GPU's pieces are the same. A hit out of time (`g_vk_astc_hit_ns` 0) encodes quickly and loads the
  pieces into the image after it. An image kept alive while the background runs ends up with the fast build too, from
  the pieces it copied into it. On the GPU, a background stopped after one piece leaves it in the cache, and the next
  makes only the others.

The CPU decoder was also compared with ARM's astcenc 5.7.0 (`astcenc -dl` and `-dH`, Apache-2.0, a release binary
used only as a tool), on the same kind of random blocks for every footprint. Every texel agreed within one step of
rounding (astcenc's 8 bit output rounds differently), and every block one rejects the other rejects too. In the LDR
profile astcenc also rejects HDR endpoint modes, which this decoder decoded then. After HDR endpoints became the
error color, the CPU decoder was compared with edenprobe's LDR profile reference decoder (`source/astcref.c`, held
to astcenc by edenprobe's reference suite) on 20,000 random blocks for each of the 14 footprints: every texel was
one of the two bytes the reference allows, error blocks included. The sRGB decode mode was checked
the same way against `astcenc -cs` and `-ds` on five footprints: every byte agreed, alpha included.
