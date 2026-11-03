# 3ds_vk_draw_present_validation reports a layout mismatch after the shared effects

## What is seen

`ctest -R 3ds_vk_draw_present_validation` fails on this machine with

```
VUID-vkCmdDraw-imageLayout-00344: Cannot use VkImage ... with SHADER_READ_ONLY_OPTIMAL
that doesn't match the previous known layout TRANSFER_SRC_OPTIMAL
```

The non-validation `3ds_vk_draw --present` passes, and `3ds_windows` passes in both modes. The image whose layout is
named is a texture the draw samples, not the presented frame. The shared effects leave their source in
SHADER_READ_ONLY and their output in TRANSFER_SRC, so the next frame's PICA draw may see a texture that the
presenter touched.

## Next step

Reproduce with `VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation` and a name on every image
(`vkSetDebugUtilsObjectNameEXT`) to name the image the validation message reports. Then make the presenter restore a
guest render target's layout after the effects, or keep the effects off a guest texture. Check that every `image_barrier`
in `src/3ds/gpu/present.c` uses the layout the renderer's target tracking expects.

## Status: fixed

`record_copy` and `record_fsr` leave their output in `TRANSFER_SRC_OPTIMAL`. The FXAA, SMAA and SSAA passes then
sample it, and `record_copy` had not moved it to `SHADER_READ_ONLY_OPTIMAL`. `snx_fx_render` now does that before the
anti aliasing pass. `3ds_vk_draw_present_validation` and `3ds_vk_draw_after_draw_validation` pass, and the Switch
device loss that the same omission caused under FXAA/SSAA is gone.
