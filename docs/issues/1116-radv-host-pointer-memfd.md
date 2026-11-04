# The 3DS device stops when RADV refuses a file-backed host pointer

## What was seen

On the development machine (AMD Radeon RX 9060 XT, RADV, Mesa 25.x) `scarletnx-3ds` stopped at its first GSP command:

```
3ds gpu: the driver cannot import the VRAM region
No Vulkan device: no frames will be drawn, shown or saved
```

The 3DS board keeps all of its RAM in one `memfd` object (`src/common/memory/host.c`, `snx_host_memory_new`). The
device imported each region as its own `VK_EXT_external_memory_host` buffer. The first region (FCRAM, 128 MiB)
loaded, and the next (`VRAM`, 6 MiB) failed in `vkAllocateMemory` with `VK_ERROR_INVALID_EXTERNAL_HANDLE`
(-1000072003).

## Minimal reproduction

A memfd mapped `MAP_SHARED` and imported as a host pointer fails, while the same size mapped
`MAP_PRIVATE | MAP_ANONYMOUS` succeeds:

```c
int fd = memfd_create("ram", MFD_ALLOW_SEALING);
ftruncate(fd, size);
void* p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
memset(p, 0, size);
fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW);
/* VkImportMemoryHostPointerInfoEXT with pHostPointer = p: VK_ERROR_INVALID_EXTERNAL_HANDLE on RADV */
```

`shm_open` (tmpfs) fails the same way. `vkGetMemoryHostPointerPropertiesEXT` answers `VK_SUCCESS` and names the GTT
cached memory type, so the failure is only at allocation.

## Cause

RADV implements the import through `buffer_from_ptr` (amdgpu userptr). It takes an anonymous mapping and refuses
the file-backed one. The board needs a file-backed mapping because its address spaces map views of the same pages at
other addresses (`snx_host_map`, `MAP_SHARED | MAP_FIXED`). Anonymous shared memory cannot provide those views at
offsets.

## Fix

The device tries the host pointer first. When allocation fails and the driver has `VK_EXT_external_memory_dma_buf`
and `VK_KHR_external_memory_fd`, it creates a udmabuf over the board's memfd (`/dev/udmabuf`, `UDMABUF_CREATE`), and
imports that dma-buf into one buffer. The whole board RAM is imported once, and each region is a range of that
buffer (`SnxCtrVkRam.buffer_offset`). The dma-buf is imported by RADV at the same pages, and `3ds_windows` and the
presentation run again.

The memfd is created with `MFD_ALLOW_SEALING`, and `F_SEAL_SHRINK | F_SEAL_GROW` is set after the mapping, because
udmabuf refuses a memfd that can still resize.

## Where

- `src/common/memory/host.c`: the memfd flags and seals.
- `src/3ds/memory/ctr_memory.h`, `memory.c`: `snx_ctr_memory_fd`.
- `src/3ds/gpu/vk/device.c`: `udmabuf_create`, `import_ram`, the one-buffer import and `buffer_offset`.

## Check

```
build/tests/snx-test-3ds-windows /tmp/frames
build/tests/snx-test-3ds-windows /tmp/frames --separate
```

Both pass and the log has no import error. A game still needs a PICA feature that the port has (see
`3ds-pica-shadow-rendering.md`).
