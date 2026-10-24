# GPU-owned guest memory (design)

Status: phases 1 to 4 are done on Linux (below), inline-to-memory aside. The engines that worked in guest memory on
the CPU (NVDEC output, VIC, the DMA copy engine, the 2D engine) run on the GPU, with no CPU fallback. Their
results stay on the GPU: the guest memory they cover becomes GPU-owned. Every GPU consumer finds the data through
the ownership map and copies it on the GPU. Only when the guest CPU (recompiled code, or the runtime's own C code)
touches an owned range is the data written back into guest memory, the one readback the rules allow, and the range
is the CPU's again.

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

A host without these (older kernels, FreeBSD, Android's 5.10 kernels) uses Skyline's own shape: `mprotect` and a
signal handler checked before the crash handler, which hands the work to the same handler thread and waits for it
rather than landing in the signal handler.

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
5. Windows and the other hosts. Until then their `plat_trap_*` fail, and a published result lands at once (a readback
   per result, correct and slow).
