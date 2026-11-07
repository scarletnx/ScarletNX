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
* [Bitmaps](#bitmaps)
* [Over an app's own Vulkan](#over-an-apps-own-vulkan)
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

The GPU draws with 4x MSAA where the device has it, and resolves to the window's swapchain image, or to the layer
over an app's own Vulkan frames ([below](#over-an-apps-own-vulkan)). Without a display (`DISPLAY` and
`WAYLAND_DISPLAY` unset), it draws into an image of its own. `--shot=FILE.ppm` saves frame
`--shot-frame` (60 when not given). This is the one readback, for tests.

## Bitmaps

A `Bitmap` is a GPU image, as a hardware bitmap is on Android. The CPU never draws its pixels:

- A `Canvas` on a bitmap records into the bitmap's own display list. Each frame takes what was recorded since the
  last one (`Frame` flattens it as a node's list) and draws it into the bitmap's image before the frame's own draws.
  Compose caches its vector icons this way. The image has a multisampled image of its own that keeps what was
  drawn, so that the next drawing goes over it with the same 4x MSAA. When the image holds more than the
  multisampled one (after an upload), a draw copies it over first.
- Pixels that the app sets (`setPixels`, `eraseColor`, `copyPixelsFromBuffer`) go up whole, premultiplied, before
  the bitmap is drawn again.
- `createBitmap` of a part, a matrix or a scale, and `copy`, draw the source bitmap into the new one through a
  `Canvas`.
- `DRAW_BITMAP` draws the image's source rectangle onto the destination with the paint's alpha. A color filter of
  `SrcIn` (or `SrcAtop`) gives the filter's color where the bitmap is, as an icon's tint does, and one of `Modulate`
  multiplies the texels.
- A Bitmap that the app lets go (its `Cleaner` runs) frees its images at the next frame.

`getPixel` and the other reads see only the pixels the app set, not what a canvas drew: a readback is the last
resort, for an app that reads them.

An encoded image (`BitmapFactory`, `ImageDecoder`, a drawable resource) decodes on the CPU into a bitmap's pixels,
which then go up as the app's own pixels do. `src/android/hwui/image.c` is the codec, the HLE of Skia's: FFmpeg's
PNG, JPEG, WebP, GIF and BMP decoders find the format from its signature, and libswscale converts the decoder's
format to Android's int colors (`AV_PIX_FMT_RGB32`, 0xAARRGGBB in the host's byte order, as a `jint` holds it). It
also does the sample size (rounded down, up for a JPEG, as Skia's codecs do) and the density scale of a resource
(`(int)(size * scale + 0.5f)`, as `BitmapFactory` does) with area averaging, in the same pass. `Bitmap.compress`
encodes PNG and JPEG with FFmpeg's encoders.

A PNG's `npTc` chunk, which aapt writes for a `.9.png`, becomes the bitmap's nine-patch chunk in a device's byte
order (little-endian), as Android's `NinePatchPeeker` makes it, scaled with the bitmap. A drawable resource with it
is a `NinePatchDrawable` with the chunk's padding. `Canvas.drawPatch` draws the nine parts: the fixed ones keep their
size (they shrink together when the rectangle is smaller), and the stretchable ones share the rest.

## Over an app's own Vulkan

A game draws into its `SurfaceView`'s surface with its own device and swapchain on the window (the Vulkan
pass-through, [NATIVE](NATIVE.md)). On Android, the surface is below the app's window: the `SurfaceView` clears its
area of the window to transparent, and the compositor shows the window's views and dialogs over the game. The
same happens here:

- `SurfaceView` draws its area with `drawColor(0, CLEAR)`. Each frame is every window's last recording, the first
  one attached at the bottom (`snx.android.ui.Host`), so that a dialog shows over the activity.
- When the game makes its surface, the renderer gives the window up (`render_yield`). It then draws its frames into
  a ring of three layer images, cleared to transparent, with the same pipelines.
- The layer images are exportable (`VK_KHR_external_memory_fd` with an opaque fd on Linux and FreeBSD, an opaque
  Win32 handle on Windows), with a dedicated allocation. The game's device imports each one (`hle_vulkan.c`) when its
  physical device's `deviceUUID` and `driverUUID` are the renderer's. macOS has no such handle here (MoltenVK), so
  there is no layer.
- At each present of the game, when the layer has visible content, a submission on the presenting queue waits for
  the present's semaphores. It blends the layer over the swapchain image (a triangle that covers it, premultiplied
  alpha, `LOAD_OP_LOAD`), and signals a semaphore of its own, which the present waits for. `--shot` copies the image
  after the blend: from then on the shots are of the game's presents, and the renderer saves none of its own.
- Nothing is added to a present when no window shows over the game. The renderer finds that on the CPU: each draw
  that colors the layer adds its bounds, and a transparent rectangle that replaces the layer (the `SurfaceView`'s
  hole) takes away the bounds inside it. What it cannot follow counts as visible.

The two devices share no semaphore. The renderer's thread draws into an image that is not the one shown and that no
blend still reads (the game's side tells it, from the fences of its blends). It releases the image to
`VK_QUEUE_FAMILY_EXTERNAL` in layout `GENERAL`, then submits a fence on its queue after the frame. A present
blends the newest image whose fence is signaled, and the older one until then. So a present never waits for the
renderer, and the renderer waits for the game only when the game's blends still read every other image.

The blend is in the encoding of the swapchain: a UNORM swapchain blends the layer's bytes as they are. An sRGB
swapchain reads the layer through an sRGB view, so that its edges of partial alpha are a little different from
Android's.

## What is missing

- An animated GIF or WebP decodes to its first frame (`Movie` and `AnimatedImageDrawable` do not play). An animated
  WebP does not decode, as FFmpeg's WebP decoder has no animation.
- `Bitmap.compress` writes PNG and JPEG, not WebP, and only the pixels the app set.
- Gradients and bitmap shaders draw as one color, the average of their colors.
- Clips to a rounded outline or a path clip to their bounds. A clip of difference does not clip.
- Layers (`saveLayer`, a node's layer) multiply their alpha into what they hold, with no offscreen image.
- Dashes, mask filters (blur) and shadows (elevation) do not draw.
