# GPU-owned guest memory (design)

Status: phases 1 to 4 are done on Linux (below), inline-to-memory aside. The engines that worked in guest memory on
the CPU (NVDEC output, VIC, the DMA copy engine, the 2D engine) run on the GPU, with no CPU fallback. Their
results stay on the GPU: the guest memory they cover becomes GPU-owned. Every GPU consumer finds the data through
the ownership map and copies it on the GPU. Only when the guest CPU (recompiled code, or the runtime's own C code)
touches an owned range is the data written back into guest memory, the one readback the rules allow, and the range
is the CPU's again.

Render targets stay the renderer's: their pixels reach guest memory only when a consumer asks. Pitch-linear color
targets are the exception, as a game draws into those to read the result on the CPU: each release publishes the ones
drawn since the last (`vk_rt_publish_linear` in `vk/surface.c`), and storage images are published the same way
(`vk_surf_publish`). A block linear target the CPU reads gets no write-back yet.

The trap model is Skyline's (read for the design only: its source is MPL-2.0 and was withdrawn at the console maker's
request, so nothing is copied from it). The fault is taken differently on each host (below).

## The trap model

- A trap covers one producer's byte ranges (a video surface, a VIC output plane, a DMA destination) and has three
  levels, as Skyline's: none, write-only (reads pass) and read-write (every access traps). A host page gets the
  strictest level of the traps on it, so one fault serves every trap sharing the page.
- A fault on a read-write trap lands the producer's data and drops the trap: the range is the CPU's again. A read
  could keep the GPU copy (write-only), which Skyline never did because it doesn't decode the fault's direction.
  That is a later step, once the landing path is measured.
- Deadlocks: the trap's own lock is never held across a Vulkan call, a guest memory access or a wait (Skyline's lock
  callback rule). A handler that would wait for a producer drops the trap lock, waits, and looks the trap up again.
- Writeback never goes through the trapped view: the data is written through a second view of the same pages (a
  mirror), then the trapped view is opened. No other thread can see the range half written.

## Linux: userfaultfd, minor faults, the alias pool

Guest memory is a private anonymous reservation (`plat_vm_reserve`), and memory mapped at two guest addresses comes
from a memfd mapped shared (the alias pool, `plat_posix.c`). An owned span is moved into the pool when its trap is
made (`mem.c`'s `promote_nl`), so it has a mirror (`plat_alias_host`) and its edge pages keep the CPU's bytes in the
page cache.

- The span's guest views are registered with the runtime's userfaultfd (the one write tracking uses) in minor mode
  and their page table entries dropped (`MADV_DONTNEED`). A guest access then blocks the faulting thread in the
  kernel. A handler thread, in ordinary thread context, lands the GPU data through the mirror and installs the page
  with `UFFDIO_CONTINUE`. No Vulkan call, lock or allocation runs in a signal handler.
- Owned spans are not write-tracked: their consumers use the ownership map. `plat_ww_scan` skips them.
- `UFFD_USER_MODE_ONLY` (needed without privileges) means a system call that reads or writes an owned page from the
  kernel gets EFAULT rather than a fault. Every system call that takes a guest pointer (files, sockets, TLS) and
  `pread`/`pwrite` on the pool first acquires the range (`own_acquire`).
- Kernel features: minor faults on shmem 5.13, user mode only 5.11, write tracking's asynchronous write protect and
  `PAGEMAP_SCAN` 6.7. Debian 13 (6.12) has all of them.
- To be settled by tests before anything depends on it: a minor fault still arrives on a span that write tracking
  scanned before (write-protect markers left after the zap), re-registering a span replaces its modes, and a
  kernel-mode access gets EFAULT.

A Linux host without minor faults (older kernels, Android's 5.10 kernels, or any Linux with `SNX_TRAPS=signal`)
uses Skyline's own shape: `plat_trap_arm` makes the span's views inaccessible with `mprotect`, and the SIGSEGV
handler, checked before the crash handler, posts the page to the trap's own thread through a lock-free ring and waits
on a futex. That thread lands it as the userfaultfd's would, and `plat_trap_continue` opens the pages and wakes the
waiters, whose accesses then go again. Nothing but atomics, `mprotect` and futexes runs in the handler. FreeBSD and
macOS have no alias pool yet, so no mirror and no traps.

## Windows, macOS

- Windows: `VirtualProtect(PAGE_NOACCESS)` on every view of an owned span and a vectored exception handler, which runs
  in normal context on the faulting thread. The mirror needs the alias pool, which Windows doesn't have yet:
  placeholders (`VirtualAlloc2`) and pagefile section views (`MapViewOfFile3`).
- macOS: Mach exception ports deliver the fault to a handler thread with the faulting thread suspended (the
  userfaultfd shape), and `mach_vm_remap` gives a mirror.
- A host with no correct trap doesn't make ranges GPU-owned: a producer's results are copied at its own fence into
  host-cached memory and landed, as render target writebacks are now. The GPU still does the work, nothing is
  skipped, and the mode is logged at startup.

## The ownership map

`src/nx/memory/own.c`, over a `plat_trap_*` layer per host:

| Call | What |
| --- | --- |
| `own_begin(cpu, size, producer, key)` | the range is the producer's from now on: its trap is made, older owners of the bytes split |
| `own_lookup(cpu, size, cb)` | GPU consumers (texture and buffer uploads, the vi present, VIC inputs) find the GPU copy to copy from |
| `own_acquire(cpu, size, mode)` | the runtime's own C code before it touches guest memory: read, write, or discard (bytes it overwrites whole skip the readback) |
| `own_drop(range)` | unmapped or freed memory: no writeback |

- Ranges are byte exact, keyed by their pool offset so every alias of a page finds the same owner. A sorted array or
  interval tree under a leaf lock, an atomic "anything owned" flag and a page bitmap keep the common path (nothing
  owned) a load and a branch in `gmem_*`.
- Lock order: `vk_lock`, then mem.c's lock, then the trap lock, then the map's lock. A landing takes `vk_lock` before
  it marks its entries, lands them, lets their pages go and releases their resources, then lets `vk_lock` go. Every
  thread that waits for a mark (publishing, dropping or landing the same bytes) holds `vk_lock` too, so the mark it
  waits for is never another thread's that waits for `vk_lock` in turn.
- Who may fault on owned memory: a thread holding the kernel's lock may, since a landing takes none. That holds only
  while no thread holding `vk_lock` waits for the kernel's lock: the guest thread faults, the handler waits for
  `vk_lock`, and its holder waits for the kernel's lock. A syncpoint a release performs under `vk_lock` signals the
  kernel events waiting for it once `vk_lock` is let go (`syncpt_add_quiet`, `syncpt_events`). Leaf locks such as
  nvdrv's `sync_lk` and the GPU queue's lock are taken under `vk_lock`, and no holder of one faults. A thread holding
  mem.c's lock may not fault, since `vk_lock` holders wait for it (publishing, every mapping change): mem.c lands what
  it touches first (`own_acquire`).
- A fault by the thread holding `vk_lock` on an owned range is a bug in the runtime (the GPU thread touching it
  without `own_acquire`): fatal, with the range, when that thread is waiting in the fault. A late fault message, from a
  thread that has taken `vk_lock` since, lands as any other. Its fault on a page no entry is on any more (landed, not
  let go yet) goes on.
- Before that fatal, `ownership-fault:` lines record the read/write direction, guest page and host page, whether
  the wait was confirmed, and up to sixteen overlapping owners (guest ranges, pool offsets, resource identities,
  resource offsets and landing state). The address is page-granular, not the exact byte accessed.
  On Linux the report reads the **blocked thread's** PC and SP from `/proc/self/task/TID/syscall`, then records its
  executable mapping and file offset. It scans at most 128 native stack words for executable addresses, explicitly
  labeled `stack-candidate`: these may be saved callers or unrelated values, **not an unwound backtrace**. It never
  reads shared guest pool pages for this scan or resumes the thread. Missing procfs/register/stack access is logged.
  Use the build revision printed at startup and that build's unstripped binary/debug symbols to resolve addresses;
  a file offset must be translated through the ELF load segments before using it as an addr2line address.
  See https://man7.org/linux/man-pages/man5/proc_pid_syscall.5.html for the blocked-thread register format.
- Landing is a memcpy into the mirror. A producer's GPU copy keeps the range's layout (block linear surfaces are
  swizzled on the GPU), and covers the range's padding: it is filled from guest memory when the producer takes the
  range.
- The runtime's reads land only the bytes they read. `gmem_read` (`copy_runs`) reads a run none of whose bytes are
  owned through `own_stage`, which reads a trapped page through the mirror, so an owner beside the run on its pages
  stays on the GPU. A shader's code window (`gmem_read_code_cpu`) stops at the first owned byte, and lands only when
  the program reaches that byte (`gmem_code_cut`). Before, each of these landed GPU results in one game several times
  a frame.

## Memory that the CPU and the GPU share (UMA)

The console has one memory. A game writes vertices, indices and constants with the CPU, and the GPU reads the same
bytes with no copy. The renderer's buffer cache (`vk/buffer.c`) is a copy design: it checks each guest range at each
guest submit and copies what changed into a staging ring, and the results of shaders come back through a readback.
On a discrete GPU those bytes cross PCIe. On a GPU that shares system memory (an integrated GPU: a Steam Deck, a
laptop, a phone) the staging ring is the same memory as guest memory, and the copy is only cost: CPU time on the GPU
thread to hash and copy, and memory bandwidth.

### The memory model

`vk/uma.c` decides at device creation, and the log says it on one line (`vulkan: the device ...`):

- UMA: the device type is `VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU` (or CPU), or its biggest device-local heap is
  host-visible and holds most of the memory.
- Discrete with resizable BAR: device-local, host-visible memory is at least half of the biggest device-local heap.
- Discrete with a small BAR: anything else. An RTX 2080 without resizable BAR has a 246 MiB window.

The `[Debug]` setting `vk_memory` (also `--vk_memory=N`) chooses the path for guest buffers: 0 auto (in place on
UMA, copied otherwise), 1 copy, 2 in place. In place needs `VK_EXT_external_memory_host`. Without it the log warns
and the path is the copy. On a discrete GPU, 2 works but the GPU reads guest memory across the bus: it is for testing
correctness there, not for speed.

With resizable BAR, the staging ring of streamed uploads goes in device-local memory that the CPU maps
(`vk_stage_upload`, within the heap's budget): the CPU's sequential writes cross the bus once, and every draw that
reads them reads local memory. With a small BAR the ring stays in host memory (the window is shared with the whole
desktop, `cmd.c`).

### What the GPU reads in place

Vertex and index ranges and read-only storage buffers (`vk_buf_get_input`, `vk_buf_get_window`). For each range,
`buf_get` first checks the pending results as before (a graphics or compute result that has not landed, which only
the GPU holds) and GPU ownership (`own.c`). If guest memory holds the bytes, `vk_uma_get` binds the import that covers
them, at the range's offset: no hash, no copy, no own buffer, no window.

Guest memory is imported with `VK_EXT_external_memory_host` in pieces. An import covers the 2 MiB windows that a range
touches, clipped to mapped guest memory. Imports never overlap: a range across two imports makes one import of their
union. At most 512 imports, the least recently used goes past that, and an import unused for 600 presents goes. A
window that the driver refuses is kept in a list of refused spans for 600 presents, or until its pages change, and
its ranges are copied. On an RTX 2080 a span with one page the driver cannot pin (a `PROT_NONE` page) fails as a whole
with `VK_ERROR_OUT_OF_DEVICE_MEMORY`, while anonymous and shared (memfd, the alias pool's) pages import. Before the
list, every range in such a window asked the driver again, and one scene fell from 28 to 5 frames a second. The whole guest heap is never one import. A driver that checks each import's
pages at each submit (amdgpu's user pointers) pays for the bytes imported, and LASRecomp measured 45 ms of driver
time a frame on AMD with the whole heap as one import.

### What still copies, and why

- Constant buffers. The GPU front end writes them between draws (`CONST_BUFFER_DATA`, `r3d_const_upload`) on the CPU,
  when the GPU thread meets the method. A draw recorded before the write but run by the GPU after it would read the
  new constants. In place, they need the write done on the GPU in order (`vkCmdUpdateBuffer` into the import), with a
  CPU copy for the runtime's own readers (bindless texture handles are read from constant buffers when a draw is
  recorded).
- Writable storage and transform feedback. Their results land through the pending results (`GfxOut`), which the
  runtime's own readers (texture uploads, the shader translator, `gmem_need`) wait for. In place, the GPU writes guest
  memory at once, and that is the next step: the pending results then only need the fence, not the copy back. That is
  where the `vk_finish` landings of the agreed work go away on UMA.
- Ranges under pending results, and GPU-owned ranges: their bytes are not in guest memory.
- Textures and render targets: their layout differs from guest memory (block linear, formats the host lacks), so they
  are always made on the GPU from a staging copy.
- Compute dispatches read their storage through `vk_buf_get_strict`: copied.

### What keeps it correct

- Releases. The copy path read guest bytes when a command was recorded, so a release (a syncpoint, a semaphore) could
  be performed once the work before it landed and the stream stopped (`release.c`). In place, the GPU reads the bytes
  when it runs the command, and after a release the guest writes the next frame's data there. So a release the stream
  stops holding also waits for every command recorded up to then that read guest memory in place
  (`hold_inplace`, `vk_uma_bound`), the commands after it too, which the console's GPU runs at once.
  `tests/vulkan_release.c` checks it.
- Pages that change under an import. The driver pins the pages it imported. When mem.c gives guest pages back
  (unmap, decommit), or moves them into the alias pool (`promote_nl`), the host pages at that guest address are other
  pages, and the import still has the old ones. mem.c tells the renderer (`mem_set_pages_replaced`), and the imports on
  those pages go at the next lookup. `vulkan_headless` (`check_uma`) unmaps and maps a range again, and checks that the
  GPU reads the new bytes.
- Hazards. A GPU-side write to guest memory (a front-end write, a landed result) on a page that a command not finished
  yet reads in place can change what that command reads. They are counted (`gpu_set_write_watch`), not prevented, and
  the report every 600 presents gives the count:

  `vulkan: in place over 600 presents: N ranges (M MB) read in guest memory, N copied; N imports made (M MB, T ms),
  N failed, N dropped; N imports hold M MB; N hazards (...)`

  The count is by 4 KiB page, so a constant buffer next to vertices on one page counts too.

### Why imports and not placed maps (yet)

`VK_EXT_map_memory_placed` maps device memory at an address the caller gives. With it, the guest heap would be driver
memory, with nothing to import or pin. This host's driver has it (`memoryMapPlaced`, not `memoryMapRangePlaced`). It
is not used yet, for these reasons:

- Write tracking and the traps use userfaultfd on guest memory (above). The kernel registers userfaultfd only on
  anonymous, shmem and hugetlb mappings, and a mapping of driver memory is none of those. The texture cache and the
  shader cache would lose write tracking on those pages and hash them again.
- The alias pool maps one page at two guest addresses (`svcMapMemory` views, code). A placed map maps one memory object
  once, at one address.
- Without `memoryMapRangePlaced`, only a whole memory object maps placed, so the heap would be one object or one per
  mapping.

Placed maps need write tracking by protection for those ranges (the signal trap path has it) and aliases by `mremap`.
That is the step after imports, and it is the one that removes the per-submit cost of user pointers on amdgpu.

### Measurements on a discrete GPU (RTX 2080, 246 MiB BAR)

Each scene is the benchmark save and script of one game, `-H --fps_limit=0 --vsync=0 --nvn_direct=2`, with
`vk_gpu_profile=1` for the queue's times. The machine ran other jobs (load 4 to 16), so a single frame rate moves by
several frames a second from run to run.

Where the time goes in the copy path, measured before the branch moved to a newer base: a present's time, the GPU's
busy time in its command buffers, and its idle time between submissions, split by what the CPU did. The present's
time includes the profile's own cost, so it is longer than the frame rate gives:

| Scene | Present | GPU busy | Idle, CPU still recording | Idle, after a GPU wait of the GPU thread | Idle, submitted, not started | PCIe RX / TX |
| --- | --- | --- | --- | --- | --- | --- |
| A (a town street, 28 fps) | 35.9 ms | 15.3 ms | 10.6 ms | 2.3 ms | 7.8 ms | 576 / 239 MB/s |
| B (a field, 28 fps) | 36.8 ms | 11.5 ms | 19.2 ms | 0.9 ms | 5.2 ms | 243 / 279 MB/s |
| C (a tower, 30 fps) | 44.5 ms | 8.7 ms | 24.9 ms | 0.0 ms | 10.8 ms | 243 / 123 MB/s |
| D (stairs, 37 fps) | 27.4 ms | 11.8 ms | 12.2 ms | 0.1 ms | 3.4 ms | 448 / 910 MB/s |

The GPU is idle for 60 to 80% of a present, and nearly all of that is the GPU thread recording slower than the GPU
runs. Round trips (the GPU thread waiting for the GPU, then landing results) are 0 to 2.3 ms a present, and no
`vk_finish` waits are left in these scenes. The bus carries well under 1 GB/s each way, against about 12 GB/s that
PCIe 3.0 x16 gives. These scenes are bound by the CPU's work on the GPU thread, not by PCIe or GPU stalls. "Submitted,
not started" (3 to 11 ms) is the recording thread's lag and the compute queue's semaphores: the next thing to look at.

In place forced on this GPU (`--vk_memory=2`) ran all four scenes to their end with no validation message and with
screenshots that differ from the copy path's only where the scene moves (animation, the time of a camera move). That
build asked the driver again for every range in a refused window: scene B fell to 5 frames a second, the reason for
the list of refused spans. Scene D's imports were all refused (a page in each window the driver could not pin), so it
copied as before but paid for the releases' longer gates (29 frames a second against 37 copied).

The same two scenes A and B on the newer base, two rounds of the unchanged runtime against `--vk_memory=2`, with no
profile:

| Scene | Copy, frames a second | In place, frames a second | Copy, PCIe RX / TX | In place, PCIe RX / TX |
| --- | --- | --- | --- | --- |
| A | 20.2, 14.9 | 35.0, 36.7 | 998 / 315, 322 / 130 MB/s | 1303 / 400, 1231 / 381 MB/s |
| B | 23.9, 27.8 | 29.0, 29.2 | 237 / 267, 192 / 210 MB/s | 409 / 1072, 464 / 1131 MB/s |

With the profile, scene A's copy path took 103 ms a present, 45 ms of it with the GPU idle while the GPU thread
recorded, and staged 5.5 GB every 600 presents. In place it took 29 ms, 13 ms of that idle, and staged 433 MB. The
time that went is the GPU thread's hash and copy of each changed vertex range: the cost that a GPU sharing memory pays
for nothing. Scene B was the same either way with the profile (34 ms a present). There the in place run put the
frame's device ring in host memory (25 GB every 600 presents across the bus), which the copy run did not: why is not
found yet, and it is where scene B's higher TX comes from.

The screenshots matched in both scenes, but for scene A's last one (difference 32 to 35 of 255, two copy runs 3):
in place ran at twice the frame rate, and the game's camera had turned further by then. In place capped at 18 frames a
second (`--fps_limit=18`) matched the copy runs there too (1.6 and 3.9). Under the validation layer with
synchronization validation, scene A gave the same 30 messages in both modes (pipeline interface and output warnings,
and a write-after-write of two image-to-buffer copies, all in the copy run too).

The hazard counter found 2,500 GPU-side writes every 600 presents in scene A on pages that unfinished work read in
place, 300 in scene B. They are counted by page, so constant buffers beside vertices count too, and the screenshots
show nothing. Which of them are real is not traced yet: that comes before in place is the default anywhere.

### Testing on a Steam Deck (or any integrated GPU)

Nothing of the game leaves the machine: the steps below compare numbers and screenshots on the Deck itself.

1. Build the runtime from this branch on the Deck (or take the build it gives), and check the log's line
   `vulkan: the device shares system memory (UMA); guest buffers read in place where they can be (vk_memory=auto)`.
   If it says `copied`, the driver has no `VK_EXT_external_memory_host`: say which driver it is (the `vulkan:` line
   with the device name and driver version).
2. Pick a scene that comes back the same way each run: a save, and a script of inputs that ends there
   (`--script`, `--press_len`, `snx-run` skill), with a screenshot every few seconds (`--shot=DIR/s --shot_every=60`).
   Run with `-H --fps_limit=0 --vsync=0 --vk_gpu_profile=1 --vk_buffer_stats=1` so that the frame rate is the
   renderer's, not the display's.
3. Run the scene three times with `--vk_memory=1` (copy) and three times with `--vk_memory=2` (in place), one after
   the other, with nothing else running. Keep each run's log and screenshots apart.
4. Compare, for each pair:
   - the frame rate over the same span of the scene (the presents between two screenshot times);
   - the screenshots at the same script times: they must match as closely as two copy runs match each other. A
     difference only in place is a bug: keep both screenshots and the two logs;
   - the lines `vulkan: GPU queue over N presents (X ms each): busy ...` and the `idle` lines after it: in place should
     take CPU time off the GPU thread ("the CPU still recording the next one" goes down);
   - `vulkan: in place over 600 presents: ...`: the ranges read in place against the ones copied, the imports that
     failed (amdgpu may refuse user pointers to memory that isn't anonymous, such as the alias pool's: those ranges are
     copied), the megabytes the imports hold (amdgpu checks every import's pages at every submission: if the frame
     rate falls as that number grows, say so with the number of submissions a present), and the hazards;
   - `vulkan: staged by reason` and `ring MB over 600 presents`: the bytes copied, which in place should cut.
   - any `vulkan validation:` lines with `--vk_validation=1` (one short run in each mode, the layer from the
     distribution's package), and any `device lost`.
5. Report the numbers of the three pairs, not one: a single run's frame rate on a handheld moves with its temperature
   and power limit.

## Phases

1. Done. The trap layer and the map, Linux first, with `tests/memory_own.c` (synthetic owners, no Vulkan): read and write
   faults return the owner's bytes, a neighbor's writes on an edge page are not lost, the write-tracking markers
   question, every alias of a page, system calls with and without acquire, unmapping, a fault under `vk_lock`, and a
   stress run of threads reading and writing while owners come and go.
2. Done. The consumers: texture uploads, render target loads, guest buffers, the vi present and VIC inputs look the map up
   and copy on the GPU.
3. Done. NVDEC and VIC: FFmpeg stays the decoder. Its planes are staged whole and laid out on the GPU (NV12, block linear)
   into GPU-owned surfaces. VIC composes from them on the GPU into a GPU-owned output, with no fence wait and no copy
   back. `compose_cpu` goes: large jobs go in bands, and the shader takes the cases the GPU path refuses now.
   Vulkan Video decode is an option, off by default and marked experimental: the `vulkan_video` setting gives FFmpeg
   this device with a decode queue (`vk/nvdec.c`), and its H.264 pictures are copied out of their images on the GPU.
   VP9 stays on the CPU decoder (the NVDEC probability patch is in FFmpeg's software VP9). It has not run on a GPU
   with Vulkan Video yet: lavapipe has no decode queue.
4. Done, inline-to-memory aside. DMA, 2D, inline-to-memory: every copy, layout change, remap and fill as a GPU copy or dispatch from the source's
   owner (or its guest bytes, staged whole) into a GPU-owned destination. `f2d_blit_memory`, the DMA engine's CPU
   loops and the render target flush and reload around them go. The clear before the renderer starts is kept for
   the renderer. Inline-to-memory stays a CPU write: its data is born on the CPU (the pushbuffer), and the runtime's
   own readers of what it writes (the shader translator, MME macros) would land every GPU-owned copy of it again.
   `vulkan_headless` holds the DMA engine and the 2D blit to `dma_ref.c` and `fermi2d_ref.c` on lavapipe.
5. Windows and the other hosts. Linux without minor faults and Android trap by protection now (`SNX_TRAPS=signal`
   tests that path on any Linux). Windows, macOS and FreeBSD need the alias pool first (placeholders and section
   views on Windows, a shared memory object on the others). Until then their `plat_trap_*` fail, and a published
   result lands at once (a readback per result, correct and slow). The environment variable `SNX_NO_TRAPS` makes a Linux host take that path, to measure and test it
   there.
6. Started: memory the CPU and the GPU share (above). Guest vertex, index and read-only storage ranges are read in
   place on a GPU that shares memory (`vk_memory`). Next: writable storage in place (its results then need the fence,
   not the copy back), constant buffers with the front end's writes done on the GPU in order, the hazards traced, and
   placed maps once write tracking works without userfaultfd on those pages.
