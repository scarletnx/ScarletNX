# Effects proof across the games

## Index

* [Notes](#notes)
* [Window proof](#window-proof)
* [Modes on one game](#modes-on-one-game)
* [GBA](#gba)
* [3DS](#3ds)
* [Vita](#vita)
* [Frame interpolation, per frontend](#frame-interpolation-per-frontend)
* [3DS shadows](#3ds-shadows)

The method and the commands are in `docs/GAME-EFFECTS-METHOD.md`. Effects means
`--aa=fxaa --upscaler=fsr1 --hdr=auto`. Baseline means
`--aa=off --upscaler=off --hdr=off`. Both ran for 5 seconds on the private headless X11 display,
with the runtime this branch built and deployed. A native frame is the runtime's `--shot`.

| Game | Title ID | System | Baseline | Effects | Frames | Effects pass | Validation | Device lost |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Super Mario Bros. Wonder | `010015100B514000` | switch | 0 / frames=228 | 0 | frames=248 | 1 | 0 | 0 |
| Mario Kart 8 Deluxe | `0100152000022000` | switch | 0 / frames=121 | 0 | frames=139 | 1 | 0 | 0 |
| Super Mario 3D World + Bowser's Fury | `010028600EBDA000` | switch | 0 / frames=225 | 0 | frames=28 | 1 | 0 | 0 |
| Super Mario 3D All-Stars | `010049900F546000` | switch | 0 / frames=281 | 0 | frames=281 | 1 | 0 | 0 |
| Minecraft: Switch Edition | `01006BD001E06000` | switch | 0 / frames=265 | 0 | frames=212 | 1 | 0 | 0 |
| 01008CF01BAAC000 | `01008CF01BAAC000` | switch | 124 / none | 124 | none | 0 | 0 | 0 |
| Luigi's Mansion 3 | `0100DCA0064A6000` | switch | 0 / frames=55 | 0 | frames=19 | 1 | 0 | 0 |
| New Super Mario Bros. U Deluxe | `0100EA80032EA000` | switch | 0 / frames=1094 | 0 | frames=824 | 1 | 0 | 0 |
| The Legend of Zelda: Tears of the Kingdom | `0100F2C0115B6000` | switch | 0 / frames=19 | 0 | frames=0 | 0 | 0 | 0 |

## Notes

- A baseline and an effects result of 0 with frames above 0 is a game that works both ways.
- A baseline that fails as the effects does is a game that does not work on this runtime yet.
- `01008CF01BAAC000` is Echoes of Wisdom. Its recompiled code is ABI 14 where this runtime is ABI 17,
  so it must be recompiled before it can be tested.
- `0100F2C0115B6000` is Tears of the Kingdom. It needs more than 5 seconds to reach its first frame.
  A 30 second run reached 712 frames with `presentation effects ready` and 0 validation messages.
- `0100DCA0064A6000` reaches its first frame late in a 5 second run, so its frame count is small.

## Window proof

`--shot` writes the native frame, not the window. The effects proof is the `presentation effects ready`
line, the frame count and the validation count. The synthetic `snx-test-present-fx` panels and the 3DS
`3ds_windows` test prove the window path itself.

## Modes on one game

Super Mario Bros. Wonder was run through every mode. The table is `matrix-010015100B514000` in the demo
folder. Each row is a 4 second run with the log named after the mode.

| Mode | Settings | Exit | Frames | Effects | Validation | Device lost |
| --- | --- | --- | --- | --- | --- | --- |
| AA off | `--aa=off --upscaler=fsr1 --hdr=off` | 0 | 183 | 0 | 0 | 0 |
| AA Auto | `--aa=auto --upscaler=fsr1 --hdr=off` | 0 | 185 | 0 | 0 | 0 |
| FXAA | `--aa=fxaa --upscaler=fsr1 --hdr=off` | 0 | 187 | 1 | 0 | 0 |
| SMAA | `--aa=smaa --upscaler=fsr1 --hdr=off` | 0 | 185 | 1 | 0 | 0 |
| SSAA 2x | `--aa=ssaa2 --upscaler=fsr1 --hdr=off` | 0 | 190 | 1 | 0 | 0 |
| Upscaler off | `--aa=off --upscaler=off --hdr=off` | 0 | 190 | 0 | 0 | 0 |
| Bilinear | `--aa=off --upscaler=bilinear --hdr=off` | 0 | 191 | 0 | 0 | 0 |
| FSR 1 | `--aa=off --upscaler=fsr1 --hdr=off` | 0 | 191 | 0 | 0 | 0 |
| NIS | `--aa=off --upscaler=nis --hdr=off` | 0 | 195 | 0 | 0 | 0 |
| SGSR | `--aa=off --upscaler=sgsr --hdr=off` | 0 | 189 | 0 | 0 | 0 |
| HDR off | `--aa=off --upscaler=fsr1 --hdr=off` | 0 | 196 | 0 | 0 | 0 |
| HDR Auto | `--aa=off --upscaler=fsr1 --hdr=auto` | 0 | 191 | 0 | 0 | 0 |
| HDR On | `--aa=off --upscaler=fsr1 --hdr=on` | 0 | 189 | 0 | 0 | 0 |
| HDR On validated | `--aa=fxaa --upscaler=fsr1 --hdr=on --vk_validation=1` | 0 | 191 | 1 | 0 | 0 |
| FSR 1, 0.5 resolution | `--resolution=0.5 --aa=fxaa --upscaler=fsr1 --hdr=off` | 0 | 194 | 1 | 0 | 0 |
| NIS, 0.5 resolution | `--resolution=0.5 --aa=fxaa --upscaler=nis --hdr=off` | 0 | 196 | 1 | 0 | 0 |
| SGSR, 0.5 resolution | `--resolution=0.5 --aa=fxaa --upscaler=sgsr --hdr=off` | 0 | 196 | 1 | 0 | 0 |

At `resolution=1` the game renders 1920x1080 into a 1280x720 window, so the upscalers see a downscale and NIS
falls back to FSR 1. The three `0.5 resolution` rows render 960x540 into 1280x720, and the logs name NVIDIA NIS
and Qualcomm SGSR themselves. `Effects 0` on the upscaler rows is expected: the Switch presenter logs
`vulkan: upscaling ...` rather than the shared effects' line.



## GBA

`AGB KIRBY DX` (the `kirby` folder, cartridge `A7KE`) ran for 5 seconds on the private X11 display with
`--aa=fxaa --upscaler=fsr1 --hdr=auto --frame_interp=blend`. The log `games/gba-kirby-effects.log` has
`frame interpolation: Blend (display 60 Hz)` and `vulkan: presentation effects ready`. The native frame is
`games/gba-kirby-native.ppm` and `panels/gba-kirby-title.png`. The `--shot` frame is the board's own 240x160,
not the window.

A `--frames=300 --shot` run without a window is the control. It writes the same native frame and exits 0.

## 3DS

- `snx-test-3ds-windows OUT` and `--separate`: both pass, top 400x240 and bottom 320x240.
- `3ds_vk_draw --present`, `--after-draw`, `--texture-after-draw`, `--draw-after-index-copy`: pass.
- The four `3ds_vk_validation.py` runs: pass, 0 validation messages.
- `Luigi's Mansion: Dark Moon` (0004000000055F00) starts, imports the board RAM and reaches the presenter, then
  stops at PICA's shadow rendering before the first draw. This is a renderer gap, not a presentation one. See
  `docs/issues/3ds-pica-shadow-rendering.md`.

## Vita

No Vita homebrew is on this machine. The proof is the synthetic `snx-test-present-fx` run, which reads FSR 1,
FXAA, SMAA, SSAA 2x and the PQ encode back from the GPU, plus the Vita build. The two-image interpolation path is
compiled and uses the same tested `snx_fx_blend`.

## Frame interpolation, per frontend

- **Switch.** Super Mario Bros. Wonder with `--frame_interp=blend --aa=fxaa --upscaler=fsr1 --hdr=auto
  --vk_validation=1` exited 0 with 256 frames and no validation message; the log says
  `frame interpolation Blend: a blend and the frame itself for each game frame (display 59 Hz, game 60 Hz)`.
  With `--frame_interp=auto --fps_limit=30` it says `Auto ... (display 59 Hz, game 30 Hz)`. SMAA and SSAA 2x at
  `--resolution=0.5` with the blend and validation also exit 0. Logs: `games/swi-*.log` in the demo folder.
- **GBA.** A window run of `AGB KIRBY DX` with blur/auto and the effects logs
  `frame interpolation: Blend (display 60 Hz)` and `Auto (display 60 Hz)`, and `vulkan: presentation effects ready`.
  Logs: `games/gba-win-*.log`.
- **3DS.** `snx-test-3ds-windows OUT` and `--interp` pass, the latter with `frame_interp=blend` (two swapchain
  images, the blend then the pair).
- **Vita.** The synthetic `present_fx` path covers the blend; no Vita title is on this machine.

## 3DS shadows

`snx-test-3ds-vk-draw --shadow` renders a shadow map, checks every packed word (depth 0x7FFFFF, strength 255), then
samples it: a fragment in front is lit and one behind is shadowed. `3ds_vk_draw_shadow_validation` runs it under
Khronos synchronization validation with no error. The retail title is blocked before the GPU by an APT wait, not by
the shadow path; see `docs/issues/3ds-pica-shadow-rendering.md`.
