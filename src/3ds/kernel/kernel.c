/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   CTR SVC ABI/result/arbiter semantics referenced against Citra/Azahar
   (GPL-2.0-or-later), src/core/hle/kernel/{svc,handle_table,address_arbiter}.cpp
   and errors.h at 68202bc4ff7edf9d7280ec0dcd6572b7cd1ae26e, and the F1 kernel (timers, resource limits, memory
   blocks, QueryMemory, scheduling) against {svc,thread,timer,mutex,semaphore,resource_limit,shared_memory,
   vm_manager}.cpp at 987f18405073960622e1043bb44005989135c6e1 (svc.cpp is GPL-3.0-or-later there).
   Independent serialized implementation; no reference runtime dependency. */
#include "3ds/kernel/ctr_kernel.h"
#include "3ds/runtime/internal.h"
#include "common/utilities/bits.h"
#include "common/utilities/endian.h"
#include <stdlib.h>
#include "3ds/services/gsp.h"
#include <string.h>
#include <stdio.h>
#include "common/utilities/common.h"

#define OK                  0u
#define INVALID_HANDLE      0xd8e007f7u
#define OUT_OF_HANDLES      0xd8600413u
#define OUT_OF_MEMORY       0xd86007f3u
#define OUT_OF_HEAP         0xc8601bf3u
#define INVALID_ADDRESS     0xe0e01bf5u
#define MISALIGNED_ADDRESS  0xe0e01bf1u
#define MISALIGNED_SIZE     0xe0e01bf2u
#define INVALID_SIZE        0xe0e01bfdu
#define INVALID_PERMISSION  0xe0e01beeu
#define INVALID_COMBINATION 0xe0e01beeu
#define INVALID_ENUM        0xd8e093edu /* ResultInvalidEnumValueFnd */
#define INVALID_ENUM_KERNEL 0xd8e007edu /* ResultInvalidEnumValue */
#define INVALID_STATE       0xe0a01bf5u
#define NOT_IMPLEMENTED     0xe0e01bf4u
#define OUT_OF_RANGE        0xd8e007fdu
#define TIMED_OUT           CTR_TIMED_OUT
#define DONT_CARE           0x10000000u /* MemoryPermission::DontCare */
#define LOADER_BASE         0x00100000u /* the range ldr:ro may map a CRO in */
#define LOADER_END          0x04000000u
#define WRONG_PERMISSION    0xd900182eu /* ResultWrongPermission */

uint32_t snx_ctr_handle_object(SnxCtrProcess* p, uint32_t handle) {
    if (handle == CTR_PROCESS_HANDLE) return 1;
    if (handle == CTR_THREAD_HANDLE) return p->current_thread;
    uint32_t slot = handle & 511, generation = handle >> 9;
    if (!slot || slot > CTR_HANDLES || !generation) return 0;
    CtrHandle* h = &p->handles[slot - 1];
    return h->generation == generation ? h->object : 0;
}
uint32_t snx_ctr_handle_add(SnxCtrProcess* p, uint32_t id, uint32_t* out) {
    *out = 0;
    if (!id || id >= CTR_OBJECTS || !p->objects[id].type) return INVALID_HANDLE;
    for (unsigned i = 0; i < CTR_HANDLES; ++i) {
        CtrHandle* h = &p->handles[i];
        if (h->object) continue;
        h->generation = h->generation % 65535 + 1;
        h->object = id;
        ++p->objects[id].refs;
        *out = (h->generation << 9) | (i + 1);
        return OK;
    }
    return OUT_OF_HANDLES;
}
uint32_t snx_ctr_object_create(SnxCtrProcess* p, uint32_t type, uint32_t* out) {
    *out = 0;
    for (unsigned i = 3; i < CTR_OBJECTS; ++i) {
        if (p->objects[i].type) continue;
        p->objects[i].type = type;
        uint32_t result = snx_ctr_handle_add(p, i, out);
        if (result) p->objects[i] = (CtrObject){0};
        return result;
    }
    return OUT_OF_MEMORY;
}
uint32_t snx_ctr_handle_close(SnxCtrProcess* p, uint32_t handle) {
    uint32_t id = snx_ctr_handle_object(p, handle);
    if (!id || handle == CTR_PROCESS_HANDLE || handle == CTR_THREAD_HANDLE) return INVALID_HANDLE;
    p->handles[(handle & 511) - 1].object = 0;
    snx_ctr_object_release(p, id);
    return OK;
}
static uint32_t read32(SnxCtrProcess* p, uint32_t va, uint32_t* out) {
    unsigned char b[4];
    if (snx_ctr_read_ram(p->space, va, b, 4) != SNX_CTR_MEM_OK) return INVALID_ADDRESS;
    *out = snx_ld_le32(b);
    return OK;
}
static uint32_t write32(SnxCtrProcess* p, uint32_t va, uint32_t value) {
    unsigned char b[4];
    snx_st_le32(b, value);
    return snx_ctr_write_ram(p->space, va, b, 4) == SNX_CTR_MEM_OK ? OK : INVALID_ADDRESS;
}
static bool available(SnxCtrProcess* p, uint32_t va, uint32_t size) {
    for (uint32_t off = 0; off < size; off += 4096)
        if (snx_ctr_query(p->space, va + off, NULL, NULL) == SNX_CTR_MEM_OK) return false;
    return true;
}
/* QueryMemory's coarse class for one page: 0 free, 2 IO, 3 static, 4 code,
   5 private RAM, 6 a kernel-registered shared block. Permissions come from
   the actual mapping. */
/* Azahar's MemoryState values, as QueryMemory reports them */
enum {
    STATE_FREE,
    STATE_IO = 2,
    STATE_STATIC,
    STATE_CODE,
    STATE_PRIVATE,
    STATE_SHARED,
    STATE_CONTINUOUS,
    STATE_ALIASED,
    STATE_ALIAS,
    STATE_LOCKED = 11
};
static bool within(uint32_t page, uint32_t base, uint32_t size) {
    return page >= base && (uint64_t)page < (uint64_t)base + size;
}
/* The ranges a QueryMemory classifies by what the kernel put there rather than by the mapping alone, gathered once
   per call: the owner ranges of the application's memory blocks (Locked while the block lives) */
typedef struct {
    uint32_t count, base[CTR_OBJECTS], size[CTR_OBJECTS];
} Locks;
static void locks(SnxCtrProcess* p, Locks* l) {
    l->count = 0;
    for (uint32_t i = 1; i < CTR_OBJECTS; ++i)
        if (p->objects[i].type == CTR_OBJ_SHARED && p->objects[i].data.shared.address) {
            l->base[l->count] = p->objects[i].data.shared.address;
            l->size[l->count++] = p->objects[i].data.shared.size;
        }
}
/* One page's memory state and permissions, as Azahar's mappings give them (kernel/process.cpp, memory.cpp): the code
   and read-only segments Code, the main stack and TLS Locked, linear-heap pages Continuous, a MAP source Aliased and
   its target Alias, a memory block's owner range Locked, mapped blocks and the config and shared pages Shared,
   anything else the application owns Private. */
static void query_class(SnxCtrProcess* p, const Locks* l, uint32_t page, uint32_t* state, uint32_t* perm) {
    *state = STATE_FREE;
    *perm = 0;
    uint32_t pa = 0;
    unsigned perms = 0;
    SnxCtrMemResult result = snx_ctr_query(p->space, page, &pa, &perms);
    if (result == SNX_CTR_MEM_UNMAPPED) return;
    if (result != SNX_CTR_MEM_OK) {
        *state = STATE_IO;
        return;
    }
    *perm = perms;
    for (unsigned i = 0; i < 8; ++i)
        if (p->shared_maps[i].object && within(page, p->shared_maps[i].va, p->shared_maps[i].size)) {
            *state = STATE_SHARED;
            return;
        }
    for (unsigned i = 0; i < sizeof p->aliases / sizeof *p->aliases; ++i)
        if (p->aliases[i].size && (within(page, p->aliases[i].target, p->aliases[i].size) ||
                                   within(page, p->aliases[i].source, p->aliases[i].size))) {
            *state = within(page, p->aliases[i].target, p->aliases[i].size) ? STATE_ALIAS : STATE_ALIASED;
            return;
        }
    for (uint32_t i = 0; i < l->count; ++i)
        if (within(page, l->base[i], l->size[i])) {
            *state = STATE_LOCKED;
            return;
        }
    if (page == CTR_CONFIG || page == CTR_SHARED_PAGE) {
        *state = STATE_SHARED;
        return;
    }
    if (pa < CTR_PHYS_BASE) { /* VRAM, DSP RAM and the other areas outside FCRAM */
        *state = STATE_IO;
        return;
    }
    if (within(page, p->segment[0].address, p->segment[0].pages * 4096) ||
        within(page, p->segment[1].address, p->segment[1].pages * 4096))
        *state = STATE_CODE;
    else if (within(page, SNX_CTR_STACK_TOP - p->stack_size, p->stack_size) ||
             within(page, SNX_CTR_INITIAL_TLS & ~4095u, CTR_TLS_BYTES))
        *state = STATE_LOCKED;
    else if (pa < p->physical_end && p->physical_owner[(pa - CTR_PHYS_BASE) / 4096] == CTR_LINEAR_PAGE)
        *state = STATE_CONTINUOUS;
    else
        *state = STATE_PRIVATE;
}
/* the application's memory in use: every page of its pool it owns (code, stack, TLS, heap, linear heap) */
static uint64_t memory_used(SnxCtrProcess* p) {
    uint64_t used = 0;
    for (uint32_t i = 0; i < p->app_limit / 4096; ++i)
        if (p->physical_owner[i]) used += 4096;
    return used;
}
/* The application's own objects of a type: what its SVCs made (the HLE services' objects count against theirs on the
   console), and every live thread */
static uint32_t live_objects(SnxCtrProcess* p, uint32_t type) {
    uint32_t count = 0;
    for (uint32_t i = 1; i < CTR_OBJECTS; ++i)
        count += p->objects[i].type == type &&
                 (type == CTR_OBJ_THREAD ? !p->objects[i].data.thread.dead : !!p->objects[i].from_app);
    return count;
}
/* Create an object for the application's SVC within its APPLICATION limit, refused as Azahar's Reserve refuses it:
   Result(OutOf<kind>, OS, OutOfResource, Status), 0xC8601800 | the kind's code (errors.h ErrCodes) */
static uint32_t create_limited(SnxCtrProcess* p, uint32_t type, uint32_t* out) {
    static const struct {
        uint32_t type, limit, error;
    } limits[] = {{CTR_OBJ_EVENT, 0x20, 0xc860180fu},  {CTR_OBJ_MUTEX, 0x20, 0xc860180du},
                  {CTR_OBJ_SEMAPHORE, 8, 0xc860180eu}, {CTR_OBJ_TIMER, 8, 0xc8601810u},
                  {CTR_OBJ_SHARED, 0x10, 0xc860180bu}, {CTR_OBJ_ARBITER, 2, 0xc8601833u}};
    *out = 0;
    for (unsigned i = 0; i < sizeof limits / sizeof *limits; ++i)
        if (limits[i].type == type && live_objects(p, type) >= limits[i].limit) return limits[i].error;
    uint32_t result = snx_ctr_object_create(p, type, out);
    if (!result) p->objects[snx_ctr_handle_object(p, *out)].from_app = 1;
    return result;
}
/* The APPLICATION resource limit (Azahar kernel/resource_limit.cpp), in ResourceLimitType order: priority, commit,
   thread, event, mutex, semaphore, timer, shared memory, address arbiter, CPU time. The CPU time limit is 0: no
   share of the syscore has been granted (APT's SetApplicationCpuTimeLimit is not modelled). */
static uint32_t limit_value(SnxCtrProcess* p, uint32_t name, uint64_t* value) {
    static const uint32_t limits[10] = {CTR_PRIORITY_LIMIT, 0, 0x20, 0x20, 0x20, 0x8, 0x8, 0x10, 0x2, 0};
    *value = name == 1 ? p->app_limit : limits[name];
    return OK;
}
static uint32_t current_value(SnxCtrProcess* p, uint32_t name, uint64_t* value) {
    static const uint32_t types[10] = {0,
                                       0,
                                       CTR_OBJ_THREAD,
                                       CTR_OBJ_EVENT,
                                       CTR_OBJ_MUTEX,
                                       CTR_OBJ_SEMAPHORE,
                                       CTR_OBJ_TIMER,
                                       CTR_OBJ_SHARED,
                                       CTR_OBJ_ARBITER,
                                       0};
    *value = name == 1 ? memory_used(p) : name < 10 && types[name] ? live_objects(p, types[name]) : 0;
    return OK;
}
static bool overlaps(uint32_t a, uint32_t a_size, uint32_t b, uint32_t b_size) {
    return (uint64_t)a < (uint64_t)b + b_size && (uint64_t)b < (uint64_t)a + a_size;
}
/* A range that is not plain private memory, which ControlMemory may not free and which cannot be a MAP source or a
   memory block's owner range: part of a MAP pair, a mapped memory block (whose pages another range may own), or the
   owner's range of a memory block */
static bool held(SnxCtrProcess* p, uint32_t va, uint32_t size) {
    for (unsigned i = 0; i < sizeof p->aliases / sizeof *p->aliases; ++i)
        if (p->aliases[i].size && (overlaps(va, size, p->aliases[i].target, p->aliases[i].size) ||
                                   overlaps(va, size, p->aliases[i].source, p->aliases[i].size)))
            return true;
    for (unsigned i = 0; i < sizeof p->shared_maps / sizeof *p->shared_maps; ++i)
        if (p->shared_maps[i].object && overlaps(va, size, p->shared_maps[i].va, p->shared_maps[i].size)) return true;
    for (uint32_t i = 1; i < CTR_OBJECTS; ++i) {
        const CtrObject* o = &p->objects[i];
        if (o->type == CTR_OBJ_SHARED && o->data.shared.address &&
            overlaps(va, size, o->data.shared.address, o->data.shared.size))
            return true;
    }
    return false;
}
/* Private heap pages, read-write and held by nothing else: what MAP's source and a memory block's owner range must be
   (Azahar's MemoryState::Private with VMAPermission::ReadWrite) */
static bool private_heap(SnxCtrProcess* p, uint32_t va, uint32_t size) {
    if (va < CTR_HEAP || (uint64_t)va + size > CTR_HEAP_END || held(p, va, size)) return false;
    for (uint32_t off = 0; off < size; off += 4096) {
        uint32_t pa;
        unsigned permissions;
        if (snx_ctr_query(p->space, va + off, &pa, &permissions) != SNX_CTR_MEM_OK || pa < CTR_PHYS_BASE ||
            pa >= p->physical_end || p->physical_owner[(pa - CTR_PHYS_BASE) / 4096] != CTR_HEAP_PAGE ||
            (permissions & 3) != 3)
            return false;
    }
    return true;
}
/* The data segment with its BSS, read-write and held by nothing else: private memory as the heap is (Azahar's
   Process::LoadModule maps it MemoryState::Private), so it can be a memory block's owner range. A game's SDK makes
   the microphone's buffer from a page of its BSS. MAP still takes its source from the heap only */
static bool private_data(SnxCtrProcess* p, uint32_t va, uint32_t size) {
    const SnxCtrSegment* data = &p->segment[2];
    uint64_t end = data->address + (uint64_t)data->pages * 4096 + ((p->bss_size + 4095u) & ~4095u);
    if (va < data->address || va + (uint64_t)size > end || held(p, va, size)) return false;
    for (uint32_t off = 0; off < size; off += 4096) {
        uint32_t pa;
        unsigned permissions;
        if (snx_ctr_query(p->space, va + off, &pa, &permissions) != SNX_CTR_MEM_OK || (permissions & 3) != 3)
            return false;
    }
    return true;
}
/* Map size bytes at va onto the physical pages behind source in this process, page runs at a time; nothing stays
   mapped on failure */
static bool mirror(SnxCtrProcess* p, uint32_t va, uint32_t source, uint32_t size, unsigned permissions) {
    for (uint32_t off = 0; off < size;) {
        uint32_t pa, next;
        if (snx_ctr_query(p->space, source + off, &pa, NULL) != SNX_CTR_MEM_OK) break;
        uint32_t run = 4096;
        while (off + run < size && snx_ctr_query(p->space, source + off + run, &next, NULL) == SNX_CTR_MEM_OK &&
               next == pa + run)
            run += 4096;
        if (snx_ctr_map(p->space, va + off, pa, run, permissions) != SNX_CTR_MEM_OK) break;
        off += run;
        if (off == size) return true;
    }
    for (uint32_t off = 0; off < size; off += 4096) (void)snx_ctr_unmap(p->space, va + off, 4096);
    return false;
}
bool snx_ctr_kernel_private(SnxCtrProcess* p, uint32_t address, uint32_t size) {
    if (!size) return true;
    uint64_t end = (uint64_t)address + size;
    const SnxCtrSegment* data = &p->segment[2];
    uint64_t data_end = (uint64_t)data->address + data->pages * 0x1000ull + snx_align_up(p->bss_size, 4096);
    bool heap = address >= CTR_HEAP && end <= CTR_HEAP_END;
    if ((!heap && !(address >= data->address && end <= data_end)) || held(p, address, size) ||
        snx_ctr_check_ram(p->space, address, size, SNX_CTR_READ | SNX_CTR_WRITE) != SNX_CTR_MEM_OK)
        return false;
    if (heap)
        for (uint64_t at = snx_align_down(address, 4096); at < end; at += 4096) {
            uint32_t pa;
            if (snx_ctr_query(p->space, (uint32_t)at, &pa, NULL) != SNX_CTR_MEM_OK || pa < CTR_PHYS_BASE ||
                pa >= p->physical_end || p->physical_owner[(pa - CTR_PHYS_BASE) / 4096] != CTR_HEAP_PAGE)
                return false;
        }
    return true;
}
bool snx_ctr_kernel_loader_map(SnxCtrProcess* p, uint32_t target, uint32_t source, uint32_t size) {
    if (!size || ((target | source | size) & 4095) || target < LOADER_BASE || (uint64_t)target + size > LOADER_END ||
        !available(p, target, size) || !snx_ctr_kernel_private(p, source, size))
        return false;
    return mirror(p, target, source, size, SNX_CTR_READ | SNX_CTR_WRITE);
}
/* ControlMemory MAP and UNMAP (Azahar Process::Map and Unmap, non-privileged): target mirrors heap pages at source
   with the given permissions; the source stays mapped, and neither can be freed until the UNMAP of the same pair.
   Azahar does not check the pair; its comment there gives the console's answer to a mismatched UNMAP, 0xD8E007F5,
   which this returns rather than unmapping memory no MAP made. */
static uint32_t alias(SnxCtrProcess* p, bool map, uint32_t target, uint32_t source, uint32_t size,
                      uint32_t permissions) {
    if (source < CTR_HEAP || (uint64_t)source + size > CTR_HEAP_END) return INVALID_ADDRESS;
    if (source - target < size || target - source < size) return INVALID_STATE;
    unsigned slot = sizeof p->aliases / sizeof *p->aliases;
    for (unsigned i = 0; i < sizeof p->aliases / sizeof *p->aliases; ++i) {
        bool same = p->aliases[i].target == target && p->aliases[i].source == source && p->aliases[i].size == size;
        if (map ? !p->aliases[i].size : same && size) {
            slot = i;
            break;
        }
    }
    if (map) {
        if ((uint64_t)target + size > 0x20000000u || !available(p, target, size) || !private_heap(p, source, size))
            return INVALID_STATE;
        if (slot == sizeof p->aliases / sizeof *p->aliases) return OUT_OF_MEMORY;
        if (!mirror(p, target, source, size, permissions)) return OUT_OF_MEMORY;
        p->aliases[slot].target = target;
        p->aliases[slot].source = source;
        p->aliases[slot].size = size;
        return OK;
    }
    if (slot == sizeof p->aliases / sizeof *p->aliases) return 0xd8e007f5u; /* not a pair MAP made */
    if (snx_ctr_unmap(p->space, target, size) != SNX_CTR_MEM_OK) return INVALID_STATE;
    p->aliases[slot].size = 0;
    (void)snx_ctr_protect(p->space, source, size, permissions); /* the source is private again, with these rights */
    return OK;
}
/* Record a MapMemoryBlock mapping in a free slot. The mapping keeps the block alive (a reference) until
   UnmapMemoryBlock, as the console's does. */
static uint32_t record_map(SnxCtrProcess* p, uint32_t id, uint32_t va, uint32_t size) {
    for (unsigned i = 0; i < sizeof p->shared_maps / sizeof *p->shared_maps; ++i)
        if (!p->shared_maps[i].object) {
            p->shared_maps[i] = (CtrSharedMap){va, size, id};
            ++p->objects[id].refs;
            return OK;
        }
    return OUT_OF_MEMORY;
}
static bool free_map_slot(const SnxCtrProcess* p) {
    for (unsigned i = 0; i < sizeof p->shared_maps / sizeof *p->shared_maps; ++i)
        if (!p->shared_maps[i].object) return true;
    return false;
}
/* MapMemoryBlock of a block the application made over its own heap (Azahar SharedMemory::Map with the owner as
   target): the rights asked (r2: read, write or both, never execute or don't-care) within the block's own; the
   rights another process gets (r3) must include the block's own, and don't-care is refused for a heap-backed block
   (ResultWrongPermission, ResultInvalidCombination). The address lies in [0x08000000, 0x14000000). The mapping
   mirrors the owner's pages. */
static uint32_t map_own_block(SnxCtrProcess* p, uint32_t id, uint32_t address, uint32_t asked, uint32_t other) {
    CtrObject* o = &p->objects[id];
    uint32_t size = o->data.shared.size, own = o->data.shared.permissions == DONT_CARE ? 3 : o->data.shared.permissions;
    if (!asked || asked > 3 || (asked & ~own) || other == DONT_CARE) return INVALID_COMBINATION;
    if (own & ~other) return WRONG_PERMISSION;
    if ((address & 4095) || address < CTR_HEAP || (uint64_t)address + size >= CTR_LINEAR) return INVALID_ADDRESS;
    if (!available(p, address, size)) return INVALID_STATE;
    if (!free_map_slot(p)) return OUT_OF_MEMORY;
    if (!mirror(p, address, o->data.shared.address, size, asked)) return OUT_OF_MEMORY;
    return record_map(p, id, address, size);
}
/* Heap/linear commit, free and protect, and MAP/UNMAP aliases; no mappings, ownership or accounting change on
   rejected calls. */
uint32_t snx_ctr_kernel_memory(SnxCtrProcess* p, uint32_t op, uint32_t va, uint32_t source, uint32_t size,
                               uint32_t permissions, uint32_t* output) {
    *output = 0;
    if ((va | source) & 4095) return MISALIGNED_ADDRESS;
    if (size & 4095) return MISALIGNED_SIZE;
    if (permissions & ~3u) return INVALID_PERMISSION;
    op &= ~0xf00u; /* the region bits: Azahar ignores them, every allocation is the application's */
    if (op & ~(0x10000u | 0xffu)) return INVALID_COMBINATION;
    bool linear = !!(op & 0x10000);
    op &= 0xff;
    if (linear && op != 3) return INVALID_COMBINATION;
    if (op != 1 && op != 3 && op != 4 && op != 5 && op != 6) return INVALID_COMBINATION;
    if (!size) {
        *output = va;
        return OK;
    }
    if (op == 4 || op == 5) { /* MAP and UNMAP write no address out */
        uint32_t result = alias(p, op == 4, va, source, size, permissions);
        if (!result) p->cpu.state.cpu.exclusive_valid = 0;
        return result;
    }
    /* FREE leaves what is not plain private memory alone; PROTECT reprotects any of it, as Azahar's ReprotectRange */
    if (op == 1 && held(p, va, size)) return INVALID_STATE;
    uint32_t base = linear ? CTR_LINEAR : CTR_HEAP;
    uint32_t end = linear ? CTR_LINEAR + p->app_limit : CTR_HEAP_END;
    if (op != 3) {
        if (va >= CTR_LINEAR && va < CTR_LINEAR + p->app_limit) {
            base = CTR_LINEAR;
            end = CTR_LINEAR + p->app_limit;
            linear = true;
        }
    }
    if (size > end - base) return INVALID_SIZE;
    if (va && (va < base || (uint64_t)va + size > end)) return INVALID_ADDRESS;
    if (op == 3) {
        if (!va && !linear) return INVALID_ADDRESS;
        if (va && !available(p, va, size)) return INVALID_STATE;
        uint32_t pages = size / 4096, total = p->app_limit / 4096, first = 0, run = 0;
        uint32_t* plan = malloc(pages * sizeof *plan);
        if (!plan) return OUT_OF_MEMORY;
        if (linear) {
            if (va) {
                first = (va - CTR_LINEAR) / 4096;
                for (uint32_t i = first; i < first + pages; ++i)
                    if (p->physical_owner[i]) {
                        free(plan);
                        return INVALID_STATE;
                    }
            } else {
                for (uint32_t i = 0; i < total; ++i) {
                    if (p->physical_owner[i])
                        run = 0;
                    else if (++run == pages) {
                        first = i + 1 - pages;
                        break;
                    }
                }
                if (run != pages) {
                    free(plan);
                    return OUT_OF_HEAP;
                }
            }
            va = CTR_LINEAR + first * 4096;
            for (uint32_t i = 0; i < pages; ++i) plan[i] = first + i;
        } else {
            /* Ordinary heap pages need not be physically contiguous. */
            for (uint32_t i = 0; i < total && run < pages; ++i)
                if (!p->physical_owner[i]) plan[run++] = i;
            if (run != pages) {
                free(plan);
                return OUT_OF_HEAP;
            }
        }
        if (!available(p, va, size)) {
            free(plan);
            return INVALID_STATE;
        }
        uint32_t mapped = 0;
        while (mapped < pages) {
            uint32_t count = 1;
            while (mapped + count < pages && plan[mapped + count] == plan[mapped] + count) ++count;
            if (snx_ctr_map(p->space, va + mapped * 4096, CTR_PHYS_BASE + plan[mapped] * 4096, count * 4096,
                            SNX_CTR_READ | SNX_CTR_WRITE) != SNX_CTR_MEM_OK) {
                if (mapped) snx_ctr_unmap(p->space, va, mapped * 4096);
                free(plan);
                return OUT_OF_MEMORY;
            }
            mapped += count;
        }
        unsigned char zero[4096] = {0};
        for (uint32_t off = 0; off < size; off += 4096) {
            if (snx_ctr_write_ram(p->space, va + off, zero, sizeof zero) != SNX_CTR_MEM_OK) {
                snx_ctr_unmap(p->space, va, size);
                free(plan);
                return INVALID_STATE;
            }
        }
        if (snx_ctr_protect(p->space, va, size, permissions) != SNX_CTR_MEM_OK) {
            snx_ctr_unmap(p->space, va, size);
            free(plan);
            return INVALID_STATE;
        }
        for (uint32_t i = 0; i < pages; ++i) p->physical_owner[plan[i]] = linear ? CTR_LINEAR_PAGE : CTR_HEAP_PAGE;
        free(plan);
    } else {
        if (!va) return INVALID_ADDRESS;
        uint32_t pages = size / 4096;
        uint32_t* physical = malloc(pages * sizeof *physical);
        if (!physical) return OUT_OF_MEMORY;
        for (uint32_t i = 0; i < pages; ++i) {
            uint32_t pa;
            if (snx_ctr_query(p->space, va + i * 4096, &pa, NULL) != SNX_CTR_MEM_OK || pa < CTR_PHYS_BASE ||
                pa >= p->physical_end ||
                p->physical_owner[(pa - CTR_PHYS_BASE) / 4096] != (linear ? CTR_LINEAR_PAGE : CTR_HEAP_PAGE)) {
                free(physical);
                return INVALID_STATE;
            }
            physical[i] = (pa - CTR_PHYS_BASE) / 4096;
        }
        SnxCtrMemResult result =
            op == 1 ? snx_ctr_unmap(p->space, va, size) : snx_ctr_protect(p->space, va, size, permissions);
        if (result != SNX_CTR_MEM_OK) {
            free(physical);
            return INVALID_STATE;
        }
        if (op == 1) {
            for (uint32_t i = 0; i < pages; ++i) p->physical_owner[physical[i]] = CTR_UNUSED;
        }
        free(physical);
    }
    p->cpu.state.cpu.exclusive_valid = 0;
    *output = va;
    return OK;
}
static SnxCtrSvcResult arbitrate(SnxCtrProcess* p, SnxArmCPU* cpu) {
    uint32_t* r = cpu->r;
    uint32_t id = snx_ctr_handle_object(p, r[0]), va = r[1], type = r[2], threshold = r[3], value;
    if (!id || p->objects[id].type != CTR_OBJ_ARBITER) {
        r[0] = INVALID_HANDLE;
        return SNX_CTR_SVC_RETURN;
    }
    if (type > 4) {
        r[0] = INVALID_ENUM;
        return SNX_CTR_SVC_RETURN;
    }
    if (va & 3) {
        r[0] = MISALIGNED_ADDRESS;
        return SNX_CTR_SVC_RETURN;
    }
    if (type == 0) {
        uint32_t remaining = threshold & 0x80000000u ? UINT32_MAX : threshold;
        /* the best (effective) priority first, the one waiting longest among equals (Azahar's
           ResumeHighestPriorityThread takes the first of its wait list) */
        uint32_t priority[CTR_OBJECTS];
        snx_ctr_thread_priorities(p, priority);
        while (remaining) {
            uint32_t best = 0;
            for (uint32_t i = 1; i < CTR_OBJECTS; ++i) {
                if (p->objects[i].type != CTR_OBJ_THREAD) continue;
                CtrThread* t = &p->objects[i].data.thread;
                if (t->waiting_object != id || t->waiting_address != va) continue;
                if (!best || priority[i] < priority[best] ||
                    (priority[i] == priority[best] && t->wait_seq < p->objects[best].data.thread.wait_seq))
                    best = i;
            }
            if (!best) break;
            CtrThread* t = &p->objects[best].data.thread;
            t->waiting_object = t->waiting_address = 0;
            t->wake_ns = 0; /* a timed wait signaled before its deadline */
            t->context.r[0] = t->context.exit = t->context.syndrome = 0;
            snx_ctr_object_release(p, id);
            snx_ctr_thread_ready(p, best);
            --remaining;
        }
        r[0] = OK;
        return SNX_CTR_SVC_RETURN;
    }
    uint32_t result = read32(p, va, &value);
    if (result) {
        r[0] = result;
        return SNX_CTR_SVC_RETURN;
    }
    /* CTR's timed variants return Timeout even when the predicate does not
       suspend the thread, matching the reference kernel-facing behavior. */
    if ((value ^ 0x80000000u) >= (threshold ^ 0x80000000u)) {
        r[0] = type >= 3 ? TIMED_OUT : OK;
        return SNX_CTR_SVC_RETURN;
    }
    if (type == 2 || type == 4) {
        result = write32(p, va, value - 1);
        if (result) {
            r[0] = result;
            return SNX_CTR_SVC_RETURN;
        }
    }
    if (type >= 3 && !r[4] && !r[5]) {
        r[0] = TIMED_OUT;
        return SNX_CTR_SVC_RETURN;
    }
    snx_ctr_thread_block(p, id, va);
    /* the timed variants wake at their deadline with Timeout (nanoseconds: r4 low, r5 high), as Azahar's
       AddressArbiter's timeout callback does */
    if (type >= 3) snx_ctr_thread_deadline(p, r[4], r[5]);
    return SNX_CTR_SVC_BLOCKED;
}
static SnxCtrSvcResult wait_one(SnxCtrProcess* p, uint32_t* r) {
    uint32_t id = snx_ctr_handle_object(p, r[0]);
    if (!id) {
        r[0] = INVALID_HANDLE;
        return SNX_CTR_SVC_RETURN;
    }
    CtrObject* o = &p->objects[id];
    if (o->type == CTR_OBJ_EVENT && o->data.event.signaled) {
        if (o->data.event.reset_type == CTR_RESET_ONESHOT) o->data.event.signaled = 0;
        r[0] = OK;
        return SNX_CTR_SVC_RETURN;
    }
    if (o->type == CTR_OBJ_TIMER && o->data.timer.signaled) {
        if (o->data.timer.reset_type == CTR_RESET_ONESHOT) o->data.timer.signaled = 0;
        r[0] = OK;
        return SNX_CTR_SVC_RETURN;
    }
    /* Waits on the GSP interrupt event complete at real host-paced vblank
       deliveries, not at arbitrary dispatch calls. */
    if (o->type == CTR_OBJ_THREAD && o->data.thread.dead) {
        r[0] = OK;
        return SNX_CTR_SVC_RETURN;
    }
    if (o->type == CTR_OBJ_SEMAPHORE && o->data.semaphore.count) {
        --o->data.semaphore.count;
        r[0] = OK;
        return SNX_CTR_SVC_RETURN;
    }
    if (o->type == CTR_OBJ_PROCESS) return SNX_CTR_SVC_UNSUPPORTED; /* waiting on a process is not modelled */
    if (!snx_ctr_waitable(o->type)) { /* a session, a memory block and the like cannot be waited on */
        r[0] = INVALID_HANDLE;
        return SNX_CTR_SVC_RETURN;
    }
    if (o->type == CTR_OBJ_EVENT || o->type == CTR_OBJ_THREAD || o->type == CTR_OBJ_SEMAPHORE ||
        o->type == CTR_OBJ_TIMER || (o->data.mutex.depth && o->data.mutex.owner != p->current_thread)) {
        if (!r[2] && !r[3]) {
            r[0] = TIMED_OUT;
            return SNX_CTR_SVC_RETURN;
        }
        snx_ctr_thread_block(p, id, 0);
        snx_ctr_thread_deadline(p, r[2], r[3]); /* nanoseconds: r2 low, r3 high */
        return SNX_CTR_SVC_BLOCKED;
    }
    if (o->data.mutex.depth == UINT32_MAX)
        r[0] = INVALID_STATE;
    else {
        o->data.mutex.owner = p->current_thread;
        ++o->data.mutex.depth;
        r[0] = OK;
    }
    return SNX_CTR_SVC_RETURN;
}
bool snx_ctr_kernel_blocked(const SnxCtrProcess* p) {
    return p && p->objects[p->current_thread].data.thread.waiting_object;
}
SnxCtrSvcResult snx_ctr_svc(SnxCtrProcess* p, uint32_t number) {
    if (!p) return SNX_CTR_SVC_UNSUPPORTED;
    if (snx_ctr_kernel_blocked(p)) return SNX_CTR_SVC_BLOCKED;
    SnxArmCPU* cpu = &p->cpu.state.cpu;
    uint32_t* r = cpu->r;
    p->last_svc = number;
    ++p->svc_count;
    ++p->svc_counts[number & 255];
    cpu->exclusive_valid = 0; /* exception entry may clear the local monitor */
    switch (number) {
        case 0x01: {
            uint32_t out;
            uint32_t result = snx_ctr_kernel_memory(p, r[0], r[1], r[2], r[3], r[4], &out);
            r[0] = result;
            r[1] = out;
            return SNX_CTR_SVC_RETURN;
        }
        case 0x02: {
            /* QueryMemory: the region around the address in r2 whose pages share a state and permissions (Azahar
               merges neighbouring mappings that way). The answer is in registers, as libctru's svcQueryMemory stores
               it: MemInfo's base, size, permissions and state in r1 to r4, PageInfo's flags (0) in r5. Walks stay
               inside the user VA window [0, 0x20000000). */
            if (r[2] >= 0x20000000u) {
                r[0] = INVALID_ADDRESS;
                return SNX_CTR_SVC_RETURN;
            }
            Locks* l = malloc(sizeof *l);
            if (!l) {
                r[0] = OUT_OF_MEMORY;
                return SNX_CTR_SVC_RETURN;
            }
            locks(p, l);
            uint32_t base = r[2] & ~4095u, end = base + 4096, first = 0, first_perm = 0;
            query_class(p, l, base, &first, &first_perm);
            while (base >= 4096) {
                uint32_t state = 0, perm = 0;
                query_class(p, l, base - 4096, &state, &perm);
                if (state != first || perm != first_perm) break;
                base -= 4096;
            }
            while (end < 0x20000000u) {
                uint32_t state = 0, perm = 0;
                query_class(p, l, end, &state, &perm);
                if (state != first || perm != first_perm) break;
                end += 4096;
            }
            free(l);
            r[0] = OK;
            r[1] = base;
            r[2] = end - base;
            r[3] = first_perm;
            r[4] = first;
            r[5] = 0;
            return SNX_CTR_SVC_RETURN;
        }
        case 0x03: return SNX_CTR_SVC_EXIT;
        case 0x0b: { /* GetThreadPriority: handle r1, the priority the thread runs at (inherited included, Azahar's
                        GetPriority) out at r1, 0 on failure (libctru stores r1 either way) */
            uint32_t id = snx_ctr_handle_object(p, r[1]);
            bool thread = id && p->objects[id].type == CTR_OBJ_THREAD;
            r[0] = thread ? OK : INVALID_HANDLE;
            r[1] = thread ? snx_ctr_thread_priority(p, id) : 0;
            return SNX_CTR_SVC_RETURN;
        }
        case 0x0c: { /* SetThreadPriority: handle r0, priority r1, checked before the handle as CreateThread checks it
                        (Azahar svc.cpp): above 63 out of range, better than the APPLICATION limit not authorized */
            uint32_t id = snx_ctr_handle_object(p, r[0]);
            if (r[1] > 0x3f)
                r[0] = 0xe0e01bfdu; /* ResultOutOfRange */
            else if (r[1] < CTR_PRIORITY_LIMIT)
                r[0] = 0xd9001beau; /* ResultNotAuthorized */
            else if (!id || p->objects[id].type != CTR_OBJ_THREAD)
                r[0] = INVALID_HANDLE;
            else
                p->objects[id].data.thread.priority = r[1], r[0] = OK;
            return SNX_CTR_SVC_RETURN;
        }
        case 0x08: {
            uint32_t out = 0, result = snx_ctr_thread_create(p, r, &out);
            r[0] = result;
            r[1] = out;
            return SNX_CTR_SVC_RETURN;
        }
        case 0x09: /* ExitThread */
            cpu->exit = SNX_ARM_OK;
            cpu->syndrome = 0;
            return snx_ctr_thread_exit(p);
        case 0x0a: { /* SleepThread: the thread is not runnable until a host deadline, and the others run meanwhile;
                        the scheduler sleeps the host only when no thread can run. As Azahar's: zero gives the core
                        to another thread that can run, if any; -1 sleeps forever. */
            int64_t ns = (int64_t)((uint64_t)r[0] | (uint64_t)r[1] << 32);
            cpu->exit = SNX_ARM_OK; /* resumes after the SVC when it wakes */
            cpu->syndrome = 0;
            if (!ns) { /* behind every runnable thread of its priority, and after any other that can run now */
                snx_ctr_thread(p)->yield = 1;
                snx_ctr_thread_ready(p, p->current_thread);
                return SNX_CTR_SVC_RETURN;
            }
            snx_ctr_thread_deadline(p, r[0], r[1]);
            if (ns == -1) snx_ctr_thread(p)->wake_ns = UINT64_MAX;
            return SNX_CTR_SVC_BLOCKED;
        }
        case 0x13: { /* CreateMutex: locked by the caller when r1 is nonzero (Azahar takes it as a bool) */
            uint32_t out = 0, result = create_limited(p, CTR_OBJ_MUTEX, &out);
            if (!result && r[1]) {
                uint32_t id = snx_ctr_handle_object(p, out);
                p->objects[id].data.mutex.owner = p->current_thread;
                p->objects[id].data.mutex.depth = 1;
            }
            r[0] = result;
            r[1] = out;
            return SNX_CTR_SVC_RETURN;
        }
        case 0x15: { /* CreateSemaphore: initial count at r1, maximum at r2; an initial count above the maximum is
                        ResultInvalidCombinationKernel (Azahar semaphore.cpp) */
            uint32_t initial = r[1], max = r[2];
            if ((int32_t)initial < 0 || (int32_t)max < 0 || initial > max) {
                r[0] = 0xd90007eeu;
                r[1] = 0;
                return SNX_CTR_SVC_RETURN;
            }
            uint32_t out = 0, result = create_limited(p, CTR_OBJ_SEMAPHORE, &out);
            if (!result) {
                uint32_t id = snx_ctr_handle_object(p, out);
                p->objects[id].data.semaphore.count = initial;
                p->objects[id].data.semaphore.max = max;
            }
            r[0] = result;
            r[1] = out;
            return SNX_CTR_SVC_RETURN;
        }
        case 0x16: { /* ReleaseSemaphore: handle at r1, release count at r2, the previous count out at r1; past the
                        maximum is ResultOutOfRangeKernel (Azahar semaphore.cpp), a count of 0 changes nothing */
            uint32_t id = snx_ctr_handle_object(p, r[1]);
            if (!id || p->objects[id].type != CTR_OBJ_SEMAPHORE) {
                r[0] = INVALID_HANDLE;
                r[1] = 0;
                return SNX_CTR_SVC_RETURN;
            }
            CtrObject* o = &p->objects[id];
            uint32_t previous = o->data.semaphore.count;
            if ((int32_t)r[2] < 0 || r[2] > o->data.semaphore.max - previous) {
                r[0] = OUT_OF_RANGE;
                r[1] = 0;
                return SNX_CTR_SVC_RETURN;
            }
            o->data.semaphore.count = previous + r[2];
            snx_ctr_wake_waiters(p, id);
            r[0] = OK;
            r[1] = previous;
            return SNX_CTR_SVC_RETURN;
        }
        case 0x14: {
            uint32_t id = snx_ctr_handle_object(p, r[0]);
            if (!id || p->objects[id].type != CTR_OBJ_MUTEX)
                r[0] = INVALID_HANDLE;
            else if (p->objects[id].data.mutex.owner != p->current_thread || !p->objects[id].data.mutex.depth)
                r[0] = 0xd8e0041fu;
            else {
                if (!--p->objects[id].data.mutex.depth) {
                    p->objects[id].data.mutex.owner = 0;
                    snx_ctr_wake_waiters(p, id);
                }
                r[0] = OK;
            }
            return SNX_CTR_SVC_RETURN;
        }
        case 0x17: { /* CreateEvent: reset type at r1, one-shot, sticky or pulse, unchecked as in Azahar (any other
                        value stays signaled like a sticky event) */
            uint32_t out = 0;
            uint32_t reset = r[1], result = create_limited(p, CTR_OBJ_EVENT, &out);
            if (!result) p->objects[snx_ctr_handle_object(p, out)].data.event.reset_type = reset;
            r[0] = result;
            r[1] = out;
            return SNX_CTR_SVC_RETURN;
        }
        case 0x18: { /* SignalEvent: a pulse wakes its waiters and is clear again (Azahar Event::Signal) */
            uint32_t id = snx_ctr_handle_object(p, r[0]);
            if (!id || p->objects[id].type != CTR_OBJ_EVENT)
                r[0] = INVALID_HANDLE;
            else {
                p->objects[id].data.event.signaled = 1;
                snx_ctr_wake_waiters(p, id);
                if (p->objects[id].data.event.reset_type == CTR_RESET_PULSE) p->objects[id].data.event.signaled = 0;
                r[0] = OK;
            }
            return SNX_CTR_SVC_RETURN;
        }
        case 0x1a: { /* CreateTimer: reset type at r1, unchecked as in Azahar, the handle out at r1 */
            uint32_t out = 0;
            uint32_t reset = r[1], result = create_limited(p, CTR_OBJ_TIMER, &out);
            if (!result) p->objects[snx_ctr_handle_object(p, out)].data.timer.reset_type = reset;
            r[0] = result;
            r[1] = out;
            return SNX_CTR_SVC_RETURN;
        }
        case 0x1b: { /* SetTimer: handle r0, initial delay r2 (low) r3 (high), interval r1 (low) r4 (high), as libctru's
                        svcSetTimer loads them; negative values are refused before the handle (Azahar svc.cpp) */
            int64_t initial = (int64_t)((uint64_t)r[3] << 32 | r[2]), interval = (int64_t)((uint64_t)r[4] << 32 | r[1]);
            uint32_t id = snx_ctr_handle_object(p, r[0]);
            if (initial < 0 || interval < 0)
                r[0] = OUT_OF_RANGE;
            else if (!id || p->objects[id].type != CTR_OBJ_TIMER)
                r[0] = INVALID_HANDLE;
            else {
                /* Azahar Timer::Set: the previous schedule is cancelled; no initial delay signals at once */
                uint64_t now = snx_ctr_now_ns();
                CtrObject* o = &p->objects[id];
                o->data.timer.interval = (uint64_t)interval;
                o->data.timer.deadline = initial ? now + (uint64_t)initial : now;
                if (!initial) snx_ctr_timer_signal(p, id, now);
                r[0] = OK;
            }
            return SNX_CTR_SVC_RETURN;
        }
        case 0x1c:   /* CancelTimer */
        case 0x1d: { /* ClearTimer */
            uint32_t id = snx_ctr_handle_object(p, r[0]);
            if (!id || p->objects[id].type != CTR_OBJ_TIMER)
                r[0] = INVALID_HANDLE;
            else {
                if (number == 0x1c)
                    p->objects[id].data.timer.deadline = 0;
                else
                    p->objects[id].data.timer.signaled = 0;
                r[0] = OK;
            }
            return SNX_CTR_SVC_RETURN;
        }
        case 0x19: {
            uint32_t id = snx_ctr_handle_object(p, r[0]);
            if (!id || p->objects[id].type != CTR_OBJ_EVENT)
                r[0] = INVALID_HANDLE;
            else {
                p->objects[id].data.event.signaled = 0;
                r[0] = OK;
            }
            return SNX_CTR_SVC_RETURN;
        }
        case 0x1e: { /* CreateMemoryBlock: other permissions r0, address r1, size r2, own permissions r3 (libctru's
                        svcCreateMemoryBlock), the handle out at r1 (Azahar svc.cpp and shared_memory.cpp) */
            uint32_t other = r[0], address = r[1], size = r[2], mine = r[3], out = 0, result = OK;
            bool valid = (mine <= 3 || mine == DONT_CARE) && (other <= 3 || other == DONT_CARE);
            if (size & 4095)
                result = MISALIGNED_SIZE;
            else if (!valid)
                result = INVALID_COMBINATION;
            else if (!address)
                result =
                    0xd92007eau; /* an APPLICATION process may not ask the kernel for a block (3dbrew, Azahar TODO) */
            else if (address & 4095)
                result = MISALIGNED_ADDRESS;
            else if (address < 0x00100000u || (uint64_t)address + size > 0x14000000u)
                result = INVALID_ADDRESS;
            else if (!size || !(private_heap(p, address, size) || private_data(p, address, size)))
                result = INVALID_STATE; /* the owner's range must be private read-write memory; it is Locked now */
            else
                result = create_limited(p, CTR_OBJ_SHARED, &out);
            if (!result) {
                CtrObject* o = &p->objects[snx_ctr_handle_object(p, out)];
                o->data.shared.size = size;
                o->data.shared.permissions = mine;
                o->data.shared.address = address;
                o->data.shared.other_permissions = other;
                o->data.shared.by_application = 1;
                /* the owner keeps its own rights to the Locked range (Azahar CreateSharedMemory); they return to
                   read-write when the block goes (snx_ctr_object_release) */
                (void)snx_ctr_protect(p->space, address, size, mine == DONT_CARE ? 3u : mine);
                p->cpu.state.cpu.exclusive_valid = 0;
            }
            r[0] = result;
            r[1] = out;
            return SNX_CTR_SVC_RETURN;
        }
        case 0x20: { /* UnmapMemoryBlock: handle r0, the address it was mapped at r1 */
            uint32_t id = snx_ctr_handle_object(p, r[0]);
            if (!id || p->objects[id].type != CTR_OBJ_SHARED) {
                r[0] = INVALID_HANDLE;
                return SNX_CTR_SVC_RETURN;
            }
            r[0] = INVALID_STATE;
            for (unsigned i = 0; i < sizeof p->shared_maps / sizeof *p->shared_maps; ++i) {
                CtrSharedMap* m = &p->shared_maps[i];
                if (m->object != id || m->va != r[1]) continue;
                if (snx_ctr_unmap(p->space, m->va, m->size) == SNX_CTR_MEM_OK) {
                    *m = (CtrSharedMap){0};
                    snx_ctr_object_release(p, id); /* the mapping's reference */
                    r[0] = OK;
                }
                break;
            }
            return SNX_CTR_SVC_RETURN;
        }
        case 0x1f: {
            uint32_t id = snx_ctr_handle_object(p, r[0]);
            if (!id || p->objects[id].type != CTR_OBJ_SHARED)
                r[0] = INVALID_HANDLE;
            else if (p->objects[id].data.shared.by_application)
                r[0] = map_own_block(p, id, r[1], r[2], r[3]);
            else {
                CtrObject* o = &p->objects[id];
                if ((r[1] & 4095) || r[1] < 0x10000000 || (uint64_t)r[1] + o->data.shared.size > 0x1f800000)
                    r[0] = INVALID_ADDRESS;
                else if (r[2] != o->data.shared.permissions || (r[3] != 0 && r[3] != 0x10000000))
                    return SNX_CTR_SVC_UNSUPPORTED;
                else if (!free_map_slot(p))
                    r[0] = OUT_OF_MEMORY;
                else {
                    /* each mapping has its own record and reference, as map_own_block's do: a block mapped twice
                       is unmapped address by address */
                    SnxCtrMemResult mapped =
                        snx_ctr_map(p->space, r[1], o->data.shared.physical, o->data.shared.size, r[2]);
                    r[0] = mapped == SNX_CTR_MEM_OK      ? record_map(p, id, r[1], o->data.shared.size)
                           : mapped == SNX_CTR_MEM_NOMEM ? OUT_OF_MEMORY
                                                         : INVALID_STATE;
                }
            }
            return SNX_CTR_SVC_RETURN;
        }
        case 0x21: { /* CreateAddressArbiter */
            uint32_t out = 0, result = create_limited(p, CTR_OBJ_ARBITER, &out);
            r[0] = result;
            r[1] = out;
            return SNX_CTR_SVC_RETURN;
        }
        case 0x22: return arbitrate(p, cpu);
        case 0x23: r[0] = snx_ctr_handle_close(p, r[0]); return SNX_CTR_SVC_RETURN;
        case 0x24: return wait_one(p, r);
        case 0x25: return snx_ctr_wait_many(p, r);
        case 0x27: {
            uint32_t id = snx_ctr_handle_object(p, r[1]), out = 0;
            r[0] = id ? snx_ctr_handle_add(p, id, &out) : INVALID_HANDLE;
            r[1] = out;
            return SNX_CTR_SVC_RETURN;
        }
        case 0x28: {
            double elapsed = snx_seconds() - p->clock_origin;
            if (!(elapsed >= 0 && elapsed < 0x1p64 / 268111856.0)) return SNX_CTR_SVC_UNSUPPORTED;
            uint64_t ticks = (uint64_t)(elapsed * 268111856.0);
            r[0] = (uint32_t)ticks;
            r[1] = (uint32_t)(ticks >> 32);
            return SNX_CTR_SVC_RETURN;
        }
        case 0x29: { /* GetHandleInfo: handle r1, type r2, the value out at r1:r2 (Azahar svc.cpp) */
            uint32_t id = snx_ctr_handle_object(p, r[1]), type = r[2];
            uint64_t value = 0;
            if (!id)
                r[0] = INVALID_HANDLE;
            else if (type == 0 || type == 2 || type == 0x32107) /* process elapsed ticks: created at the clock origin */
                r[0] = OK;
            else if (type == 1) /* the reference count, as Azahar approximates it */
                r[0] = OK, value = p->objects[id].refs - 1;
            else
                r[0] = INVALID_ENUM_KERNEL;
            r[1] = (uint32_t)value;
            r[2] = (uint32_t)(value >> 32);
            return SNX_CTR_SVC_RETURN;
        }
        case 0x2a: { /* GetSystemInfo: type r1, parameter r2, the value out at r1:r2, as Azahar answers each */
            uint32_t type = r[1], param = r[2];
            uint64_t value = 0;
            uint32_t result = OK;
            bool new_3ds = snx_ctr_memory_profile(p->memory)->arm11_cores == 4;
            if (type == 0 && (param == 0 || param == 1))
                value = memory_used(p); /* APPLICATION (with SYSTEM and BASE, which hold nothing, for 0) */
            else if (type == 26)
                value = 5;                             /* the kernel's spawned process count */
            else if (type == 0x10000 && param == 0x80) /* Luma: the APPLICATION region's size */
                value = p->app_limit;
            else if (type == 0x10000 && param == 0x201) /* Luma: a New 3DS */
                value = new_3ds;
            else if (type == 0x10001 && !new_3ds)
                result = INVALID_ENUM_KERNEL; /* New 3DS information, only on a New 3DS */
            /* 0x20000 is the emulator information Azahar gives about itself: nothing is claimed here (0) */
            r[0] = result;
            r[1] = (uint32_t)value;
            r[2] = (uint32_t)(value >> 32);
            return SNX_CTR_SVC_RETURN;
        }
        case 0x2b: { /* GetProcessInfo: process r1, type r2, the value out at r1:r2 */
            uint32_t id = snx_ctr_handle_object(p, r[1]), type = r[2];
            uint64_t value = 0;
            uint32_t result = OK;
            if (!id || p->objects[id].type != CTR_OBJ_PROCESS)
                result = INVALID_HANDLE;
            else if (type == 0 || type == 2)
                value = memory_used(p);
            else if (type == 20) /* FCRAM's physical address less the linear heap's virtual one */
                value = (uint32_t)(CTR_PHYS_BASE - CTR_LINEAR);
            else if (type >= 21 && type <= 23)
                result = NOT_IMPLEMENTED; /* QTM, as in Azahar */
            else if (type == 0x10000)
                for (unsigned i = 0; i < sizeof p->name; ++i) /* the codeset's eight-character name, as bytes */
                    value |= (uint64_t)(unsigned char)p->name[i] << (8 * i);
            else if (type == 0x10001)
                value = p->title_id;
            else if (type >= 0x10002 && type <= 0x10004) { /* text, read-only and data sizes, the data with BSS */
                unsigned s = type - 0x10002;
                value = (uint64_t)p->segment[s].pages * 4096 + (s == 2 ? (p->bss_size + 4095u) & ~4095u : 0);
            } else if (type >= 0x10005 && type <= 0x10007)
                value = p->segment[type - 0x10005].address;
            else if (!(type <= 8))
                result = INVALID_ENUM_KERNEL; /* 1 and 3 to 8 succeed unanswered, as in Azahar */
            r[0] = result;
            r[1] = (uint32_t)value;
            r[2] = (uint32_t)(value >> 32);
            return SNX_CTR_SVC_RETURN;
        }
        case 0x2c: { /* GetThreadInfo: thread r1, type r2; only 0x10000, the thread's TLS address */
            uint32_t id = snx_ctr_handle_object(p, r[1]);
            uint64_t value = 0;
            if (!id || p->objects[id].type != CTR_OBJ_THREAD)
                r[0] = INVALID_HANDLE;
            else if (r[2] != 0x10000)
                r[0] = INVALID_ENUM_KERNEL;
            else
                r[0] = OK, value = id == p->current_thread ? p->cpu.state.cpu.tpidruro
                                                           : p->objects[id].data.thread.context.tpidruro;
            r[1] = (uint32_t)value;
            r[2] = (uint32_t)(value >> 32);
            return SNX_CTR_SVC_RETURN;
        }
        case 0x36: { /* GetProcessIdOfThread: thread r1, the process ID out at r1 */
            uint32_t id = snx_ctr_handle_object(p, r[1]);
            bool thread = id && p->objects[id].type == CTR_OBJ_THREAD;
            r[0] = thread ? OK : INVALID_HANDLE;
            r[1] = thread ? CTR_PROCESS_ID : 0;
            return SNX_CTR_SVC_RETURN;
        }
        case 0x38: { /* GetResourceLimit: process r1, a handle to its resource limit out at r1 */
            uint32_t id = snx_ctr_handle_object(p, r[1]), out = 0, result = OK;
            if (!id || p->objects[id].type != CTR_OBJ_PROCESS)
                result = INVALID_HANDLE;
            else if (p->resource_limit)
                result = snx_ctr_handle_add(p, p->resource_limit, &out);
            else if (!(result = snx_ctr_object_create(p, CTR_OBJ_RESOURCE_LIMIT, &out))) {
                p->resource_limit = snx_ctr_handle_object(p, out);
                ++p->objects[p->resource_limit].refs; /* the process keeps it */
            }
            r[0] = result;
            r[1] = out;
            return SNX_CTR_SVC_RETURN;
        }
        case 0x39:   /* GetResourceLimitLimitValues */
        case 0x3a: { /* GetResourceLimitCurrentValues: values out r0, limit r1, names r2, count r3 (s64 each) */
            uint32_t id = snx_ctr_handle_object(p, r[1]), values = r[0], names = r[2], count = r[3];
            if (!id || p->objects[id].type != CTR_OBJ_RESOURCE_LIMIT) {
                r[0] = INVALID_HANDLE;
                return SNX_CTR_SVC_RETURN;
            }
            uint32_t result = OK;
            for (uint32_t i = 0; i < count && !result; ++i) {
                uint32_t name;
                if ((result = read32(p, names + i * 4, &name))) break;
                uint64_t value;
                if (number == 0x39 && name >= 10)
                    result = INVALID_ENUM_KERNEL;
                else if (!(result = number == 0x39 ? limit_value(p, name, &value) : current_value(p, name, &value)))
                    result = write32(p, values + i * 8, (uint32_t)value) |
                             write32(p, values + i * 8 + 4, (uint32_t)(value >> 32));
            }
            r[0] = result;
            return SNX_CTR_SVC_RETURN;
        }
        case 0x3c: /* Break: reason r0. 0 panic, 1 assert and 2 user mean the title gave up, so the run stops here;
                      3 and 4 (USERBREAK_LOAD_RO, UNLOAD_RO) tell a debugger that ldr:ro loaded or unloaded a CRO,
                      and with none attached they return (Azahar logs every reason and carries on) */
            if (r[0] == 3 || r[0] == 4) {
                r[0] = OK;
                return SNX_CTR_SVC_RETURN;
            }
            fprintf(stderr, "3ds: svcBreak(%s, 0x%08x, 0x%08x)\n",
                    r[0] == 0   ? "panic"
                    : r[0] == 1 ? "assert"
                    : r[0] == 2 ? "user"
                                : "unknown",
                    r[1], r[2]);
            return SNX_CTR_SVC_UNSUPPORTED;
        case 0x52: /* InvalidateProcessDataCache */
        case 0x53: /* StoreProcessDataCache */
        case 0x54: /* FlushProcessDataCache: process r0, address r1, size r2. Guest memory has no cache here */
            r[0] = snx_ctr_handle_object(p, r[0]) == 1 ? OK : INVALID_HANDLE;
            return SNX_CTR_SVC_RETURN;
        case 0x93: /* Luma's InvalidateInstructionCacheRange */
        case 0x94: /* Luma's InvalidateEntireInstructionCache: compiled code is checked against guest memory at every
                      block, so code written since stops at its first block instead (SNX_ARM_CODE_CHANGED) */
            r[0] = OK;
            return SNX_CTR_SVC_RETURN;
        case 0x2d: {
            uint32_t out = 0, result = snx_ctr_named_connect(p, r[1], &out);
            r[0] = result;
            r[1] = out;
            return SNX_CTR_SVC_RETURN;
        }
        case 0x37: { /* GetThreadId: the thread's object index, unique among the process's live threads */
            uint32_t id = snx_ctr_handle_object(p, r[1]);
            bool thread = id && p->objects[id].type == CTR_OBJ_THREAD;
            r[0] = thread ? OK : INVALID_HANDLE;
            r[1] = thread ? id : 0; /* libctru stores r1 either way */
            return SNX_CTR_SVC_RETURN;
        }
        case 0x3d: { /* OutputDebugString: validated in full, captured to the host log */
            /* a negative length writes nothing (Azahar), a longer one is cut to 256 bytes */
            if ((int32_t)r[1] < 0) {
                r[0] = OK;
                return SNX_CTR_SVC_RETURN;
            }
            uint32_t length = r[1] > 256u ? 256u : r[1];
            unsigned char text[256];
            if (length && (snx_ctr_check_ram(p->space, r[0], length, SNX_CTR_READ) != SNX_CTR_MEM_OK ||
                           snx_ctr_read_ram(p->space, r[0], text, length) != SNX_CTR_MEM_OK)) {
                r[0] = INVALID_ADDRESS;
                return SNX_CTR_SVC_RETURN;
            }
            uint32_t used = 0;
            while (used < length && text[used]) ++used;
            if (used) fprintf(stderr, "3ds debug: %.*s\n", (int)used, (const char*)text);
            r[0] = OK;
            return SNX_CTR_SVC_RETURN;
        }
        case 0x32: {
            uint32_t result = 0;
            SnxCtrSvcResult state = snx_ctr_ipc_send(p, r[0], &result);
            if (state == SNX_CTR_SVC_RETURN) r[0] = result;
            return state;
        }
        case 0x35: {
            uint32_t id = snx_ctr_handle_object(p, r[1]);
            r[0] = id && p->objects[id].type == CTR_OBJ_PROCESS ? 0 : INVALID_HANDLE;
            r[1] = r[0] ? 0 : CTR_PROCESS_ID;
            return SNX_CTR_SVC_RETURN;
        }
        default: return SNX_CTR_SVC_UNSUPPORTED;
    }
}
