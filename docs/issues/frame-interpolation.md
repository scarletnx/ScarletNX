# Frame interpolation: GBA now, the GPU frontends next

## What is done

`frame_interp` and `frame_interp_key` (Ctrl+Shift+F) exist. The GBA runner reads them, and `auto` or `blend` presents
a byte average of the previous and current 240x160 frame before the current frame. `auto` runs only where
`snx_fx_interp_auto` finds the display at least 1.9 times the game's rate, so two presented frames fit between the
game's own. On a 60 Hz display with the GBA's 59.73 Hz, Auto stays Off. A 120 Hz display runs it. The hotkey cycles
Off, Auto and Blend with a log line, and the `.ini` is not changed.

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
