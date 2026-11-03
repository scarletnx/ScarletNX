# A 3DS title stops at PICA's shadow rendering

## What was seen

`scarletnx-3ds` on Luigi's Mansion: Dark Moon now starts the GPU and runs the client, then stops:

```
3ds gpu: the command list at 0x21959920 stopped at register 0x22f: shadow rendering
scarletnx-3ds: unsupported guest operation or missing static entry: ... ipc_header=0x000c0000
```

The 3DS port does not implement PICA's shadow rendering register yet (`0x22f`). The presenter, HDR and the
anti aliasing are not reached, so this is a renderer gap, not a presentation one.

## Next step

Implement the PICA shadow rendering path in `src/3ds/gpu/` as the parity roadmap lists. Until then the synthetic
`present_fx`, `3ds_windows` and `3ds_vk_draw` tests cover the presenter, and another title without shadows can
reach it.
