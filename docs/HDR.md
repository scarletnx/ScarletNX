# HDR: the mapping, and the mistakes to avoid

What the presented frame's HDR path does, the numbers it must use, and the three faults that were found and fixed
here, so a later change (macOS, a new compositor, a new kind) does not bring them back.

## The model

An SDR game frame is decoded to linear light, turned from BT.709's primaries to BT.2020's, scaled so its white is the
**paper white**, and written as one of three signals:

| Kind | Format | What 1.0 means |
| --- | --- | --- |
| **HDR10 (PQ)** | 10-bit `A2B10G10R10`, `VK_COLOR_SPACE_HDR10_ST2084_EXT` | absolute nits through SMPTE ST 2084, BT.2020 primaries |
| **scRGB** | `R16G16B16A16_SFLOAT`, `VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT` | **80 nits**, BT.709 primaries |
| **Display P3** | `R16G16B16A16_SFLOAT`, `VK_COLOR_SPACE_DISPLAY_P3_LINEAR_EXT` | **80 nits**, P3 primaries |

The SDR range lands at the **paper white**; the light above it is the **headroom**, up to the display's **peak**. The
compositor maps the paper white to its own SDR white and tone-maps above what the display reaches. Two numbers decide
the result, and both belong to the *display*, not to a setting:

- **paper white**: the display's own SDR white (KWin's `sdrBrightness`, 400 nits in a common setup).
- **peak**: the display's own peak (KWin's `maxPeakBrightnessOverride`).

The HDR10 swapchain also carries `VK_EXT_hdr_metadata`: `maxLuminance` and `maxContentLightLevel` = the peak,
`maxFrameAverageLightLevel` = the paper white, BT.2020's primaries for PQ (BT.709's for scRGB). Without the metadata the
compositor guesses the content's peak, and a guess that is too high tone-maps the whole SDR range into the display.

Where the display reports nothing (SDL exposes no SDR white or peak on Wayland for a Vulkan window), the settings
`hdr_paperwhite` and `hdr_peak` stand in. The frontends read KWin's output config (`~/.config/kwinoutputconfig.json`,
`sdrBrightness` and `maxPeakBrightnessOverride`) as the report there; see `snx_vk_paper_nits` and `snx_vk_peak_nits` in
`src/common/gpu/vk/window.c`.

## The reference: Horizon

Horizon (`https://github.com/horizon-rust/horizon`) does the same job and is worth checking against:

- `crates/horizon-hdr/src/lib.rs`: `set_metadata` passes `peak` as `maxLuminance`/`maxContentLightLevel` and the paper
  white as `maxFrameAverageLightLevel`; the primaries are BT.709's.
- `crates/horizon-hdr/src/wayland.rs`: it asks the compositor for the surface's preferred image description
  (`wp_color_management_v1`): the reference white and the target's minimum and maximum luminance. That is the report
  SDL does not give us; reading KWin's config is the stand-in.
- `crates/horizon-client-render/src/gfx/hdr.rs` and `hdr.wgsl`: `paper` is in scRGB units, `nits / 80`; the frame is
  decoded with gamma 2.2, written as `pq(BT709_TO_BT2020 * paper * 80)` for HDR10 or `paper` for scRGB; the matrix is
  written columns first.
- `paper_white_nits()`/`peak_nits()`: the player's value, or on Auto the compositor's report, else 203 and 1000.

## The mistakes already made (do not repeat)

1. **The extended kinds divided by the paper white.** scRGB and P3 linear have 1.0 = 80 nits, not 1.0 = the paper
   white. Dividing by the paper white put the picture at `paper / 80` times the light the compositor read (5x with a
   400-nit paper). Divide by 80; PQ stays absolute nits.
2. **The metadata named `hdr_peak` while the display's peak was lower.** A 1000-nit claim on a 400-nit display made the
   compositor tone-map the whole signal into 400, compressing the SDR range near its top: bright parts went towards
   opaque white. The peak is the display's own where it can be read.
3. **The paper white was 203 while the display's white was 400.** The SDR range sat above the display's white and read
   as washed out. The paper white is the display's own SDR white where it can be read.
4. **The UI's BT.709→BT.2020 matrix was transposed.** `mat3` takes its columns; the rows written as columns turned
   white (1,1,1) into (0.71, 1.34, 0.95), a green cast over every toast, menu and on-screen control. The compose
   shader's `dot` form was right; the UI's matrix had to be written by columns.
5. **The UI shaders are checked in as SPIR-V.** `src/nx/gpu/vk/ui_spv.h` is made by `scripts/build-ui-shaders.py`, not
   by CMake: editing `src/nx/gpu/vk/shaders/ui.frag` alone changes nothing. Regenerate it (and its source hash) with
   that script; `scripts/build-ui-shaders.py --check` fails when it is stale.
6. **The HDR shaders were duplicated.** The Switch compiled its own copies of `hdr.vert`, `hdr_compose.frag`,
   `hdr_encode.frag` and `hdr_test.frag`, byte for byte the same as `src/common/gpu/vk/fx/`. They are one source now;
   change `src/common/gpu/vk/fx/` and every frontend gets it.

## macOS

- The device code prefers the extended kinds on macOS because an HDR10 (PQ) layer's luminance is misrepresented there
  (about half the exposure on an HDR display, mpv #16899); 1.0 of an extended linear layer is the display's SDR white
  on every display. Do not switch macOS to PQ without checking that first.
- macOS reports EDR through SDL (`SDL_PROP_WINDOW_HDR_HEADROOM_FLOAT`), so the headroom there comes from SDL, not from
  a settings guess; the paper white still needs to be the display's SDR white.

## Where the code is

- `src/common/gpu/vk/fx/hdr_compose.frag`, `hdr_encode.frag`, `hdr_test.frag`, `hdr.vert`: the passes every frontend
  compiles.
- `src/nx/gpu/vk/hdr.c`: the Switch's state (mode, source, paper, peak, expansion), the surface probing and the
  description; `src/nx/gpu/vk/device.c` picks the swapchain kind, the metadata and the order of the kinds.
- `src/common/gpu/vk/present_fx.c`: the 3DS, Vita and GBA's compose and encode (`snx_fx_hdr_*`).
- `src/3ds/gpu/present.c`, `src/vita/gpu/vk.c`, `src/agb/runtime/vk.c`: their paper and peak, resolved once at start.
- `tests/present_fx.c`: the headless check of the paper white's effect on the light (compose, PQ encode, decode back).
- `tests/hdr_select.c`: the shaders' numbers against `hdr_select.c`'s.
