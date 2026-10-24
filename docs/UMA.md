# Unified memory (design)

Status: done on Linux under the `uma` setting (on by default, `[Debug]`), for every GPU with
`VK_EXT_external_memory_host` and a host-coherent memory type for it. It has run on lavapipe only (the headless
tests), not on RADV, ANV or NVIDIA, and not in a game yet.

- Vertex, index, constant and storage buffers are read where they are in guest memory.
- Shaders (draws, transform feedback, dispatches) write storage buffers in place.
- Textures are checked against guest memory and rebuilt on the GPU, with no CPU hash and no write tracking.

Without the extension the setting is a warning at start, and the renderer works as before. Buffers are copied into
staging and their own buffers, results land at the fence, and textures are hashed and watched with write tracking
(`plat_ww_scan`). Write tracking stays, as that fallback and for the ranges UMA leaves to it (below).

The console's GPU reads and writes the same memory its CPU does, and the game's fences keep the two apart. ScarletNX
keeps those fences faithful. A release (a syncpoint increment, a semaphore) waits for the Vulkan submit that holds the
work before it (`vk/release.c`). So the guest can't write what a recorded draw still reads, or read what a recorded
draw still writes, without breaking its own fences. A draw may then use guest memory as it is when the GPU runs it.
The copies, landings, hashes and write tracking were only there because the GPU couldn't see guest memory.

## Imports

`vk/uma.c` imports guest memory as buffers, on demand.

- A range is imported as the 64 MiB aligned chunk around it, or the two it reaches across. The span is narrowed to the
  guest memory around it whose host pages are the reservation's own and committed (`mem_private_span`: mapped, not
  the alias pool's, not I/O). Imports may overlap. A lookup takes the last one found, then a binary search.
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

## Reads and writes in place

A range is used in place (`vk_uma_read`, `vk_uma_write`) when four things hold:

- it is one run of guest memory, at an offset the binding can take;
- no landing result, queued release or storage image is pending over it;
- nothing over it is GPU-owned;
- the runtime didn't write it while recording.

Otherwise it goes the old way.

- Buffers (`buffer.c`'s `buf_get`) are bound with no Vb entry, hash, own buffer or write-tracking question.
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

A range becoming GPU-owned (`vk_own_publish`) counts as such a write. The commands recorded with the imports over it
are also submitted, and those imports are retired before its pages are trapped.

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

Write tracking (or hashing without it) still checks:

- textures over memory the runtime wrote while recording (`front`);
- depth textures;
- formats a store can't take as they are, with neither extension;
- shaders, index scans and render targets' baselines (`pipeline.c`, `render.c`, `surface.c`).

## Next

1. Batch the texture checks. Each one ends the rendering pass, once a texture in each epoch. The checks due could be
   recorded together at the first one, and checks of textures a frame doesn't change could happen less often.
2. The rebuild's CPU bookkeeping (`wgen`, the render target copies `tex_take_drawn` makes) runs at every check, as
   if the texture had changed: render targets copied into textures are copied again each epoch.
3. Aliased storage bindings (`vk_buf_draw_prepare`'s atlas) still take the copy: one import over all of them would
   do in place.
4. Windows and macOS take the same path (mem.c's hook is the same there), untested. So are RADV, ANV, NVIDIA and both
   copy extensions. Lavapipe (Mesa 25.0) lists `VK_NV_copy_memory_indirect` without its `indirectCopy` feature, so
   only the store fallback has run.
