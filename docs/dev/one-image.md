<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# Render targets as views of their textures

## Index

* [What the copies cost](#what-the-copies-cost)
* [What the emulators do](#what-the-emulators-do)
* [The model](#the-model)
* [Finding a storage](#finding-a-storage)
* [The order they come in](#the-order-they-come-in)
* [Layouts and barriers](#layouts-and-barriers)
* [Guest memory](#guest-memory)
* [Scale](#scale)
* [Lifetimes](#lifetimes)
* [What stays a copy](#what-stays-a-copy)
* [The steps](#the-steps)
* [Risks](#risks)
* [The first form: hosting](#the-first-form-hosting)

The renderer keeps a render target and a texture over the same guest memory as two Vulkan images. A texture that
samples a drawn target copies the target's pixels into its own image (a take, `texture.c tex_take_drawn`). This
design gives a guest surface one image: a render target becomes a view of a level or a layer of the texture's image,
as a native driver has it, and the copy goes.

## What the copies cost

A race of an Ounce game on the desktop (RX 9070 XT, RADV), 2026-11-25. The race is GPU-bound: the GPU's work spans
about 10.2 ms of a 12.4 ms frame, and the renderer thread is idle about a third of the time. Of that span, about 2.6
ms is the runtime's own copies and moves. The takes (`copy_layers`) are the largest part, 0.65 ms a frame and about
154 image copies.

A count of each take by its shape, a present:

| A take of | Takes | Bytes |
| --- | --- | --- |
| a target into one layer of an array (shadow cascades, 128x128x5 arrays) | 44 | 121 MB |
| a target into a level past the first (downsample chains, a level a target) | 86 | 2.3 MB |
| a target into level 0 of a texture of several levels | 5 | 6.4 MB |
| a storage image into a level or a layer | 19 | 1.6 MB |

Every take was of the same format and at the same scale. No take was of a whole texture: a texture of one level over
a target already samples the target's own image (`tex_from_tic`), or a crop of it. A texture of several levels, or of
several layers, never does. It loads from memory, and then takes each target over it, in each frame that draws them.

A Switch game's intro (an engine of another vendor) made no takes. The design must cost such a game nothing.

The gain is bounded: the takes are about 6% of the GPU's frame here. The design also removes the loads from memory
that come before the takes, and the target writes that a texture of several levels makes the runtime publish.

## What the emulators do

From a read of their sources (2026-11-25):

- **Ryujinx** (MIT, `Ryujinx.Graphics.Gpu/Image/TextureCache.cs`, `TextureGroup.cs`). One storage for each guest
  texture, found by its physical ranges. A render target that is a level or a layer of a texture is a view of it
  (`CreateView`). A texture made over a smaller one that came first gets the old one's data (`CopyTo`), and the old
  object becomes a view of the new storage (`ReplaceView`). An overlap that no view can be is a copy dependency, made
  when something reads it (`CreateCopyDependency`, `SynchronizeDependents`). CPU reads of pages the GPU changed fault
  and flush those pages (`FlushAction`), as our own traps do.
- **The yuzu family** (Eden's `texture_cache.h` and `util.cpp`, GPL-3.0-or-later and GPL-2.0-or-later AND MIT).
  One image for each surface, found by GPU address. A subresource is a view (`FindSubresource`). A larger image that
  comes later absorbs the smaller ones (`JoinImages`): it gets their levels or layers, copies what the GPU changed in
  them, and deletes them. Overlaps that no view can be stay two images (`BadOverlap`), kept in step by copies in the
  order of their writes (`SynchronizeAliases`).
- **Cemu** (MPL-2.0, which this project cannot port) relates textures and copies between them on use, as our takes
  do.

Both use a list of view formats on the image (`VkImageFormatListCreateInfo`), not a mutable format alone, as a
mutable format turns off compression on some GPUs. `format.c` already does so.

## The model

**A storage** is one Vulkan image and its memory, with what the guest's layout of it is: its address, its levels and
layers, each level's block height, the layer stride, the pitch of a pitch linear one, the sample layout and the
scale. It holds the layout and the last stage and access of each subresource (a level of a layer), where a `Surf`
holds one for the whole image now. It counts the views that use it.

**A `Surf`** (a texture, a render target, a storage image) is a view of a storage: its first level and their
number, its first layer and their number, and its view format. Its attachment view (`att`) and its sampled views are
views of that range of the storage's image. What the code asks of a `Surf` now (its address, its size, `wgen`, its
memory checks) stays on the `Surf`. What is the image's (`img`, `mem`, `layout`, `stage`, `access`, the takes) moves
to the storage.

At first each `Surf` has a storage of its own, so the step that brings the storage in changes no behavior.

## Finding a storage

The surface table finds a `Surf` by its address now. A storage index finds a storage by any byte of its range, so a
target finds the texture whose level or layer it is.

A target is a view of a storage when all of these hold:

- its address is where one of the storage's levels starts in one of its layers (`take_place` computes this already);
- its size is that level's, or the level's rows of whole GOBs (`padded_row`);
- its format is one that the storage's format list has, of the same texel size;
- its block height is the level's, or its pitch and layer stride are the storage's;
- its sample layout and its scale are the storage's.

A shape that fails one of these keeps a storage of its own, and today's paths for it stay: the take, the load from
memory, the crop.

## The order they come in

**The texture comes first.** A target made later (`rt_create_`) at a level or a layer of it becomes a view of its
storage. Its draws go into the texture's image, and nothing copies them.

**The targets come first.** This is the usual order: a game draws its shadow cascades, or its downsample chain, then
binds the array or the mip chain as a texture. The texture's storage is made at the texture's shape. Each target over
it that was drawn since memory had its pixels is copied into its subresource one time, the last take. Then the target
moves to the new storage: its `Surf` becomes a view of it, and its old image goes once the frames that used it are
done (`vk_defer_image`). From the next frame on, the target draws into the texture's image.

**A bigger shape comes later.** A target past the levels or layers of a storage, or a texture of more of them, gets a
new storage of the union. The views of the old one move to it after one copy of what the GPU changed, as Eden's
`JoinImages` does. A storage grows a bounded number of times: past that, the shape keeps its own storage and takes.

## Layouts and barriers

A storage keeps a layout for each subresource. `vk_surf_use` changes the layout of its view's range, and the barrier
names that range. So a pass draws into level N while a draw samples level N - 1, as a downsample chain does, with each
level in its own layout.

`VK_IMAGE_LAYOUT_GENERAL` for every subresource would be simpler. It is not the design: on AMD it can turn off the
compression of a color target.

A draw that samples the subresource it renders to is a feedback loop, which today's path handles (`through a copy`,
or the target read in place). That stays.

`writer_barriers` (`surface.c vk_surf_written_flush`) batches the barriers of the surfaces a pass wrote. It works on
`Surf`s now, and works on storage ranges after the change.

## Guest memory

What is newest is still decided as now, now by subresource, not by image:

- A draw makes the image newer than memory over that view's range (`wgen`, the drawn list).
- A write of the CPU or of an engine over the range makes memory newer, and the next use of a view over it loads the
  pieces from memory into the storage (`tex_check_pieces`, `vk_rt_note_read`). There is one image now, so a target's
  load and a texture's load are one load.
- A CPU access to pages a view is newer over publishes that view (`vk_rt_watch_hit`, the traps of `own.c`), as now. A
  publish writes the range of the view, not the whole storage.

The rules of `vk_rt_drawn_fresh` and of the takes become one rule: for each subresource, which of the image and
memory has the newer write. The take's search over drawn targets (`vk_rt_drawn_in`) remains for the shapes that keep
their own storage.

## Scale

A storage has one scale. A target that the resolution scale draws larger than the guest's size stays a storage of its
own, and a texture over it takes it with a blit, as now. Sampling a scaled storage directly needs the translator to
correct texel fetches and size queries for the scale, as Ryujinx and Eden do. That is a later step, and only scaled
games need it.

## Lifetimes

A storage lives while a view uses it. A texture freed while its targets draw leaves the storage to them, and a target
parked (`rt_create_` with its address's top bit set) keeps it. The memory trim (`vk_surf_reclaim`) frees storages: it
frees a storage with all its views, and a view that the game asks for again gets a new storage, loaded from memory as
a texture is now.

## What stays a copy

- Formats of another texel size or another class: depth and color, a compressed format and a color one.
- Another block height or pitch than the level's: other bytes, not a view.
- Scaled targets under textures that are not (above).
- MSAA targets read as textures of their samples (`gpu/vk/msaa.c`).
- Overlaps of other shapes. The yuzu family keeps them as two images in step. Here they keep today's take and load.

## The steps

Each step has a counter of what it removed, and a warm A/B/B/A (`warmrt.sh`) of the race and of a Switch game.

1. **Storage behind each `Surf`.** One storage for each `Surf`: no behavior changes. The tests pass, and the race's
   counters and screenshots are the same.
2. **Layouts for each subresource.** `vk_surf_use` with ranges. Sync validation (`VK_LAYER_VALIDATE_SYNC=1`) finds no
   new hazard.
3. **Targets come first, array layers.** The cascades and the arrays move to the texture's storage. Of the four
   lines above, this is 121 MB of the bytes. Checks: the takes into layers fall to about 0 a present after the first
   frame, and `copy_layers`'s GPU time falls.
4. **Targets come first, levels.** The downsample chains. The texture of several levels stops its load from memory.
5. **The texture comes first.** A target made over a storage is a view at once.
6. **Storage images.** A dispatch's storage image over a texture's subresource writes the texture's image.

A shadow mode, as for the pool map, can run the views and the takes side by side in steps 3 and 4. It compares the
texels of each view with the take's copy, and counts each one that differs.

## Risks

- A target moves to another storage while a command buffer that is recording uses its old image. The move must take
  effect at a submission's boundary, with the old image deferred, as `tex_image_swap` does.
- Eviction under memory pressure frees a storage that several views use. Each view must find out (a generation on the
  storage), as the texture memo's checks find a freed texture now.
- A game that draws a subresource in one format and samples it in another of the same texel size. The view format
  must be in the storage's list, so the list is fixed when the storage is made. A format that comes later and is not
  in the list makes a new storage, one time.

## The first form: hosting

Hosting does steps 3 and part of 4 without the storage of step 1. It runs wherever the device has unified image
layouts (`VK_KHR_unified_image_layouts`), where every image is in `GENERAL`, so a layout for each subresource (step 2)
is not needed. Elsewhere targets keep images of their own and the takes, as before. The tables below compare it off
and on, with a switch the measurements had.

- A texture image that a target can be a view of gets the attachment usage (`tex_create`, `tex_info`): block linear,
  2D, an array or a cube, one sample, its format as memory holds it, and a format the device can draw into.
- After the take of a target into such a texture (`tex_take_drawn`), the target is hosted (`vk_rt_host`), when it has
  the texture's format, it is not scaled, and it covers the level's size and the layers whole. Its image goes, and its
  attachment view is made on the texture's image at its level and layer. Its pixels are already there from the take.
- A hosted target keeps no state of its own. `vk_surf_use` keeps the host's, so a barrier names the host's whole image.
- The paths that read a target's image as level 0 and layer 0 add `vk_surf_level0` and `vk_surf_layer0` (the
  attachment views, the publishes, the loads, the takes into other textures, the sampled views). The other paths
  first give the target an image of its own again with a copy (`vk_rt_own`): the moves of a parked target's pixels,
  the present, the upscaler, readbacks, captures, crops and storage use.
- Before a texture's image goes (`tex_image_swap`, a free), its targets get images of their own (`vk_tex_unhost`).
  Before a texture loads from memory, its hosted targets that memory lacks are published first (`tex_hosted_publish`).

A target that its rows of whole GOBs make wider than its level (a 48x22 target over a level of 40x22, a 16x1 one over
1x1) is hosted too. Its view is the level's size (`vk_surf_view_w`): the render area, and so the scissors and clears,
the loads and publishes and its copy out stop there. A publish of it through the block linear move zeroes the
columns past the level. A texture of its own size samples it through an image of its own again.

Depth targets are hosted too. Their own paths take the place in the host: the depth packs of a publish
(`vk_depth_pack`), the copies of a save state (`zt_copy`) and the depth shadow (`depth_shadow.c`). The 2D engine's
blits (`blit2d`, `blit_depth`, `resolve_depth`) give a hosted source or destination an image of its own first. In the
race, the depth takes were 3.5 a present and 62 MB (3072x3072 shadow cascades into a 3-layer array), the largest part
of the bytes. They fell to none.

A texture's check of its pieces leaves out those under a fresh target it hosts (`take_cover`), as it left out those
a take would copy. A texture with holes in its mapping (`piecewise`) hosts nothing: its loads clear the image first.

An A/B/B/A of the race with all of the above, 2026-11-25:

| Hosting | fps (median) | GPU busy a present | Copy gaps a present | Takes a present |
| --- | --- | --- | --- | --- |
| 0 | 81.2, 70.9 | 7.61, 7.82 ms | 1.10, 1.16 ms | 101, 99 |
| 1 | 83.0, 86.4 | 7.29, 7.00 ms | 0.76, 0.79 ms | 0.7, 0.5 |

Storage image surfaces are hosted too, where the texture has storage usage: a dispatch's image over a level or layer
of a texture writes the texture's image through its storage view. A hosted texture surface has no loads or takes of
its own: its checks are its host's (`tex_check`, `tex_take_drawn`), and a copy engine's write into its level goes to
memory, which the host's check loads. In the race this took the storage surfaces' takes from 12 a present to none. A
bound measured before (the takes left out) showed no gain in fps or GPU time there: other games use storage images
more.

A texture that is one level of another's layers, of its format and layer stride, joins that one as a view of its image
(`vk_tex_join`): its pixels are copied there once, and the targets it hosts move with it. One game samples each of its
reflection cubes alone (6 layers, 1 level) and in an array of the cubes (12 layers, 7 levels). The faces were hosted
in the cube, and the array took them each frame: 18 takes a present. Joined, the race takes 0.3 a present.

Not hosted yet: scaled targets.

An A/B/B/A of the race, 2026-11-24:

| Hosting | fps (median) | GPU busy a present | Takes a present |
| --- | --- | --- | --- |
| 0 | 78.1, 81.6 | 7.82, 7.60 ms | 104, 99 |
| 1 | 80.8, 83.3 | 7.40, 7.19 ms | 37, 35 |

With the padded targets hosted too, the drawn targets taken fell to about 11 a present, and the storage surfaces stay
at 19.

After the rebase on main of 2026-11-25, an A/B/B/A of the race (main's changes made the race slower with hosting off,
see below):

| Hosting | fps (median) | GPU busy a present | Copy gaps a present | Takes a present |
| --- | --- | --- | --- | --- |
| 0 | 52.4, 59.3 | 8.53, 8.47 ms | 2.99, 2.59 ms | 96, 98 |
| 1 | 59.8, 64.5 | 7.87, 7.66 ms | 2.13, 2.07 ms | 5.5, 5.5 |

A Switch game's world (a load into an outpost) took 31.7 targets a present with hosting off and 0.1 with it on, at 60
to 66 fps off and 66 to 70 on, one run each.

Sync validation found no new hazard, and the screenshots match. About half of the hosted targets stay hosted. The
others go back when their texture's image is remade or freed, and the next take hosts them again. A Switch game
that makes no takes hosts nothing.

