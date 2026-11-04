# The Switch presenter still needs anti aliasing and frame interpolation

## What is done

The shared effects (`src/common/gpu/vk/present_fx.c`) run on the 3DS, Vita and GBA presenters. The Switch presenter
still has its own upscaler and HDR path from #1104.

## Why it is not wired yet

The Switch presenter records commands through `src/nx/gpu/vk/record.c`, which begins and ends a command buffer on the
renderer's own frame slots. `present_fx` records raw `vkCmd*` calls and expects a command buffer that is already
recording. A first attempt fed it the presenter's command buffer and RADV answered `vkCmdPipelineBarrier2 was called
before vkBeginCommandBuffer`, then lost the device. The pass needs the renderer's `rec_Cmd*` bridge, or a callback
that begins the frame's recording first.

## Next step

1. Give `SnxFx` a recording callback pair: `begin(cb)` and `end(cb)`. The Switch presenter passes
   `vk_cmd`/`vk_finish` wrappers, so the common pass records on a buffer the renderer began.
2. Run the AA pass after `vk_upscale_process` and before `vk_hdr_compose`, using the same image the upscaler wrote.
3. For frame interpolation, keep the previous presented image and present the blend through the same HDR/blit path,
   with two swapchain images per game frame, as `docs/issues/frame-interpolation.md` describes.
4. Test a game at resolution 0.5 with FSR 1 and FXAA, then SMAA, then SSAA 2x, with `--vk_validation=1`.

The hotkey and the settings already exist. Do not enable them on the Switch presenter until this bridge is in place:
the GPU reset is worse than a missing filter.
