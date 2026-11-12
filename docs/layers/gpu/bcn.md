# BCn without BCn: ETC2 and EAC on phone GPUs

## Index

* [The encoder](#the-encoder)
* [As ASTC](#as-astc)
* [What it costs](#what-it-costs)
* [Tests](#tests)

Most games' textures are BC1 to BC7. Desktop GPUs sample them as they are; most phone GPUs (Mali, Adreno with
Qualcomm's own driver, PowerVR) sample ETC2, EAC and ASTC instead, and not BCn. There ScarletNX encodes each BCn level
again on the GPU as it loads, into the ETC2 or EAC format of the same bytes a texel, where decoding it would take four
to eight times the memory.

| BCn | where the GPU samples ETC2 | bytes a texel | decoded (no ETC2 either) | bytes a texel |
| --- | --- | --- | --- | --- |
| BC1 | ASTC 4x4 (1 byte a texel) while memory lasts, then ETC2 RGB8A1 (punch-through alpha: BC1's transparent black; `bcn_astc`, [below](#as-astc)) | 0.5 | RGBA8 | 4 |
| BC2, BC3, BC7 | ASTC 4x4 where the GPU samples ASTC (`bcn_astc`, [below](#as-astc)), else ETC2 RGBA8 (an EAC alpha block, then an ETC2 RGB one) | 1 | RGBA8 | 4 |
| BC4 | EAC R11, unsigned or signed | 0.5 | R16 | 2 |
| BC5 | EAC RG11, unsigned or signed | 1 | RG16 | 4 |
| BC6H | none (ETC2 has no HDR): decoded | 8 | RGBA16F | 8 |

`format.c` picks it (`TB`), from the features `vk_blocks_want` turns on: `textureCompressionBC` and the `native_bc`
setting for BCn as it is, else `textureCompressionETC2` with `native_etc2` for ETC2. A 3D BCn texture is decoded
where the device makes no 3D image of the ETC2 format. As the device starts, the log says how each family is held,
from the formats `vk_tic_format` picks (`vk_blocks_say`), on a Mali-G52 for instance:

```
vulkan: ASTC textures (native_astc=1, the GPU samples ASTC, astc_mode=0): sampled as the game's blocks: ASTC
vulkan: ETC2 and EAC textures (native_etc2=1, the GPU samples ETC2): sampled as the game's blocks: ETC2 RGB, ...
vulkan: BCn textures (native_bc=1, the GPU samples no BCn, ETC2 sampled): encoded again on the GPU: BC1 as ETC2
RGB8A1, BC2 as ETC2 RGBA8, BC3 as ETC2 RGBA8, BC4 as EAC R11, BC5 as EAC RG11, BC7 as ETC2 RGBA8; decoded on the
GPU: BC6H to RGBA16F
```

## The encoder

`texconv.comp` built a second time with `SNX_TC_ETC2`, its own pipeline (`vk/texconv.c`), runs a block an
invocation: `texconv.glsl`'s BCn decoders, then `gpu/texture/etc2_encode.glsl`. The texels go through
`texture.c`'s uploads as the decoded ones did: the dispatch writes rows of blocks, and the copy into the image counts
them as rows of texels four times as wide.

A submission holds 16384 blocks of it at most (`TC_ENCODE_SUBMIT_BLOCKS` in `vk/texconv.c`): a level's conversion
goes in pieces of whole rows of blocks, and a piece that would take the submission past that goes in the next one
(`encode_rows` in `vk/texture.c`, `vk_submit_split`). A phone's kernel driver ends a submission that runs past its
timeout, 2 s in kgsl, and its fence then never signals. An Adreno 610 (stock driver 512.502.0) takes 27 us a block: a
2048x2048 level is 7.1 s of it, which the kernel ended as one dispatch and as 16 dispatches in one submission, and
which came out whole in 16 submissions of 0.44 to 0.48 s each.

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

## As ASTC

ETC2 RGBA8 holds less than BC7 does (below: 5.5 to 11 dB lost against the picture), and the GPUs that sample ETC2 and
not BCn sample ASTC too, whose 4x4 blocks are the same bytes a texel. With `bcn_astc=1`, the default, BC2, BC3 and BC7
are encoded again as ASTC 4x4 there: `texconv.comp` built a third time with `SNX_TC_ASTC`, the BCn decoders followed
by `gpu/texture/astc_encode.glsl`, a pipeline of its own (`vk/texconv.c`). BC4 and BC5 keep EAC.

BC1 is half ASTC 4x4's bytes, and the ASTC encoder is several times the ETC2 one's speed, so BC1 is tiered: ASTC 4x4
while memory lasts, for the loads' sake, and ETC2 RGB8A1 once it runs short. Short is what the trim finds
(`surface.c`: an image that found no memory, the caches past their budget after a trim, or a trim that took what the
last eight presents used); `vk_bc1_tight` then sets `g_vk.bc1_tight` for the rest of the run and the log says so once.
A texture keeps the format it was made with, so nothing is encoded twice: the BC1 textures made from then on are ETC2,
and ETC2 takes ASTC's place as the trim evicts textures and the game loads them again. `bcn_astc=2` keeps BC1 as ASTC
whatever the memory, and `0` sends all four to ETC2. No texel is written out between the decoder and the encoder: an invocation decodes a
block and encodes it.

The fitting is the BC7 encoder's (`bc7_encode.glsl` built with `SNX_ASTC_ENC`, [ASTC compute
uploads](astc.md#encoded-again-as-bc7)): ASTC interpolates as BC7 does, `((64 - w) a + w b + 32) >> 6`, with the same
weights of 2 and 3 bits, so a subset's line is fitted by the same code, packed bytes and all. What differs is what a
block's 128 bits buy, which is what the candidates are:

- a block of one colour is a void extent, the colour in 16 bits a channel: exact;
- one partition with a 4x4 grid of weights: an opaque block as RGB ends with 4 bit weights (ends of 192 values) or 3
  bit ones (8 bit ends); any other as RGBA ends with 3 bit weights (192 values), 4 bit (48) or 2 bit (8 bit ends);
- two planes, alpha with weights of its own (2 bit weights each, ends of 48 values), what BC7's modes 4 and 5 hold;
- two partitions of an opaque block (2 bit weights, ends of 40 values): of 64 of ASTC's partition seeds, those
  nearest BC7's 64 shapes (30 are the same), the one whose subsets spread least.

How many values an end's channel takes is not in the block: the decoder works it out from the bits the header and the
weights leave, as the largest encoding that fits. So each layout has exactly one, and the ends are quantized to it
while they are fitted. The encodings with a trit or a quint come from tables that `tools/astc_tables.py` makes by
turning `astc_decode.glsl`'s own rules round (the bits of five trits and of three quints, the nearest value an
encoding holds and its code, the partition seeds), into `astc_encode_tables.glsl`.

**The source's own lines.** A BCn block is lines already, so the encoder is told them (`as_hint`, set by
`texconv.glsl` as it decodes the block) and tries them before any search:

| Source | Taken as |
| --- | --- |
| BC7 mode 6 | one line: an opaque block's as RGB ends with 4 bit weights, any other's as RGBA ends with 3 bit weights and with 4, the better kept |
| BC7 modes 4 and 5 | two planes, the rotated channel apart; an opaque block's with RGB ends (160 values an end, where RGBA ends have 48); with alpha apart and every alpha 255, one line as RGB ends with the source's 2 or 3 bit weights and 8 bit ends |
| BC7 modes 1 and 3 | two partitions, BC7's own partition number (`ASTC_P2` is in BC7's order), each subset's line the source's |
| BC2, BC3 | two planes, alpha apart, the colour's line BC1's two ends; opaque, one line as RGB ends with 2 bit weights and 8 bit ends, which hold BC1's ends and weights as they are |
| BC1 | that one line; a block of three colours, whose third is halfway, with 3 bit weights; one with a transparent texel (black, off the line) nothing |
| BC7 modes 0, 2 and 7 | nothing: three subsets, and two with alpha, have no layout here |

A hinted line is taken as it is: its ends quantized to the layout's colour encoding and each texel given the weight
nearest its place, one pass of the texels where the search fits a box and least squares over several. Its error is
still measured on the texels, so a hint the layout holds badly (ASTC's partition is not always BC7's, and two
partitions leave an end 40 values) costs time and no more: past `SNX_ASTC_HINT_ENOUGH` (a squared error of 256 a
block, the BC7 encoder's fast build's line too) the block is searched for, and the search takes the hint as well, the
source's partition fitted in place of 64 scored, and no partition at all for a source of one line.

**No texel decoded.** Some BCn blocks are an ASTC layout already, field for field, and those are moved, not encoded
(`gpu/texture/astc_direct.glsl`): the ends are read out of the block and written in ASTC's encoding, and the weights
are moved as the 32 or 64 bits they are, with the bit BC7 leaves out of its anchor index put back and BC1's indices
put in ASTC's order by two masks. Nothing is fitted and nothing measured.

| Source | Moved as | Off by, a channel |
| --- | --- | --- |
| BC7 mode 6, opaque | RGB ends of 192 values, the same 16 indices of 4 bits | 1.0 on average on random blocks, 7 at most: ASTC's 4 bit weights are BC7's within 1 of 64 |
| BC7 mode 6 with alpha | RGBA ends: of 48 values with the 4 bit indices where the ends lie more than 40 apart, else of 192 with 3 bit weights, an index's top three bits (the nearest 3 bit weight for all 16) | within the same 7 |
| BC7 modes 4 and 5, one alpha, no rotation | the colour's line with its own 2 or 3 bit weights, 8 bit ends | 0: the weights are BC7's and the ends fit |
| BC7 mode 5, a rotation onto a channel 255 at both ends | two planes of an opaque block, RGB ends of 160 values, both streams of 2 bit indices | an end's step |
| BC1 of four colours, alone or under BC2's or BC3's alpha where it is 255 throughout | RGB ends of 8 bits, BC1's own, 2 bit weights | 0.28 on average, 4 at most: BC1's thirds as the Switch's GPU rounds them against ASTC's 21 and 43 of 64 |
| any of them with one colour (ends alike, or every index alike) | a void extent | 0 |

**Three passes** (`SNX_ASTC_PASS`, `texconv.glsl`'s ASTC main), as BC7's are made in two ([ASTC compute
uploads](astc.md#two-passes)): a GPU runs invocations abreast at the pace of the slowest, so each way of making a
block is a dispatch of its own. With hints and the search in one dispatch the hints made a GeForce GT 1030 slower, a
third of the operations though they are.

1. Over the strip's blocks: those above, moved. The others are listed, by what the block says it is: with lines of its
   own to try (BC7's modes 1, 3, 4 and 5, BC1's blocks of three colours, BC2 and BC3 with alpha), or without.
2. Over the first list: the block decoded and its lines tried (the hints above); what they don't settle joins the
   second list.
3. Over the second list: the block decoded and searched for.

The second and third are indirect dispatches, as many workgroups as their list has blocks (the work lists, a third
buffer). The blocks are the bytes one invocation doing all three would write, which is what the tests run on the CPU.

What it gives, on nine levels of two games' textures as BC7 blocks (the BC7 encoder's fast build of each) made ASTC
(`snx-test-astc-encode-glsl --measure FILE.astc...`): 50.86 dB against what the BC7 blocks decode to. Every block
decoded, its lines tried and measured, gave 51.31, and searched for with no hint 50.29: moving the blocks costs
half a dB of what the hints had gained. Against the textures themselves 47.67 dB, where the BC7 blocks are at 50.55.

Time, on the GT 1030 (which samples neither ASTC nor ETC2, so only the conversions run): a 2048x2048 level, 262,144
blocks, its blocks in device memory and its output left there, the dispatches alone
(`snx-test-astc-vulkan --bench-etc2 FILE.astc`; read back over the bus, as that bench did at first, the output's 4 MB
took another 10 ms and hid all of this):

| Level, as BC7 blocks | Decoded to RGBA8 | As ETC2 RGBA8 | As ASTC 4x4 |
| --- | --- | --- | --- |
| 8x8, smooth: every block mode 6 | 1.9 ms | 96 ms | 0.6 ms |
| 8x6, mostly one colour | 2.2 ms | 78 ms | 1.2 ms |
| 6x6 | 2.8 ms | 150 ms | 2.6 ms |
| 4x4, with alpha | 2.3 ms | 134 ms | 2.7 ms |
| 8x5 | 2.4 ms | 135 ms | 6.1 ms |
| 8x8, 18% hard blocks | 4.9 ms | 167 ms | 6.2 ms |

Random blocks, which nearly all take the search: BC1 28 ms (ETC2 172), BC3 44 ms (186), BC7 90 ms (198).

What it does not have yet: three and four partitions, two partitions with alpha, and blocks moved for BC7's modes 1
and 3 (two partitions of 2 bit weights are ASTC's too, where its partition is BC7's, but their ends have 40 values a
channel), for mode 4 with a rotation and for BC1 with a transparent texel.

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

BC7 loses most: ETC2 RGBA8 holds less than BC7's partitions and 4 bit weights do, which is why BC2, BC3 and BC7 go to
ASTC 4x4 where the GPU samples it ([As ASTC](#as-astc)).

Time, `snx-test-astc-vulkan --bench-etc2` on llvmpipe (a 12-core x86 CPU, other work running), a 2048x2048 level of
random blocks, the dispatch alone: BC1 7 ms decoded, 105 ms encoded again; BC3 9 and 95; BC4 5 and 35; BC5 10 and 65;
BC7 26 and 117. Random blocks take every candidate; a game's blocks of one colour or one alpha stop earlier. The
pipeline is compiled the first time a BCn texture loads (1.3 s on llvmpipe), and kept in the pipeline cache.

## Tests

- `bcn_etc2_glsl` runs the encoder on the CPU, its blocks decoded by `tests/support/etc2.c`: every candidate stores the
  mode it means, at the error the encoder worked out; the block written is the best candidate tried; a block of one
  4 bit colour, a planar gradient, an EAC block of every table and an alpha the same throughout come back exact, and
  1600 of 2000 random ETC1 blocks at least (744 with each subblock's base its mean alone) and 400 of 500 gradients
  the decoder clamps (241 with one walk of the planar levels around the fit); BC1's transparent texels
  stay transparent; images whose sides aren't multiples of 4, of several slices, transcode block for block as one
  block alone; and each kind holds the PSNR it reached on a picture of gradients, edges, noise and alpha.
- `tex_upload_rows`: an encoder's level in pieces under a small bound, block linear, pitch linear and a volume, no
  submission holding more, each piece recorded into the command buffer recording then, every block where the image
  has it.
- `astc_vulkan` runs the SPIR-V: random blocks of every kind against the CPU's run, byte for byte, whole and in pieces
  of 3 rows a submission, and, where the
  device samples ETC2, the device's own decoding of them against `etc2.c`. llvmpipe samples no ETC2, so on the CI the
  second is skipped; a phone runs it.
- `astc_encode_glsl` (no GPU) runs the ASTC encoder on the CPU and decodes its blocks with the CPU's ASTC decoder
  (`tests/support/astc.c`): 6000 blocks of noise, edges, gradients, single colours and alpha each decode to the squared
  error the encoder worked out (so each layout's bits, colour encoding and weights are where the decoder reads them),
  none is a block the decoder refuses, every layout is written, and a picture holds its PSNR. `astc_vulkan` holds the
  SPIR-V of the ASTC build, its two passes, to the CPU's run of it in one (`tests/glsl/bcn_astc_cpu.cpp`, hints and
  all) on random BC2, BC3 and BC7 blocks, byte for byte.
- `vk_block_formats` runs `vk_blocks_want`, `vk_tic_format` and `vk_blocks_say` on stand-in devices, the first a
  player's Mali-G52 MC2 from its vkjson (ASTC and ETC2 sampled, BCn not).
