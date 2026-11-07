# On-screen control skins

A skin is a [RetroArch overlay](https://docs.libretro.com/development/retroarch/input/overlay/): a `.cfg` and the PNGs it
names, as libretro's [common-overlays](https://github.com/libretro/common-overlays) are made. The game's `.ini` picks
one with `controls_skin`: a skin of this folder by its name (`line`, the default, or `yorha`), empty for the built-in
look, or the path of any overlay's `.cfg`. The skins here are compiled into the runtime (`cmake/controls_skins.cmake`), so every system
has them; a change of `controls_skin` while the game runs takes effect at once.
`src/nx/runtime/platform/vpad_skin.c` reads, of its pages whose name has no `portrait`, the one that gives the most of
the controls below a look (the first of those: libretro's `retropad.cfg` gives its `landscape-both-analog`, with both
sticks), placed from 0 to 1 (`normalized = true`):

- `a b x y l r l2 r2 l3 r3 select start menu_toggle screenshot`: A, B, X, Y, L, R, ZL, ZR, L3, R3, minus, plus,
  Home and Capture.
- `analog_left`, `analog_right`: the sticks, the image moving with the finger when `movable = true`, pushed all the
  way at `saturate_pct` of the range; a `nul` descriptor with an image at the same middle is its ring.
- `dpad_area`, or `up`, `down`, `left` and `right`: the D-pad, the box around the four its box.
- Each descriptor's `range_mod`, `reach_*` and `alpha_mod` (the page's when it has none); `radial` or `rect` hit areas.
- A two-frame APNG is an image and its pressed look; a one-frame image is drawn pressed at its alpha times `alpha_mod`.

Every other descriptor is left out, said in the log with why, and a control the skin has none for keeps the built-in
look. Sizes follow the page's `aspect_ratio` (16:9 when it has none).

`line/` and `yorha/` are ScarletNX's own, drawn by `scripts/build-skins.py`, every control where the built-in look has
it:

- `line`: white lines alone, after the look of libretro's flat
  [retropad](https://github.com/libretro/common-overlays/tree/master/gamepads/flat) (none of its art: the script draws
  every line, letters included): one line width on the screen, a faint dark edge under the lines for a bright game,
  sticks as a ring and a knob with a field of dots, and a pressed control filled white with its mark in dark.
- `yorha`: the launcher's `yorha` theme: its page and ink, thin lines, square markers, a pressed control inverted as a
  focused row is, and the letters of the menus' font (`src/common/utilities/font.c`) pixel by pixel.

The SVGs are in each skin's `svg/`, each rendered as many pixels as the built-in sprite of its control. After changing
the script, run it and commit what it writes; `--check` (the `controls_skins_current` test) fails when the files are
not what it and the font make.
