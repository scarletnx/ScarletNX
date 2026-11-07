# On-screen control skins

A skin is a [RetroArch overlay](https://docs.libretro.com/development/retroarch/input/overlay/): a `.cfg` and the PNGs it
names, as libretro's [common-overlays](https://github.com/libretro/common-overlays) are made. The game's `.ini` picks
one with `controls_skin`: empty for the built-in look, a skin of this folder by its name (`yorha`), or the path of any
overlay's `.cfg`. The skins here are compiled into the runtime (`cmake/controls_skins.cmake`), so every system has them;
a change of `controls_skin` while the game runs takes effect at once.
`src/nx/runtime/platform/vpad_skin.c` reads its first page whose name has no `portrait`, placed from 0 to 1
(`normalized = true`):

- `a b x y l r l2 r2 l3 r3 select start menu_toggle screenshot`: A, B, X, Y, L, R, ZL, ZR, L3, R3, minus, plus,
  Home and Capture.
- `analog_left`, `analog_right`: the sticks, the image moving with the finger when `movable = true`, pushed all the
  way at `saturate_pct` of the range; a `nul` descriptor with an image at the same middle is its ring.
- `dpad_area`, or `up`, `down`, `left` and `right`: the D-pad, the box around the four its box.
- Each descriptor's `range_mod`, `reach_*` and `alpha_mod` (the page's when it has none); `radial` or `rect` hit areas.
- A two-frame APNG is an image and its pressed look; a one-frame image is drawn pressed at its alpha times `alpha_mod`.

Every other descriptor is left out, said in the log with why, and a control the skin has none for keeps the built-in
look. Sizes follow the page's `aspect_ratio` (16:9 when it has none).

`yorha/` is ScarletNX's own, drawn by `scripts/build-skins.py` as the launcher's `yorha` theme looks: its page and ink,
thin lines, square markers, a pressed control inverted as a focused row is, and the letters of the menus' font
(`src/common/utilities/font.c`) pixel by pixel. The SVGs are in `yorha/svg/`, each rendered as many pixels as the
built-in sprite of its control. After changing the script, run it and commit what it writes; `--check` (the
`controls_skins_current` test) fails when the files are not what it and the font make.
