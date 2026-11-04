# A 3DS title stops at PICA's shadow rendering

## What was seen

`scarletnx-3ds` on Luigi's Mansion: Dark Moon started the GPU and ran the client, then stopped:

```
3ds gpu: the command list at 0x21959920 stopped at register 0x22f: shadow rendering
scarletnx-3ds: unsupported guest operation or missing static entry: ... ipc_header=0x000c0000
```

The 3DS port did not implement PICA's shadow rendering register yet (`0x22f`). The presenter, HDR and the
anti aliasing were not reached, so this was a renderer gap, not a presentation one.

## What is done

The GPU renderer (`src/3ds/gpu/vk/`) now draws the shadow pass:

- `OM_MODE` 3 selects a shadow target: the colour buffer holds one packed word per texel (`depth << 8 | strength`,
  Azahar's `EncodeShadow`). The target's image is `R32_UINT`, its depth attachment is off, and the target is never
  scaled with the 3D rendering, so the map keeps the PICA's own size.
- The fragment shader generator (`fsgen.c`) generates Azahar's `UpdateShadow` and its compare-and-swap into a storage
  image, bound at set 2, binding 0.
- `shaders/shadow.comp` stores and loads the map between the tiled guest memory and the image, byte for byte.
- A `Shadow2D` texture unit binds the target's own `R32_UINT` image, so the map is sampled in the coordinates the
  shadow pass wrote (`SampleShadow2D`, already generated for the fragment shader). A map no target keeps reads as
  fully lit instead of stopping the draw. Shadow cubes still read the fully lit map.
- The software reference (`gpu/raster.c`) still stops on a shadow draw; the tests use it only for the scenes it draws.

## Tests

- `tests/3ds_vk_draw.c --shadow`: renders a shadow map, reads its packed words (all equal, the strength unchanged,
  the depth in range), then samples it with a `Shadow2D` unit: a fragment in front of the map is lit and one behind
  it is shadowed. All three checks pass.
- `3ds_vk_draw_shadow_validation` runs the same mode under Khronos synchronization validation with no errors. The
  four earlier validation modes and the CPU-reference scenes still pass.

## The retail title now reaches the presenter

On 2026-11-04 the retail title started, reached the presenter and drew its title screen and its "Press A" prompt
(see `docs/GAME-EFFECTS-PROOF.md`). The APT wait on `apt:u` command `0x0055` that stopped an earlier run before the
first draw no longer blocks startup. The shadow path is still unverified in a real scene: the title screen does not
draw one, and the APT wait may still come after the prompt. Next step: press A into a scene that uses shadows and
watch for the `3ds gpu:` shadow line, with the PICA shadow pass already covered by its tests.
