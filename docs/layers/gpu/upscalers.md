<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# Upscalers

## Index

* [Who calls it](#who-calls-it)
* [The interface](#the-interface)
  * [Capabilities](#capabilities)
  * [The choice and the fallback rule](#the-choice-and-the-fallback-rule)
  * [Contexts and evaluations](#contexts-and-evaluations)
* [The backends](#the-backends)
* [In the renderer](#in-the-renderer)
* [Tests](#tests)

The runtime upscales a game's rendering in two places: a Switch 2 game's DLSS evaluations, and the upscalers of a
game's own that a hook module replaces ([Hooks, Modules](../../platforms/nx/hooks.md#modules)). Both go through one
interface, `src/nx/gpu/vk/upscaler/upscaler.h`. Its caller gives a picture at a render size with whatever else it has
(depth, motion vectors, exposure, jitter), and gets it at an output size. The caller never names a technology: the
`render_upscaler` setting prefers one, and the caller says which kinds it takes. When no upscaler it takes can run,
it is told so and runs its own fallback.

The presented frame's effects (FXAA, the present-time upscalers of the window) are another thing: they scale the
frame the game finished, after everything here.

## Who calls it

- **A DLSS evaluation** (`nvnCommandBufferEvaluateDlss`, `gpu/nvn/nvn_dlss.c`): one `OP_DLSS` record, which the replay
  gives to the renderer's `upscale` (`gpu/nvn/nvn_copy.c`). It takes a temporal upscaler only. Its fallback is the
  front end's filtered copy, which unjitters to the nearest output pixel.
- **A hook module's recorded upscale** (`record_upscale` of a CPU hook's handler, [Hooks, CPU hooks in
  libraries](../../platforms/nx/hooks.md#cpu-hooks-in-libraries)): one `OP_UPSCALE` record in the game's command
  buffer, which the replay gives to the same `upscale` (`gpu/nvn/nvn_copy.c`) with `GPU_UPSCALE_MODULE`. It says which
  kinds it takes besides a temporal one (`GPU_UPSCALE_SPATIAL_OK`, `GPU_UPSCALE_COPY_OK`). Its fallback is the front
  end's filtered copy of the color onto the output, and the log names what ran.

`render_upscaler=3` (None) turns the DLSS evaluations to that copy, 1:1 when the game renders at the output's size.
With `render_anti_aliasing=1` or `jitter=1` the interface's copy runs in its place, at any resolution scale, after its passes
before the upscale: the picture copied into images at the render size (taken back by the caller's jitter for
`jitter=1`), then SMAA there (`up_smaa.comp`, the reference shader with its area and search textures): 1x, or T2x (`render_anti_aliasing=2`),
whose velocity pass turns the caller's motion vectors into SMAA's and whose resolve blends the frame with the one before
(an image of the context's, the two results taking turns, the history dropped at the caller's reset, at a new context
and after a frame without T2x). T2x takes each frame back by the game's own jitter first (the caller gives it as the
shift, as for `jitter=1`), so both frames of the resolve sit still and the result doesn't shake; motion vectors that
include the jitter are corrected by the shift's change. It uses the area texture's 1x lookups, not SMAA's two
positions. Its presets (`smaa_quality`) are push constants, Low and Medium a build of the weights pass without the
diagonal and corner searches, and its edges (`smaa_edges`) luma, color or the caller's depth. SMAA S2x and 4x are left
out: they take the game's frame drawn with multisampling. A copy or a spatial upscale that a hook module records takes
them too, as 1x without motion vectors, a temporal upscaler never.
A module that takes a copy gets the interface's filtered copy, and one that takes none gets the best upscaler that can
run. The size the game renders at before the upscale is `internal_resolution`'s, a setting apart from the resolution
scale ([Settings, Graphics](../../user/settings.md#graphics)).

## The interface

Like `gpu/vk/fsr2/fsr2.h`, the interface knows nothing of the renderer, the settings or the log. The caller gives the
device, the images in the layouts said and the command buffer, and each backend records plain `vkCmd*` calls into it.
What can't run comes back as a reason in words, which the caller logs. It is the library `snx_upscaler`, which
`tests/vk_hook_upscale.c` links without the renderer.

### Capabilities

Each backend has a `SnxUpscalerCaps` (`snx_upscaler_caps`):

| Field | Says |
| --- | --- |
| `kind` | temporal (it keeps a history), spatial (one frame) or the copy |
| `needs` | the inputs it can't run without: depth, motion vectors |
| `takes` | the inputs it uses when given: jitter, exposure, a reactive mask |
| `range` | the color it takes: display-referred (LDR), linear and unbounded (HDR), or both |
| `scale_min`, `scale_max` | the output's size over the render size, on each axis |
| `sharpens` | whether the sharpness does something |
| `filtered` | whether it samples the color with a linear filter, so that the color's format must filter (every backend stores its output, whose format must be one the device stores) |

`snx_upscaler_available` says whether a backend is built in and the device can run it, and why not.

### The choice and the fallback rule

`snx_upscaler_choose` takes the preferred backend, the device and what the call has (`SnxUpscaleNeeds`: the inputs
given, the kinds the caller takes, the color's range, both sizes, and the color's and the output's formats). It tries the preferred one, then Best's order:
DLSS, newer FSR, XeSS, FSR 2, the spatial upscaler, the copy. The first that passes every test runs:

1. the caller takes its kind
2. it is available
3. the call gives every input it needs
4. it takes the color's range
5. the device stores the output's format, and filters the color's where the backend filters the color (the copy
   and the spatial upscaler sample it with a linear filter)
6. both scales are within its range

When the preferred one is passed over, the reason comes back with the one that stands in, for the log. When none
passes, the call gets no backend and the reason of the first one it would take that is built in, and runs its own
fallback. So a DLSS evaluation with `render_upscaler` set to the spatial upscaler still gets FSR 2: it takes temporal
upscalers only. A module that gives no motion vectors gets the spatial upscaler under Best, when it takes one, and the
filtered copy when it doesn't, unless it gives depth and a reprojection matrix (hook ABI 1.2,
[Hooks](../../platforms/nx/hooks.md#modules)): then `gpu/vk/upscaler/reproject.c` makes the camera's motion vectors
from the depth before the upscaler runs, and a temporal one takes the frame.

### Contexts and evaluations

`snx_upscaler_create` makes a context of one backend for the largest render size it will be given, the output size,
the color's and the output's formats and the conventions (`SNX_UP_FLAG_*`, the bits of `fsr2.h`: HDR color, motion vectors at the
output's size, motion vectors that include the jitter, inverted depth, the backend's own exposure).
`snx_upscaler_evaluate` records one evaluation (`SnxUpscaleEval`):

- the color, and the depth, motion vectors, exposure and reactive mask that are given, in
  `VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL`. The output is in `VK_IMAGE_LAYOUT_GENERAL` with a storage view, and is
  written whole at the context's output size
- the render size of this evaluation, up to the context's, from the images' corner
- the jitter in render pixels (the picture moved by +jitter), the motion vectors' scale (to pixels of their own size,
  each pointing to where its pixel was the frame before), the pre-exposure, the frame time
- the camera's planes and vertical field of view, which only weigh depth comparisons: 0 takes 0.1, 1000 and 1 radian
- the reset of the history, and the sharpness, 0 (no sharpening pass) to 1.

A context goes round its descriptors over `SNX_UPSCALER_IN_FLIGHT` (4) evaluations whose commands haven't finished.
`snx_upscaler_destroy` needs the device done with them.

## The backends

| Backend | Kind | Needs | Color | Scale | Built from |
| --- | --- | --- | --- | --- | --- |
| FSR 2.2.1 | temporal | depth, motion vectors | LDR, HDR | any | AMD FidelityFX Super Resolution 2.2.1 (MIT), `gpu/vk/fsr2` |
| FSR 1 (EASU, RCAS) | spatial | none | LDR | 1 to 4 | FSR 1's `ffx_fsr1.h` from the FSR 2.2.1 archive (MIT), `up_easu.comp` and `up_rcas.comp` |
| filtered copy | copy | none | LDR, HDR | 1/8 to 16 | `up_copy.comp`: a bilinear sample at each output pixel's centre, kept half a texel inside the picture |
| DLSS | temporal | depth, motion vectors | LDR, HDR | 1 to 3 | NVIDIA DLSS Super Resolution through NVIDIA Streamline (`dlss.cpp`): Streamline's MIT headers in a Windows build, NVIDIA's runtime from the user's `dlss_runtime` folder ([DLSS](../../user/dlss.md)), available once Streamline took the device |
| FSR 3.1 or 4 | temporal | depth, motion vectors | LDR, HDR | 1 to 3 | not built in: AMD's FidelityFX API runtime |
| XeSS | temporal | depth, motion vectors | LDR, HDR | 1 to 3 | not built in: Intel's XeSS runtime |

- DLSS goes through Streamline's manual hooking: `device.c` starts it before the instance (only with `dlss_runtime`
  set and `render_upscaler` Best or DLSS), adds the extensions and features it asks for when the device has them all,
  hands it the device, and calls the swapchain through Streamline's own functions so its present runs every frame.
  Anything missing stops it, and the choice goes on without it. An evaluation records Streamline's options,
  constants and tags (`eValidUntilEvaluate`) into the frame's command buffer on the recording thread. A failure there
  makes DLSS unavailable from then on, so the next evaluation chooses FSR 2. The mode follows the scale, the motion
  vectors' scale is divided by the render size (Streamline's -1 to 1), and the jitter goes as the game gives it.
- FSR 2's range refuses nothing. Its quality modes go from a scale of 1 to 3 and a dynamic resolution further, and
  every DLSS evaluation it took before the choice checked a range stays its own.
- The spatial upscaler is EASU into an image of its own at the output's size, then RCAS into the output when the
  sharpness is above 0, or EASU straight into the output when it is 0. RCAS takes the sharpness as FSR 2 does, 2 - 2s
  stops below the strongest. FSR 1 is made for display-referred color, so it takes no HDR.
- FSR 2's archive carries FSR 1's header, which its own sharpening pass includes. FSR 2 never compiles EASU from it,
  and EASU's float path stores a comparison in an integer, which GLSL refuses:
  `.patch/fidelityfx-fsr2/0002-fsr1-easu-glsl.patch` makes that one variable a boolean in the package (`cpmfile.json`'s
  patches). FSR 2's own passes don't change.
- The three compute passes share one layout: the picture sampled at binding 0, the result stored at binding 1 (a
  storage image without a format, which every built-in backend needs), and the sizes and sharpness as push constants.
  They are compiled from GLSL at build time (`cmake/glsl.cmake`, its `INCLUDES` for FSR's headers).
- The stubs describe the backends so that the setting and the choice know them, and say why each is not available.
  Each would be a backend file and a case in `upscaler.c`, with the runtime loaded at run time and only MIT headers in
  the tree.

## In the renderer

`gpu/vk/upscale.c` is the renderer's `upscale`. It takes the game's color, depth and motion vectors where they are
drawn (render targets the renderer holds), makes the context the choice names, records the evaluation into an image
of its own (the output's format without its sRGB transfer, as a storage image) on the recording thread, and copies
that onto the game's output target.

- One context at a time. It takes any render size up to the one it was made for, so a game's dynamic resolution
  doesn't make another. Another backend, output size, output format or convention makes a new one, after the device is
  done with the old one's commands. The color's format doesn't: the choice checks it at each call, and no backend
  keeps it. A setup that failed is not tried again until one of those changes.
- The color's range is the caller's when it says (`GPU_UPSCALE_RANGE_GIVEN`), else its format's: a float format is
  HDR.
- At a resolution multiplier other than 1 (the `resolution` setting, or a hook module's from the internal resolution
  target) the render targets are the guest's sizes times it, and so are the call's render and output sizes, its
  output corner, its jitter and its motion vector scale, which count render pixels. Anything it can't take returns 0,
  and the caller's fallback runs, the reason in the log once.
- The debug setting `fsr_input_conventions` flips the conventions of every call, to find a game's.

## Tests

- `vk_fsr2` runs FSR 2 through `fsr2.h` directly: a still scene drawn with a new jitter each frame comes out nearer
  the scene than a bilinear upscale, and only with the jitter's sign right.
- `vk_hook_upscale` runs the interface on a real device with no renderer, as the replay of a module's recorded upscale
  asks for it: the choice and the fallback rule (an unfilterable color included), the stubs not available, the spatial
  upscaler (with and without RCAS), the copy matching a bilinear upscale, none for FSR 2 without depth, and FSR 2
  through the interface converging as `vk_fsr2` measures it (0.0135 against the scene on a Radeon 890M, 0.0134 on
  llvmpipe, as through `fsr2.h`). `nvn_copy_formats` checks the record's replay and its filtered copy.
- Both are skipped (77) without a Vulkan device that writes storage images without a format.
