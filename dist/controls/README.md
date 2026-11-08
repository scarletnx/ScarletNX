# On-screen controls

The drawables here are rEden's Android touch overlay, `src/android/app/src/main/res/drawable/` there:
Copyright 2023 yuzu Emulator Project, GPL-3.0-or-later (its `.reuse/dep5`), as Eden and rEden carry them.

`controls.png` is made from them by `scripts/build-controls.py` (rsvg-convert and Pillow): each drawable at 1.5
pixels per dp, the stick buttons at 4.5 (rEden draws their small drawables larger than the others), its alpha the
drawable's over 0.6, the most the drawables have, so the most opaque texel is opaque; the runtime draws them on a curve
that gives 0.6 at `controls_alpha`'s 50 (`SNX_VPAD_BUILTIN_CURVE`, vpad.h), the drawables' own look; packed into one
atlas with room for the game menu's font and a white block. The script also writes
`src/nx/runtime/platform/controls_atlas.h`, the sprites' places, and `--check` (a test) fails when either is not what
these files make. After changing a drawable, run the script and commit the three together.

These are the controls' built-in look; the default is a skin, `line`. Skins, RetroArch overlays, are in
[skins/](skins/README.md).
