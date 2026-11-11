# BCn without BCn: ETC2, EAC and ASTC on phone GPUs

## Index

* [The encoder](#the-encoder)
* [ASTC 4x4 between frames](#astc-4x4-between-frames)
* [The ASTC encoder](#the-astc-encoder)
* [What it costs](#what-it-costs)
* [Tests](#tests)

Most games' textures are BC1 to BC7. Desktop GPUs sample them as they are; most phone GPUs (Mali, Adreno with
Qualcomm's own driver, PowerVR) sample ETC2, EAC and ASTC instead, and not BCn. There ScarletNX encodes each BCn level
again on the GPU as it loads, into the ETC2 or EAC format of the same bytes a texel, where decoding it would take four
to eight times the memory. Where the GPU samples ASTC too, a BC2, BC3 or BC7 texture that is then sampled for a while
and whose alpha is one value within every block is made ASTC 4x4 between frames, which holds its colours nearer.

| BCn | where the GPU samples ETC2 | bytes a texel | decoded (no ETC2 either) | bytes a texel |
| --- | --- | --- | --- | --- |
| BC1 | ETC2 RGB8A1 (punch-through alpha: BC1's transparent black) | 0.5 | RGBA8 | 4 |
| BC2, BC3, BC7 | ETC2 RGBA8 (an EAC alpha block, then an ETC2 RGB one); ASTC 4x4 later, or at once without ETC2 | 1 | RGBA8 | 4 |
| BC4 | EAC R11, unsigned or signed | 0.5 | R16 | 2 |
| BC5 | EAC RG11, unsigned or signed | 1 | RG16 | 4 |
| BC6H | none (ETC2 has no HDR): decoded | 8 | RGBA16F | 8 |

`format.c` picks it (`TB`), from the features `vk_blocks_want` turns on: `textureCompressionBC` and the `native_bc`
setting for BCn as it is, else `textureCompressionETC2` with `native_etc2` for ETC2, else, for BC2, BC3 and BC7,
`textureCompressionASTC_LDR` with `native_astc` for ASTC 4x4 as they load. A 3D BCn texture is decoded where the
device makes no 3D image of the ETC2 format. As the device starts, the log says how each family is held,
from the formats `vk_tic_format` picks (`vk_blocks_say`), on a Mali-G52 for instance:

```
vulkan: ASTC textures (native_astc=1, the GPU samples ASTC, astc_mode=0): sampled as the game's blocks: ASTC
vulkan: ETC2 and EAC textures (native_etc2=1, the GPU samples ETC2): sampled as the game's blocks: ETC2 RGB, ...
vulkan: BCn textures (native_bc=1, the GPU samples no BCn, ETC2 sampled, ASTC sampled): encoded again on the GPU:
BC1 as ETC2 RGB8A1, BC2 as ETC2 RGBA8, BC3 as ETC2 RGBA8, BC4 as EAC R11, BC5 as EAC RG11, BC7 as ETC2 RGBA8;
decoded on the GPU: BC6H to RGBA16F
vulkan: BC2, BC3 and BC7 textures held as ETC2 RGBA8 are made ASTC 4x4 between frames, 4096 blocks a frame at most,
once sampled in 60 presents since they were written, where their alpha is one value within every block (the GPU
samples ASTC and ETC2, native_astc=1)
```

## The encoder

`texconv.comp` built a second time with `SNX_TC_ETC2`, its own pipeline (`vk/texconv.c`), runs a block an
invocation: `texconv.glsl`'s BCn decoders, then `gpu/texture/etc2_encode.glsl`. The texels go through
`texture.c`'s uploads as the decoded ones did: the dispatch writes rows of blocks, and the copy into the image counts
them as rows of texels four times as wide.

ETC2 RGB tries the modes that suit the block and keeps the one nearest it (squared error, as the decoder decodes,
clamping and all):

- ETC1's differential mode (5 bit colours and a 3 bit difference), and the individual one (two 4 bit colours) where
  the difference can't hold both subblocks, each split side by side and one above the other. Each subblock starts from
  its texels' mean; then every table takes the base its indices want from there (each texel less the modifier its index
  adds), and the pair of table and base nearest the texels is kept.
- T and H, from the two clusters the texels fall in (2-means from the two farthest texels), every distance tried.
- Planar, each channel's least squares fit over the 4x4 grid, then the levels around it while that is better.

A block with transparent texels clears its opaque bit, so index 2 is transparent: only the differential, T and H modes
are tried, and every transparent texel takes index 2 and no opaque one. EAC tries the sixteen tables with the two
multipliers around the one that spans the block's values, the base that centres each, and the best base a step either
way; an alpha the same throughout is written at once. Integer arithmetic only, so the tests run the same file on the
CPU (`tests/glsl/glsl.hpp`) and get the GPU's bytes.

## ASTC 4x4 between frames

ETC2 RGBA8 and ASTC 4x4 take the same bytes, and neither holds everything. Against the BCn blocks decoded, on the
pictures of [What it costs](#what-it-costs):

| | colours, dB | alpha where it moves, dB |
| --- | --- | --- |
| BC2, BC3 as ETC2 RGBA8 | 37.2 to 49.1 | 50.0 to 58.6 |
| BC2, BC3 as ASTC 4x4 | 53.8 to 60.1 | 38.8 to 41.1 |
| BC7 as ETC2 RGBA8 | 35.8 to 48.4 | 50.2 to 53.5 |
| BC7 as ASTC 4x4 | 38.7 to 55.3 | 40.9 to 42.7 |

BC2's, BC3's and ETC2's alpha is a block of its own, 8 or 16 levels a texel. An ASTC 4x4 block that gives alpha
weights of its own has two planes of four levels each and ends of 48, and this encoder, which does not search for
the blocks another kind would hold better, gives every such block those. So ETC2 is how they load, fast and safe
for any alpha, and ASTC is for the textures it holds better in every channel: those whose alpha is one value
within every block, where an ASTC block spends nothing on it (`gpu/texture/bcn_astc.h`).

`texture.c` does it after the frames' presents (`vk_tex_later_frame`, the BC7 encoder's place):

- A texture counts the presents it is sampled in since it was last written. At 60 (`VK_ASTC_LATER_READS`) its blocks
  are read once on the CPU, as they lie in guest memory, 16 bytes each: `snx_bcn_alpha_fixed` says of each whether
  its alpha is one value, from the block's own bytes (BC2's sixteen nibbles; BC3's two ends and indices; BC7's mode,
  rotation and alpha ends). It may call a block of one alpha moving, which only leaves a texture as ETC2; never the
  other way round. A texture with one such block stays ETC2 until it is written again.
- The others queue. Each frame the first in the queue gets up to 4096 blocks (`VK_ASTC_LATER_BLOCKS`), a row of a
  level at least: read from guest memory as an upload reads them, encoded by `texconv.comp`'s ASTC build into an
  image of its own, in its own submission. The game samples the ETC2 image meanwhile.
- Whole, the ASTC image takes the ETC2 one's place, which goes once the frames that sample it are done.
- A write drops what was made and counts the presents from none: a texture written every few frames never gets
  there, and stays as it loaded. One written whole after it became ASTC is ETC2 again first.

Block linear 2D textures and arrays whose memory is all mapped are taken; the others stay ETC2. A texture is ASTC
60 presents and a frame for each 4096 of its blocks after its last write: a 1024x1024 one with its mip levels (87
thousand blocks) in 82 presents, a 256x256 one in 62.

## The ASTC encoder

`texconv.comp` built a third time with `SNX_TC_ASTC` runs `gpu/texture/astc_encode.glsl` after the BCn decoders,
a block an invocation. It does not search: a BCn block says what it is, and the ASTC block follows it.

| the block | the ASTC 4x4 block | ends | weights |
| --- | --- | --- | --- |
| one colour | a void extent block, exact | 16 bits a channel | none |
| colours on a line, alpha 255 | one partition, RGB | 192 levels | 16 levels |
| colours and alpha on a line (BC7's mode 6 with alpha; one colour under an alpha that moves) | one partition, RGBA | 192 levels | 8 levels |
| colours on a line under one alpha, in a block that keeps alpha apart (BC2, BC3) | one partition, RGBA | 256 levels, the alpha exact | 4 levels, BC1's four colours |
| a channel with indices of its own that moves while others do (BC2's and BC3's alpha, what BC7's modes 4 and 5 rotate out, mode 7's alpha) | two planes, RGBA | 48 levels | 4 levels each |
| two or three subsets without alpha (BC7's modes 0 to 3) | two partitions, RGB | 40 levels | 4 levels |

The ends' ranges are not chosen: the format gives the bits the weights leave to the largest range that fits. ASTC
draws its partitions from a hash of a 10 bit seed, not from BC7's tables, so two tables name the seed nearest each
BC7 shape and the texels of its second partition: 30 of BC7's 64 two subset shapes exist as they are, the other 34
are 2 texels off on average, and three subsets become one alone and the other two together.

A subset's ends are the corners of its texels' bounding box, which a BCn subset's texels lie between: the channel
that moves most says which texels are the line's first and last, and every other channel takes its low or its high
value at the first as it has there. Each end is stored as its nearest level, and a texel's weight is its place
between the two ends as stored.

A block that says more than "one line" is built as one line first, whose ends and weights have the most levels. If
the line holds the texels within a level a channel on average (`AX_NEAR`), that is the block: an encoder takes
subsets and planes for precision too, on texels one ASTC line holds better (most of a photograph's BC7 blocks).
Else the source's way is built as well, and kept unless the line is nearer. How far a block is from its texels is
measured with the weights taken as evenly spaced, a sum of whole numbers, not by decoding.

The numbers are small integers in floats of relaxed precision (`mediump`), which a phone's GPU computes in 16 bits,
several at a time. A 16 bit float holds every integer up to 2048, so sums and products that stay within it are the
same in 16 bits as in 32: the bounding box, the ends' corners, a texel's place (2040 at most). Three products do
round, each a multiplication by a constant that is then floored, and each was proved to give the integer result on
every input, in 16 bit floats and in 32 bit ones, with [ucas](https://github.com/scarletnx/ucas) (its `f16` type):

```
rank48(w: u8 in 0..127) = (1512 * u32(w) + 4347) >> 13
rank48h(w: u8 in 0..127) = u32(floor((f16(w) + f16(2.875)) * f16(0.1845703125)))
equiv rank48h rank48          # equal on the domain (exhaustive); the same with f32
third(k: u8 in 0..23) = k / 3
thirdh(k: u8 in 0..23) = u8(floor(f16(k) * f16(0.33349609375)))
equiv thirdh third            # equal (exhaustive), f16 and f32
rank40h(w: u8 in 0..127) = u32(floor((f16(w) + f16(4)) * f16(0.152587890625)))    # = (625 * (w + 4)) >> 12
fifthh(k: u8 in 0..19) = u8(floor(f16(k) * f16(0.2001953125)))                      # = k / 5
```

The first is the rank of the level of 48 nearest a value, the second splits it into the trit and the bits the
format stores; the last two are the same for 40 levels. So the blocks do not depend on the width a GPU computes
in. A texel's weight is one 16 bit integer multiplication (below 65536, `range`), and the comparison of two
candidates is in 32 bit floats, whole numbers below 2^24.

Each of the encoder's bodies is called from one place, a loop over a block's passes, as `etc2_encode.glsl`'s are:
the compilers inline every call, and the encoder written a call a kind took an Intel driver 90 s to compile where
this one takes 7.6 s.

## What it costs

Measured against the BCn blocks decoded, which is what a GPU without BCn showed before, on three 512x512 photographs
and two 256x256 icons with alpha, made BCn by the test's own encoders and the BC7 one (`snx-test-bcn-etc2-glsl
--image`):

| BCn | colour, dB | alpha, dB | lost against the original picture, past what BCn lost |
| --- | --- | --- | --- |
| BC1 | 36.9 to 49.1 | exact | 0.1 to 1.4 dB |
| BC2, BC3 | 37.2 to 49.1 | 50.0 to 58.6 | 0.7 to 3.0 dB |
| BC4, BC5 | 46.5 to 63.6 | | |
| BC7 | 35.8 to 48.4 | 50.2 to 53.5 | 5.5 to 11 dB |

BC7 loses most: ETC2 RGBA8 holds less than BC7's partitions and 4 bit weights do.

The same pictures as ASTC 4x4 (`snx-test-bcn-astc-glsl --image`), colours in dB and the SSIM of their luma against
the BCn blocks decoded. The three photographs are opaque: every block has one alpha, and they are made ASTC. The two
icons' alpha moves in 228 to 518 of their 4096 blocks, and they stay ETC2:

| picture | BC3 as ETC2 | BC3 as ASTC | BC7 as ETC2 | BC7 as ASTC |
| --- | --- | --- | --- | --- |
| photograph 1 | 49.10, 0.99602 | 60.14, 0.99967 | 48.42, 0.99492 | 53.38, 0.99835 |
| photograph 2 | 37.24, 0.98912 | 53.76, 0.99974 | 35.82, 0.98234 | 41.80, 0.99655 |
| photograph 3 | 46.64, 0.99641 | 59.43, 0.99993 | 47.40, 0.99675 | 54.92, 0.99933 |
| icon 1 (stays ETC2) | 42.87, alpha 58.58 | 59.23, alpha 41.14 | 42.41, alpha 53.51 | 55.27, alpha 40.93 |
| icon 2 (stays ETC2) | 41.63, alpha 54.67 | 55.71, alpha 38.78 | 37.45, alpha 50.16 | 38.68, alpha 42.74 |

Time on an Intel HD Graphics 620 (Mesa 26.1.8), which samples all three families: the conversion's dispatch alone
by the GPU's timestamps, a 2048x2048 level (262144 blocks) of a photograph's blocks, the median of 7, each block
compared with the CPU's run of the shader and the same:

| blocks | decoded | as ETC2 RGBA8 | as ASTC 4x4 | a block as ASTC |
| --- | --- | --- | --- | --- |
| BC7, a photograph | 8.2 ms | 291 ms | 137 ms | 522 ns |
| BC7, an icon | 6.8 ms | 262 ms | 74 ms | 283 ns |
| BC7, random (every block two candidates) | 8.6 ms | 368 ms | 172 ms | 656 ns |
| BC3, a photograph | 5.1 ms | 278 ms | 38 ms | 143 ns |
| BC3, random | 5.5 ms | 344 ms | 84 ms | 322 ns |

Where a BC7 block's time goes there: decoding it into the texels the encoder reads, 212 ns (the plain build, which
writes each texel as it is decoded, takes 31: the array the texels wait in is the cost); their bounding box, under
1; one line's ends, weights and bits, 70; and the source's own way where the line isn't near enough, 240 a block of
the photograph's on average. BC3 decodes in 14 ns and encodes in 130. 32 bit floats in place of relaxed precision
make no difference on that GPU, which has no 16 bit ones; a phone's has, and is not measured yet. Between frames,
4096 blocks are 3.2 to 3.9 ms of a frame there, the CPU's staging and the submission with them.

The pipelines are compiled the first time each is used, and kept in the pipeline cache: on that driver the plain
build takes 3.9 s, the ASTC build 7.6 s and the ETC2 build 34 s; on llvmpipe 0.3, 0.2 and 1.4 to 2.2 s.

On llvmpipe (a 12-core x86 CPU with other work running), the same dispatches of the same blocks: a photograph's
BC7 level 23 ms decoded, 124 as ETC2 and 60 as ASTC; its BC3 level 4, 105 and 15; random BC7 blocks 36, 153 and 63.

## Tests

- `bcn_etc2_glsl` runs the encoder on the CPU, its blocks decoded by `tests/support/etc2.c`: every candidate stores the
  mode it means, at the error the encoder worked out; the block written is the best candidate tried; a block of one
  4 bit colour, a planar gradient, an EAC block of every table and an alpha the same throughout come back exact, and
  1600 of 2000 random ETC1 blocks at least (744 with each subblock's base its mean alone) and 400 of 500 gradients
  the decoder clamps (241 with one walk of the planar levels around the fit); BC1's transparent texels
  stay transparent; images whose sides aren't multiples of 4, of several slices, transcode block for block as one
  block alone; and each kind holds the PSNR it reached on a picture of gradients, edges, noise and alpha.
- `bcn_astc_glsl` runs the ASTC encoder on the CPU twice, with 32 bit floats and with every operation rounded to 16
  bits (`glsl.hpp`'s `half`), and every block has to be the same bytes both ways. Its blocks are decoded by the
  runtime's ASTC decoder: each level stored for a value is its range's nearest and decodes to what the encoder
  reckons with; a block of one colour and lines the decoder makes of two levels come back as they were, in every
  kind of block; a table's texels for a seed are the ones the decoder draws; random BC2, BC3 and BC7 blocks of every
  mode decode near what they held and never to the error colour; a block said to have one alpha has one, by the
  reference decoder, in every kind and mode, and keeps it; the picture's see-through region is not a texture of
  one alpha a block; and each kind holds the PSNR it reached on the picture.
- `tex_upload_rows` runs `texture.c` on a model of the GPU: a BC7 texture held as ETC2 is queued at 60 presents and
  no sooner, not at all when a block's alpha moves, encoded 4096 blocks a frame from guest memory into an image of
  its own through the ASTC build, each block where the layout has it, and put in the ETC2 one's place when whole;
  a write drops it, and one of the ASTC image makes it ETC2 again.
- `vulkan_headless` does the same on the device (`tests/vulkan_bcn_later.inc`) where it samples ETC2 and ASTC and
  BCn is encoded again, the image's blocks read back against the CPU's run of the encoder: llvmpipe samples
  neither, so the CI skips it; `snx-test-vulkan -H --native_bc=0` runs it on a GPU of all three.
- `astc_vulkan` runs the SPIR-V: random blocks of every kind against the CPU's run, byte for byte (the ETC2 build
  and the ASTC build), and, where the device samples ETC2 or ASTC, the device's own decoding of them against
  `etc2.c` and `astc_cpu.c`. llvmpipe samples neither, so on the CI those are skipped; a phone runs them, and an
  Intel GPU.
- `vk_block_formats` runs `vk_blocks_want`, `vk_tic_format`, `vk_bcn_astc_later` and `vk_blocks_say` on stand-in
  devices, the first a player's Mali-G52 MC2 from its vkjson (ASTC and ETC2 sampled, BCn not).
