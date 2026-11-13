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
  * [By what the blocks hold](#by-what-the-blocks-hold)
  * [Between frames](#between-frames)
  * [The fast build](#the-fast-build)
  * [The BC7 cache](#the-bc7-cache)
  * [Very large levels](#very-large-levels)
* [Textures bigger than one allocation](#textures-bigger-than-one-allocation)
* [Sparse textures](#sparse-textures)
* [Building the shader](#building-the-shader)
* [Tests](#tests)
* [Findings and what is left](#findings-and-what-is-left)
  * [What was found](#what-was-found)
  * [What is left](#what-is-left)

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

The fast build's second pass takes what the ASTC blocks say of each block (`SNX_BC7_HINTS`, `astc_bc7.glsl`), in
place of the search's own choice of partitions and rotations. Texels of one ASTC partition lie on one line, so a block
of one such set takes mode 6 alone; a block of two sets (two partitions, or two ASTC blocks' texels) takes the
partition of BC7's 64 with the fewest texels on the wrong side, found by counting bits where the search sums 128
partitions' spreads; a second plane names the rotation of modes 4 and 5. More than two sets, two planes of different
channels, or a partition more than two texels from every one of BC7's leave the block to the whole search. It took
the two levels above with hard blocks from 95 to 67 ms and from 69 to 51. It costs PSNR: over 11 of that game's levels
51.13 dB against the ASTC blocks' own texels where the search alone gives 51.27 (the full build 51.70, mode 6 alone
47.52), and one level of text lost 1.8 dB (`snx-test-astc-bc7-glsl --measure FILE...` gives each build's PSNR, the
modes written and the CPU's seconds). The full build keeps the whole search. With the hints the fast build's blocks
are no longer the encoder's of the decoded texels alone, so the tests hold the GPU's blocks to the same shader run on
the CPU over the same ASTC blocks (`tests/glsl/astc_bc7_cpu.cpp`); `vulkan_headless` reads those blocks back from
what the texture keeps.

One more cut is an option of the build that no build takes: `SNX_BC7_QUICK` (`bc7_encode.glsl`), with which mode 6
first takes its box's line with one choice of p-bits and each texel at the weight nearest its place, and keeps it under
an error. Under 64 it took the first pass of a level mode 6 holds from 18.5 to 12.8 ms for 0.3 to 0.8 dB, and made the
levels with hard blocks no faster.

### Auto, Exact and BC7

`astc_mode` (a `startup` setting) says how a GPU that samples BCn but not ASTC holds the ASTC textures it decodes:

| Value | Name | Held as |
| --- | --- | --- |
| `0` | Auto, the default | Exact while the heap images go to has room, BC7 from its caches' first trim, Exact again once the room is back (below); an Ounce game's, BC7 at load |
| `1` | Exact | RGBA8, as the game's blocks decode, four bytes a texel |
| `2` | BC7 | RGBA8 at once, then BC7 made between frames, a byte a texel |
| `3` | BC7 at load | BC7 as each texture loads, with no RGBA8 image before it |

Memory is what a desktop GPU runs short of. As RGBA8, one game's textures took 5.7 GB of a 5.7 GB budget and were
evicted and uploaded again every frame (600 to 1000 ms frames); as BC7 they took 2.5 GB. Another took 3.9 GB of an
RX 6700 XT's 11.8 GB budget as RGBA8, 0.4 GB more than as BC7, and its frames were smoother so. BC7 is not exact (its
PSNR is below), mostly seen in UI text. So Auto holds them Exact while the memory has room for it, and follows the
room as the game runs (`gpu/texture/astc_bc.h`'s `SnxAstcAuto`, `device.c`'s `vk_astc_auto_trimmed` and
`vk_astc_auto_present`). The room is the budget of the heap images go to, `VK_EXT_memory_budget`'s or `vk_budget_mb`
where that is lower, less the caches' use, both as `vk_mem_use` gives them:

- **BC7** from the caches' first trim (`surface.c`'s `vk_mem_trim`): their use past `vk_mem_trim_start`, the budget less
  a head (`trim_head`: a 16th of the budget from 128 to 256 MiB, a quarter at most), from where each new RGBA8 level
  costs an eviction, or an image that found no memory. The textures that load from then on are held as BC7. Those loaded
  before as RGBA8 whose memory is unchanged (unwritten where write tracking watches it, else the hash they loaded with)
  go too (`texture.c`'s `vk_tex_later_exact`): each is read again from memory at its next use, its RGBA8 image the same
  and its blocks kept, and queued for the encoder, 16 MB of RGBA8 a present at most and one at least (`later_reread`),
  so the frame after the switch does not decode at once every texture it draws. Keeping the blocks of every texture held
  Exact would cost memory for a switch that may never come, and leaving them RGBA8 left one state load's 1091 MB of them
  past a 1800 MB budget. One whose memory changed stays Exact. The room their encode frees joins the band
  (`snx_astc_auto_converted`).
- **Exact again** at a present (after the frame's trim) whose room under that mark is more than the most room any trim
  of the run has left under it (the band). A trim takes the caches from past the mark down to two heads under the budget
  or a little under, and with no band the next present would find room and turn Exact again, and the next trim BC7: two
  switches a trim. With the band, a trim alone never brings Exact back; memory the game freed (a scene left behind) or a
  budget that grew does. The band is not a number set here: the run's trims measure it, on this GPU and for this game.

Where the driver has no `VK_EXT_memory_budget`, both numbers are the renderer's own (`devmem.c`: its memory blocks for
the use, 80% of the heap's size for the budget), blind to what other programs take; Auto follows them, and its first
line says so. Its lines say the numbers as the device starts and at every switch, and Auto's state with the stats of
every 600 presents (an RX 6700 XT's, with `vk_budget_mb=1800`):

```
vulkan: astc_mode=0 (Auto): ASTC textures that load are decoded to RGBA8, exact, in four times BC7's memory while heap 1 has room, and BC7 from the first trim of its caches, past 1620 of the 1800 MB budgeted, until the room under that is more than any trim has left: 0 MB in use as the device starts
vulkan: astc_mode=0 (Auto): ASTC textures that load from now on are BC7, decoded to RGBA8 at once and encoded again as BC7 between frames, a quarter of RGBA8's memory: heap 1 has 1622 of 1800 MB budgeted in use, 177 MB free, past the 1620 MB where its caches' trim starts
vulkan: astc_mode=0 (Auto): ASTC textures that load now are BC7, after 1 switches; the trims left at most 0 MB under where they start
```

Held as RGBA8 by Exact (`astc_mode=1`), the log suggests `astc_mode=2` once a run, with the budget, the use and the
megabytes of ASTC textures decoded to RGBA8, when the images run short (`surface.c`'s `astc_bc_suggest`): an image
allocation finds no memory, a trim leaves the caches past their budget, or a trim takes what the last eight presents
used (the working set, which the next frames load again). Auto makes them BC7 itself then.

### By what the blocks hold

Auto also reads what each ASTC texture's blocks can hold, as it is made, and holds it in the smallest BCn format that
holds that, whatever the room: luminance alone as BC4 (8 bytes a 4x4 block, an eighth of RGBA8), luminance and alpha as
BC5 (16, as BC7), opaque colour on one line a 4x4 block as BC1 (8, half of BC7). The rest is held as above, Exact or
BC7 by the room. Exact (`astc_mode=1`), BC7 (`2`) and BC7 at load (`3`) are as they were, and so are 3D textures and
textures with holes in their mapping.

**The kind** (`gpu/texture/astc_kind.h`) comes from each block's header, its colour endpoint modes and, for the RGB and
RGBA modes, its endpoints' values, not from its texels: luminance modes (0, 1) decode R = G = B with alpha 255,
luminance and alpha (4, 5) R = G = B, the RGB ones (6, 8, 9) alpha 255, each partition a line through its two
endpoints. A partition of an RGB or RGBA mode (6, 8, 9, 10, 12, 13) whose two endpoints, made from its colour values as
the decoders make them (the integer sequence, unquantized, then blue contraction, the base and offset's bit transfer and
clamp), have R, G and B within a level of each other (`SNX_ASTC_GREY_SPREAD`) counts as luminance, or luminance and
alpha: its texels are grey within two levels, which the BC4 and BC5 views give back as R. Encoders write grey there
too: astcenc's `-normal` encodings of two normal maps (X in R, G and B, Y in alpha) used the RGBA direct mode (12) in
17% and 0.1% of their blocks, which read by mode alone would have kept them from BC5, and 8 and 16 of their texels in a
million and four are a level off grey. A second plane on R, G or B weighs that channel
apart, two partitions of colour are two lines, and an HDR mode, an illegal block or a void extent with a reserved bit
off decode as the error colour, which takes the texture to the usual format. Every test the decoders (`tests/support/astc.c`,
`astc_decode.glsl`) make a block the error colour by is made here in the same order. BC1's one line a block also needs
each 4x4 block inside one ASTC block: at 5x5, 6x6 or 10x8 a 4x4 block takes the texels of two to four ASTC blocks, each
a line of its own, and on random blocks BC1 erred by up to 141 levels there against 39 at 4x4, 8x8 and 12x12, so
colour goes to BC1 only at those three footprints. `texture.c`'s `astc_kind_of` reads every level of every layer from guest memory through
its block linear layout (or pitch rows), the layout's padding left out, and stops at the first block that leaves no
BCn format; memory mapped in more than one run, with bytes the GPU holds or a write of the GPU's pending over it, is
not read, and the texture is held as usual. One of the tests' 128x96 textures of 8 levels took 15 to 22 us (a -O0
build).

**sRGB.** BC4 and BC5 have no sRGB formats, and their UNORM image would give the shader the sRGB codes unconverted.
So an sRGB texture of luminance goes to BC1's sRGB format, the grey line in 565 (a level or two of tint where R and B
have 5 bits and G 6), and one of luminance and alpha keeps the usual format.

**Sampling.** BC4 holds the texels' R and BC5 their R and alpha, in its R and G. The view gives them back as the game's
blocks decode (`render.c`'s `tex_map`): the header's R, G and B read the image's R, its alpha 1 (BC4) or the image's G
(BC5), and its constants stay. BC1 is opaque: its blocks are four colours, the first end the greater, or both ends one
with every index 0, so no texel decodes transparent.

**Encoding.** The encoders are a pass each, a fraction of the decoder's cost, so such a texture goes up straight to its
format as it loads (`astc_bcn.comp`, its body `astc_bcn.glsl` over `bcn_encode.glsl`, in the strips BC7's upload makes),
with no RGBA8 image before it and nothing between frames, the cache or the background. BC4: the least and greatest
value as the ends, eight values, then the same pulled in by a sixteenth and a least squares fit to the steps the texels
took; BC5 is two of them. BC1: the bounding box's line, each channel turned to run with the widest (the sign of their
covariance), as it is and pulled in by a sixteenth, then a least squares fit to the colours chosen; a block of one
colour takes, a channel at a time, the 565 ends whose colour a third of the way between comes nearest it. Integer
arithmetic in the part of GLSL that is also C++, so the tests run the same file on the CPU and get the GPU's bytes.

**Written again.** A texture held so and loaded whole again reads its blocks again (`astc_kind_again`): blocks that
take another format now (luminance and alpha over a BC4 texture, colour over either) put an image of that format in its
place, or of the usual one, RGBA8 or BC7 between frames as Auto holds the rest then. Pieces load it whole, which reads
them: a piece of other blocks would show in a format that does not hold them. A texture held as usual is not read again.

**The log** says the format and what the blocks hold for each texture held so (the first 32), each texture that turns
when loaded again (16), and with the stats of every 600 presents the textures read by outcome, the CPU's time, the
levels and megabytes of each format against RGBA8's, and those read again:

```
vulkan: a 128x96x1 ASTC 8x8 texture of 8 levels holds luminance alone: held as BC4 (read in 22 us)
vulkan: a 128x96x1 ASTC 8x8 texture held as BC4 is loaded again with blocks of luminance and alpha: held as BC5 from now on
```

**What it costs**, against the texels the game's blocks decode to (the CPU decoder's), as the view gives them back
(`snx-test-astc-bcn-glsl --measure [--srgb] FILE.astc...`, the shaders' bodies run on the CPU, the GPU's bytes): PSNR over
RGBA, and the largest error of R, G, B and A in levels of 256. BC7 is the encoder's fast build on the same blocks, what
Auto gives them under pressure. The pictures are astcenc's `-medium` encodings of the Khronos glTF sample images
astc-encoder's tests carry (MIT), and edenprobe's astcimage picture:

| Blocks | Footprint | Held as | Its PSNR, largest errors | BC7's |
| --- | --- | --- | --- | --- |
| A 1024x1024 occlusion map (luminance) | 4x4 | BC4 | 54.36 dB, 12 12 12 0 | 59.89 dB, 5 5 5 0 |
| | 6x6 | BC4 | 54.86 dB, 12 12 12 0 | 59.96 dB, 7 7 7 0 |
| | 8x8 | BC4 | 56.27 dB, 10 10 10 0 | 59.74 dB, 5 5 5 0 |
| | 12x12 | BC4 | 57.47 dB, 10 10 10 0 | 59.71 dB, 5 5 5 0 |
| The same map encoded as sRGB | 4x4 | BC1 sRGB | 46.81 dB, 24 22 24 0 | 59.90 dB, 6 6 6 0 |
| | 6x6 | BC1 sRGB | 46.88 dB, 27 28 27 0 | 59.96 dB, 7 7 7 0 |
| | 8x8 | BC1 sRGB | 47.49 dB, 24 22 24 0 | 59.68 dB, 5 5 5 0 |
| | 12x12 | BC1 sRGB | 47.66 dB, 22 23 22 0 | 59.67 dB, 5 5 5 0 |
| edenprobe's gradients, 64x64 | 8x8 | BC1 | 45.33 dB, 5 4 5 0 | 53.57 dB, 2 2 2 0 |
| The same, sRGB | 8x8 | BC1 sRGB | 45.25 dB, 5 4 5 0 | 53.72 dB, 2 2 2 0 |
| A 1024x1024 normal map (`-normal`: X in R, G and B, Y in alpha) | 4x4 | BC5 | 50.08 dB, 17 17 17 14 | 46.67 dB, 29 29 29 90 |
| | 6x6 | BC5 | 50.01 dB, 17 17 17 14 | 47.61 dB, 37 37 37 102 |
| | 8x8 | BC5 | 50.57 dB, 17 17 17 14 | 48.95 dB, 67 67 67 101 |
| | 12x12 | BC5 | 50.76 dB, 20 20 20 14 | 51.18 dB, 32 32 32 108 |
| A 2048x2048 normal map, the same | 4x4 | BC5 | 52.73 dB, 17 17 17 20 | 50.63 dB, 28 28 28 74 |
| | 6x6 | BC5 | 52.71 dB, 17 17 17 17 | 51.73 dB, 29 29 29 66 |
| | 8x8 | BC5 | 53.15 dB, 20 20 20 17 | 53.01 dB, 30 30 30 78 |
| | 12x12 | BC5 | 53.11 dB, 18 18 18 16 | 53.83 dB, 35 35 35 69 |
| Random blocks of luminance, every footprint | | BC4 | 41.08 dB, 17 17 17 0 | |
| Random blocks of luminance and alpha | | BC5 | 40.20 dB, 17 17 17 19 | |
| Random blocks of colour on one line, 4x4, 8x8, 12x12 | | BC1 | 36.75 dB, 39 32 33 0 | |
| Random blocks of luminance, sRGB | | BC1 sRGB | 34.35 dB, 40 43 40 0 | |

Random blocks are the encoders' worst case, each texel at a weight of its own; `astc_bcn_glsl` holds them to those
numbers. The rest of those pictures keeps the usual format: the four RGB ones (diffuse, emissive, two metal and
roughness maps, 4x4, 8x8 and 12x12, `-esw rgb1`) mix partitions of colour, and luminance blocks among colour ones;
RGBA and edenprobe's whole picture hold colour and alpha. On the normal maps BC5 loses less than BC7's fast build at 6
of the 8 footprints and errs far less in alpha (Y), its two channels apart where BC7 fits lines through RGBA.

**Memory and time.** BC1 and BC4 take 8 bytes a 4x4 block, half a byte a texel, an eighth of RGBA8's four and half of
BC7's 16; BC5 takes BC7's 16. Their encoders cost little more than the decode: on an RX 6700 XT (RADV), a level
uploaded from the start of the upload to the GPU's end of it (`snx-test-astc-vulkan --bench`, the median of four runs
after the first, which makes the pipeline; a -O0 test build, under `lock.sh gpu-measure`):

| Blocks | Level | RGBA8 | BC7 (fast build) | BC1 | BC4 | BC5 |
| --- | --- | --- | --- | --- | --- | --- |
| Random 8x8 | 4096x4096 | 1.5 ms | 13.4 ms | 5.2 ms | 2.5 ms | 2.8 ms |
| | 2040x2040 | 0.7 ms | 4.0 ms | 1.5 ms | 0.7 ms | 0.9 ms |
| The occlusion map's 4x4, repeated | 4096x4096 | 4.0 ms | 7.7 ms | 4.9 ms | 2.8 ms | 3.0 ms |
| | 2040x2040 | 1.0 ms | 2.5 ms | 2.0 ms | 1.2 ms | 1.2 ms |
| The occlusion map's 8x8, repeated | 4096x4096 | 1.5 ms | 4.9 ms | 4.9 ms | 2.5 ms | 2.5 ms |
| | 2040x2040 | 0.5 ms | 1.2 ms | 1.4 ms | 0.8 ms | 0.8 ms |

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
game: one made BC7 between frames goes up as RGBA8 first, above) of the very large size or more, every layer of a 2D
array and every slice of a 3D level counted, now goes another way. The size starts at `astc_quick_mtexels`
megatexels (8 by default: a 4096x2048 level, or two layers of 2048x2048) and then follows the encoder's speed here
([The size from the encoder's time](#the-size-from-the-encoders-time)):

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

#### The size from the encoder's time

The stall a very large level saves is the fast build's time over it, and that time is the GPU's: 25 ns a texel on the
Radeon 890M above, 65 to 1400 megatexels a second on a GeForce RTX 2080 by the blocks (a game's normal maps to its
albedo). So `astc_quick_mtexels` stands for a time, what that many megatexels take the 890M (8: 0.21 s), and the size is
the one this GPU encodes in that time (`gpu/texture/astc_bc.h`'s `snx_astc_quick_texels`): the setting's own until the
fast build has been timed over as many texels, then that time over the cost a texel measured. Every transcode of the
fast build (its two passes) of a megatexel or more goes between a pair of timestamps on the graphics queue, each after
all the work before it (`astc.c`'s `bc7_time_begin`, 32 pairs at once, the rest untimed), and each pair's time goes into
the sums once its submission is done (`vk_astc_frame_done`). The sums are of the game's own blocks, so the cost is
theirs. A smaller dispatch is timed not: its time is the latency of its slowest blocks more than the encoder's
throughput, which is what a very large level's strips cost (`SNX_ASTC_QUICK_TIMED_TEXELS`, with the numbers that
measured it). The levels encoded between frames go in strips of 32768 blocks at most, half a megatexel, so a game whose
ASTC textures are made BC7 between frames times none, and its size stays the setting's: only the levels held as BC7 at
load are ever very large. A queue that writes no timestamps keeps the setting's size, and the log says why. The log says
the size once it comes from the time measured (with the cost a texel, the megatexels timed and the time the setting
stands for), and with the stats of every 600 presents where anything was timed or a texture is held as BC7 at load, what
the size is and where it came from. On an RX 6700 XT (RADV) the fast build of random blocks of every mode took 4.5 ns a
texel in a 1024x1024 level and 5.1 in a 1536x768 one (the tests below), so 8 is about 40 megatexels there; lavapipe took
27 to 41 ns, so 8 is 5 to 7 there.

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
- The blocks of device memory (`devmem.c`) stay within the cap.

The `[Debug]` setting `vk_max_allocation_mb` (`--vk_max_allocation_mb=4` for a run) lowers the cap, to test the pieces on a driver that accepts anything (lavapipe).

## Sparse textures

A sparse texture's memory is mapped a 64 KiB tile at a time; for blocks of 16 bytes a tile is 64x64 of them. Where the
device has sparse residency of 2D images and reads what isn't bound as zeros, `texture.c` gives such a texture's image
memory only for its mapped tiles. An ASTC 4x4 one held as BC7 (`astc_mode`, above) takes part: BC7's sparse block is
256x256 texels, 64x64 blocks of 16 bytes too, so each mapped tile is one block of the image, decoded and encoded on its own as
a texture's changed runs are. Other footprints (an 8x8 tile is 2x2 BC7 blocks) and ASTC held as RGBA8 (blocks of
128x128 texels) keep the whole image, and the log line saying so names the format and the device's block in the
layout's terms.

A game maps one tile of its memory at many tiles of a sparse texture, and the blocks of those tiles show the same
texels. Where the device binds memory to more than one place of an image (`sparseResidencyAliased`), such blocks
share one allocation. `texture.c` gives each block a key: a hash of which guest memory lies under it and where in the
block each run of that memory lies, with the block's level. `memory.c` binds the blocks of one key to the same memory
(`vk_image_resident` for a texture bound a tile at a time, `vk_image_sparse_bind` for the others). A block that joins
the memory of its key has that memory's texels and is not loaded. A block whose key changed is bound again. What is
loaded into one block of a key is in all of them, which is right because they show the same bytes. A block at a
level's edge and a mip tail share nothing. `vk_sparse_share=0` gives each block its own memory.

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
  production `vk/astc.c`, `vk/cmd.c` and `vk/memory.c`, `vk/devmem.c`, volk and the SPIR-V the build makes, and compare raw image
  readbacks with the CPU decoder byte for byte. They cover all 14 footprints with UNORM and SRGB images, each against
  the CPU decoder in its decode mode, and decode the same blocks in both modes for every footprint, checking that
  alpha is the same, that R, G and B differ by at most one step, and that some do. They also cover images whose size
  is not a multiple of the block size, dispatch padding, five mip levels down to 1x1, six layers, 3D slices batched
  into strips, scratch chunking, frame reuse and staging ring growth. `astc_vulkan_pieces` runs it under a 4 MiB
  allocation cap, so the bigger images are bound in pieces and read back in bands. `astc_vulkan_sync` runs it under
  the validation layer with its synchronization checks (`VK_LAYER_VALIDATE_SYNC=1`), an error each a failure: a
  hazard there is a wrong result on a driver that runs commands abreast. It is skipped where the layer isn't installed.
  No game files or window are needed:

  ```sh
  cmake -B build -DSNX_TEST_VULKAN=ON && cmake --build build --target snx-test-astc-vulkan
  VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json ctest --test-dir build -R astc_vulkan
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
- `astc_kind` (ctest, no GPU) holds `gpu/texture/astc_kind.h` to the CPU decoder: 168,000 random blocks of every
  block mode, partition count and endpoint mode, and 42,000 of RGB and RGBA modes whose values make grey ends or
  ends a level or two apart, all 14 footprints, both decode modes: a block said to have no colour decodes grey within
  two levels in every texel (these come to one), each RGB or RGBA partition's endpoints as far apart as the decoder's
  own (`tests/support/astc.c`'s `astc_block_endpoints`), RGBA direct with G a level over R luminance and alpha and with
  G 2 levels over R colour, one said to have no alpha opaque, one said to be colour on one line no more than 1.6
  (squared) off one, and only blocks the decoder makes all the error colour are taken for it. Each format comes from
  at least 500 of them. Then a level's kind read through block linear layouts (GOB blocks of several heights and
  depths, padding columns, slices) and pitch rows, the padding blocks of the error colour left out, a coloured block
  found wherever it is, a layout a byte short refused; the ISE's bit counts and the colour values' encoding against the
  specification's search; and the format each of the 16 kinds takes in each decode mode.
- `astc_bcn_glsl` (ctest, no GPU) runs `astc_bcn.comp`'s body on the CPU over levels of random blocks of each kind at
  every footprint, decodes its blocks (`tests/support/bcn.c`) as the view gives them back and holds the loss against
  the CPU decoder's texels to the floors measured (the table above), BC1's blocks to four colours; a block of one value
  or colour comes back exact as BC4 and BC5, within a level as BC1. `--measure` gives the table's columns.
  `astc_vulkan` and `astc_vulkan_pieces` upload random blocks straight into BC1, BC4 and BC5 images (BC1 in sRGB
  too), the strips BC7's take, with mips, layers and many strips, and hold the GPU's blocks to the CPU's, byte for
  byte; `--bench` times them beside RGBA8 and BC7. `vulkan_headless`'s `check_astc_kind` (`SNX_TEST_ASTC_KIND_ONLY=1`
  runs it alone) makes 128x96 ASTC 8x8 textures of luminance, of luminance and alpha, of colour on one line, of sRGB
  luminance and of RGBA direct blocks with grey ends under Auto, checks each is held as BC4, BC5, BC1, sRGB BC1 or BC5
  straight as it loads (the same RGBA blocks with ends apart stay RGBA8, their Exact twin's texels), its image as the
  view gives it back within the floors of its Exact twin, and the view's components for two of the header's
  swizzles; then loads one BC4 texture again with blocks of luminance and alpha (BC5), and writes one block of colour
  and alpha over it (loaded whole, RGBA8, its Exact twin's texels exactly).
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
  freed while it is made. It also times the encoder: a 1024x1024 texture held as BC7 at load adds its first level,
  one dispatch of a megatexel, to the sums once its submission is done, where the queue writes timestamps, and its
  smaller levels nothing; and it follows Auto: a texture loads RGBA8 with room, a trim under a budget lowered to the
  caches' use turns the next one BC7 and opens the game's BC7 cache, and the budget back turns the third RGBA8 again.
  `astc_bc_suggest` (ctest, no GPU) holds the modes and Auto with budgets given: BC7 from a trim, Exact again only
  past the band, a trim never bringing it back by itself over a grid of uses and frees, one switch over 200 trims of a
  sawtooth where no band makes 399, and the size from the times given (the start until as many texels are timed, five
  times faster five times bigger, sums of unlike costs).
- `astc_vulkan` and `astc_vulkan_pieces` also upload very large levels (`astc_quick_mtexels=1` there, a megatexel):
  bands of block rows of 4x4 and 6x6 blocks (whose bands step 3 rows) and pieces of several layers, once with the
  background on the CPU and once on the GPU. The first upload's image is the quick build of the CPU decoder's texels,
  byte for byte, and the level goes to the background; once it is done (`vk_astc_background_drain`), the next upload,
  and one after the file is opened again, copy the pieces, which are the fast build of the whole level byte for byte:
  the CPU's and the GPU's pieces are the same. A hit out of time (`g_vk_astc_hit_ns` 0) encodes quickly and loads the
  pieces into the image after it. An image kept alive while the background runs ends up with the fast build too, from
  the pieces it copied into it. On the GPU, a background stopped after one piece leaves it in the cache, and the next
  makes only the others. These cases keep the setting's size (`vk_astc_quick_measure(0)`). Then a 1536x768 level
  with the quick encode off adds its one dispatch of the fast build to the sums; with 2 megatexels the start, another
  takes the fast build while the setting's size holds, and the quick build once times of a GPU twice as slow as the
  890M make the size a megatexel, its quick strip and its background's pieces adding nothing to the sums.

The CPU decoder was also compared with ARM's astcenc 5.7.0 (`astcenc -dl` and `-dH`, Apache-2.0, a release binary
used only as a tool), on the same kind of random blocks for every footprint. Every texel agreed within one step of
rounding (astcenc's 8 bit output rounds differently), and every block one rejects the other rejects too. In the LDR
profile astcenc also rejects HDR endpoint modes, which this decoder decoded then. After HDR endpoints became the
error color, the CPU decoder was compared with edenprobe's LDR profile reference decoder (`source/astcref.c`, held
to astcenc by edenprobe's reference suite) on 20,000 random blocks for each of the 14 footprints: every texel was
one of the two bytes the reference allows, error blocks included. The sRGB decode mode was checked
the same way against `astcenc -cs` and `-ds` on five footprints: every byte agreed, alpha included.

## Findings and what is left

Both transcoders are meant: ASTC made BC7 (this page) and BCn made ASTC ([BCn without BCn](bcn.md#as-astc)), which
share the fitter (`bc7_encode.glsl`). Every time is a GeForce GT 1030's, the one GPU any of this was measured on, on
levels two games load (`astc_dump=DIR`) and on random blocks.

### What was found

Each is a measurement that decided how the shaders are written. Most have their numbers where the thing is
described; they are together here so that a change that goes against one is made knowing it.

**Where the time is.**

- *The texels' traffic was never the cost.* The transcoders were first two dispatches with RGBA8 between them, and
  the stall was taken for bandwidth. Decoding a 2040x2040 level to RGBA8 is 4 ms and encoding it 230 to 390: the
  round trip was 1% ([Straight from the blocks](#straight-from-the-blocks)). Going straight from the blocks is kept
  for what it frees, a level of RGBA8 that a phone has no room for, and not for speed.
- *Invocations run abreast at the pace of the slowest.* This is the finding everything else follows from. One hard
  block in a group of 32 holds the other 31, so a level with 18% hard blocks took 21 times one with none, for 5
  times the operations. Hence a pass for each way of making a block, with work lists and indirect dispatches
  between: two for BC7 (392 to 94 ms, [Two passes](#two-passes)), three for ASTC ([three passes](bcn.md#as-astc)).
  A cheaper path put *inside* a pass does not help and can hurt: the BCn block's hints in one dispatch with the
  search made the ASTC build slower, a third of the operations though they were, and a quick first line for mode 6
  (`SNX_BC7_QUICK`) made levels with hard blocks no faster. A new shortcut wants a pass of its own, or the blocks it
  takes listed apart.
- *The workgroup's shared memory costs workgroups in flight.* 10 KB of it declared and not even used took the encoder
  from 230 to 285 ms on NVIDIA. So an ASTC block is decoded again by each BC7 block over it, up to nine times,
  into the invocation's own storage, and that is still faster than decoding it once into shared memory. Decoding each
  ASTC block once ("cells") was tried again after the two passes and was not worth it either.
- *An array a shader indexes by a value is the phone's shared memory.* It is scratch memory on Adreno and Mali, and
  glslang compiles a constant array read at a value into one, the table copied in at every place it is read (ten
  copies of a 768 word table in one pass, before the tables went). The two opposite constraints leave one form that
  suits both: words in registers, fields cut at constant bits, tables as arithmetic or as selects. The first two
  passes of the ASTC build are written so ([No texel decoded](bcn.md#as-astc)).
- *Tables in a buffer were tried and dropped.* The colour encodings' tables as a storage buffer gave the same bytes
  and needed a fourth binding and an upload; arithmetic (`astc_quant.glsl`) left the pass that moves blocks as fast
  or faster (0.7 to 0.45 ms with the arrays gone) and costs only the search, 9%.
- *Reading the result back is not the transcoder.* `--bench-etc2` once read each output over the bus, 10 ms for 4 MB,
  which hid everything under it: ASTC looked twice as fast as ETC2 and is 20 to 190 times. The benches time the
  dispatches alone now (`tc_time`). Time passes apart, too: the search is 88 of a hard level's 90 ms, and a change to
  the other two cannot be seen in the total.

**The fitter.**

- *Its inner loop is written for the operations a GPU has* (`Eval`): two dot products of packed bytes for a texel's
  place along the line, both channels' pairs interpolated in one multiplication each, and the division a
  multiplication by a reciprocal with the remainder setting it right, so the bytes are the division's. 26% faster
  for the same blocks; SPIR-V's packed dot product, where the GPU says it is accelerated, 5% more (`g_vk.bc7_dot`).
- *A block of one colour needs no fit*, and has a closed form in mode 5 (the ends `c >> 1` and that plus one where
  the low bit wants it, at the weight 21 of 64). Mode 6 has none in general: its p-bit is shared by colour and alpha,
  and it cannot hold 0 beside 255.
- *The source says what the search looks for.* An ASTC block's partitions, planes and block boundaries name BC7's
  partition and rotation (67 ms where the search took 94, for 0.14 dB); a BCn block is lines already, which the ASTC
  encoder tries as they are. A hint is still measured on the texels, so a wrong one costs time and not the picture.
- *Where to stop is worth more than how to search.* Keeping a hinted line up to a squared error of 1024 a block and
  not 256 halved a level's time for a quarter of a dB: the blocks past the line are few, and nearly all of the time.
- *A block that is the target's layout already is moved, not encoded.* BC7 mode 6 is ASTC's one partition with 4 bit
  weights, modes 4 and 5 its 2 and 3 bit ones, BC1 its 8 bit ends with 2 bit weights. No texel is decoded and nothing
  is measured, so each kind is bounded by a test and not at run time. This is the cheapest block there is, and most
  of a game's are of these kinds: a level of them takes 0.5 ms where the decode alone to RGBA8 takes 2 to 5.
- *ASTC's 2 and 3 bit weights are BC7's*, and its 16 are BC7's within 1 of 64, which is why the ASTC encoder is the
  BC7 fitter with another quantizer and another packer, and why the weights move as the bits they are.

**What was tried and left out.**

- A two pass ETC2 encoder: the same bytes and no gain, since every block there tries all eight candidates and none
  is a hundred times another. ETC2 stays one dispatch, and stays only for BC1, BC4 and BC5 under memory pressure.
- Integer comparisons for a texel's weight in the BC7 encoder: the same index, 3% slower than the reciprocal on
  NVIDIA. They are in the array-free pass alone, for GPUs that convert between integers and floats slowly.
- A build for the GT 1030 beside a generic one: nothing found was particular to it but the packed dot product, which
  is a feature bit. One shader, two builds by that bit.
- `ucas` over the shaders found the peepholes noted where they are (`Weight`, `Widen`, `Quant`). It fails on whole
  modules and wants a function at a time, and a prelude of plain globals makes it report values as constant that
  are not.

**How it is held.** The shaders are written in what GLSL and C++ share, so each runs on the CPU
(`tests/glsl/glsl.hpp`) and the GPU's bytes are held to that run, block for block. Every rewrite above was made
against it: the same bytes, or a bound stated in a test. The arithmetic that stands in for a table is checked against
the decoder's own rules over every input. Keep that: it is what let the fitter's inner loop, the quantizer and two
passes be rewritten in a day each.

### What is left

What the transcoders do not do yet, for whoever takes them further.

**Measure on a phone.** The BCn to ASTC build's first two passes keep no arrays, read no table and, for the layouts
they write, run no floating point, because Adreno and Mali are said to be slow with each. None of it has been timed
on either. Before anything below: the three passes' times on an Adreno 6xx and a Mali, with arrays and without
(`SNX_NO_ARRAYS`), and with the colour encodings worked out and read from a table (the commit that removed the tables
has them).

- The array-free second pass reads its 16 texels through a tree of selects (`PX`) inside loops of 16. That is free
  only where the driver's compiler unrolls the loops; where it does not, every read is 15 selects. If a phone shows
  that, unroll at build (`[[unroll]]` and `spirv-opt --loop-unroll`, or the loops' bodies written out as the decode
  is by `RG_FILL`).
- That pass's SPIR-V is twice what it was (327 KB), a texel's decode being written out 16 times. Its pipeline's
  creation time on a phone is not known. A loop with `PX_SET` is the small form, at 16 selects a texel written.

**The third pass, the search, still has arrays.** It is the fitter the BC7 encoder shares, and nearly all of the
time of a level that is not mostly moved (88 of 90 ms).

- The texels are behind `PX` already. Left: `cand_s0`, `cand_s1`, `best_s0` and `best_s1` (three subsets' ends),
  the partition tables `P2`, `P3` and `ANCHORS`, `ASTC_PART`, read 64 times a block where the partitions are scored
  (`Spread`: a tree of selects is too dear there, so the score wants each partition's mask some other way, or the
  hash that makes ASTC's partitions worked out), and `texconv.glsl`'s decoder of every BC7 mode (`bc7_block`: `e[24]`,
  the block's four words, the modes' tables), since modes 0, 2 and 7 arrive here. `bcn_regs.glsl` has the pattern.
- The same for the ASTC to BC7 build: `astc_bc7.comp` keeps the decoded ASTC block in `astc_block[39]`, of the
  invocation's own, and `astc_decode.glsl` reads it at fields worked out per texel.
- The fitter's quantizer is arithmetic since the tables went (`astc_quant.glsl`), which costs the search 9% (80 to
  87 ms). It needs the value alone, not the code: a formula for the value in one multiplication, or thresholds, would
  take that back. A table in the desktop builds would too, at the price of two quantizers to keep alike.
- A texel's weight of 4 bits is still a reciprocal in floating point with a remainder to set it right. The
  comparisons the second pass has for 2 and 3 bits would be 15 thresholds, four halvings; in the BC7 encoder the 2 and
  3 bit ones were 3% slower than the reciprocal, so this is for a phone to decide. Half floats were not tried: `n`
  and `dd` pass a half's range and would be shifted down first, the remainder then setting right an error of one as
  it does now, and the CPU run of the shader (`tests/glsl/glsl.hpp`) has no half float to hold the bytes to.

**Blocks that could be moved and are decoded.** Every block the first pass moves costs a hundredth of one searched
for, so each kind below is worth more than any tuning of the fitter.

- BC7 modes 1 and 3 whose partition ASTC has exactly: 30 of the 64 (`tools/astc_tables.py` counts them). Two
  partitions of RGB ends leave an end 40 values where BC7 has 128 or 256, so the move wants a bound first, as
  `tests/glsl/astc_encode_glsl.cpp`'s `direct_blocks` gives the others.
- BC7 mode 4 with a rotation, and mode 5's rotated blocks that are not opaque: two planes of RGBA ends, 48 values a
  channel. Too coarse unmeasured; fine where the ends lie near.
- BC7 modes 4 and 5 with two alphas and no rotation: the same two planes, alpha apart.
- BC1 with a transparent texel: ASTC has no punch-through, so it is two planes of RGBA or a void extent where the
  block is all transparent, which is not caught today. These go straight to the search now.
- BC2 and BC3 with alpha: the colour line is BC1's and could be moved as it is, and only alpha's plane fitted. BC3's
  alpha is a line already (its two ends, 8 values): two planes with the weights requantized, no texel decoded.
- BC3's second pass is a tenth slower without arrays (5.5 against 5.0 ms). Its alpha is three integer divisions a
  texel as NVIDIA rounds them (`bc3_alpha`, `bc1_pixel`), which the above would remove.

**Layouts the ASTC encoder does not write.** Three and four partitions (BC7 modes 0 and 2 have three subsets and
are searched for as one or two); two partitions with alpha (mode 7); two partitions with two planes; weight grids
smaller than 4x4, which buy the ends more values; the blue contraction and the base and offset endpoint modes, which
give ends near each other a bit more each. Each is a layout in `AstcEncode`'s list and a row in `AsPack`.

**Smaller things.**

- A block of two partitions reads `ASTC_PART` three times (`SetShape` twice, the seed once): in the array-free pass
  that is three trees of 63 selects. Keep the word.
- `SNX_ASTC_HINT_ENOUGH` (1024) and `SNX_ASTC_DIRECT_NEAR` (40) were set on nine levels of two games. More games,
  and the share of blocks each pass takes for each, would say whether they hold.
- The colour encoding of trits over 5 bits has no rank in one multiplication and tries two (`AsQuantKey`). No layout
  comes to it; one that does should look for a better form first.
- The BCn to ASTC build's passes were not given to `ucas` since they were rewritten; the BC7 fitter and the ETC2
  encoder were.
- BC1, BC4 and BC5 still go to ETC2 and EAC once memory is short, an encoder 20 to 190 times the ASTC one's time.
  BC4 and BC5 as ASTC (one and two channels, a line each, which is the block as it stands) would end that.
- BC6H has no path to ASTC's HDR modes and is decoded to RGBA16F.
