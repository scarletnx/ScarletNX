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
#include "nx/runtime/clock.h"

#define R_BADPERM    0xD801 /* InvalidNewMemoryPermission */
#define R_BADPOINTER 0xE601 /* InvalidPointer: a user copy that faults */
#define MIB          (1ull << 20)

/* the MemoryState flags the checks below test (k_memory_block.h, Switchbrew's MemoryState) */
#define MSF_CAN_REPROTECT   (1u << 8)
#define MSF_CAN_ALIAS       (1u << 15)
#define MSF_CAN_TRANSFER    (1u << 17)
#define MSF_CAN_CHANGE_ATTR (1u << 24)
#define MSF_CAN_CODE_MEMORY (1u << 25)
/* MemoryAttribute: Locked, IpcLocked, DeviceShared, Uncached, PermissionLocked */
#define MA_LOCKED   1u
#define MA_SET_MASK 0x18u /* what SetMemoryAttribute may change: Uncached and PermissionLocked */

int g_opt_mem_gb;
u64 g_core_mask = 0xF, g_prio_mask = 0xFFFFFFFFFFFFFFF0ull; /* until the npdm says (main.c): an application's */

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
    if (a & 0xFFF) return R_BADADDR;
    if (!size || (size & 0xFFF)) return R_BADSIZE;
    if (a + size <= a) return R_BADMEMSTATE;
    return 0;
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
    if (g_opt_mem_gb >= 8) return 6964 * MIB;
    if (g_opt_mem_gb >= 6) return 4916 * MIB;
    return 3285 * MIB;
}
/* the loaded images, the main thread's stack, the heap, physical memory and the system resource, charged once */
u64 used_memory(void) {
    return g_code_memory + g_main_stack_size + __atomic_load_n(&g_heap_size, __ATOMIC_ACQUIRE) +
           __atomic_load_n(&g_phys_mapped, __ATOMIC_ACQUIRE) + g_sysres_size;
}
/* what the heap and physical memory may use between them */
static u64 heap_budget(void) {
    u64 fixed = g_code_memory + g_main_stack_size + g_sysres_size, total = mem_total();
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
    LOG("heap: %llu MB requested%s (result %x)\n", (unsigned long long)(size >> 20), r ? ", refused" : "", r);
    return r;
}
/* svcMapPhysicalMemory and svcUnmapPhysicalMemory: page-aligned (InvalidAddress), whole pages (InvalidSize), no wrap
   (InvalidMemoryRegion), a process with a system resource (InvalidState), inside the alias region
   (InvalidMemoryRegion) */
static u32 check_physical(u64 addr, u64 size) {
    if (addr & 0xFFF) return R_BADADDR;
    if (!size || (size & 0xFFF)) return R_BADSIZE;
    if (addr + size <= addr) return R_BADREGION;
    if (!g_sysres_size) return R_BADSTATE;
    if (!in_region(addr, size, ALIAS_BASE, ALIAS_SIZE)) return R_BADREGION;
    return 0;
}
u32 svc_map_physical(u64 addr, u64 size) {
    u32 r = check_physical(addr, size);
    return r ? r : mem_result(mem_map_physical(addr, size, heap_budget()));
}
u32 svc_unmap_physical(u64 addr, u64 size) {
    u32 r = check_physical(addr, size);
    return r ? r : mem_result(mem_unmap_physical(addr, size));
}

/* ============================================================ permissions, attributes, MapMemory */
/* svcSetMemoryPermission: none, read or read-write, on memory whose state may be reprotected and has no attribute */
u32 svc_set_mem_perm(u64 addr, u64 size, u32 perm) {
    u32 r = check_range(addr, size);
    if (r) return r;
    if (perm != PERM_NONE && perm != PERM_R && perm != PERM_RW) return R_BADPERM;
    if (!in_region(addr, size, 0, AS_END) || !range_ok(addr, size, 0, MSF_CAN_REPROTECT, ANY, ~0u, 0))
        return R_BADMEMSTATE;
    set_perm(addr, size, perm);
    return 0;
}
/* svcSetMemoryAttribute: mask names the attributes that change (a superset of attr), only Uncached and
   PermissionLocked; the memory's state must allow it and it must be neither locked nor shared with a device */
u32 svc_set_mem_attr(u64 addr, u64 size, u32 mask, u32 attr) {
    u32 r = check_range(addr, size);
    if (r) return r;
    if ((mask | attr) != mask || (mask | attr | MA_SET_MASK) != MA_SET_MASK) return R_BADCOMBO;
    if (!in_region(addr, size, 0, AS_END) || !range_ok(addr, size, 0, MSF_CAN_CHANGE_ATTR, ANY, ~MA_SET_MASK, 0))
        return R_BADMEMSTATE;
    mem_set_attr(addr, size, mask, attr);
    return 0;
}
/* svcMapMemory: a view of src at dst, in the stack region (in the stack state) or, as the first firmwares had it,
   the alias region (as normal memory). The view is a copy, src locked meanwhile (no permission, locked), and
   svcUnmapMemory copies it back: the host reservation maps each guest page once */
static u32 check_map_memory(u64 dst, u64 src, u64 size, u32* state) {
    if ((dst | src) & 0xFFF) return R_BADADDR;
    if (!size || (size & 0xFFF)) return R_BADSIZE;
    if (dst + size <= dst || src + size <= src) return R_BADMEMSTATE;
    if (!in_region(src, size, 0, AS_END)) return R_BADMEMSTATE;
    if (can_contain(dst, size, MS_STACK))
        *state = MS_STACK;
    else if (ALIAS_SIZE && in_region(dst, size, ALIAS_BASE, ALIAS_SIZE))
        *state = MS_NORMAL;
    else
        return R_BADREGION;
    return 0;
}
u32 svc_map_memory(u64 dst, u64 src, u64 size) {
    u32 state, r = check_map_memory(dst, src, size, &state);
    int m;
    if (r) return r;
    if (!range_ok(src, size, 0, MSF_CAN_ALIAS, PERM_RW, ~0u, 0)) return R_BADMEMSTATE;
    m = mem_map(dst, size, state, PERM_RW);
    if (m) return m == -2 ? R_OUTOFMEM : R_BADMEMSTATE;
    memcpy(GP(dst), GP(src), size);
    set_perm(src, size, PERM_NONE);
    mem_set_attr(src, size, MA_LOCKED, MA_LOCKED);
    return 0;
}
u32 svc_unmap_memory(u64 dst, u64 src, u64 size) {
    u32 state, r = check_map_memory(dst, src, size, &state);
    if (r) return r;
    if (!range_ok(src, size, 0, MSF_CAN_ALIAS, PERM_NONE, ~0u, MA_LOCKED) ||
        !range_ok(dst, size, state, 0, ANY, MA_LOCKED, 0))
        return R_BADMEMSTATE;
    memcpy(GP(src), GP(dst), size);
    mem_unmap(dst, size);
    mem_set_attr(src, size, MA_LOCKED, 0);
    set_perm(src, size, PERM_RW);
    return 0;
}

/* ============================================================ QueryMemory */
/* svcQueryMemory: the block at addr as the console's MemoryInfo (base, size, the state's low byte, attributes,
   permission, IPC and device reference counts), into info */
u32 svc_query(u64 info, u64 addr) {
    MemBlock b;
    u32 w[6];
    if (!user_writable(info, 0x28)) return R_BADPOINTER;
    mem_query(addr, &b);
    w[0] = b.state & 0xFF;
    w[1] = b.attr;
    w[2] = b.perm;
    w[3] = b.ipc;
    w[4] = b.dev;
    w[5] = 0;
    gw64(info, b.addr);
    gw64(info + 8, b.size);
    memcpy(GP(info + 16), w, sizeof w);
    return 0;
}

/* ============================================================ shared memory */
/* A service's shared memory lives in host memory of its own until the game maps it; a mapping is a copy, and while
   it lasts kshmem_ptr gives the service the guest's pages, so both sides see the same bytes. Unmapping copies the
   guest's pages back */
static void shm_destroy(KObj* o) {
    KShmem* s = (KShmem*)o;
    free(s->host);
    free(o->waiters);
    free(s);
}
/* with no reference: the first handle takes one, or the service that keeps it */
KShmem* kshmem_new(u64 size, const char* name) {
    KShmem* s = (KShmem*)calloc(1, sizeof *s);
    if (!s || !(s->host = (u8*)calloc(1, (size_t)((size + 0xFFF) & ~0xFFFull))))
        fatal("Out of memory for the shared memory %s", name);
    s->o.type = KO_SHMEM;
    s->o.destroy = shm_destroy;
    s->o.name = name;
    s->size = size;
    s->perm = PERM_RW;
    return s;
}
u8* kshmem_ptr(KShmem* s) {
    u64 a = __atomic_load_n(&s->gaddr, __ATOMIC_ACQUIRE);
    return a ? GP(a) : s->host;
}
/* svcMapSharedMemory: read or read-write within what its owner allows (InvalidNewMemoryPermission), where shared
   memory may go (InvalidMemoryRegion), on free pages. The console wants the object's own size (InvalidSize); the
   runtime's sizes are its own guesses at the services', so another size is logged and the mapping covers at least the
   whole object. One mapping at a time (InvalidState for a second): the game maps each once. The services' side moves
   from the host copy to the guest's pages under g_klock, where they are published */
u32 svc_map_shm(u32 h, u64 addr, u64 size, u32 perm) {
    u32 r = check_range(addr, size);
    KShmem* s;
    u64 none = 0, whole;
    int m;
    if (r) return r;
    if (perm != PERM_R && perm != PERM_RW) return R_BADPERM;
    s = (KShmem*)h_get_ref(h, KO_SHMEM);
    if (!s) return R_BADHANDLE;
    whole = (s->size + 0xFFF) & ~0xFFFull;
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
    else if (!__atomic_compare_exchange_n(&s->gsize, &none, size, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        r = R_BADSTATE;
    else if ((m = mem_map(addr, size, MS_SHARED, perm)) != 0) {
        r = m == -2 ? R_OUTOFMEM : R_BADMEMSTATE;
        __atomic_store_n(&s->gsize, 0, __ATOMIC_RELEASE);
    } else {
        pm_lock(&g_klock);
        memcpy(GP(addr), s->host, (size_t)s->size);
        __atomic_store_n(&s->gaddr, addr, __ATOMIC_RELEASE);
        pm_unlock(&g_klock);
    }
    kobj_unref(&s->o);
    return r;
}
/* svcUnmapSharedMemory: only where the object is mapped (InvalidCurrentMemory elsewhere), all of what was mapped */
u32 svc_unmap_shm(u32 h, u64 addr, u64 size) {
    u32 r = check_range(addr, size);
    KShmem* s;
    if (r) return r;
    s = (KShmem*)h_get_ref(h, KO_SHMEM);
    if (!s) return R_BADHANDLE;
    if (!can_contain(addr, size, MS_SHARED))
        r = R_BADREGION;
    else if (__atomic_load_n(&s->gaddr, __ATOMIC_ACQUIRE) != addr)
        r = R_BADMEMSTATE;
    else {
        pm_lock(&g_klock);
        memcpy(s->host, GP(addr), (size_t)s->size);
        __atomic_store_n(&s->gaddr, 0, __ATOMIC_RELEASE);
        pm_unlock(&g_klock);
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
    u32 r = check_range(addr, size);
    KTmem* m;
    if (r) return r;
    if (perm != PERM_NONE && perm != PERM_R && perm != PERM_RW) return R_BADPERM;
    if (!in_region(addr, size, 0, AS_END) || !range_ok(addr, size, 0, MSF_CAN_TRANSFER, PERM_RW, ~0u, 0))
        return R_BADMEMSTATE;
    m = (KTmem*)calloc(1, sizeof *m);
    if (!m) fatal("Out of memory for a transfer memory");
    m->o.type = KO_TMEM;
    m->o.destroy = tmem_destroy;
    m->o.name = "transfer memory";
    m->addr = addr;
    m->size = size;
    m->perm = perm;
    set_perm(addr, size, perm);
    mem_set_attr(addr, size, MA_LOCKED, MA_LOCKED);
    *out = m;
    return 0;
}
void tmem_destroy(KObj* o) {
    KTmem* m = (KTmem*)o;
    mem_set_attr(m->addr, m->size, MA_LOCKED, 0);
    set_perm(m->addr, m->size, PERM_RW);
    free(o->waiters);
    free(m);
}

/* ============================================================ code memory */
/* svcCreateCodeMemory and svcControlCodeMemory, for games with a JIT: the source is locked, and the owner (read-write)
   and slave (executable) mappings are copies of it, as MapMemory's view is. Code the game writes into the owner
   mapping after the slave is mapped doesn't reach the slave */
typedef struct {
    KObj o;
    u64 addr, size, owner, slave;
} KCodeMem;
static void codemem_destroy(KObj* o) {
    KCodeMem* c = (KCodeMem*)o;
    if (c->owner) {
        memcpy(GP(c->addr), GP(c->owner), c->size);
        mem_unmap(c->owner, c->size);
    }
    if (c->slave) mem_unmap(c->slave, c->size);
    mem_set_attr(c->addr, c->size, MA_LOCKED, 0);
    set_perm(c->addr, c->size, PERM_RW);
    free(o->waiters);
    free(c);
}
u32 svc_codemem_new(u64 addr, u64 size, KObj** out) {
    u32 r = check_range(addr, size);
    KCodeMem* c;
    if (r) return r;
    if (!in_region(addr, size, 0, AS_END) || !range_ok(addr, size, 0, MSF_CAN_CODE_MEMORY, PERM_RW, ~0u, 0))
        return R_BADMEMSTATE;
    c = (KCodeMem*)calloc(1, sizeof *c);
    if (!c) fatal("Out of memory for a code memory");
    c->o.type = KO_CODEMEM;
    c->o.destroy = codemem_destroy;
    c->o.name = "code memory";
    c->addr = addr;
    c->size = size;
    set_perm(addr, size, PERM_NONE);
    mem_set_attr(addr, size, MA_LOCKED, MA_LOCKED);
    *out = &c->o;
    return 0;
}
/* op 0 MapOwner (read-write), 1 MapSlave (read or read-execute), 2 UnmapOwner, 3 UnmapSlave */
u32 svc_codemem_control(KObj* o, u32 op, u64 addr, u64 size, u32 perm) {
    KCodeMem* c = (KCodeMem*)o;
    u64* at;
    u32 r = check_range(addr, size), state;
    int m;
    if (r) return r;
    if (op > 3) return R_BADENUM;
    state = op & 1 ? MS_CODEOUT : MS_GENCODE;
    at = op & 1 ? &c->slave : &c->owner;
    if (op == 0 && perm != PERM_RW) return R_BADPERM;
    if (op == 1 && perm != PERM_R && perm != PERM_RX) return R_BADPERM;
    if (!can_contain(addr, size, state)) return R_BADREGION;
    if (size != c->size) return R_BADSIZE;
    if (op < 2) {
        if (*at) return R_BADSTATE;
        if ((m = mem_map(addr, size, state, perm)) != 0) return m == -2 ? R_OUTOFMEM : R_BADMEMSTATE;
        memcpy(GP(addr), GP(op == 1 && c->owner ? c->owner : c->addr), size);
        *at = addr;
        return 0;
    }
    if (*at != addr) return R_BADMEMSTATE;
    if (op == 2) memcpy(GP(c->addr), GP(addr), size);
    mem_unmap(addr, size);
    *at = 0;
    return 0;
}

/* ============================================================ GetInfo */
/* RandomEntropy: four words fixed for the run, from the clock at their first use (splitmix64) */
static u64 entropy(u32 i) {
    static u64 e[4];
    static int made;
    if (!__atomic_load_n(&made, __ATOMIC_ACQUIRE)) {
        u64 s = plat_ns() ^ 0x9E3779B97F4A7C15ull, v[4];
        int k;
        for (k = 0; k < 4; k++) {
            u64 z = (s += 0x9E3779B97F4A7C15ull);
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
            z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
            v[k] = (z ^ (z >> 31)) | 1;
        }
        pm_lock(&g_klock);
        if (!made) {
            memcpy(e, v, sizeof e);
            __atomic_store_n(&made, 1, __ATOMIC_RELEASE);
        }
        pm_unlock(&g_klock);
    }
    return e[i];
}
/* UserExceptionContextAddress: the process's local region, a kernel page of its own, made on first use */
static u64 process_local_region(void) {
    static u64 a;
    u64 v = __atomic_load_n(&a, __ATOMIC_ACQUIRE), none = 0;
    if (v) return v;
    v = kmem_alloc(0x1000, MS_TLS, PERM_RW);
    if (!__atomic_compare_exchange_n(&a, &none, v, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        mem_unmap(v, 0x1000);
        v = none;
    }
    return v;
}
/* svcGetInfo (Eden's svc_info.cpp), the ids that ask about the process: subtype 0 (InvalidEnumValue otherwise) and
   the process's handle (InvalidHandle) */
static u32 process_info(u32 id, u64* out) {
    switch (id) {
        case 0: *out = g_core_mask; return 0;
        case 1: *out = g_prio_mask; return 0;
        case 2: *out = ALIAS_BASE; return 0;
        case 3: *out = ALIAS_SIZE; return 0;
        case 4: *out = HEAP_BASE; return 0;
        case 5: *out = HEAP_SIZE; return 0;
        case 6: *out = mem_total(); return 0;
        case 7: *out = used_memory(); return 0;
        case 12: *out = AS_BASE; return 0;
        case 13: *out = AS_END - AS_BASE; return 0;
        case 14: *out = STACK_BASE; return 0;
        case 15: *out = STACK_SIZE; return 0;
        case 16: *out = g_sysres_size; return 0;
        case 17: { /* no slab allocator here: the page tables the mapped memory would take, within the resource */
            u64 used = (used_memory() - g_sysres_size) / 512;
            used = (used + 0xFFFF) & ~0xFFFFull;
            *out = used < g_sysres_size ? used : g_sysres_size;
            return 0;
        }
        case 18: *out = g_title_id; return 0;
        case 20: *out = process_local_region(); return 0;
        case 21: *out = mem_total() - g_sysres_size; return 0;
        case 22: *out = used_memory() - g_sysres_size; return 0;
        case 23: *out = 1; return 0; /* IsApplication */
        case 24: {                   /* FreeThreadCount */
            int live = kthread_live_count();
            *out = live < THREAD_MAX ? (u64)(THREAD_MAX - live) : 0;
            return 0;
        }
        case 28: *out = 0; return 0; /* AliasRegionExtraSize: no extra alias region */
        default: return R_BADENUM;
    }
}
/* The rest of svcGetInfo; ResourceLimit (9), which makes a handle, is svc.c's */
u32 svc_get_info(u32 id0, u32 h, u64 id1, u64* out) {
    switch (id0) {
        case 0: case 1: case 2: case 3: case 4: case 5: case 6: case 7: case 12: case 13: case 14: case 15: case 16:
        case 17: case 18: case 20: case 21: case 22: case 23: case 24: case 28: {
            KObj* p;
            if (id1) return R_BADENUM;
            if (h != H_CUR_PROC) {
                p = h_get_ref(h, KO_PROCESS);
                if (!p) return R_BADHANDLE;
                kobj_unref(p);
            }
            return process_info(id0, out);
        }
        case 8: *out = 0; return 0; /* DebuggerAttached */
        case 10:                    /* IdleTickCount: a core, or -1 for all of them */
            if (id1 != ~0ull && id1 >= NCORES) return R_BADCOMBO;
            if (h) return R_BADHANDLE;
            *out = 0;
            return 0;
        case 11: /* RandomEntropy */
            if (h) return R_BADHANDLE;
            if (id1 >= 4) return R_BADCOMBO;
            *out = entropy((u32)id1);
            return 0;
        case 19: /* InitialProcessIdRange: the kernel's own processes */
            if (h) return R_BADHANDLE;
            if (id1 > 1) return R_BADCOMBO;
            *out = id1 ? 0x50 : 1;
            return 0;
        case 25:
        case 0xF0000002: { /* ThreadTickCount (0xF0000002 before 13.0.0): -1 for the thread's total, or a core's */
            KObj* t;
            if (id1 != ~0ull && id1 >= NCORES) return R_BADCOMBO;
            t = h_get_ref(h, KO_THREAD);
            if (!t) return R_BADHANDLE;
            kobj_unref(t);
            /* no per-thread CPU time is kept: the system tick stands in for it, the total and each core's */
            *out = guest_ticks();
            return 0;
        }
        default:
            WARN_LIMIT(16, "svcGetInfo %x (sub-id %llx): unknown, InvalidEnumValue\n", id0, (unsigned long long)id1);
            return R_BADENUM;
    }
}
