/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from Eden (src/core/hle/kernel/svc/svc_memory.cpp, svc_info.cpp, svc_physical_memory.cpp,
   svc_shared_memory.cpp, svc_transfer_memory.cpp, svc_code_memory.cpp, svc_query_memory.cpp, k_page_table_base.cpp,
   k_memory_block.h):
   SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
   SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
   SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project */
/* the memory SVCs (heap, physical memory, permissions and attributes, MapMemory, shared, transfer and code memory,
   QueryMemory), the process's memory accounting, and GetInfo */
#include "nx/hle/kernel/kernel_int.h"
#include "nx/hle/hle.h"
#include "nx/runtime/clock.h"

#define R_BADPERM    0xD801 /* InvalidNewMemoryPermission */
#define R_BADPOINTER 0xE601 /* InvalidPointer: a user copy that faults */
#define MIB          (1ull << 20)

/* the MemoryState flags the checks below test (k_memory_block.h, Switchbrew's MemoryState) */
#define MSF_CAN_REPROTECT   (1u << 8)
#define MSF_CAN_ALIAS       (1u << 15)
#define MSF_CAN_TRANSFER    (1u << 17)
#define MSF_CAN_MAP_PROCESS (1u << 23)
#define MSF_CAN_CHANGE_ATTR (1u << 24)
#define MSF_CAN_CODE_MEMORY (1u << 25)
/* MemoryAttribute: Locked, IpcLocked, DeviceShared, Uncached, PermissionLocked */
#define MA_LOCKED   1u
#define MA_SET_MASK 0x18u /* what SetMemoryAttribute may change: Uncached and PermissionLocked */

/* ============================================================ helpers */
static int in_region(u64 a, u64 size, u64 base, u64 rsize) { return a >= base && size <= rsize && a - base <= rsize - size; }
static int overlaps(u64 a, u64 size, u64 base, u64 rsize) { return rsize && a < base + rsize && base < a + size; }
/* the console's KPageTableBase::CanContain, for the states mapped here: normal memory in the heap region, the stack
   state in the stack region, the rest (shared, generated code, code out) anywhere in the address space outside the
   heap and alias regions */
static int can_contain(u64 a, u64 size, u32 state) {
    if (state == MS_NORMAL) return in_region(a, size, HEAP_BASE, HEAP_SIZE);
    if (state == MS_STACK) return in_region(a, size, STACK_BASE, STACK_SIZE);
    return in_region(a, size, AS_BASE, AS_END - AS_BASE) && !overlaps(a, size, HEAP_BASE, HEAP_SIZE) &&
           !overlaps(a, size, ALIAS_BASE, ALIAS_SIZE);
}
/* every block of [a, a + size) has the state flag (state itself, with flag 0), the permission perm (ANY: any) and,
   of the attributes in attr_mask, exactly attr */
#define ANY 0xFFFFFFFFu
static int range_ok(u64 a, u64 size, u32 state, u32 flag, u32 perm, u32 attr_mask, u32 attr) {
    u64 end = a + size;
    while (a < end) {
        MemBlock b;
        mem_query(a, &b);
        if (b.addr > a || b.addr + b.size <= a) return 0;
        if (flag ? !(b.state & flag) : b.state != state) return 0;
        if ((perm != ANY && b.perm != perm) || (b.attr & attr_mask) != attr) return 0;
        a = b.addr + b.size;
    }
    return 1;
}
/* the permission of each block of the range, keeping its state */
static void set_perm(u64 a, u64 size, u32 perm) {
    u64 end = a + size;
    while (a < end) {
        MemBlock b;
        u64 hi;
        mem_query(a, &b);
        hi = b.addr + b.size < end ? b.addr + b.size : end;
        if (b.state != MS_FREE) mem_set_state(a, hi - a, b.state, perm);
        a = hi;
    }
}
/* the argument checks every mapping SVC makes first: InvalidAddress, InvalidSize, then InvalidCurrentMemory for a
   range that wraps around */
static u32 check_range(u64 a, u64 size) {
    R_UNLESS(!(a & 0xFFF), R_BADADDR);
    R_UNLESS(size && !(size & 0xFFF), R_BADSIZE);
    R_UNLESS(a + size > a, R_BADMEMSTATE);
    R_SUCCEED();
}
/* guest memory the kernel writes for the game (QueryMemory's info, a thread context): mapped and writable */
static int user_writable(u64 a, u64 size) {
    u64 end = a + size;
    if (a >= AS_END || size > AS_END - a) return 0;
    while (a < end) {
        MemBlock b;
        mem_query(a, &b);
        if (b.state == MS_FREE || !(b.perm & PERM_W)) return 0;
        a = b.addr + b.size;
    }
    return 1;
}

/* ============================================================ accounting */
/* The application pool for each memory_gb (Mesosphere's KSystemControl application pool sizes for the 4, 6 and 8 GiB
   boards), see docs/MEMORY_ACCOUNTING.md */
u64 mem_total(void) {
    if (g_settings.memory_gb >= 8) return 6964 * MIB;
    if (g_settings.memory_gb >= 6) return 4916 * MIB;
    return 3285 * MIB;
}
/* the loaded images, the main thread's stack, the heap, physical memory and the system resource, charged once */
u64 used_memory(void) {
    return g_code_memory + g_main_stack_size + __atomic_load_n(&g_heap_size, __ATOMIC_ACQUIRE) +
           __atomic_load_n(&g_phys_mapped, __ATOMIC_ACQUIRE) + g_hle.kernel.sysres_size;
}
/* what the heap and physical memory may use between them */
static u64 heap_budget(void) {
    u64 fixed = g_code_memory + g_main_stack_size + g_hle.kernel.sysres_size, total = mem_total();
    return fixed < total ? total - fixed : 0;
}
static u32 mem_result(int r) {
    return r == MEM_LIMIT ? R_LIMIT : r == MEM_NOMEM ? R_OUTOFMEM : r == MEM_BADSTATE ? R_BADMEMSTATE : 0;
}
/* for the crash report: read without the memory lock, which the crashed thread may hold */
void mem_report(char* out, int n) {
    snprintf(out, (size_t)n, "heap %llu MiB, physical %llu MiB, code %llu MiB, %llu of %llu MiB used",
             (unsigned long long)(__atomic_load_n(&g_heap_size, __ATOMIC_RELAXED) >> 20),
             (unsigned long long)(__atomic_load_n(&g_phys_mapped, __ATOMIC_RELAXED) >> 20),
             (unsigned long long)(g_code_memory >> 20), (unsigned long long)(used_memory() >> 20),
             (unsigned long long)(mem_total() >> 20));
}

/* ============================================================ the heap and physical memory */
/* svcSetHeapSize: 2 MiB steps below 8 GiB (InvalidSize), within what the heap region leaves beside the process's own
   memory and within heap_max_mb (OutOfMemory), then the resource limit (LimitReached). Each request is logged: a
   refused one is often why a game stops at boot */
u32 svc_set_heap_size(u64 size) {
    u64 proc = g_code_memory + g_main_stack_size;
    u32 r;
    if ((size & 0x1FFFFF) || size >= (8ull << 30))
        r = R_BADSIZE;
    else if (proc > HEAP_SIZE || size > HEAP_SIZE - proc || (g_heap_max && size > g_heap_max))
        r = R_OUTOFMEM;
    else
        r = mem_result(mem_heap_set(size, heap_budget()));
    LOG("heap: %llu MB requested%s (result %x)\n", (unsigned long long)(size >> 20), R_FAILED(r) ? ", refused" : "",
        r);
    return r;
}
/* svcMapPhysicalMemory and svcUnmapPhysicalMemory: page-aligned (InvalidAddress), whole pages (InvalidSize), no wrap
   (InvalidMemoryRegion), a process with a system resource (InvalidState), inside the alias region
   (InvalidMemoryRegion) */
static u32 check_physical(u64 addr, u64 size) {
    R_UNLESS(!(addr & 0xFFF), R_BADADDR);
    R_UNLESS(size && !(size & 0xFFF), R_BADSIZE);
    R_UNLESS(addr + size > addr, R_BADREGION);
    R_UNLESS(g_hle.kernel.sysres_size, R_BADSTATE);
    R_UNLESS(in_region(addr, size, ALIAS_BASE, ALIAS_SIZE), R_BADREGION);
    R_SUCCEED();
}
u32 svc_map_physical(u64 addr, u64 size) {
    R_TRY(check_physical(addr, size));
    return mem_result(mem_map_physical(addr, size, heap_budget()));
}
u32 svc_unmap_physical(u64 addr, u64 size) {
    R_TRY(check_physical(addr, size));
    return mem_result(mem_unmap_physical(addr, size));
}

/* ============================================================ permissions, attributes, MapMemory */
/* svcSetMemoryPermission: none, read or read-write, on memory whose state may be reprotected and has no attribute */
u32 svc_set_mem_perm(u64 addr, u64 size, u32 perm) {
    R_TRY(check_range(addr, size));
    if (perm != PERM_NONE && perm != PERM_R && perm != PERM_RW) R_THROW(R_BADPERM);
    if (!in_region(addr, size, 0, AS_END) || !range_ok(addr, size, 0, MSF_CAN_REPROTECT, ANY, ~0u, 0))
        R_THROW(R_BADMEMSTATE);
    set_perm(addr, size, perm);
    R_SUCCEED();
}
/* svcSetMemoryAttribute: mask names the attributes that change (a superset of attr), only Uncached and
   PermissionLocked; the memory's state must allow it and it must be neither locked nor shared with a device */
u32 svc_set_mem_attr(u64 addr, u64 size, u32 mask, u32 attr) {
    R_TRY(check_range(addr, size));
    R_UNLESS((mask | attr) == mask && (mask | attr | MA_SET_MASK) == MA_SET_MASK, R_BADCOMBO);
    if (!in_region(addr, size, 0, AS_END) || !range_ok(addr, size, 0, MSF_CAN_CHANGE_ATTR, ANY, ~MA_SET_MASK, 0))
        R_THROW(R_BADMEMSTATE);
    mem_set_attr(addr, size, mask, attr);
    R_SUCCEED();
}
/* svcMapMemory: a view of src at dst, in the stack region (in the stack state) or, as the first firmwares had it,
   the alias region (as normal memory), src locked meanwhile (no permission, locked). The view and src are the same
   pages with the alias pool (mem_alias_begin); without it the view is a copy that svcUnmapMemory copies back */
static u32 check_map_memory(u64 dst, u64 src, u64 size, u32* state) {
    R_UNLESS(!((dst | src) & 0xFFF), R_BADADDR);
    R_UNLESS(size && !(size & 0xFFF), R_BADSIZE);
    R_UNLESS(dst + size > dst && src + size > src, R_BADMEMSTATE);
    R_UNLESS(in_region(src, size, 0, AS_END), R_BADMEMSTATE);
    if (can_contain(dst, size, MS_STACK))
        *state = MS_STACK;
    else if (ALIAS_SIZE && in_region(dst, size, ALIAS_BASE, ALIAS_SIZE))
        *state = MS_NORMAL;
    else
        R_THROW(R_BADREGION);
    R_SUCCEED();
}
u32 svc_map_memory(u64 dst, u64 src, u64 size) {
    u32 state, r;
    R_TRY(check_map_memory(dst, src, size, &state));
    R_UNLESS(range_ok(src, size, 0, MSF_CAN_ALIAS, PERM_RW, ~0u, 0), R_BADMEMSTATE);
    R_TRY(mem_result(mem_alias_begin(src, size)));
    r = mem_result(mem_alias_map(dst, src, size, state, PERM_RW));
    if (R_FAILED(r)) {
        mem_alias_end(src, size);
        R_THROW(r);
    }
    set_perm(src, size, PERM_NONE);
    mem_set_attr(src, size, MA_LOCKED, MA_LOCKED);
    R_SUCCEED();
}
u32 svc_unmap_memory(u64 dst, u64 src, u64 size) {
    u32 state;
    R_TRY(check_map_memory(dst, src, size, &state));
    if (!range_ok(src, size, 0, MSF_CAN_ALIAS, PERM_NONE, ~0u, MA_LOCKED) ||
        !range_ok(dst, size, state, 0, ANY, MA_LOCKED, 0))
        R_THROW(R_BADMEMSTATE);
    mem_alias_unmap(dst, size, src);
    mem_alias_end(src, size);
    mem_set_attr(src, size, MA_LOCKED, 0);
    set_perm(src, size, PERM_RW);
    R_SUCCEED();
}

/* svcMapProcessMemory: a view of a process's memory at dst, shared code read and written, src staying as it is. The
   one process here is the caller's own, which mods for Atmosphère map their code through to patch it (exlaunch's
   RwPages): the view and the code are the same pages with the alias pool, and a copy written back at the unmap
   without. Either way the code compiled from src is stale once the view goes (mem_alias_unmap) */
static u32 check_process_memory(u64 dst, u32 process, u64 src, u64 size) {
    KObj* p;
    R_UNLESS(!((dst | src) & 0xFFF), R_BADADDR);
    R_UNLESS(size && !(size & 0xFFF), R_BADSIZE);
    R_UNLESS(dst + size > dst && src + size > src, R_BADMEMSTATE);
    p = h_get_ref(process, KO_PROCESS);
    R_UNLESS(p, R_BADHANDLE);
    kobj_unref(p);
    R_UNLESS(can_contain(dst, size, MS_SHAREDCODE), R_BADREGION);
    R_UNLESS(in_region(src, size, 0, AS_END), R_BADMEMSTATE);
    R_SUCCEED();
}
u32 svc_map_process_memory(u64 dst, u32 process, u64 src, u64 size) {
    u32 r;
    int i;
    R_TRY(check_process_memory(dst, process, src, size));
    R_UNLESS(range_ok(src, size, 0, MSF_CAN_MAP_PROCESS, ANY, ~0u, 0), R_BADMEMSTATE);
    R_UNLESS(range_ok(dst, size, MS_FREE, 0, ANY, ~0u, 0), R_BADMEMSTATE);
    R_TRY(mem_result(mem_alias_begin(src, size)));
    r = mem_result(mem_alias_map(dst, src, size, MS_SHAREDCODE, PERM_RW));
    if (R_FAILED(r)) {
        mem_alias_end(src, size);
        return r;
    }
    /* the view of code keeps src as it is now, before anything is written through it (svc_code_line) */
    pm_lock(&g_host.svcmem.views_lk);
    for (i = 0; i < KCODEVIEW_MAX && g_hle.svcmem.views[i].size; i++) {}
    if (i < KCODEVIEW_MAX) {
        KCodeView* v = &g_hle.svcmem.views[i];
        v->dst = dst;
        v->src = src;
        v->size = size;
        v->snap = (u8*)malloc((size_t)size);
        if (v->snap) memcpy(v->snap, GP(src), (size_t)size);
    } else
        WARN_LIMIT(1, "kernel: more than %d views of code at once: their flushes recompile what they cover\n",
                   KCODEVIEW_MAX);
    pm_unlock(&g_host.svcmem.views_lk);
    R_SUCCEED();
}
u32 svc_unmap_process_memory(u64 dst, u32 process, u64 src, u64 size) {
    int i;
    R_TRY(check_process_memory(dst, process, src, size));
    R_UNLESS(range_ok(dst, size, MS_SHAREDCODE, 0, PERM_RW, ~0u, 0), R_BADMEMSTATE);
    R_UNLESS(range_ok(src, size, 0, MSF_CAN_MAP_PROCESS, ANY, ~0u, 0), R_BADMEMSTATE);
    mem_alias_unmap(dst, size, src);
    mem_alias_end(src, size);
    pm_lock(&g_host.svcmem.views_lk);
    for (i = 0; i < KCODEVIEW_MAX; i++)
        if (g_hle.svcmem.views[i].size && g_hle.svcmem.views[i].dst == dst) {
            free(g_hle.svcmem.views[i].snap);
            memset(&g_hle.svcmem.views[i], 0, sizeof g_hle.svcmem.views[i]);
            break;
        }
    pm_unlock(&g_host.svcmem.views_lk);
    R_SUCCEED();
}
/* A line a flush names through a view or in the code it shows changed when its bytes differ from the view's
   snapshot. A mod may flush its whole view after a few writes (exlaunch's RwPages), as a cache line that didn't
   change costs the console nothing: here such a line keeps its compiled code */
int svc_code_line(u64 addr, u64* code) {
    int i, r = -1;
    addr &= ~(u64)63;
    *code = addr;
    pm_lock(&g_host.svcmem.views_lk);
    for (i = 0; i < KCODEVIEW_MAX; i++) {
        KCodeView* v = &g_hle.svcmem.views[i];
        u64 at;
        if (!v->size) continue;
        if (addr - v->dst < v->size)
            at = addr - v->dst;
        else if (addr - v->src < v->size)
            at = addr - v->src;
        else
            continue;
        *code = v->src + at;
        if (!v->snap || at + 64 > v->size) break; /* nothing kept to compare with: it may have changed */
        r = memcmp(v->snap + at, GP(*code), 64) != 0;
        if (r) memcpy(v->snap + at, GP(*code), 64);
        break;
    }
    pm_unlock(&g_host.svcmem.views_lk);
    return r;
}

/* ============================================================ QueryMemory */
/* svcQueryMemory: the block at addr as the console's MemoryInfo (base, size, the state's low byte, attributes,
   permission, IPC and device reference counts), into info */
u32 svc_query(u64 info, u64 addr) {
    MemBlock b;
    u32 w[6];
    R_UNLESS(user_writable(info, 0x28), R_BADPOINTER);
    mem_query(addr, &b);
    w[0] = b.state & 0xFF;
    w[1] = b.attr;
    w[2] = b.perm;
    w[3] = b.ipc;
    w[4] = b.dev;
    w[5] = 0;
    snx_le32_array(w, 6);
    gw64(info, b.addr);
    gw64(info + 8, b.size);
    memcpy(GP(info + 16), w, sizeof w);
    R_SUCCEED();
}

/* ============================================================ shared memory */
/* A service's shared memory is pages of the alias pool from its creation: the service writes them through host, and
   each guest mapping maps the same pages, as many at once as the game makes, each holding a reference to the object
   until it is unmapped (as the console's process does). Without the pool (Windows) host is memory of its own until
   the game maps it, once: the mapping is a copy, kshmem_ptr gives the service the guest's pages while it lasts, and
   unmapping copies them back */
static u64 shm_whole(const KShmem* s) { return (s->size + 0xFFF) & ~0xFFFull; }
void kshmem_destroy(KObj* o) {
    KShmem* s = (KShmem*)o;
    if (s->pool)
        mem_pool_host_free(s->host, s->pool - 1, shm_whole(s));
    else
        free(s->host);
    pm_lock(&g_host.kernel.pool_lk);
    POOL_PUT(s);
    pm_unlock(&g_host.kernel.pool_lk);
}
/* with no reference: the first handle takes one, or the service that keeps it. The object is from g_hle.svcmem.shmems
   (KSHMEM_MAX), its pages (host) of the size the service asks for its own */
KShmem* kshmem_new(u64 size, const char* name) {
    KShmem* s;
    u64 off;
    pm_lock(&g_host.kernel.pool_lk);
    s = (KShmem*)POOL_TAKE(g_hle.svcmem.shmems, g_hle.svcmem.shmem_next, "shared memories");
    s->o.type = KO_SHMEM;
    pm_unlock(&g_host.kernel.pool_lk);
    s->size = size;
    if ((s->host = mem_pool_host(shm_whole(s), &off)) != NULL)
        s->pool = off + 1;
    else if (!(s->host = (u8*)calloc(1, (size_t)shm_whole(s))))
        fatal("Out of memory for the shared memory %s", name);
    kobj_set_name(&s->o, name);
    s->perm = PERM_RW;
    return s;
}
u8* kshmem_ptr(KShmem* s) {
    u64 a;
    if (s->pool) return s->host;
    a = __atomic_load_n(&s->gaddr, __ATOMIC_ACQUIRE);
    return a ? GP(a) : s->host;
}
/* a mapping of the pool's pages, and of plain shared pages past the object for a larger size; the reference it
   holds is the one h_get_ref took */
static u32 shm_map_pool(KShmem* s, u64 addr, u64 size, u32 perm) {
    u64 whole = shm_whole(s);
    int m;
    R_TRY(mem_result(mem_map_pool(addr, s->pool - 1, whole, MS_SHARED, perm)));
    if (size > whole && (m = mem_map(addr + whole, size - whole, MS_SHARED, perm)) != 0) {
        mem_unmap(addr, whole);
        R_THROW(mem_result(m));
    }
    R_SUCCEED();
}
/* svcMapSharedMemory: read or read-write within what its owner allows (InvalidNewMemoryPermission), where shared
   memory may go (InvalidMemoryRegion), on free pages. The console wants the object's own size (InvalidSize); the
   runtime's sizes are its own guesses at the services', so another size is logged and the mapping covers at least the
   whole object. Without the pool, one mapping at a time (InvalidState for a second): the game maps each once. The
   services' side moves from the host copy to the guest's pages under g_klock, where they are published */
u32 svc_map_shm(u32 h, u64 addr, u64 size, u32 perm) {
    u32 r = check_range(addr, size);
    KShmem* s;
    u64 none = 0, whole;
    int m;
    if (r) return r;
    if (perm != PERM_R && perm != PERM_RW) R_THROW(R_BADPERM);
    s = (KShmem*)h_get_ref(h, KO_SHMEM);
    R_UNLESS(s, R_BADHANDLE);
    whole = shm_whole(s);
    if (size != whole) {
        WARN_LIMIT(4, "kernel: the game maps the shared memory %s as 0x%llx bytes, which is 0x%llx: mapping 0x%llx\n",
                   s->o.name, (unsigned long long)size, (unsigned long long)whole,
                   (unsigned long long)(size > whole ? size : whole));
        if (size < whole) size = whole;
    }
    if (!can_contain(addr, size, MS_SHARED))
        r = R_BADREGION;
    else if ((perm & ~s->perm) != 0)
        r = R_BADPERM;
    else if (s->pool) {
        r = shm_map_pool(s, addr, size, perm);
        if (R_SUCCEEDED(r)) return r; /* the mapping keeps the reference */
    } else if (!__atomic_compare_exchange_n(&s->gsize, &none, size, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        r = R_BADSTATE;
    else if ((m = mem_map(addr, size, MS_SHARED, perm)) != 0) {
        r = m == -2 ? R_OUTOFMEM : R_BADMEMSTATE;
        __atomic_store_n(&s->gsize, 0, __ATOMIC_RELEASE);
    } else {
        pm_lock(&g_host.kernel.klock);
        memcpy(GP(addr), s->host, (size_t)s->size);
        __atomic_store_n(&s->gaddr, addr, __ATOMIC_RELEASE);
        pm_unlock(&g_host.kernel.klock);
    }
    kobj_unref(&s->o);
    return r;
}
/* svcUnmapSharedMemory: only where the object is mapped (InvalidCurrentMemory elsewhere), all of what was mapped.
   With the pool, a mapping larger than the object goes with the size the game gives (at least the object's) */
u32 svc_unmap_shm(u32 h, u64 addr, u64 size) {
    u32 r = check_range(addr, size);
    KShmem* s;
    if (r) return r;
    s = (KShmem*)h_get_ref(h, KO_SHMEM);
    R_UNLESS(s, R_BADHANDLE);
    if (!can_contain(addr, size, MS_SHARED))
        r = R_BADREGION;
    else if (s->pool) {
        u64 whole = shm_whole(s), n = size > whole ? size : whole;
        /* the object's pages are checked and unmapped at once, so two threads can't both drop the mapping's
           reference */
        if ((n > whole && !range_ok(addr + whole, n - whole, MS_SHARED, 0, ANY, 0, 0)) ||
            mem_unmap_pool(addr, s->pool - 1, whole, MS_SHARED))
            r = R_BADMEMSTATE;
        else {
            if (n > whole) mem_unmap(addr + whole, n - whole);
            kobj_unref(&s->o); /* the mapping's */
        }
    } else if (__atomic_load_n(&s->gaddr, __ATOMIC_ACQUIRE) != addr)
        r = R_BADMEMSTATE;
    else {
        pm_lock(&g_host.kernel.klock);
        memcpy(s->host, GP(addr), (size_t)s->size);
        __atomic_store_n(&s->gaddr, 0, __ATOMIC_RELEASE);
        pm_unlock(&g_host.kernel.klock);
        mem_unmap(addr, __atomic_load_n(&s->gsize, __ATOMIC_ACQUIRE));
        __atomic_store_n(&s->gsize, 0, __ATOMIC_RELEASE);
    }
    kobj_unref(&s->o);
    return r;
}

/* ============================================================ transfer memory */
/* svcCreateTransferMemory: the source stays where it is, its permission lowered to perm and locked, until the
   object goes (tmem_destroy). A service reads it in place */
u32 svc_tmem_new(u64 addr, u64 size, u32 perm, KTmem** out) {
    KTmem* m;
    R_TRY(check_range(addr, size));
    if (perm != PERM_NONE && perm != PERM_R && perm != PERM_RW) R_THROW(R_BADPERM);
    if (!in_region(addr, size, 0, AS_END) || !range_ok(addr, size, 0, MSF_CAN_TRANSFER, PERM_RW, ~0u, 0))
        R_THROW(R_BADMEMSTATE);
    pm_lock(&g_host.kernel.pool_lk);
    m = (KTmem*)POOL_TAKE(g_hle.svcmem.tmems, g_hle.svcmem.tmem_next, "transfer memories");
    m->o.type = KO_TMEM;
    pm_unlock(&g_host.kernel.pool_lk);
    kobj_set_name(&m->o, "transfer memory");
    m->addr = addr;
    m->size = size;
    m->perm = perm;
    set_perm(addr, size, perm);
    mem_set_attr(addr, size, MA_LOCKED, MA_LOCKED);
    *out = m;
    R_SUCCEED();
}
void tmem_destroy(KObj* o) {
    KTmem* m = (KTmem*)o;
    mem_set_attr(m->addr, m->size, MA_LOCKED, 0);
    set_perm(m->addr, m->size, PERM_RW);
    pm_lock(&g_host.kernel.pool_lk);
    POOL_PUT(m);
    pm_unlock(&g_host.kernel.pool_lk);
}

/* ============================================================ code memory */
/* svcCreateCodeMemory and svcControlCodeMemory, for games with a JIT: the source is locked, and the owner (read-write)
   and slave (executable) mappings are views of it, as MapMemory's is: with the alias pool all three are the same
   pages, so code the game writes through the owner is in the slave at once. Without the pool they are copies, the
   slave of the owner when it is mapped, and code written after the slave is mapped doesn't reach it. A KCodeMem, from
   g_hle.svcmem.codemems (KHANDLED_MAX) */
void kcodemem_destroy(KObj* o) {
    KCodeMem* c = (KCodeMem*)o;
    if (c->owner) mem_alias_unmap(c->owner, c->size, c->addr);
    if (c->slave) mem_alias_unmap(c->slave, c->size, 0);
    mem_alias_end(c->addr, c->size);
    mem_set_attr(c->addr, c->size, MA_LOCKED, 0);
    set_perm(c->addr, c->size, PERM_RW);
    pm_lock(&g_host.kernel.pool_lk);
    POOL_PUT(c);
    pm_unlock(&g_host.kernel.pool_lk);
}
u32 svc_codemem_new(u64 addr, u64 size, KObj** out) {
    KCodeMem* c;
    R_TRY(check_range(addr, size));
    if (!in_region(addr, size, 0, AS_END) || !range_ok(addr, size, 0, MSF_CAN_CODE_MEMORY, PERM_RW, ~0u, 0))
        R_THROW(R_BADMEMSTATE);
    R_TRY(mem_result(mem_alias_begin(addr, size)));
    pm_lock(&g_host.kernel.pool_lk);
    c = (KCodeMem*)POOL_TAKE(g_hle.svcmem.codemems, g_hle.svcmem.codemem_next, "code memories");
    c->o.type = KO_CODEMEM;
    pm_unlock(&g_host.kernel.pool_lk);
    kobj_set_name(&c->o, "code memory");
    c->addr = addr;
    c->size = size;
    set_perm(addr, size, PERM_NONE);
    mem_set_attr(addr, size, MA_LOCKED, MA_LOCKED);
    *out = &c->o;
    R_SUCCEED();
}
/* op 0 MapOwner (read-write), 1 MapSlave (read or read-execute), 2 UnmapOwner, 3 UnmapSlave */
u32 svc_codemem_control(KObj* o, u32 op, u64 addr, u64 size, u32 perm) {
    KCodeMem* c = (KCodeMem*)o;
    u64* at;
    u32 state;
    R_TRY(check_range(addr, size));
    R_UNLESS(op <= 3, R_BADENUM);
    state = op & 1 ? MS_CODEOUT : MS_GENCODE;
    at = op & 1 ? &c->slave : &c->owner;
    if (op == 0 && perm != PERM_RW) R_THROW(R_BADPERM);
    if (op == 1 && perm != PERM_R && perm != PERM_RX) R_THROW(R_BADPERM);
    R_UNLESS(can_contain(addr, size, state), R_BADREGION);
    R_UNLESS(size == c->size, R_BADSIZE);
    if (op < 2) {
        R_UNLESS(!*at, R_BADSTATE);
        R_TRY(mem_result(mem_alias_map(addr, op == 1 && c->owner ? c->owner : c->addr, size, state, perm)));
        *at = addr;
        R_SUCCEED();
    }
    R_UNLESS(*at == addr, R_BADMEMSTATE);
    mem_alias_unmap(addr, size, op == 2 ? c->addr : 0);
    *at = 0;
    R_SUCCEED();
}

/* ============================================================ GetInfo */
/* RandomEntropy: four words fixed for the run, from the clock at their first use (splitmix64) */
static u64 entropy(u32 i) {
    u64* e = g_hle.svcmem.entropy;
    int* made = &g_hle.svcmem.entropy_made;
    if (!__atomic_load_n(made, __ATOMIC_ACQUIRE)) {
        u64 s = plat_ns() ^ 0x9E3779B97F4A7C15ull, v[4];
        int k;
        for (k = 0; k < 4; k++) {
            u64 z = (s += 0x9E3779B97F4A7C15ull);
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
            z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
            v[k] = (z ^ (z >> 31)) | 1;
        }
        pm_lock(&g_host.kernel.klock);
        if (!*made) {
            memcpy(e, v, sizeof v);
            __atomic_store_n(made, 1, __ATOMIC_RELEASE);
        }
        pm_unlock(&g_host.kernel.klock);
    }
    return e[i];
}
/* UserExceptionContextAddress: the process's local region, a kernel page of its own, made on first use */
static u64 process_local_region(void) {
    u64* a = &g_hle.svcmem.local_region;
    u64 v = __atomic_load_n(a, __ATOMIC_ACQUIRE), none = 0;
    if (v) return v;
    v = kmem_alloc(0x1000, MS_TLS, PERM_RW);
    if (!__atomic_compare_exchange_n(a, &none, v, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        mem_unmap(v, 0x1000);
        v = none;
    }
    return v;
}
/* svcGetInfo (Eden's svc_info.cpp), the ids that ask about the process: subtype 0 (InvalidEnumValue otherwise) and
   the process's handle (InvalidHandle) */
static u32 process_info(u32 id, u64* out) {
    switch (id) {
        case 0: *out = g_hle.svcmem.core_mask; R_SUCCEED();
        case 1: *out = g_hle.svcmem.prio_mask; R_SUCCEED();
        case 2: *out = ALIAS_BASE; R_SUCCEED();
        case 3: *out = ALIAS_SIZE; R_SUCCEED();
        case 4: *out = HEAP_BASE; R_SUCCEED();
        case 5: *out = HEAP_SIZE; R_SUCCEED();
        case 6: *out = mem_total(); R_SUCCEED();
        case 7: *out = used_memory(); R_SUCCEED();
        case 12: *out = AS_BASE; R_SUCCEED();
        case 13: *out = AS_END - AS_BASE; R_SUCCEED();
        case 14: *out = STACK_BASE; R_SUCCEED();
        case 15: *out = STACK_SIZE; R_SUCCEED();
        case 16: *out = g_hle.kernel.sysres_size; R_SUCCEED();
        case 17: { /* no slab allocator here: the page tables the mapped memory would take, within the resource */
            u64 used = (used_memory() - g_hle.kernel.sysres_size) / 512;
            used = (used + 0xFFFF) & ~0xFFFFull;
            *out = used < g_hle.kernel.sysres_size ? used : g_hle.kernel.sysres_size;
            R_SUCCEED();
        }
        case 18: *out = g_hle.kernel.title_id; R_SUCCEED();
        case 20: *out = process_local_region(); R_SUCCEED();
        case 21: *out = mem_total() - g_hle.kernel.sysres_size; R_SUCCEED();
        case 22: *out = used_memory() - g_hle.kernel.sysres_size; R_SUCCEED();
        case 23: *out = 1; R_SUCCEED(); /* IsApplication */
        case 24: {                   /* FreeThreadCount */
            int live = kthread_live_count();
            *out = live < THREAD_MAX ? (u64)(THREAD_MAX - live) : 0;
            R_SUCCEED();
        }
        case 28: *out = 0; R_SUCCEED(); /* AliasRegionExtraSize: no extra alias region */
        default: R_THROW(R_BADENUM);
    }
}
/* The rest of svcGetInfo; ResourceLimit (9), which makes a handle, is svc.c's */
u32 svc_get_info(u32 id0, u32 h, u64 id1, u64* out) {
    switch (id0) {
        case 0: case 1: case 2: case 3: case 4: case 5: case 6: case 7: case 12: case 13: case 14: case 15: case 16:
        case 17: case 18: case 20: case 21: case 22: case 23: case 24: case 28: {
            KObj* p;
            R_UNLESS(!id1, R_BADENUM);
            if (h != H_CUR_PROC) {
                p = h_get_ref(h, KO_PROCESS);
                R_UNLESS(p, R_BADHANDLE);
                kobj_unref(p);
            }
            return process_info(id0, out);
        }
        case 8: *out = 0; R_SUCCEED(); /* DebuggerAttached */
        case 10:                    /* IdleTickCount: a core, or -1 for all of them */
            if (id1 != ~0ull && id1 >= NCORES) R_THROW(R_BADCOMBO);
            R_UNLESS(!h, R_BADHANDLE);
            *out = 0;
            R_SUCCEED();
        case 11: /* RandomEntropy */
            R_UNLESS(!h, R_BADHANDLE);
            R_UNLESS(id1 < 4, R_BADCOMBO);
            *out = entropy((u32)id1);
            R_SUCCEED();
        case 19: /* InitialProcessIdRange: the kernel's own processes */
            R_UNLESS(!h, R_BADHANDLE);
            R_UNLESS(id1 <= 1, R_BADCOMBO);
            *out = id1 ? 0x50 : 1;
            R_SUCCEED();
        case 25:
        case 0xF0000002: { /* ThreadTickCount (0xF0000002 before 13.0.0): -1 for the thread's total, or a core's */
            KObj* t;
            if (id1 != ~0ull && id1 >= NCORES) R_THROW(R_BADCOMBO);
            t = h_get_ref(h, KO_THREAD);
            R_UNLESS(t, R_BADHANDLE);
            kobj_unref(t);
            /* no per-thread CPU time is kept: the system tick stands in for it, the total and each core's */
            *out = guest_ticks();
            R_SUCCEED();
        }
        default:
            WARN_LIMIT(16, "svcGetInfo %x (sub-id %llx): unknown, InvalidEnumValue\n", id0, (unsigned long long)id1);
            R_THROW(R_BADENUM);
    }
}
