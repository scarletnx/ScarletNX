# BCn without BCn: ETC2 and EAC on phone GPUs

## Index

* [The encoder](#the-encoder)
  * [What a texel costs](#what-a-texel-costs)
* [As ASTC](#as-astc)
* [By vendor](#by-vendor)
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

### What a texel costs

The formats are Strom and Akenine-Moller's iPACKMAN, ETC1 ("iPACKMAN: High-Quality, Low-Complexity Texture
Compression for Mobile Phones", Graphics Hardware 2005), and Strom and Pettersson's ETC2 ("ETC2: Texture Compression
using Invalid Combinations", Graphics Hardware 2007), whose T and H blocks start from the block's two colour clusters,
as `et_cluster`'s do.

The candidates are searched as above, and each step below writes the same blocks, byte for byte, as the search written
out plainly did (a checksum of 20000 blocks of every candidate and of the encoder's choice holds them to it):

- A texel takes the nearest of four colours (ETC1's base and its table's modifiers, or T's and H's colours), which are
  the same for each texel: they are clamped as the decoder clamps them once for the base and table (`et1_setup`) or
  the distance (`et_th_word`), not once a texel and index. A texel p's squared distance from a colour c is |p|^2 +
  |c|^2 - 2 p.c, and |p|^2 is the same for the four, so the nearest is the least of |c|^2 - 2 p.c: three multiply-adds
  a colour, with no branch (`et_near4`). An index and its error share a word, error above, so the first of the least is
  the minimum of four words.
- Where the GPU says its packed 4x8 bit dot product is accelerated (`g_vk.bc7_dot`: Pascal's DP4A, Valhall's and
  Adreno's from a6xx's third generation) the build `texconv_etc2_dot_spv` makes each p.c one instruction.
- A colour of a base and a modifier clamps only at 255 where the modifier is not negative, and only at 0 where it is;
  the planar mode's texel (x, y) is ((4 - x - y) eo + x eh + y ev + 2) >> 2, which needs no clamp where x + y <= 4,
  13 of the 16 texels. ucas's `simplify` on the build's SPIR-V found these, and ucas's tests `etc2planar.ucas` and
  `etc1index.ucas` hold the proofs.
- EAC works out a candidate's eight values (table, multiplier and base, clamped) once, and a texel's index is the
  minimum of eight words of its distance and index.
- The planar mode's search stops once the channels done reach the best candidate's error, and each pass after the first
  tries only the levels the one before it didn't.
- A subblock's loops walk its 8 texels, not the block's 16.

What was measured and not kept: the early compression-mode decision of Nah's QuickETC2 ("QuickETC2: Fast ETC2
Texture Compression using Luma Differences", ACM Transactions on Graphics 39, 6, 2020,
[doi:10.1145/3414685.3417787](https://doi.org/10.1145/3414685.3417787)), which tries
T and H only where a block's texels spread widely and the planar search only where its fit is good, makes the
encoder slower on a GPU. Four Kodak crops made BC7 took 93 to 128 ms on a GT 1030 with it and 85 to 106 without
(before the dot build), for 0.02 to 0.18 dB less. A subgroup runs the candidates any of its invocations takes, and
a picture's 32 neighbouring blocks seldom all skip one.

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
while they are fitted. The encodings with a trit or a quint are worked out, with no table (`astc_quant.glsl`): an
encoding's values come in two halves that mirror each other, and in a half the value of rank r is a multiply and a
shift (the decoder's own rule), while the rank of the value nearest a colour is one more multiply and shift, with
constants found by search over every colour. The bits five trits or three quints share are the decoder's rule turned
round, a few selects. A table of 768 words read at a colour is a cache line a read, and an array a shader indexes is
memory mobile GPUs are slow with; the arithmetic costs the search pass about 6% on a GT 1030 and the pass most blocks
take nothing. `tests/glsl/astc_encode_glsl.cpp` holds every answer to the decoder's rules. The one table left is the
partition seeds' (`astc_encode_tables.glsl`, from `tools/astc_tables.py`), 64 words that only blocks of two
partitions read.

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
partitions leave an end 40 values) costs time and little else: past `SNX_ASTC_HINT_ENOUGH` (a squared error of 1024
a block, 4 a channel as a root mean square) the block is searched for, and the search takes the hint as well, the
source's partition fitted in place of 64 scored, and no partition at all for a source of one line. The line was 256
at first, the BC7 encoder's fast build's: the blocks searched for were then 6% of one game's level and nearly all of
its time, and what the search finds for them is seldom much nearer. At 1024 that level takes 3.5 ms where it took
6.2, for 0.27 dB.

**No texel decoded.** Some BCn blocks are an ASTC layout already, field for field, and those are moved, not encoded
(`gpu/texture/astc_direct.glsl`): the ends are read out of the block and written in ASTC's encoding, and the weights
are moved as the 32 or 64 bits they are, with the bit BC7 leaves out of its anchor index put back and BC1's indices
put in ASTC's order by two masks. Nothing is fitted and nothing measured.

This pass has no arrays and reads no table. GPU drivers each have a kind of memory they are slow with: an array of
the invocation's own indexed by a value (scratch memory on Adreno and Mali), or the workgroup's shared memory (fewer
workgroups in flight on NVIDIA). So the block is its four words in registers, a field is cut out of them at a bit
that is a constant of its mode, an end is the word of its four channels, the ends' codes are two words of a byte
each, and the ASTC block is four words put together with shifts. The compiled pass declares no array but the work
list's head.

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
   own to try (BC7's modes 1, 3, 4 and 5, BC1's blocks of three colours with no transparent texel, BC2 and BC3 with
   alpha), or without.
2. Over the first list: the block decoded and its lines tried (the hints above); what they don't settle joins the
   second list.
3. Over the second list: the block decoded and searched for.

The second and third are indirect dispatches, as many workgroups as their list has blocks (the work lists, a third
buffer). The blocks are the bytes one invocation doing all three would write, which is what the tests run on the CPU.

The second pass keeps no arrays either (`SNX_NO_ARRAYS`), for the same GPUs the first is written for:

- its BC7 blocks are of modes 1, 3, 4 and 5 only, so they are decoded by `bcn_regs.glsl` and not by the decoder of
  every mode: each mode's fields cut at bits that are constants, an end the word of its channels, a stream of indices
  two words with its anchors' missing bits put in, a texel two multiplications;
- the 16 texels are 16 words, written by name and read through `PX`, a tree of selects by the texel's number: the
  loops over the texels are then for the driver's compiler to unroll, which leaves each read the word itself;
- the three tables a block of two subsets needs (BC7's partition and anchor, ASTC's seed for that partition) are read
  with selects between constants (`mux.glsl`), once a block;
- a texel's weight comes from comparisons, with no reciprocal and no floating point: `(q top + 32) >> 6` is how many
  of a few quotients `q = n / dd` reaches, and `q` reaches `Q` where `n` reaches `Q dd`. The same index, and on a
  GT 1030 3% slower than the reciprocal where the BC7 encoder was tried with it, so only this build has it.

Its compiled SPIR-V declares no array but the work lists' head, and executes no floating point for the layouts it
writes. On a GT 1030 it is as fast as with arrays or a little faster for BC7 (a 2048x2048 level of random blocks 3.8
ms where it took 4.2, a game's 2.4 where it took 2.9) and a tenth slower for BC3 (5.5 where it took 5.0); nothing is
measured on a phone yet. It is twice the size, each texel's decode being written out 16 times. The third pass, the
search, keeps its arrays: it is the fitter the BC7 encoder shares.

What it gives, on nine levels of two games' textures as BC7 blocks (the BC7 encoder's fast build of each) made ASTC
(`snx-test-astc-encode-glsl --measure FILE.astc...`): 50.67 dB against what the BC7 blocks decode to. Every block
decoded, its lines tried and held to 256, gave 51.31, and searched for with no hint 50.29: moving the blocks and
the wider line cost two thirds of a dB of what the hints had gained. Against the textures themselves 47.58 dB, where
the BC7 blocks are at 50.55.

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
| 8x5 | 2.4 ms | 135 ms | 3.0 ms |
| 8x8, 18% hard blocks | 4.9 ms | 167 ms | 3.5 ms |

Random blocks, which nearly all take the search: BC1 26 ms (ETC2 172), BC3 36 ms (186), BC7 88 ms (198).

What it does not have yet: three and four partitions, two partitions with alpha, and blocks moved for BC7's modes 1
and 3 (two partitions of 2 bit weights are ASTC's too, where its partition is BC7's, but their ends have 40 values a
channel), for mode 4 with a rotation and for BC1 with a transparent texel.

## By vendor

The second and third passes, which encode, have builds of their own for Mali and for Adreno: `texconv.glsl` includes
`gpu/texture/astc_encode_arm.glsl` or `astc_encode_qcom.glsl` in place of `astc_encode.glsl` (`SNX_ASTC_VENDOR`), and
`vk/texconv.c` runs the one of the device's Vulkan vendor (`vk_astc_encoder`: 0x13B5 Arm, 0x5143 Qualcomm, any other
the generic encoder), whatever its driver. The first pass, which moves blocks and encodes none, is the same for all.
Each vendor's two passes are built again with SPIR-V's packed dot product, which the fitter's inner loop is made of,
and run where the driver says it has an instruction for it (`g_vk.bc7_dot`: PanVK on Valhall, Turnip from a6xx's third
generation); the generic builds have none. The log says which, as the device starts:

```
vulkan: BCn encoded again as ASTC 4x4 by Mali's encoder (gpu/texture/astc_encode_arm.glsl) for vendor 13b5, with packed dot products
```

Both put a block together the same way, the generic `AsPack`'s bytes for any candidate: no 32 bit multiply (three
instructions on Adreno, Mesa's `ir3_nir_imul.py`; Valhall's `IMUL` shares a unit with its shifts), the weights packed
as words a nibble each by halving the gaps between them, the colour values in two registers a run of fields at a
time, the colour encoding's constants once a block, and in the array-free pass the block's own scans of its texels
reading each by its number. Each of those rewrites is proved equal to what it replaces by `ucas` (by SAT, over the
whole domain the callers give). And each candidate goes through the one call there is of the function that tries it
(`AsCandidate`), where the generic encoder tries the source's lines in calls of their own beside the search's: the
compiler inlines every call, and the third pass is a quarter less SPIR-V (Adreno's 255 KB, the generic 326).

- **Adreno's** writes the generic encoder's bytes, block for block.
- **Mali's** searches less: of one partition, only the opaque block's RGB ends with 4 bit weights and the others'
  RGBA ends with 3 and 4 bit weights are tried (the generic encoder also tries 8 bit ends with 3 or 2 bit weights),
  and a block stops at a squared error of 256 where the generic one stops at 16. Its blocks are legal LDR ASTC: Mali's
  decoder gives an error block (an illegal one, an HDR endpoint mode in an UNORM texture) as NaN, which an 8 bit
  target stores as 0 ([#1898](https://github.com/scarletnx/ScarletNX/issues/1898),
  [#2156](https://github.com/scarletnx/ScarletNX/issues/2156)), so none is ever written.

What Mali's search gives up, against what the BCn blocks decode to, decoded by `tests/support/astc.c`:

| Blocks | generic, dB | Mali's, dB | worst block, both (squared error, channel) | texels fitted |
| --- | --- | --- | --- | --- |
| Khronos's LDR sample textures (diffuse, emissive, metal and roughness, normals, occlusion) and two of Amberlith's ASTC pictures, 256x256 each, made BC7 by the CPU's encoder | 47.606 | 47.604 | 36113, 84 | 47% fewer |
| 4096 random BC1 | 23.326 | 23.304 | 189406, 204 | 20% fewer |
| 4096 random BC2, BC3 | 29.273, 32.159 | the same | 10363, 48; 15607, 54 | 21% fewer |
| 4096 random BC7 | 24.121 | 24.121 | 135752, 183 | 34% fewer |

A texel fitted is one `Eval` places at its three nearest weights, the work of the search. What that is in time on a
Mali is not measured: there is no Mali here.

Time on an Adreno 610 (a Poco M3), Turnip T30 (Mesa 26.3, Vulkan 1.3.359), its library loaded through Android's
driver HAL from a copy of the app's package: the three passes by the GPU's timestamps, the medians of 5 runs in each
of 2 rounds, every block the CPU's run of the same encoder. The phone's own driver (Vulkan 1.1, which ScarletNX does not take)
refuses every build's second and third passes ([#2220](https://github.com/scarletnx/ScarletNX/issues/2220)).

| Blocks | generic | Adreno's | Mali's |
| --- | --- | --- | --- |
| 16384 BC7 of Khronos's textures (diffuse, metal and roughness, normals, occlusion) | 17.0 to 19.5 ms | 12.1 to 12.2 ms | 10.9 to 11.0 ms |
| 4096 random BC7 | 134 to 139 ms | 32.9 to 33.2 ms | 26.5 to 26.9 ms |
| 4096 random BC3 | 126 to 127 ms | 30.9 to 31.3 ms | 26.6 ms |
| the third pass's pipeline made | 17.9 to 18.2 s | 12.2 to 12.4 s | 11.6 s |

Nearly all of it is the third pass, the search (10.5 ms of the generic 19.5 on the textures, 2.9 of Mali's 11.0).

The packed dot product builds are not timed here: the 610, of a6xx's first generation, has no instruction for it.

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

Time, `snx-test-astc-vulkan --bench-etc2` on a GT 1030 (the build with packed dot products), a 2048x2048 level, the
dispatch alone, encoded again as ETC2 or EAC, before and after the steps of [What a texel costs](#what-a-texel-costs):

| Blocks | before | now |
| --- | --- | --- |
| random BC1 | 199 ms | 76 ms |
| random BC3 | 178 ms | 82 ms |
| random BC4 | 55 ms | 11 ms |
| random BC5 | 119 ms | 22 ms |
| random BC7 | 192 ms | 90 ms |
| a Kodak crop made BC7 (`--bench-etc2 FILE.rgba`, 512x512 repeated) | 85 to 106 ms | 57 ms |

The same picture's blocks encoded again as ASTC 4x4 take 5 ms: most of them are moved, not searched for. Random
blocks take every candidate; a game's blocks of one colour or one alpha stop earlier. On llvmpipe, before these steps:
BC1 105 ms, BC3 95, BC4 35, BC5 65, BC7 117. The pipeline is compiled the first time a BCn texture loads (1.3 s on the
GT 1030), and kept in the pipeline cache.

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
  none is a block the decoder refuses, every layout is written, and a picture holds its PSNR. The transcoder's three passes
  as the shader builds each (the second with no arrays and no reciprocal) write the blocks one invocation does, on
  random BC1, BC2, BC3 and BC7 blocks and on a picture's BC7 blocks, most of which the second pass writes.
  `astc_vulkan` holds the SPIR-V of the ASTC build, its three passes, to the CPU's run of it in one (`tests/glsl/bcn_astc_cpu.cpp`, hints and
  all) on random BC2, BC3 and BC7 blocks, byte for byte.
- `vk_block_formats` runs `vk_blocks_want`, `vk_tic_format` and `vk_blocks_say` on stand-in devices, the first a
  player's Mali-G52 MC2 from its vkjson (ASTC and ETC2 sampled, BCn not).
  Each vendor's encoder (`vk_astc_encoder`) and the log's line for it, with and without packed dot products.
- `astc_encode_glsl` holds Adreno's encoder to the generic one's bytes on every set above, both vendors' `AsPack` to
  the generic one's on random candidates of every layout, and Mali's to the generic one's quality (each block at the
  error it worked out, a tenth of a dB on each set, its worst block's largest channel error 4 more at most); each
  vendor's three passes write what its one invocation does. `astc_vulkan` runs each vendor's builds on the device,
  the vendor ID set to Arm's and Qualcomm's in turn, against the CPU's run of that vendor's encoder, and checks the
  passes are that vendor's builds; `astc_vulkan_encode_dot` the same with packed dot products.
