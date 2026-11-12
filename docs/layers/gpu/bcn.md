# BCn without BCn: ETC2 and EAC on phone GPUs

## Index

* [The encoder](#the-encoder)
* [What it costs](#what-it-costs)
* [Tests](#tests)

Most games' textures are BC1 to BC7. Desktop GPUs sample them as they are; most phone GPUs (Mali, Adreno with
Qualcomm's own driver, PowerVR) sample ETC2, EAC and ASTC instead, and not BCn. There ScarletNX encodes each BCn level
again on the GPU as it loads, into the ETC2 or EAC format of the same bytes a texel, where decoding it would take four
to eight times the memory.

| BCn | where the GPU samples ETC2 | bytes a texel | decoded (no ETC2 either) | bytes a texel |
| --- | --- | --- | --- | --- |
| BC1 | ETC2 RGB8A1 (punch-through alpha: BC1's transparent black) | 0.5 | RGBA8 | 4 |
| BC2, BC3, BC7 | ETC2 RGBA8 (an EAC alpha block, then an ETC2 RGB one) | 1 | RGBA8 | 4 |
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

BC7 loses most: ETC2 RGBA8 holds less than BC7's partitions and 4 bit weights do. ASTC 4x4, the same bytes a texel
and sampled by the same GPUs, would hold more with an encoder of its own, which isn't written yet.

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
- `vk_block_formats` runs `vk_blocks_want`, `vk_tic_format` and `vk_blocks_say` on stand-in devices, the first a
  player's Mali-G52 MC2 from its vkjson (ASTC and ETC2 sampled, BCn not).
