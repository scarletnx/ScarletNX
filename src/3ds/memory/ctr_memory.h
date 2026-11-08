/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_CTR_MEMORY_H
#define SNX_CTR_MEMORY_H
#include "3ds/board/board.h"
#define SNX_CTR_PAGE_SIZE 4096u
#define SNX_CTR_READ      1u
#define SNX_CTR_WRITE     2u
#define SNX_CTR_EXEC      4u

typedef enum {
    SNX_CTR_MEM_OK,
    SNX_CTR_MEM_INVALID,
    SNX_CTR_MEM_NOMEM,
    SNX_CTR_MEM_UNMAPPED,
    SNX_CTR_MEM_PERMISSION,
    SNX_CTR_MEM_OVERLAP,
    SNX_CTR_MEM_DEVICE
} SnxCtrMemResult;
typedef struct SnxCtrMemory SnxCtrMemory;
typedef struct SnxCtrAddressSpace SnxCtrAddressSpace;
/* MMIO calls are a single aligned 1/2/4/8-byte access in little-endian byte
   order. False means a device fault. Execute access to MMIO is always denied. */
typedef bool (*SnxCtrMmioRead)(void* context, uint32_t physical, void* bytes, size_t size);
typedef bool (*SnxCtrMmioWrite)(void* context, uint32_t physical, const void* bytes, size_t size);
SnxCtrMemory* snx_ctr_memory_new(SnxCtrModel model);
const SnxCtrProfile* snx_ctr_memory_profile(const SnxCtrMemory* memory);
/* Address spaces must be destroyed first. No internal locking: caller serializes
   mappings/accesses/devices. Physical RAM is shared by all spaces on this board. */
void snx_ctr_memory_free(SnxCtrMemory* memory);
/* The board's region index's RAM, as the host holds it (page-aligned, the region's size), which the GPU reads and
   writes in place; NULL for an MMIO region */
unsigned char* snx_ctr_memory_region(const SnxCtrMemory* memory, size_t index);
/* All of the board's RAM as the host holds it: one object, the regions one after another, *size bytes */
const unsigned char* snx_ctr_memory_ram(const SnxCtrMemory* memory, uint64_t* size);
/* The RAM moved into handle's object, a file descriptor of the RAM's size that maps whole and at any page (the dma-buf
   of a GPU's memory, gpu/vk/device.c): what the RAM holds copied there, then every view of it mapped from the new
   object at the same address (the host's, the regions' above, each flat space's view of a RAM page at its guest
   permissions less its tracking level's), and the old object closed. The handle is the memory's from then on, closed
   by snx_ctr_memory_free. Nothing may write the RAM meanwhile: before the guest runs. False, with errno, when the new
   object or one of the views does not map: every view is the old object's again, and the handle still the caller's.
   Always false on Windows */
bool snx_ctr_memory_adopt(SnxCtrMemory* memory, intptr_t handle);
void snx_ctr_memory_mmio(SnxCtrMemory* memory, void* context, SnxCtrMmioRead read, SnxCtrMmioWrite write);
SnxCtrAddressSpace* snx_ctr_space_new(SnxCtrMemory* memory);
/* The flat space, on a host that has one (memory/host.h): guest address va is base + va for compiled code, which
   reads and writes it directly (SNX_ARM_MEMORY_FLAT), its mappings views of the board's RAM, so this API and the
   generated code see the same bytes. NULL without one: compiled code then goes through the checked bus */
unsigned char* snx_ctr_space_base(const SnxCtrAddressSpace* space);
void snx_ctr_space_free(SnxCtrAddressSpace* space);
/* Page-aligned nonempty ranges; mapping requires one physical board region.
   Failed map/protect/unmap leave all mappings unchanged. No automatic user VA
   layout is installed: process/HLE setup owns the allowed address ranges. */
SnxCtrMemResult snx_ctr_map(SnxCtrAddressSpace* space, uint32_t va, uint32_t pa, uint32_t size, unsigned permissions);
SnxCtrMemResult snx_ctr_protect(SnxCtrAddressSpace* space, uint32_t va, uint32_t size, unsigned permissions);
SnxCtrMemResult snx_ctr_unmap(SnxCtrAddressSpace* space, uint32_t va, uint32_t size);
SnxCtrMemResult snx_ctr_query(const SnxCtrAddressSpace* space, uint32_t va, uint32_t* pa, unsigned* permissions);
/* Serialized board-wide reservation generation, not a RAM value comparison.
   Every successful write or mapping/protection change advances it. Saturates
   at UINT64_MAX, after which exclusive stores must fail conservatively. */
uint64_t snx_ctr_memory_epoch(const SnxCtrAddressSpace* space);
/* RAM transfers may be unaligned and span pages, after preflighting the ENTIRE
   range. MMIO transfers may not span pages or mix with RAM. No flat host pointer
   is exported. The CPU adapter owns instruction-specific alignment rules. */
SnxCtrMemResult snx_ctr_read(SnxCtrAddressSpace* space, uint32_t va, void* output, size_t size);
SnxCtrMemResult snx_ctr_write(SnxCtrAddressSpace* space, uint32_t va, const void* input, size_t size);
SnxCtrMemResult snx_ctr_fetch(SnxCtrAddressSpace* space, uint32_t va, void* output, size_t size);
/* Whole-instruction multiword transfers: reject any MMIO before side effects,
   rather than collapsing LDM/STM or paired accesses into one device access. */
/* No-copy full-range RAM preflight, including all requested permissions. */
SnxCtrMemResult snx_ctr_check_ram(SnxCtrAddressSpace* space, uint32_t va, size_t size, unsigned permissions);
SnxCtrMemResult snx_ctr_read_ram(SnxCtrAddressSpace* space, uint32_t va, void* output, size_t size);
SnxCtrMemResult snx_ctr_write_ram(SnxCtrAddressSpace* space, uint32_t va, const void* input, size_t size);
/* Bus-master (GPU, DMA) access by physical address: the whole nonempty range inside one RAM, VRAM or DSP RAM
   region of the board. No address space or page permission applies, as the CPU's mappings do not govern the other
   masters (an application maps VRAM read-only and the GPU still renders into it). Writes advance the reservation
   epoch like CPU stores. MMIO is refused (SNX_CTR_MEM_DEVICE). */
SnxCtrMemResult snx_ctr_phys_read(SnxCtrMemory* memory, uint32_t pa, void* output, size_t size);
SnxCtrMemResult snx_ctr_phys_write(SnxCtrMemory* memory, uint32_t pa, const void* input, size_t size);

/* Write tracking: what the GPU keeps of guest RAM, by physical page, so the CPU's accesses come to it first (the
   renderer's surfaces and textures, docs/platforms/3ds/README.md). A page's level, in order:
   - NONE: the CPU's.
   - WATCH: the GPU keeps a copy made from it (a decoded texture, a render target as memory has it): writes trap.
   - OWN: the GPU's copy is newer than memory, or GPU writes to it are not done: every access traps.
   Every guest view of the page traps (its aliases in each flat space, at their guest permissions less the level's),
   and so do this API's accesses and a bus master's by physical address, before they copy. A trap is resolved a step
   at a time by the handler, which must lower the page: an owned page's data lands in memory (to WATCH, or NONE), a
   watched page's copies are dropped (to NONE). A write to an owned page takes both steps. The handler runs on the
   thread that touched the page, from a fault handler for compiled code: it may wait for the GPU and call
   snx_ctr_track_set, and it must not touch a tracked page through this API */
typedef enum { SNX_CTR_TRACK_NONE, SNX_CTR_TRACK_WATCH, SNX_CTR_TRACK_OWN } SnxCtrTrackLevel;
typedef void (*SnxCtrTrackHandler)(void* context, uint32_t pa, SnxCtrTrackLevel level);
void snx_ctr_track_handler(SnxCtrMemory* memory, SnxCtrTrackHandler handler, void* context);
/* every RAM page of [pa, pa + size) at level */
void snx_ctr_track_set(SnxCtrMemory* memory, uint32_t pa, uint32_t size, SnxCtrTrackLevel level);
SnxCtrTrackLevel snx_ctr_track_level(const SnxCtrMemory* memory, uint32_t pa);
/* A flat space's fault at va (memory/host.h's resolver): true when it was a tracked page, whose step was taken, so
   the access is made again; false for the guest's own data abort */
bool snx_ctr_track_fault(SnxCtrAddressSpace* space, uint32_t va);
#endif
