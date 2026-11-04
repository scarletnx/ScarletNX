# Frame interpolation: GBA now, the GPU frontends next

## What is done

`frame_interp` and `frame_interp_key` (Ctrl+Shift+F) exist. The GBA runner reads them, and `auto` or `blend` presents
a byte average of the previous and current 240x160 frame before the current frame. `auto` is the shared gate
(`common/gpu/vk/interp_auto.c`): over the last four seconds it takes the game's fastest 1 % of frames (the display must
refresh at least 1.9 times that, so two presents fit even then) and its slowest 1 % (within twice the fastest, so a
game that hitches every few frames is left alone), turns on after a second of fitting and off only after two seconds of
failing. On a 60 Hz display with the GBA's 59.73 Hz, Auto stays Off. A 120 Hz display runs it. The hotkey cycles Off,
Auto and Blend with a log line, and the `.ini` is not changed.

## What is not done

The 3DS, Vita and Switch presenters do not present the intermediate frame yet. Their frames are GPU images, and
the rules keep them on the GPU. The next step is the same in each:

1. Keep the previous presented source image (after the spatial upscaler and anti aliasing, or the HDR composition).
2. `snx_fx_blend(prev, current, 0.5)` draws the intermediate on the GPU.
3. Present the intermediate through the existing HDR/blit path, then the current frame. Acquire two swapchain images
   and present both, or use two submission slots.
4. `auto` uses `snx_fx_interp_auto(display_hz, measured_game_hz)`, with the game rate measured from the present
   interval, not a fixed 60.

The blend is a temporal average. It is not Dusklight's state interpolation: Dusklight is a recomp of a game whose
object and camera transforms it can read, and it renders the world at a step between two simulation ticks. An
emulator's presented frame has no such transforms unless a frontend records them from the guest's draw state. For the
Switch and Vita GPU paths the only general option is image domain interpolation. A later motion compensated pass
(coarse block matching or a small flow network) should replace the blend for both, because a plain blend shows
ghosting on fast motion.

## Where

- `src/common/gpu/vk/present_fx.c`: `snx_fx_interp_*`.
- `src/agb/runtime/host.c`: the GBA CPU blend and the `auto` gate.
- `src/3ds/gpu/present.c`, `src/vita/gpu/vk.c`, `src/nx/gpu/vk/renderer.c`: the presenters that still need the two
  image path.

## Rate and naming

It presents **one** intermediate frame between each two game frames, so the shown rate is 2x the game's, never 3x or
4x: two swapchain images are acquired per game frame and no more. A mode that generated more would need more images
and a display with that much room.

The mode is named `blend` because that is the technique, not just the state. If a motion compensated pass lands it
becomes its own mode (for example `motion`) and `blend` keeps its name; if none is possible, `blend` may be renamed
`on`, since it would then be the only frame generation there is. Nothing promises more than the temporal average
until such a pass exists.

## Defaults and practice

- `frame_interp=off` is the default. Frame generation adds latency and can show ghosting, so it is opt-in.
- `auto` is the recommended mode on a high refresh display: it runs only where the display is at least 1.9x the
  game's present rate, and it uses the cheapest method that works, the temporal blend.
- `blend` is the explicit choice for a display that is slower than 2x, with the ghosting it brings.
- The Emulation General Wiki and the Dolphin, PPSSPP and Eden graphics guides all put MSAA in the game renderer and
  post-process AA in the output stage. Dolphin and PPSSPP warn that MSAA and SSAA can break effects and cost GPU
  memory. Eden offers FXAA and SMAA as output filters, with no AA as the default.
- For the presented frame, `aa=auto` defaults to FXAA at 1.25x, the ratio where an upscale starts to show edges.
  FXAA is the cheap, compatible choice. SMAA is the manual higher quality choice. SSAA 2x and 4x are manual too.
- MSAA is not offered as a presented-frame mode, because a fullscreen pass has no geometry to sample. The Vita GXM
  renderer and the Switch NVN guest set their own sample counts.
