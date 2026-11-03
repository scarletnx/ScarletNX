# Guest buffers on unified memory

On a GPU that shares system memory, guest vertex, index and read-only storage ranges are imported into Vulkan
(`VK_EXT_external_memory_host`, or on Android moved into AHardwareBuffers) and read in place: [GPU-owned guest memory](./GPU_OWNERSHIP.md#memory-that-the-cpu-and-the-gpu-share-uma)
says how, what still copies, and what keeps it correct. Every other range takes the copy path this page is about:
`vk_buf_copy` copies bytes out of guest RAM into a buffer that `vk_stage` maps and the GPU reads. Exporting the final
image as a dma-buf, as desktop compositors do, would not remove this copy: ScarletNX already presents its Vulkan image
without an app-managed dma-buf handoff.

## Measured workload

With the constant-buffer cache enabled, a 600-present Tears of the Kingdom cave interval staged 3,688,879,936
uncached bytes in 63,098 calls. The largest call was 58,848 bytes and none were zero-filled. The only nonzero-address
staging-copy path with that cache enabled is `vk_buf_copy_writable` for writable graphics storage buffers in `stage_set`. A
per-draw wait or allocation is therefore not a viable replacement for the staging ring. The test machine has a
discrete RTX 2080; it cannot establish a unified-memory speedup.

## Correctness requirements

The input to each draw is a snapshot. Guest CPU threads can write the same pages while recorded GPU work is in flight,
and GPU virtual ranges can map fragmented host pages. The current asynchronous userfaultfd write tracker reports writes
after they happen; it does not freeze pages. Guest mapping and decommit operations can also change backing memory.
Importing `g_mem` directly would let the GPU see later CPU writes, and a shader could write guest memory before the
guest's synchronization point. A direct import must therefore own and pin the imported pages, intercept writes to
them or use copy-on-write, and wait for GPU completion before remapping or releasing them.

Mapping changes now acquire the renderer's memory lock before the guest memory manager's lock. CPU backing changes land
all pending storage output because a CPU page may have several GPU aliases. GPU-VA changes land overlapping output
before changing the page table and mark the virtual range dirty for buffer caches. This closes the backing-lifetime
gap for staged outputs; it does not pin guest pages or make direct host-pointer import safe.

Graphics storage writes now retain each draw's input bytes, destination range, and GPU buffer. A completed frame
invalidates noncoherent output memory and lands only bytes the shader changed where the guest still holds the input
value. Overlapping guest reads and writes, and stream synchronization points, wait for pending outputs. Draws can reuse
an earlier result in the same frame with a storage barrier when the binding is contained and aligned and its guest input
is unchanged; other overlaps land before taking another snapshot. Frame-ring reuse processes output before freeing its
staging allocation. The compute and graphics paths share `vk_write_changed`.

The byte-value reconciliation cannot distinguish a later CPU write that restores the original value from no write.
Games must synchronize CPU and GPU writes to the same bytes for deterministic ordering. When writable bindings in one
draw share guest backing, the bindings involved in that overlap use a shared page atlas. Each such binding maps its
logical word indices to atlas pages, so partially overlapping ranges, fragmented mappings and different GPU virtual
aliases see the same bytes. The atlas starts after earlier graphics and compute output has landed. A draw whose atlas
would exceed the device's maximum storage-buffer descriptor range is skipped with a warning, rather than rendered with
independent snapshots.

The 110-second TotK cave run with graphics writeback and same-frame reuse completed 1,757 frames, compared with 1,754
for the earlier staging run on this machine. A 600-present interval reused 7,429 GPU buffers but still forced 1,985
lands, totaling 14.7 seconds of waits. The scenes and shader caches vary, so this is a correctness smoke test, not a
controlled performance comparison. The resulting cave image still has known cyan rendering artifacts.

Vulkan references: [host pointer import](https://docs.vulkan.org/spec/latest/chapters/memory.html#host-external-memory),
[host/device synchronization](https://docs.vulkan.org/spec/latest/chapters/synchronization.html), and
[placed memory mapping](https://docs.vulkan.org/features/latest/features/proposals/VK_EXT_map_memory_placed.html).
