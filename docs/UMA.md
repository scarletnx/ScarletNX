# Unified memory (design)

Status: done on Linux under the `uma` setting (`[Debug]`), for GPUs with `VK_EXT_external_memory_host` and a
host-coherent memory type for it. It is on by default only where the GPU's memory is the CPU's: an integrated GPU, or
a CPU device like lavapipe (`uma=1`). A discrete GPU would read imported guest memory across PCIe at every use, where
its own copies sit in its own memory (`docs/UMA_MEMORY.md`), and a GTX 1050 laptop slowed down with it on.
`uma=2` imports on a discrete GPU too, to compare: buffers are then read and written in place, but textures stay the
CPU's, since checking them would pull their texels across the bus. It has run on lavapipe only (the headless tests),
not on RADV, ANV or NVIDIA, and not in a game yet.

- Vertex, index, constant and storage buffers are read where they are in guest memory.
- Shaders (draws, transform feedback, dispatches) write storage buffers in place.
- Textures are checked against guest memory and rebuilt on the GPU, with no CPU hash and no write tracking.

Without the extension the setting is a warning at start, and the renderer works as before. Buffers are copied into
staging and their own buffers, results land at the fence, and textures are hashed and watched with write tracking
(`plat_ww_scan`).

Unified memory and write tracking exclude each other. With UMA on, write tracking is off for the run (`plat_ww_off`,
from `vk_uma_init`), and what asked it compares contents instead, as on a host without it (below).

The console's GPU reads and writes the same memory its CPU does, and the game's fences keep the two apart. ScarletNX
keeps those fences faithful. A release (a syncpoint increment, a semaphore) waits for the Vulkan submit that holds the
work before it (`vk/release.c`). So the guest can't write what a recorded draw still reads, or read what a recorded
draw still writes, without breaking its own fences. A draw may then use guest memory as it is when the GPU runs it.
The copies, landings, hashes and write tracking were only there because the GPU couldn't see guest memory.

## Imports

`vk/uma.c` imports guest memory as buffers, on demand.

- Imports are stable: once made, one stays until no draw has used it for 120 presents or its pages change. A range up
  to S long (S from 2 MiB to 64 MiB, doubling) takes the import of the two S-aligned blocks from the one it starts
  in, which holds it whatever its start, or a bigger one already made that holds it. A longer range is copied. So a
  range bound from many starts (a 64 MiB storage window, a vertex stream) finds the same few imports, and a page is in
  two at most for each size used. The span is narrowed to the guest memory around the range whose host pages are the
  reservation's own and committed (`mem_private_span`: mapped, not the alias pool's, not I/O). Lookups are by key, a
  hash of the size and the block.
- Imported bytes stay under 1 GiB. A new import past it lets go of the ones used longest ago, but none used in the
  present being made, and a range with no room is copied. amdgpu counts imported pages as GTT, the heap the textures
  and render targets are in.
- Storage buffers with no size of their own (a bare address, 16 MiB from it) are bound as their own range. The
  windows they otherwise share (`vk_buf_get_window`) only save copies, and in place there are none: a 64 MiB window
  took a 128 MiB import, 32 of them at once.
- Two earlier ways were wrong on a Steam Deck. An import for each start of a long range doubled GPU memory. Imports
  grown to take in the ones they met were made again at nearly every such draw, each a userptr buffer mapped anew, at
  0 to 1 frames a second.
- An import no draw has used for 120 presents is let go (`vk_uma_frame`).
- Imports are small and few because of what drivers do with them (below).
- The alias pool's pages are never imported. GPU-owned memory lives there, trapped (`docs/GPU_OWNERSHIP.md`). RADV
  and ANV take an import's pages again through MMU notifiers at each submit. On a trapped page that would be a
  kernel-mode fault, which the userfaultfd answers with EFAULT. On lavapipe, a draw reading a trapped page would
  block its worker in the fault while the landing waits for that draw.
- NVIDIA pins an import's pages, so it keeps the old ones when guest memory is unmapped or moved. mem.c calls the
  backing hook (`mem_set_backing_hook`) for every range whose host pages are replaced or given back. That covers
  decommitting, unmapping, and moving into the pool or out of it. The import is retired and destroyed once the
  submits that used it are done (`vk_defer_call`), and the range is imported again from the new pages.
- Before the guest's mapping changes, the commands recorded with any import are submitted (`vk_uma_mapping_begin`),
  so a driver that takes the pages again at the submit takes the old ones.
- A span that can't be imported (unmapped pages, the driver refusing) isn't tried again until memory changes.

## What imports cost a driver

RADV makes host-pointer imports amdgpu userptr buffers, and the kernel's command submission
(`amdgpu_cs_parser_bos`, Linux 6.6) does this for every userptr buffer in a submission's list, at every submission:
it allocates an array of its pages, takes them all again with `hmm_range_fault`, and compares them with the ones it
had. A buffer whose pages an MMU notifier invalidated since makes the submission return `-EAGAIN`, and libdrm runs
the whole ioctl again. amdgpu's notifier also waits for the GPU to be done with the buffer
(`amdgpu_hmm_invalidate_gfx`).

So, on a Steam Deck, the cost of unified memory is the imported pages each submission names, times submissions. Its
first version imported 64 MiB chunks (16384 pages each) around whatever a draw touched and kept them for good, and it
ran far slower than with `uma=0`. Anything that changes the protection of imported pages invalidates them. Write
tracking does that by design: each scan re-protects the pages written since the last one. That is why the two
exclude each other. Transparent huge pages being collapsed still can.

The fix this design points to is the other direction: guest memory allocated as Vulkan memory and mapped into the
reservation (`VK_EXT_map_memory_placed`, which RADV has), so the GPU uses ordinary buffers. Those mappings can't take
userfaultfd, so the write tracking and the GPU-owned traps would need another way there (`docs/UMA_MEMORY.md`).
NVIDIA pins an import's pages once instead.

## Reads and writes in place

A range is used in place (`vk_uma_read`, `vk_uma_write`) when four things hold:

- it is one run of guest memory, at an offset the binding can take;
- no landing result, queued release or storage image is pending over it;
- nothing over it is GPU-owned;
- the runtime didn't write it while recording.

Otherwise it goes the old way.

- Buffers (`buffer.c`'s `buf_get`) are bound with no hash, own buffer or write-tracking question. A range that had an
  own buffer keeps it: a range read in place only some of the time (a release queued on its page, a result pending
  beside it) would otherwise be hashed and copied again each frame. Its checks find what changed meanwhile, the CPU's
  writes by epoch and the in-place writes by the GPU generation they stamp once done.
- Writable storage buffers and transform feedback buffers (`vk_buf_copy_writable`) are bound in place. Their writes
  are pending from the draw's commit until its submit is done (`UmaOut`, counted in `g_pend_any`), and nothing lands.
- Under UMA every dispatch runs on the graphics queue, in order with the draws, bound the same way. The compute
  queue's arena and resident GPU-owned results stay for the fallback.
- Ordering on the GPU: two page tables, stamped with the number of barriers so far, hold the pages shaders wrote and
  read in place since the last barrier. A read of written pages, or a write of read or written ones, ends the
  rendering pass and puts a barrier before it (`order`). The console's GPU runs its work in order, and the snapshots
  this replaces ordered it too.
- Ordering with the CPU:
  - The submit's end makes the writes visible to the host (`vk_uma_host_barrier`), before the release that tells
    the guest.
  - The runtime's own CPU readers (`gmem_read`, `gmem_ptr`, through `gmem_need`'s hook) wait for the submit that
    writes the bytes (`vk_uma_mem_need`).
  - Copies the engines and uploads stage on the GPU (`vk_guest_read`) copy pending bytes from the import after a
    barrier, without waiting.
  - The runtime's own CPU writes wait for the GPU's bytes first, then land over them.

## The runtime's own writes

The front end writes guest memory on the CPU while it records: inline to memory, constant buffer updates, release
payloads. A draw recorded before such a write would see it at run time. Two more page tables keep that apart:

- `front`: pages the runtime wrote while recording, from `gmem_before_write` (`vk_uma_before_write`). They are never
  read in place again. Constant buffers updated inline stay in staging, as before.
- `live`: the submit that last read each page in place. A write to a page that a submit not done yet reads waits for
  that submit (`vk_finish` when it is the one being recorded). That happens once per range, since the range is in
  `front` from then on.

With unified memory nothing becomes GPU-owned. The engines' results (DMA, the 2D engine, VIC, NVDEC, compute
results the old path kept resident, render targets written back) are copied into guest memory on the GPU, in stream
order, as writes in place pending until their submit is done (`vk_uma_publish`). VIC's inputs copy pending bytes on
the GPU too. A trap had let go of the imports over its pages (a driver would take trapped pages again at its next
submit), and moved them into the alias pool, which let go of them again: amdgpu mapped them anew at their next use,
for every texture the copy engine uploaded and every movie frame.

Where a result's memory isn't imported, it becomes GPU-owned as without UMA, and that counts as such a write. The
commands recorded with the imports over it are also submitted, and those imports are retired before its pages are
trapped.

These writes stay on the CPU, and that is deliberate. Making them GPU writes in stream order (`vkCmdUpdateBuffer`
into the import) doesn't fit in a rendering pass. Transfer commands can't be recorded inside one. Inside dynamic
rendering, a barrier can only order framebuffer-local stages, so a shader's write can't be ordered against the next
draw either. Constant buffers are updated before most draws, so each update would end the pass. Eden's buffer cache
makes the same choice: inline-updated uniform buffers are copied per draw.

The runtime's record-time readers of those bytes are many: texture and image handles, storage descriptors, BRX
tables, QMDs, TIC and TSC. Each would also need an overlay of the pending writes.

## Textures

A texture is a cache of guest memory that the GPU checks and rebuilds itself (`vk/texcheck.c`,
`gpu/texture/texcheck.comp`). Each check runs at the texture's first use in a frame, and again in each write epoch
(`vk_ww_epoch`, a guest submission or stream wait), or after a write of the GPU front end there.

1. A dispatch hashes the texture's guest bytes where they are (through the import, after `order`'s barrier), a 64 KiB
   piece a workgroup. It compares each piece with its hash from the last check, kept on the GPU, and sets the
   texture's flag where one differs. Every byte counts, at its place.
2. The rebuild is recorded as an upload always is, with the gate open (`VkGate`, `cmd.c`). Its dispatches
   (deswizzles, conversions, ASTC decodes) are indirect. Its copies into the image are indirect copies,
   `vkCmdCopyMemoryToImageIndirectKHR` with `VK_KHR_copy_memory_indirect`, else
   `vkCmdCopyMemoryToImageIndirectNV`.
3. A second dispatch writes their arguments, choosing between two sets the CPU recorded. With the flag set they are
   the rebuild's own. With it clear they do no work: no workgroups, and a copy no texel wide, which VUID 10967 of
   `VkCopyMemoryToImageIndirectCommandKHR` allows. The flag is then cleared. The CPU writes both sets into staging
   once the rebuild is recorded, before the submit.

Without either extension (or with `uma_indirect_copy=0`), dispatches store what the copies would
(`gpu/texture/texstore.comp`). They write into a view of the image's level in an unsigned format of the unit's size,
so the bits land as they are. The image is made able to take that view (`vk_texcheck_prepare`: storage usage,
mutable format, block-texel views for compressed formats). A rebuild with more than 512 slots of arguments runs the
rest as it is. That is slower, never wrong, since a rebuild of an unchanged texture gives the same texels. A texture
the GPU can't check now (results pending over it, memory not imported) is loaded again as it is.

A check's arguments take staging for what its rebuild may need (`gate_room`: eight slots a level of a layer and eight
a MiB of the texture, for the strips and bands of big levels, at most 16384). An estimate of a level of a layer alone,
capped at 512, ran the rest of a big array's rebuild ungated at every check: on a Steam Deck its ASTC was decoded
again each frame, 97136 dispatches and copies over 600 presents, at 2 to 3 frames a second. Gated rebuilds don't count
as texture uploads in the frame lines: the texture check line counts them.

`uma_texture_check=0` gives textures back to the CPU's checks with unified memory on, to compare. Each check ends the
rendering pass and records about seven barriers (two of them full ones) even when the rebuild is skipped, once per
texture in each frame and submission it is used in: batching the checks due is the first thing to measure there.

The GPU checks textures only on a shared-memory GPU (`g_vk.uma_shared`): its check reads every byte of a texture, a
whole 64 KiB piece at a time, at each check, which on a discrete GPU is that much PCIe traffic a submission.

A texture the GPU can't check (results pending over it, memory the runtime wrote while recording, GPU-owned or not
imported) goes to the CPU's checks for good, which is how it was checked before UMA. Reloading it at every check
instead was a staging copy and a rebuild of each such texture every frame and every submission.

With UMA on there is no write tracking, so these compare contents, as on a host without it:

- textures the GPU doesn't check (over memory the runtime wrote while recording, depth textures, formats a store can't
  take with neither copy extension, any on a discrete GPU or with `uma_texture_check=0`): hashed in each epoch until
  they settle, then sampled on a schedule (`texture.c`'s `tex_check`);
- shader code: hashed once in each epoch it is used in (`pipeline.c`'s `code_same`);
- the largest index of an indexed draw, which sizes the vertex streams bound: the indices scanned at each draw
  (`render.c`'s `draw_last_vertex`);
- buffers that aren't read in place (constant buffers updated inline): copied in each epoch, or own buffers checked
  on a schedule (`buffer.c`);
- render targets' baselines: hashed (`surface.c`).

## Next

0. The GPU's texture checks still read all of a texture at each frame and submission it is used in, on a
   shared-memory GPU. That is the GPU's own memory there, but bandwidth all the same: checks that skip what can't have
   changed (fewer epochs, pieces the GPU front end and the guest didn't touch) are next to measure.
1. Batch the texture checks. Each one ends the rendering pass, once a texture in each epoch. The checks due could be
   recorded together at the first one, and checks of textures a frame doesn't change could happen less often.
2. The rebuild's CPU bookkeeping (`wgen`, the render target copies `tex_take_drawn` makes) runs at every check, as
   if the texture had changed: render targets copied into textures are copied again each epoch.
3. Aliased storage bindings (`vk_buf_draw_prepare`'s atlas) still take the copy: one import over all of them would
   do in place.
4. Windows and macOS take the same path (mem.c's hook is the same there), untested. So are RADV, ANV, NVIDIA and both
   copy extensions. Lavapipe (Mesa 25.0) lists `VK_NV_copy_memory_indirect` without its `indirectCopy` feature, so
   only the store fallback has run.
