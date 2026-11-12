# GPU-owned guest memory (design)

## Index

* [The trap model](#the-trap-model)
* [Linux: userfaultfd, minor faults, the alias pool](#linux-userfaultfd-minor-faults-the-alias-pool)
* [Windows, macOS](#windows-macos)
* [The ownership map](#the-ownership-map)
* [Memory that the CPU and the GPU share (UMA)](#memory-that-the-cpu-and-the-gpu-share-uma)
  * [The memory model](#the-memory-model)
  * [What the GPU reads in place](#what-the-gpu-reads-in-place)
  * [What still copies, and why](#what-still-copies-and-why)
  * [What keeps it correct](#what-keeps-it-correct)
  * [Android: guest memory moved into AHardwareBuffers](#android-guest-memory-moved-into-ahardwarebuffers)
  * [Placed maps](#placed-maps)
  * [Measurements on a discrete GPU (RTX 2080, 246 MiB BAR)](#measurements-on-a-discrete-gpu-rtx-2080-246-mib-bar)
  * [Testing on a Steam Deck (or any integrated GPU)](#testing-on-a-steam-deck-or-any-integrated-gpu)
  * [The copy path, and what keeps it correct](#the-copy-path-and-what-keeps-it-correct)
* [Phases](#phases)

Status: phases 1 to 4 are done on Linux (below), inline-to-memory aside. The engines that worked in guest memory on
the CPU (NVDEC output, VIC, the DMA copy engine, the 2D engine) run on the GPU, with no CPU fallback. Their
results stay on the GPU: the guest memory they cover becomes GPU-owned. Every GPU consumer finds the data through
the ownership map and copies it on the GPU. Only when the guest CPU (recompiled code, or the runtime's own C code)
touches an owned range is the data written back into guest memory, the one readback the rules allow, and the range
is the CPU's again.

A storage buffer a draw writes stays on the GPU the same way (`keep_new` in `vk/buffer.c`), as a dispatch's does
(`res_bind` in `vk/compute.c`). Where the binding is whole pages of one run of guest memory that the CPU hasn't made
land in the last 1200 presents, its bytes are GPU-owned from the bind on, by the owner holding them or by one made from
memory, and the draws write that owner in place. The trap lands it after the submission that wrote it, so the CPU
reads the draws' stores and writes after them, and nothing is compared. Any other binding is copied, read back at its
fence and merged by the byte with what the CPU wrote meanwhile, but with unified memory, where the draws write guest
memory in place (below).

With the page cache writing (`vk_buffer_pages=2`, `vk/pagespace.c`), a binding that is not whole pages stays on the GPU too
(`keep_pages`). The cache keeps guest memory by page in sparse buffers, one for each GiB of the guest's address space.
It brings the pages the range is on up to date, the draw writes the cache's buffer in place, and the range is
published GPU-owned, each 64 KiB granule's part under an owner of that granule, so a landing waits only for the
submission that last wrote that granule. Only the range's own bytes are published. The runtime's reads of guest
memory land an owner only for bytes it holds, and a game keeps the constant buffers its draws read beside the buffers
they write. The ownership table is what tells the cache which of its pages have such bytes: such a page is trapped,
so the region holds the truth of all of it, it takes nothing from guest memory, a later read of it binds the same
buffer with no copy, and when the table lets go of it the stamp its publish left makes the cache take memory's bytes
again. A page the game's CPU code touches lands, and its granule takes the copies again for 1200 presents. A range
on pages the CPU wrote in the last 120 presents is not kept at all: a game that writes such pages every frame would
land them every frame. No result of another kind may be pending on the pages when they are published, because its landing
would write guest memory under the trap. A draw's write and another's read now share one buffer, so the cache keeps
the ranges read and written in place since the last barrier of a submission: a read of bytes written since gets a
barrier first, and a write gets one when the front end orders its draws by their place in the stream or the game
asked for a dependency. What a submission wrote in place is stamped as written by the GPU when the submission goes
to the queue. A granule nothing used for 120 presents lands before its memory goes back.

Render targets stay the renderer's: their pixels reach guest memory only when a consumer asks. Pitch-linear targets
are the exception, as a game draws into those to read the result on the CPU: each release publishes the ones
drawn since the last (`vk_rt_publish_drawn` in `vk/surface.c`), and storage images are published the same way
(`vk_surf_publish`): a dispatch's after it, a draw's at the release (`vk_surf_keep_begin` and `vk_surf_keep_end`), where
the image's span is whole pages the CPU hasn't touched lately and write tracking answers for it. A publish takes the
image's own texels and the targets it copied in, and leaves the bytes between its layers to their owners: a view of one
level of a mip chain has the other levels there. A page the CPU writes
between the draw and the release keeps the CPU's bytes. Any other draw's storage image keeps its write-back, which
reads its bytes before the draw and merges them by the byte.

A block linear target the CPU reads is rare, but it occurs: one game draws its save's thumbnail into a 320x180
target, and its CPU code reads the texels from memory with no call to the GPU, and a program may clear a depth target
with the 3D engine, wait for the GPU and read it from memory. To publish each block linear target at
each release would copy tens of megabytes a frame. So a release *watches* the ones drawn since the last
(`own_watch`, through `vk_own_watch`): a watch is an entry with no resource. Its pages are trapped as an owner's, but
it holds no bytes. `own_lookup` and `own_stage` skip it, and its CPU bytes stay readable through the mirror (in place,
`own_watch` copies each page it arms into a shadow). `own_hash` and `own_trapped` see it. A watch goes only into the
gaps between real owners, and an owner that comes later (`own_begin`) replaces it. A target that the CPU read lately
(`vk_own_cpu_read`), or that an owner already covers, is published at the release instead, as a pitch-linear one.

The CPU's first access to a watched page lands it as any other. Before the landing marks an entry, with `vk_lock`
held, it calls the renderer's `resolve` hook (`vk_rt_watch_hit`). That publishes the targets that are pending over the
page (`vk_surf_publish`, which swizzles a block linear target on the GPU into its owner), and the owners that it made
replace the watches. Then the watches that are left on the page go whole, and the landing lands the new owners. So the
CPU reads the pixels with one wait for the GPU, and a target that the CPU never reads costs one trap at its first
release and nothing more. A watch over memory that is freed or given to other data costs one fault, which finds no
pending target and only drops the watch.

The thread that held `vk_lock` before it touched a watched page (the GPU thread, which reads guest memory in a draw's
setup) cannot wait for a publish. Its `own_acquire`, or its fault on a page with only watches on it, drops the watches
and reads the CPU's bytes, as it did before there were watches. That is not fatal, as a fault on a real owner is. The
readers that test for trapped pages before a direct read see watches: `gmem_r32` and the ASTC content hash through
`own_hash`, `guest_held` and the staging ring choice through `own_trapped`. Both test whole host pages, so they also
see the edge pages that an owner shares. When a target
over watched bytes cannot be published, the write-back (`vk_surf_flush`) drops its watch before it writes memory. A
thread that waits on the page then can read it before the write ends. That occurs only when the publish fails (no
memory for its buffer).

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
- Owned spans are not write-tracked: their consumers use the ownership map. `plat_ww_scan` skips them. A span the
  trap lets go is mapped with `UFFDIO_CONTINUE` and registered for write tracking again while no scan runs: it reads as
  written once, with what the woken thread writes.
- `UFFD_USER_MODE_ONLY` (needed without privileges) means a system call that reads or writes an owned page from the
  kernel gets EFAULT rather than a fault. Every system call that takes a guest pointer and `pread`/`pwrite` on the
  pool first acquires the range (`own_acquire`): fs.c's IFile Read and Write and IStorage Read (the RomFS, a DLC's,
  a file's), bsd.c's receives and sends. The rest copy through host buffers or touch guest memory in user mode
  (TLS through OpenSSL, DNS, LDN, audio, the services' replies), which faults and lands.
- Kernel features: minor faults on shmem 5.13, user mode only 5.11, write tracking's asynchronous write protect and
  `PAGEMAP_SCAN` 6.7. Debian 13 (6.12) has all of them.
- To be settled by tests before anything depends on it: a minor fault still arrives on a span that write tracking
  scanned before (write-protect markers left after the zap), re-registering a span replaces its modes, and a
  kernel-mode access gets EFAULT.

A Linux host without minor faults (older kernels, Android's 5.10 kernels, or any Linux with `memory_traps=1`)
uses Skyline's own shape: `plat_trap_arm` makes the span's views inaccessible with `mprotect`, and the SIGSEGV
handler, checked before the crash handler, posts the page to the trap's own thread through a lock-free ring and waits
on a futex. That thread lands it as the userfaultfd's would, and `plat_trap_continue` opens the pages and wakes the
waiters, whose accesses then go again. Nothing but atomics, `mprotect` and futexes runs in the handler. FreeBSD and
macOS have no alias pool yet, so no mirror and no traps.

## Windows, macOS

- Windows traps by protection, as Linux's `memory_traps=1` does, but with no alias pool and so no mirror
  (`SNX_PLAT_TRAPS_IN_PLACE`, `plat_win32.c`); `memory_traps` 0 and 1 both mean this there, and 2 none.
  - Guest memory is one `VirtualAlloc` reservation with the write watch, and a view of mapped memory (MapMemory, code
    memory) is a copy, so a page has one address the CPU can reach. Owned memory stays the reservation's own pages
    (`mem_own_prepare` without the pool: one run, its "pool offset" its address). mem.c lands what it copies under its
    lock first (a copy view written back, code memory mapped or unmapped), as it can't fault there.
  - `plat_trap_arm` marks the pages in a bitmap (a bit per 4 KiB page of the reservation) and makes them
    `PAGE_NOACCESS`. A vectored exception handler, first in the chain (the crash handlers leave armed pages alone),
    takes an access violation on an armed page (`ExceptionInformation[1]` the address, `[0]` read or write) on the
    faulting thread, posts the page to the trap thread through the same lock-free ring as Linux's signal handler, and
    waits (`WaitOnAddress`) until a trap lets pages go; then it continues execution, and the access goes again. A
    fault on a page let go between the fault and the handler goes again; the same fault again with no page opened
    since is no trap's, and goes on to the crash handlers, and so does one that faults 4096 times in a row while other
    traps open pages (said once, with the page's state: an inaccessible page no trap holds is a bug). The trap
    thread's own fault is never waited for. `plat_thread_where` says which threads wait in a trap, and where.
  - A landing can't go through a mirror: own.c lands into a staging copy while the pages stay trapped, then
    `plat_trap_fill` opens them a page at a time, each just before its bytes go in, and let_go wakes the waiters. A
    thread that touches a page while its bytes are going in, without faulting, can see it half written: the one
    difference from the mirror's landing. The window is one page's memcpy, after a wait for the GPU of milliseconds
    in which every other access faults and waits, so it takes a thread that first reaches the page in that very
    moment: `tests/memory_own.c`'s stress, eight threads reading 32 pages at random as fast as they can, starting while
    the landing runs, finds about 25 of its 320,000 reads there, each the owner's byte once the landing is done. Each
    page opened is a system call (opening an inaccessible page takes no TLB shootdown): a 2 MiB landing makes 512 of
    them. Every entry on a page a landing opens lands with it (`close_pages_nl`), so no opened page shows an owner's
    stale bytes. A page let go is written once (an atomic OR of 0), so the write watch reports it, as Linux's tracking
    reports a page let go.
  - The CPU's bytes on a trapped page can't be read through a mirror either. An edge page, armed with CPU bytes on
    it, keeps a copy of them (its shadow) from when it was first armed: no CPU write changes them while it is armed.
    `own_stage` reads the shadow; a page without one (bytes `own_drop` uncovered on a page an owner keeps) lands before
    `own_stage` reads it. Neither a shadow's copy nor its read faults with a lock of ours held: the copy is taken with
    `ReadProcessMemory` (`plat_vm_read`, which fails rather than faults), and a read of it goes through a buffer of the
    thread's. The write watch's scan leaves armed pages out, and the landed pages' bytes that the write tracking
    compares later (`vk_ww_landed`) are read with `plat_vm_read` too, as such a page may be trapped again or given
    back.
  - A system call over an armed page fails rather than faulting: `ReadFile` into one and `WriteFile` from one with
    `ERROR_NOACCESS`, Winsock with `WSAEFAULT`, as Linux's with `EFAULT`. The runtime acquires every range it hands
    the kernel (below). Code in a system DLL that reads or writes guest memory in user mode (SDL's audio queue, OpenSSL,
    `WideCharToMultiByte`) faults into the handler like the guest does.
- On an RTX 5080 (Windows 11, driver 621.14, resizable BAR, guest buffers copied), one Ounce game's intro movie
  (`--vk_gpu_profile=1`, the same 600 presents): 16.7 frames a second without traps, 54 to 55 with them. The GPU's
  frame, first pass to last, 56.0 ms against 16.1; its dispatches and copies 19.7 ms against 6.4, the GPU mirror's
  harvest 11.5 ms against 1.4. With `--vk_buffer_stats=1` the mirror landed 47.7 GB in 600 presents without traps
  (11.6 million pages written), 40 MB with them; storage images and the mirror's pages stay GPU-owned. What still
  lands, about 1,000 times in 600 presents, is the movie's pictures: the native video service writes each one into
  its image on the GPU (`nvdec_write`, NV12 at a pitch of 2048: 2.2 MB of luma and 1.1 MB of chroma), and the game's
  CPU reads it, so on a discrete GPU it comes back. A GPU that shares memory writes them in place and lands nothing.
  The screenshots match the run without traps. One run in about thirty stopped during loading (no frame for 15 s,
  three guest threads at one light message queue, two of them waiting in a trap whose message the trap thread never
  took: it was blocked in an earlier landing). It was not seen again in about thirty more runs, with empty shader
  caches as the stopped run had, or with every thread's stack dumped when a stop came; the cause is not found.
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
- Lock order: `vk_lock`, then the renderer's `pend_lock`, then mem.c's lock, then the trap lock, then the map's lock.
  Every mapping change holds `pend_lock` while it changes memory, and so does the renderer when it adds a pending
  result or an owner. A change of guest memory alone (`GPU_MAPPING_GUEST_ONLY`: map, unmap, MapPhysicalMemory,
  UnmapPhysicalMemory) where the renderer has no pending result, no owner on its host pages and no GPU mapping of it
  takes no `vk_lock`. A landing first waits for the GPU's work on its entries with no lock held. Then it takes
  `vk_lock` before it marks its entries, lands them, lets their pages go and releases their resources, then lets
  `vk_lock` go. Every
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
  With traps by protection (no userfaultfd minor faults, as on the Android phones reported so far, or
  `memory_traps=1`) the thread waits in its handler's futex, so `/proc` shows only that wait. There the handler keeps
  the access's context instead: the report gives its PC, SP and exact address (`access-address=`), on aarch64 the link
  register too (`lr[0]`, the caller of a leaf such as memcpy), and scans the stack from that SP, bionic's named thread
  stacks included.
- Landing is a memcpy into the mirror (on Windows into the trapped pages in place, opened as it goes). A producer's
  GPU copy keeps the range's layout (block linear surfaces are swizzled on the GPU), and covers the range's padding:
  it is filled from guest memory when the producer takes the range.
- The runtime's reads land only the bytes they read. `gmem_read` (`copy_runs`) reads a run none of whose bytes are
  owned through `own_stage`, which reads a trapped page through the mirror (on Windows its shadow), so an owner beside
  the run on its pages stays on the GPU. A shader's code window (`gmem_read_code_cpu`) stops at the first owned byte, and lands only when
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
UMA, copied otherwise), 1 copy, 2 in place. In place needs `VK_EXT_external_memory_host`, or on Android with 2
`VK_ANDROID_external_memory_android_hardware_buffer` ([below](#android-guest-memory-moved-into-ahardwarebuffers)).
Without either the log warns and the path is the copy. Auto with the second alone copies, and the log says
`(vk_memory=auto: AHardwareBuffers only with vk_memory=2 or uma_memory=2)`. On a discrete GPU, 2 works but the GPU
reads guest memory across the bus: it is for testing correctness there, not for speed.

On a device with both extensions, the `[Debug]` setting `uma_memory` compares the two ways where guest memory is in
place: 0 auto (imported, and moved only without the first extension and with `vk_memory=2`), 1 imported only (copied
without the first), 2 moved into AHardwareBuffers only, under auto as well (copied without the second).

With resizable BAR, the staging ring of streamed uploads goes in device-local memory that the CPU maps
(`vk_stage_upload`, within the heap's budget): the CPU's sequential writes cross the bus once, and every draw that
reads them reads local memory. With a small BAR the ring stays in host memory (the window is shared with the whole
desktop, `cmd.c`).

The host memory of a ring that the CPU fills is of one of two kinds (`vk_stage_memory`, `stage_cached` in `cmd.c`).
Memory for writes in order (`DM_HOST` in `devmem.h`) is a type that is not cached where the device has one, and a
driver can map that write-combined. Cached memory (`DM_CACHED`) is a cached type. Cached is the default only on Intel's
Mesa driver, where both types are the same system memory. `snx-test-vulkan -H --bench-stage-memory` times the CPU's
copies into each kind on the device it runs on, for block sizes from 64 bytes to 16 MiB. On an RTX 2080 the type that
is not cached takes them faster, 7,300 to 9,700 MB/s against 5,100 to 8,900, so that device keeps it.

### What the GPU reads in place

Vertex and index ranges and read-only storage buffers (`vk_buf_get_input`, `vk_buf_get_window`), a dispatch's too
(`vk_buf_get_strict`). For each range, `buf_get` first checks the pending results as before (a graphics or compute
result that has not landed, which only the GPU holds) and GPU ownership (`own.c`). If guest memory holds the bytes,
`vk_uma_get` binds the import that covers them, at the range's offset: no hash, no copy, no own buffer, no window. Bytes
written in place by work not done yet are read there too, after a barrier (`order`, below). A dispatch's read-only
storage over a storage image not published yet or a queued release's payload is copied, which lands them first, as a
write in place is refused there. Copied, an Ounce game's tensors, each bound as the 16 MiB after its address and
written in place by the dispatch before, took a 16 MiB copy out of the import at every binding: in one game's upscaled
intro movie about 35 copies, 540 MB and 15 ms of GPU time a frame.

What the GPU writes goes there too:

- Writable storage buffers and transform feedback buffers (`vk_buf_copy_writable`, `vk_uma_write`): the range bound for
  writing is the import of its guest memory, so a draw's or dispatch's writes need no `GfxOut`, no readback and no
  landing. They are pending (`out`) until their submit is done.
- Dispatches run on the graphics queue (`compute.c`), so their storage is written in place like a draw's and in order
  with the draws, with nothing kept GPU-owned on a queue of its own.
- An engine's result (the copy engine, the 2D engine, VIC, NVDEC, a render target's write-back) over memory an import
  holds already is copied there on the GPU in stream order (`vk_uma_publish`, from `vk_own_publish`), a write in place
  like the shaders': not GPU-owned, no page trapped, and the import stays. Elsewhere it is published GPU-owned, as
  without UMA: an import made for the result would take its pages from write tracking, and a sparse texture a copy
  feeds would then load again whole instead of the pages written.

Guest memory is imported with `VK_EXT_external_memory_host` in pieces. An import covers the 2 MiB windows that a range
touches, clipped to mapped guest memory. Imports never overlap: a range across two imports makes one import of their
union. At most 512 imports, the least recently used goes past that, and an import unused for 600 presents goes. A
window that the driver refuses is kept in a list of refused spans for 600 presents, or until its pages change, and
its ranges are copied. On an RTX 2080 a span with one page the driver cannot pin (a `PROT_NONE` page) fails as a whole
with `VK_ERROR_OUT_OF_DEVICE_MEMORY`, while anonymous and shared (memfd, the alias pool's) pages import. Before the
list, every range in such a window asked the driver again, and one scene fell from 28 to 5 frames a second. The whole guest heap is never one import. A driver that checks each import's
pages at each submit (amdgpu's user pointers) pays for the bytes imported, and LASRecomp measured 45 ms of driver
time a frame on AMD with the whole heap as one import. On an RX 6700 XT (RADV, Mesa 26.1.8) a submission takes 6.4 us
more per MiB imported in one import, from 16 MiB to 512 MiB, and 7.2 us per MiB with 160 MiB in 160 imports.

### What still copies, and why

- The constant buffers the GPU front end writes between draws (`CONST_BUFFER_DATA`, `r3d_const_upload`) on the CPU,
  when the GPU thread meets the method, and every other page the runtime wrote (`front`). A draw recorded before the
  write but run by the GPU after it would read the new constants. The other constant buffers, the ones the game's CPU
  or GPU writes, are read in place (`vk_buf_get_uniform`).
- Writable storage over bytes the GPU holds elsewhere (a GPU-owned range, a compute or storage image result, a queued
  release's payload), and a page the runtime wrote (`front`): those land first and are copied, as before. A
  dispatch's logged binding (its stores logged, `vk_buf_copy_writable_logged`) that is refused only because an owner
  holds part of it, or no import can hold part of it (a 2 MiB window with an alias-pool page, which amdgpu's user
  pointers refuse), is written back on the GPU right after the dispatch instead (`back_record` in `vk/buffer.c`): the
  bytes its write log marks, and no others, are merged by `wlog_merge.comp` into the import's part in place, into the
  owner's part, and into whole pages neither holds through an owner made for them, published GPU-owned. Nothing stays
  pending, so a later reader neither lands it (a `vk_finish`) nor composes it. A run with writes in place pending
  across an import's edge is then read a part at a time, each from its import on the GPU (`owned.c`'s
  `inplace_part`), where it was waited for.
- Ranges under pending results, and GPU-owned ranges: their bytes are not in guest memory.
- Textures and render targets: their layout differs from guest memory (block linear, formats the host lacks), so they
  are always made on the GPU from a staging copy.
- A range that no one import can hold whole, as a 16 MiB storage window reaching into a GPU-owned span (a decoded movie
  frame, whose pages are in the alias pool, which amdgpu's user pointers refuse): read, copied from its import and its
  owners; written by a program that logs its writes, merged back after the dispatch (`back_record`).

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
  those pages are destroyed before anything else is submitted (`vk_uma_mapping_end`). A driver keeps every import in
  every submission, and amdgpu takes all their pages again in each one's ioctl: one page gone fails the submission
  (`The CS has been rejected (-14)`, `EFAULT`), and the device is lost. So before the pages change, the commands that
  read an import over them run, and every submission the recording thread holds reaches the driver
  (`vk_uma_mapping_begin`). Pages becoming GPU-owned are trapped, which amdgpu can't take either: the imports over
  them go before the trap (`vk_uma_owning`). An import dropped from the table (the least recently used, one a union
  replaced, a save state's load) is destroyed once the submit that last read it is done, or before its pages change
  if that comes first. The submit that last read an import is the one its last lookup was in, unless the draw's setup
  submitted after the lookup (a wait for a landing) and recorded its use in the next: so until the next draw or
  dispatch begins, an import looked up since counts the submit being recorded as its reader (`vk_uma_settle`).
  `vulkan_headless` (`check_uma`) unmaps and maps a range again, and checks that the GPU reads the new bytes;
  `check_uma_page_changes` unmaps, traps and drops imports with a submission after each; `vulkan_uma_imports.inc`
  traps pages under a read still running, under two imports, under the import made after a page landed, after an
  unmap, and under a lookup used after a submit (`SNX_TEST_UMA_ONLY=1` runs these alone, `runtime_write` or
  `constants` one by name).
- Write tracking. amdgpu takes an import's pages again, writable, at each submission, which undoes the protection a
  scan put on them: the next scan reports every imported page written with no CPU write (16 pages of 16 after each
  submission, none without the import). Trusted, those reports made a render target's write-back keep memory's old
  bytes over its pixels (`check_rt_overlap`), and a submission after a scan faults every protected page in again
  (16 ms for 32 MiB imported, against 0.3 ms with no scan). So the scans leave the pages an import holds alone, and
  `vk_written_since` can't tell there (-1): its users compare the bytes. An import's pages count as written once when
  it goes (`vk_mem_changed`), for what the CPU wrote there meanwhile.
- Hazards. A write of the runtime's own (`gmem_write`: a front-end write, a constant buffer update, a target's
  write-back) on a page that a submit not done yet reads in place waits for that submit (`vk_uma_before_write`, the
  `live` pages), and the page is copied from then on (`front`), so the next write there waits for nothing. Any other
  GPU-side write (a landed result) on such a page can change what that command reads: those are counted
  (`gpu_set_write_watch`), not prevented, and the report every 600 presents gives the count:

  `vulkan: in place over 600 presents: N ranges (M MB) read in guest memory, N copied; N imports made (M MB, T ms),
  N failed, N dropped; N imports hold M MB; N hazards (...)`

  The count is by 4 KiB page, so a constant buffer next to vertices on one page counts too.
- Writes in place. The console's GPU runs its work in order: what reads a range in place after it was written in place
  comes after a barrier, and so does what writes a range read or written before (`order`, by page since the last
  barrier). A publish records its own barriers before and after its copy, so a draw that reads the result in the next
  rendering pass reads it there without ending that pass. A GPU copy of bytes a submit not done yet writes in place
  copies them from the import, after a barrier (owned.c's `cpu_read`, VIC's `plane_stage`), not from the CPU's view.
  The runtime's own CPU readers (`gmem_need`, `gmem_read`) wait for that submit, and so does a write of the runtime's
  there (`vk_uma_before_write`). The end of a submit with writes in place makes them visible to the host
  (`vk_uma_host_barrier`), and a release waits for the submit that last bound guest memory in place, a write included
  (`vk_uma_bound`). VIC and NVDEC wait for their pictures before host1x increments the job's syncpoint, which the guest
  waits for before it reads them. The report every 600 presents adds `N MB written in place (M MB of them engines'
  results), N barriers for it, N waits for it`.

### Android: guest memory moved into AHardwareBuffers

Every Android driver takes AHardwareBuffers (`VK_ANDROID_external_memory_android_hardware_buffer`), and Qualcomm's has
no `VK_EXT_external_memory_host`. Where a device has the first and not the second, `vk_memory=2` moves guest memory
into AHardwareBuffers instead of importing it where it is (`vk_uma_way`; the migrate way in `uma.c`), and auto copies,
as no phone has measured the moves yet (`check_uma_way`); `uma_memory=2` moves it on any driver that takes them. The
windows a range touches become a region, a buffer of its own that Vulkan imports once, and their pages then move into
it with their bytes, mapped where they were
(`mem_migrate`: a copy, then the buffer's dma-buf mapped over the range, writers held off by `plat_freeze`). A region
that a range reaches moves on into one region of their union, once the writes in place pending in it have landed, as
the move copies what its pages hold (`check_uma_region_merge`). A region is 256 MiB at most: on an Adreno 830
(Snapdragon 8 Elite) Android's allocator gave 256 MiB, and 512 MiB aborted its service. The memory of an
AHardwareBuffer is taken whole when it is made and never swapped, so only what the GPU reads moves, and a region that
goes for idleness or room first moves its pages back into the reservation's own (`mem_unmigrate`).

A probe on that phone (Android 16) found the buffer's mapping to be the memory `AHardwareBuffer_lock` gives, the GPU
reading what the CPU wrote and the CPU what the GPU wrote with no sync, the CPU's bandwidth there that of `malloc`'s
memory (31.5 against 31.2 GB/s), `mprotect` trapping there, and a submission costing the same with 16 or 256 MiB
imported (7 to 30 us), where amdgpu's user pointers cost 6.4 us more per MiB.

A region's buffer is memory of its own, which no change of the guest's pages touches. Pages of a region unmapped,
decommitted or moved into the alias pool (a view, a trap) get other pages there (`give_back_nl` maps fresh ones, as
decommitting shared pages keeps their bytes); the region goes then with no wait, the commands recorded before read the
bytes it had, and it is destroyed once the last of them is done. Write tracking leaves a region's pages alone as it
leaves an import's: that phone's kernel (6.6) has no `PAGEMAP_SCAN`, and userfaultfd can't protect a dma-buf mapping.
Off Android `plat_gpu_buffer` makes a memfd, which the driver imports as host memory: `vulkan_headless`'s
`check_uma_migrate` runs the migrate way where the driver takes a memfd's pages (lavapipe) and skips where it doesn't
(amdgpu's user pointers take anonymous memory only), and `memory_alias` moves guest memory into memfds and back.

### Placed maps

`VK_EXT_map_memory_placed` maps device memory at an address the caller gives: the guest heap as driver memory, with
nothing to import or pin, and none of the per-submit cost of user pointers on amdgpu. `vk/uma.c` has it, off by default
(`vk_uma_placed`, which `check_uma_placed` and `check_uma_placed_view` turn on), on a device with `memoryMapPlaced` and
`memoryUnmapReserve`:

- A window of 2 MiB that the heap or physical memory commits whole (`mem_heap_set`, `mem_map_physical`) is a chunk:
  host-visible, coherent memory, cached where a type is, mapped at its guest address over the reservation, one object a
  window (RADV has no `memoryMapRangePlaced`). The GPU binds the chunk's buffer at the range's offset. mem.c commits and
  decommits through it (`mem_set_placed`): a page committed again is zeroed then, as the GPU may still read it; a chunk
  with no page committed goes, its address reserved again, and its memory once the submit that last bound it is done.
- A view of it (`svcMapMemory`, code memory) maps the chunk's pages again from its dma-buf
  (`VK_EXT_external_memory_dma_buf`). The alias pool never takes a chunk's pages, nor do the traps: a result over a
  chunk is copied there (`vk_uma_publish`).
- Write tracking leaves its pages alone (`vk_uma_import_in`): userfaultfd refuses to register a mapping of the
  driver's (EINVAL on RADV and on Turnip), and caches compare their contents there.
- A chunk and the migrate way's regions never share a window: a window with pages moved into a region is committed as
  host pages, and a chunk's pages never move (`mem_movable`).

On an RX 6700 XT (RADV) a chunk reads and writes as fast as malloc'd memory, the GPU and the CPU see each other's
writes with no flush, `mprotect` traps on it, and a submission costs the same with 256 MiB placed as with none. Before
it is on by default: write tracking by protection on its pages (or every texture over the heap is hashed again at each
check); one sparse buffer over the heap with each chunk bound into it (a range across two chunks is copied now); and a
view and its source counted as one address by the in-place bookkeeping (`live`, `out`).

### Measurements on a discrete GPU (RTX 2080, 246 MiB BAR)

Each scene is the benchmark save and script of one game, `-H --fps_limit=0 --vsync=0`, with NVN's calls run by the
runtime, and `vk_gpu_profile=1` for the queue's times. The machine ran other jobs (load 4 to 16), so a single frame rate moves by
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
show nothing. Which of them are real is not traced yet.

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

### The copy path, and what keeps it correct

On a GPU that shares system memory, guest vertex, index and read-only storage ranges are imported into Vulkan
(`VK_EXT_external_memory_host`, or on Android with `vk_memory=2` moved into AHardwareBuffers) and read in place, as
the sections above say. Every other range takes the copy path that this section is about:
`vk_buf_copy` copies bytes out of guest RAM into a buffer that `vk_stage` maps and the GPU reads. Exporting the final
image as a dma-buf, as desktop compositors do, would not remove this copy: ScarletNX already presents its Vulkan image
without an app-managed dma-buf handoff.

#### Measured workload

With the constant-buffer cache enabled, a 600-present interval of a cave scene in one game staged 3,688,879,936
uncached bytes in 63,098 calls. The largest call was 58,848 bytes and none were zero-filled. The only nonzero-address
staging-copy path with that cache enabled is `vk_buf_copy_writable` for writable graphics storage buffers in `stage_set`. A
per-draw wait or allocation is therefore not a viable replacement for the staging ring. The test machine has a
discrete RTX 2080; it cannot establish a unified-memory speedup.

#### Correctness requirements

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

A compute program of the SM86 translator logs the bytes its stores write instead (see
[Ounce](../../platforms/nx/ounce.md)): its results keep no input bytes, and land exactly the bytes logged, the shader's
value winning over a CPU write made to the same byte while the result was pending.

The byte-value reconciliation cannot distinguish a later CPU write that restores the original value from no write.
Games must synchronize CPU and GPU writes to the same bytes for deterministic ordering. When writable bindings in one
draw share guest backing, the bindings involved in that overlap use a shared page atlas. Each such binding maps its
logical word indices to atlas pages, so partially overlapping ranges, fragmented mappings and different GPU virtual
aliases see the same bytes. The atlas starts after earlier graphics and compute output has landed. A draw whose atlas
would exceed the device's maximum storage-buffer descriptor range is skipped with a warning, rather than rendered with
independent snapshots.

The 110-second run of that cave scene with graphics writeback and same-frame reuse completed 1,757 frames, compared with
1,754 for the earlier staging run on this machine. A 600-present interval reused 7,429 GPU buffers but still forced
1,985 lands, totaling 14.7 seconds of waits. The scenes and shader caches vary, so this is a correctness smoke test, not
a controlled performance comparison. The resulting cave image still has known cyan rendering artifacts.

Vulkan references: [host pointer import](https://docs.vulkan.org/spec/latest/chapters/memory.html#host-external-memory),
[host/device synchronization](https://docs.vulkan.org/spec/latest/chapters/synchronization.html), and
[placed memory mapping](https://docs.vulkan.org/features/latest/features/proposals/VK_EXT_map_memory_placed.html).

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
   this device with a decode queue (`vk/nvdec.c`), and its H.264 pictures, and the native video service's AV1 ones,
   are copied out of their images on the GPU. VP9 stays on the CPU decoder (the NVDEC probability patch is in FFmpeg's
   software VP9). AV1 runs on RADV (`vulkan_video_av1`); H.264 has not run on a GPU with Vulkan Video yet, and
   lavapipe has no decode queue.
4. Done, inline-to-memory aside. DMA, 2D, inline-to-memory: every copy, layout change, remap and fill as a GPU copy or dispatch from the source's
   owner (or its guest bytes, staged whole) into a GPU-owned destination. `f2d_blit_memory`, the DMA engine's CPU
   loops and the render target flush and reload around them go. The clear before the renderer starts is kept for
   the renderer. Inline-to-memory stays a CPU write: its data is born on the CPU (the pushbuffer), and the runtime's
   own readers of what it writes (the shader translator, MME macros) would land every GPU-owned copy of it again.
   `vulkan_headless` holds the DMA engine and the 2D blit to `dma_ref.c` and `fermi2d_ref.c` on lavapipe.
5. Windows and the other hosts. Linux without minor faults and Android trap by protection now (`memory_traps=1`
   tests that path on any Linux), and so does Windows, landing in place with no pool ([above](#windows-macos);
   `tests/memory_own.c` runs there too). macOS and FreeBSD need the alias pool first (a shared memory object). Until
   then their `plat_trap_*` fail, and a published result lands at once (a readback per result, correct and slow). The
   setting `memory_traps=2` makes a Linux or Windows host take that path, to measure and test it there.
6. Started: memory the CPU and the GPU share (above). Guest vertex, index and read-only storage ranges are read in
   place on a GPU that shares memory (`vk_memory`), and so are constant buffers but the pages the runtime wrote;
   writable storage, transform feedback and the engines' results are written there in place, and dispatches run on the
   graphics queue. Placed maps are there, off by default (above). Next: write tracking by protection on their pages,
   and then placed maps on by default.
