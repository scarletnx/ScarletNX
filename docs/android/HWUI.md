<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# The libhwui HLE

An app draws through `Canvas`, `Paint`, `Path` and `RenderNode`. On Android, libhwui records those calls into display
lists, and its RenderThread draws them with Skia over Vulkan or GLES. Here, the Java side records the same calls into
a byte format (`snx.android.hwui.DisplayList`), and `src/android/hwui/render.c` draws them with Vulkan. No Skia runs.

## Index

* [Recording](#recording)
* [The format](#the-format)
* [The renderer](#the-renderer)
* [What is missing](#what-is-missing)

## Recording

Each `RenderNode` holds a display list: segments of operations, with the child nodes that the list draws between
them. `snx.android.ui.Frame` flattens a root's tree into one list for each frame, each node between `NODE_BEGIN` and
`NODE_END`. So the renderer reads one buffer, and the frame does not parse an operation to flatten the tree.

The bytes are little-endian on every host, as the renderer reads them (`common/utilities/endian.h`). Nothing is
aligned: the renderer reads each value with `memcpy`.

## The format

Each operation is one byte, then its fields. `f` is a 32-bit float, `i` a 32-bit integer, `b` a byte.

| Operation | Code | Fields |
| --- | --- | --- |
| `SAVE` | 1 | |
| `RESTORE` | 2 | |
| `TRANSLATE` | 4 | `f dx, f dy` |
| `SCALE` | 5 | `f sx, f sy` |
| `ROTATE` | 6 | `f degrees` |
| `CONCAT` | 7 | 9 `f`, `Matrix.getValues()` order |
| `SET_MATRIX` | 8 | 9 `f`, relative to the node's transform |
| `CLIP_RECT` | 10 | `f left, top, right, bottom`, `b op` (0 intersect, 1 difference) |
| `CLIP_PATH` | 11 | a path, `b op` |
| `DRAW_COLOR` | 12 | `i color` (ARGB), `b blend` |
| `DRAW_PAINT` | 13 | a paint |
| `DRAW_RECT` | 14 | `f left, top, right, bottom`, a paint |
| `DRAW_RRECT` | 15 | `f left, top, right, bottom, rx, ry`, a paint |
| `DRAW_OVAL` | 16 | `f left, top, right, bottom`, a paint |
| `DRAW_CIRCLE` | 17 | `f cx, cy, radius`, a paint |
| `DRAW_ARC` | 18 | `f left, top, right, bottom, start, sweep`, `b useCenter`, a paint |
| `DRAW_LINES` | 19 | `i count`, `count` `f` (x0, y0, x1, y1 for each line), a paint |
| `DRAW_POINTS` | 20 | `i count`, `count` `f`, a paint |
| `DRAW_PATH` | 21 | a path, a paint |
| `DRAW_GLYPHS` | 22 | `i font`, `f size, scaleX, skew`, `b fakeBold`, `i n`, `n` `i` glyph IDs, `2n` `f` positions, a paint |
| `DRAW_BITMAP` | 23 | `i bitmap`, `f` source rectangle, `f` destination rectangle, a paint |
| `SAVE_LAYER` | 25 | `f left, top, right, bottom`, `i alpha`, `b blend` |
| `NODE_BEGIN` | 30 | 9 `f` transform in the parent, `f alpha, width, height`, `b clipToBounds`, the outline clip, the clip rectangle |
| `NODE_END` | 31 | |

A **path** is `b fillType` (`Path.FillType`'s order), `i verbs`, `i floats`, the verbs as bytes (move 0, line 1,
quad 2, cubic 3, close 4), then the floats of their points.

A **paint** is `i color`, `b style, cap, join, flags`, `f strokeWidth, miter`, `b blend` (Skia's `SkBlendMode`
numbers), `b shader kind, filter kind, dash count`. Then, when they are there:

- the shader: `b tileX, tileY`, a 16-bit color count, 6 `f` points, 9 `f` local matrix, the colors as `i`, `b` has
  positions and 3 bytes, the positions as `f`, `i bitmap`,
- the color filter: kind 1 is `i color`, `b blend` and 3 bytes. Kind 2 is a color matrix of 20 `f`,
- the dash: `f phase`, then the intervals.

The **outline clip** of a node is `b` 0 for none, 1 for a rounded rectangle (`f left, top, right, bottom, radius`)
and 2 for a path. Its **clip rectangle** is `b` 0, or 1 and `f left, top, right, bottom`.

## The renderer

The CPU reads the list once for each frame. It keeps the stack of transforms, clips and alphas, and it makes each
shape into triangles in device pixels (`path.c`):

- A rectangle, a rounded rectangle, an oval or a circle is a convex polygon, drawn as a fan.
- A path, a stroke and the glyphs of a text run go through the stencil, then a cover. Their triangles first count
  the winding in the stencil buffer (or flip its low bit for even-odd, or set it for the triangles of a stroke, which
  overlap). Then a rectangle over their bounds draws the paint's color where the stencil is not zero, and zeroes it
  again. So each pixel is drawn once, whatever the triangles' overlap.
- Curves are flattened with Wang's formula to a quarter of a device pixel. Glyph outlines come from HarfBuzz
  (`hb_font_draw_glyph`) once for each glyph, and are kept in font units.

The GPU draws with 4x MSAA where the device has it, and resolves to the window's swapchain image. Without a display
(`DISPLAY` and `WAYLAND_DISPLAY` unset), it draws into an image of its own. `SNX_ANDROID_SHOT=FILE.ppm` saves frame
`SNX_ANDROID_SHOT_FRAME` (60 when not given). This is the one readback, for tests.

## What is missing

- Bitmaps: decoded images, and bitmaps that a `Canvas` draws into (Compose caches its vector icons in them). These
  are to be GPU images, with a bitmap's canvas drawn on the GPU.
- Gradients and bitmap shaders draw as one color, the average of their colors.
- Clips to a rounded outline or a path clip to their bounds. A clip of difference does not clip.
- Layers (`saveLayer`, a node's layer) multiply their alpha into what they hold, with no offscreen image.
- Dashes, mask filters (blur) and shadows (elevation) do not draw.
