/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The C library's and the C++ library's allocation functions the game exports (hooks.hook's alloc family): malloc,
   calloc, realloc, free, aligned_alloc, memalign, posix_memalign, strdup, strndup and every operator new and delete,
   run by the host's own allocator, jemalloc, which the program links (cmake/jemalloc.cmake). Nothing of the game's
   allocator is emulated: its calls go to an arena of jemalloc's own (mallctl "arenas.create") whose memory comes from
   guest pages, so what it returns is a guest address the game's code and the GPU read like any other.

   The arena's pages are mapped in a window of the address space above every region of the layout (the heap, the
   alias region, the stacks and the kernel's), where the kernel maps nothing of the game's: from KERN_END up to
   AS_END, a 39-bit game's 430 GiB. A pointer is the arena's when it lies in the window. One that isn't, which the
   game's own allocator made before its functions were hooked or through a path no hook covers, goes to the hooked
   function's own code (hook_run_original), which made it. A module whose operator delete looks the block's allocator
   up by the block's address, and copies that code where no hook takes it, keeps its own operator new
   (alloc_new_shape), so that no block of the arena reaches such a copy.

   jemalloc's functions are the program's own where it links jemalloc (weak references, as an executable exports only
   the C library's names of them), else looked up at run time (dlsym, je_ names on macOS): a build without it
   (SNX_JEMALLOC=OFF, Windows) has no arena, and hook_install leaves the family's functions to the game's own code
   (alloc_ready). A 36-bit game, whose address space leaves no window, keeps its own too */
#include "nx/hle/hook/hook.h"
#include "hle/hook/hooks.hook.h"
#include "nx/hle/kernel/kernel.h"
#include "nx/memory/mem.h"
#include "nx/runtime/loader.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/tracy.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#if !defined(_WIN32)
#include <dlfcn.h>
#include <pthread.h>
#endif

/* jemalloc's extent hooks, as its jemalloc_typedefs.h declares them (5.x) */
typedef struct ExtentHooks ExtentHooks;
typedef void* (*ExtentAlloc)(ExtentHooks*, void* new_addr, size_t size, size_t align, bool* zero, bool* commit,
                             unsigned arena);
typedef bool (*ExtentDalloc)(ExtentHooks*, void* addr, size_t size, bool committed, unsigned arena);
typedef void (*ExtentDestroy)(ExtentHooks*, void* addr, size_t size, bool committed, unsigned arena);
typedef bool (*ExtentCommit)(ExtentHooks*, void* addr, size_t size, size_t offset, size_t length, unsigned arena);
typedef bool (*ExtentSplit)(ExtentHooks*, void* addr, size_t size, size_t a, size_t b, bool committed, unsigned arena);
typedef bool (*ExtentMerge)(ExtentHooks*, void* a, size_t na, void* b, size_t nb, bool committed, unsigned arena);
struct ExtentHooks {
    ExtentAlloc alloc;
    ExtentDalloc dalloc;
    ExtentDestroy destroy;
    ExtentCommit commit, decommit, purge_lazy, purge_forced;
    ExtentSplit split;
    ExtentMerge merge;
};
/* mallocx's flags (jemalloc_macros.h): an alignment's log, zeroed memory, no thread cache, an arena */
#define ALLOC_LG_ALIGN(la)  ((int)(la))
#define ALLOC_ZERO          0x40
#define ALLOC_TCACHE_NONE   (((int)-1 + 2) << 8)
#define ALLOC_ARENA(a)      (((int)(a) + 1) << 20)
#define ALLOC_PAGE          0x1000ull     /* a guest page */
#define ALLOC_GIB           (1ull << 30)

static struct {
    int ready; /* 1: the arena is made, -1: there is none this run */
    u64 lo, hi; /* the window, guest addresses */
    int flags;  /* ALLOC_ARENA and ALLOC_TCACHE_NONE */
    void* (*mallocx)(size_t, int);
    void* (*rallocx)(void*, size_t, int);
    void (*dallocx)(void*, int);
    size_t (*sallocx)(const void*, int);
    int (*mallctl)(const char*, void*, size_t*, void*, size_t);
    _Atomic u64 mapped;     /* statistics: the window's bytes mapped */
    signed char copied[32]; /* alloc_new_shape's verdict by module: 1 none, -1 its lookup is copied, 0 not read */
} g_alloc;

/* ============================================================ the arena's pages */
/* size bytes of guest pages at align, at new_addr when it is given (jemalloc growing an extent in place): mapped read
   and write, zero, in the window. NULL when the window has no such room */
static void* extent_alloc(ExtentHooks* h, void* new_addr, size_t size, size_t align, bool* zero, bool* commit,
                          unsigned arena) {
    (void)h, (void)arena;
    if (align < ALLOC_PAGE) align = ALLOC_PAGE;
    for (int tries = 0; tries < 8; tries++) {
        u64 a = new_addr ? (u64)((u8*)new_addr - g_mem) : mem_find_free(g_alloc.lo, g_alloc.hi, size, align);
        if (!a || a < g_alloc.lo || a + size > g_alloc.hi) return NULL;
        int r = mem_map(a, size, MS_NORMAL, PERM_RW);
        if (!r) {
            atomic_fetch_add_explicit(&g_alloc.mapped, size, memory_order_relaxed);
            *zero = true;
            *commit = true;
            return GP(a);
        }
        if (new_addr || r != MEM_BADSTATE) return NULL; /* another mapping took it first: look again */
    }
    return NULL;
}
static bool extent_dalloc(ExtentHooks* h, void* addr, size_t size, bool committed, unsigned arena) {
    (void)h, (void)committed, (void)arena;
    if (mem_unmap((u64)((u8*)addr - g_mem), size)) return true; /* kept: jemalloc retains it */
    atomic_fetch_sub_explicit(&g_alloc.mapped, size, memory_order_relaxed);
    return false;
}
static void extent_destroy(ExtentHooks* h, void* addr, size_t size, bool committed, unsigned arena) {
    extent_dalloc(h, addr, size, committed, arena);
}
/* the window's pages stay mapped while jemalloc holds them: committed always, never decommitted or purged (a NULL
   hook is one that never happens), and split and merged as jemalloc likes */
static bool extent_split(ExtentHooks* h, void* addr, size_t size, size_t a, size_t b, bool committed,
                         unsigned arena) {
    (void)h, (void)addr, (void)size, (void)a, (void)b, (void)committed, (void)arena;
    return false;
}
static bool extent_merge(ExtentHooks* h, void* a, size_t na, void* b, size_t nb, bool committed, unsigned arena) {
    (void)h, (void)a, (void)na, (void)b, (void)nb, (void)committed, (void)arena;
    return false;
}
static ExtentHooks g_hooks = {extent_alloc, extent_dalloc, extent_destroy, NULL, NULL, NULL, NULL, extent_split,
                              extent_merge};

#if defined(__ELF__)
/* cmake/jemalloc.cmake links jemalloc whole and unprefixed into the program, which exports only SNX_JEMALLOC_EXPORTS:
   dlsym's RTLD_DEFAULT doesn't see these. Weak, so a program without jemalloc links and finds them NULL */
[[gnu::weak]] extern void* mallocx(size_t size, int flags);
[[gnu::weak]] extern void* rallocx(void* p, size_t size, int flags);
[[gnu::weak]] extern void dallocx(void* p, int flags);
[[gnu::weak]] extern size_t sallocx(const void* p, int flags);
[[gnu::weak]] extern int mallctl(const char* name, void* old, size_t* oldlen, void* new_value, size_t newlen);
#endif
#if !defined(_WIN32)
static void* sym(const char* name) {
    void* p = dlsym(RTLD_DEFAULT, name);
    if (!p) {
        char je[64];
        snprintf(je, sizeof je, "je_%s", name); /* jemalloc's prefix on macOS */
        p = dlsym(RTLD_DEFAULT, je);
    }
    return p;
}
static void arena_make(void) {
    u64 lo = KERN_END;
    if (HEAP_BASE + HEAP_SIZE > lo) lo = HEAP_BASE + HEAP_SIZE;
    if (ALIAS_BASE + ALIAS_SIZE > lo) lo = ALIAS_BASE + ALIAS_SIZE;
    if (STACK_BASE + STACK_SIZE > lo) lo = STACK_BASE + STACK_SIZE;
    lo = (lo + ALLOC_GIB - 1) & ~(ALLOC_GIB - 1);
    g_alloc.ready = -1;
    if (lo >= AS_END || AS_END - lo < 4 * ALLOC_GIB) {
        LOG("hooks: the alloc family is off: the address space leaves no window for the host allocator's arena\n");
        return;
    }
    const char* how = "the program's own";
#if defined(__ELF__)
    if (mallocx && rallocx && dallocx && sallocx && mallctl) {
        g_alloc.mallocx = mallocx, g_alloc.rallocx = rallocx, g_alloc.dallocx = dallocx, g_alloc.sallocx = sallocx;
        g_alloc.mallctl = mallctl;
    }
#endif
    if (!g_alloc.mallctl) {
        how = "found by dlsym";
        *(void**)&g_alloc.mallocx = sym("mallocx");
        *(void**)&g_alloc.rallocx = sym("rallocx");
        *(void**)&g_alloc.dallocx = sym("dallocx");
        *(void**)&g_alloc.sallocx = sym("sallocx");
        *(void**)&g_alloc.mallctl = sym("mallctl");
    }
    if (!g_alloc.mallocx || !g_alloc.rallocx || !g_alloc.dallocx || !g_alloc.sallocx || !g_alloc.mallctl) {
        LOG("hooks: the alloc family is off: jemalloc's mallocx, rallocx, dallocx, sallocx and mallctl are neither the "
            "program's own nor found by dlsym (a build without jemalloc, SNX_JEMALLOC=OFF)\n");
        return;
    }
    g_alloc.lo = lo;
    g_alloc.hi = AS_END;
    unsigned arena = 0;
    size_t len = sizeof arena;
    ExtentHooks* hooks = &g_hooks;
    if (g_alloc.mallctl("arenas.create", &arena, &len, &hooks, sizeof hooks)) {
        LOG("hooks: the alloc family is off: jemalloc made no arena\n");
        return;
    }
    g_alloc.flags = ALLOC_ARENA(arena) | ALLOC_TCACHE_NONE;
    g_alloc.ready = 1;
    LOG("hooks: malloc and new run the host's jemalloc (%s), arena %u, in guest memory from %llx to %llx\n", how, arena,
        (unsigned long long)g_alloc.lo, (unsigned long long)g_alloc.hi);
}
#endif
int alloc_ready(void) {
#if defined(_WIN32)
    return 0;
#else
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, arena_make);
    return g_alloc.ready > 0;
#endif
}

/* ============================================================ the arena's blocks */
/* Every block the arena gave and that is not given back yet, by its guest address. Only those go back to it: a game's
   own allocator may carve its regions out of a block it took from malloc (the arena's), and its operator delete[],
   hooked too as main defines it, then hands back a piece of such a region, which lies in the window but is no block of
   the arena's. A pointer the table doesn't hold goes to the hooked function's own code, whose allocator made it.
   Open addressing, a tombstone for a block given back */
#ifndef LIVE_BITS
#define LIVE_BITS 22 /* tests/alloc_arena.c builds a smaller one */
#endif
#define LIVE_SLOTS (1u << LIVE_BITS)
#define LIVE_MAX   (LIVE_SLOTS / 4 * 3)
#define LIVE_GONE  1ull
static struct {
    _Atomic int lk; /* a spin lock: held for one probe sequence */
    u64 at[LIVE_SLOTS];
    u32 n, used;
    _Atomic u32 said; /* the blocks held, and the slots not empty: theirs and the tombstones */
} g_live;
static void live_lock(void) {
    while (atomic_exchange_explicit(&g_live.lk, 1, memory_order_acquire)) {}
}
static void live_unlock(void) { atomic_store_explicit(&g_live.lk, 0, memory_order_release); }
static u32 live_slot(u64 a) { return (u32)((a >> 4) * 0x9E3779B97F4A7C15ull >> (64 - LIVE_BITS)); }
/* the table made again with no tombstone, each held block put back: a lookup of a pointer it doesn't hold stops at the
   first empty slot, which tombstones would use up as blocks come and go. Under the lock */
static void live_rehash(void) {
    u64* held = malloc(((size_t)g_live.n + 1) * sizeof *held);
    u32 i, k, n = 0;
    if (!held) fatal("hooks: no host memory to make the alloc family's table of %u blocks again", g_live.n);
    for (i = 0; i < LIVE_SLOTS; i++)
        if (g_live.at[i] > LIVE_GONE) held[n++] = g_live.at[i];
    memset(g_live.at, 0, sizeof g_live.at);
    for (k = 0; k < n; k++) {
        for (i = live_slot(held[k]); g_live.at[i]; i = (i + 1) & (LIVE_SLOTS - 1)) {}
        g_live.at[i] = held[k];
    }
    g_live.used = n;
    free(held);
}
static void live_add(u64 a) {
    u32 i;
    if (!a) return;
    live_lock();
    if (g_live.n >= LIVE_MAX)
        fatal("hooks: the alloc family holds %u of the arena's blocks, the most its table takes", g_live.n);
    if (g_live.used >= LIVE_MAX) live_rehash();
    for (i = live_slot(a); g_live.at[i] > LIVE_GONE; i = (i + 1) & (LIVE_SLOTS - 1)) {}
    g_live.used += !g_live.at[i];
    g_live.at[i] = a;
    g_live.n++;
    live_unlock();
}
/* 1 when a is a block the arena gave, which is then no longer counted; 0 when it isn't */
static int live_take(u64 a) {
    u32 i;
    int found = 0;
    live_lock();
    for (i = live_slot(a); g_live.at[i]; i = (i + 1) & (LIVE_SLOTS - 1))
        if (g_live.at[i] == a) {
            g_live.at[i] = LIVE_GONE, g_live.n--, found = 1;
            break;
        }
    live_unlock();
    return found;
}

/* 1 when a is a block the arena gave and still holds, which stays counted */
static int live_has(u64 a) {
    u32 i;
    int found = 0;
    live_lock();
    for (i = live_slot(a); g_live.at[i]; i = (i + 1) & (LIVE_SLOTS - 1))
        if (g_live.at[i] == a) {
            found = 1;
            break;
        }
    live_unlock();
    return found;
}

/* ============================================================ the functions */
/* the arena's pointer of guest address a, NULL for one it didn't make: in the window and a block it gave (counted
   out of the table, as the caller gives it back) */
static void* own_block(u64 a, const char* by) {
    if (a < g_alloc.lo || a >= g_alloc.hi) return NULL;
    if (live_take(a)) return GP(a);
    if (atomic_fetch_add_explicit(&g_live.said, 1, memory_order_relaxed) < 16)
        LOG("hooks: %s gave %llx, in the arena's window but no block it gave: the game's own code takes it\n", by,
            (unsigned long long)a);
    return NULL;
}
static u64 guest(const void* p) { return p ? (u64)((const u8*)p - g_mem) : 0; }
/* the power of two that is align or the next above it, 63 at most: operator new's alignment is whatever the game
   passes, and above 2^63 there is no power of two to reach */
static int lg(u64 align) {
    int l = 0;
    while (l < 63 && ((u64)1 << l) < align) l++;
    return l;
}
/* the alignment of a block of size bytes: align, 16 at least as the guest's malloc gives, and a page for a page or
   more, as the SDK's allocator starts such a block. Games hand those to the kernel, which takes whole pages (a game's
   graphics init makes transfer memory of its malloc's 8 MiB), and jemalloc offsets a large block within its page */
static u64 align_of(u64 size, u64 align) {
    if (align < 16) align = 16;
    return size >= ALLOC_PAGE && align < ALLOC_PAGE ? ALLOC_PAGE : align;
}
/* size bytes at align (align_of's), zeroed or not: 0 when there is no room */
static u64 take(u64 size, u64 align, int zero) {
    u64 p;
    align = align_of(size, align);
    p = guest(
        g_alloc.mallocx(size ? (size_t)size : 1, g_alloc.flags | ALLOC_LG_ALIGN(lg(align)) | (zero ? ALLOC_ZERO : 0)));
    live_add(p);
    if (p) SNX_MEM_ALLOC_D((const void*)(uintptr_t)p, size, "game malloc");
    return p;
}
/* p given back: the arena's own, or the hooked function's own code's (the registers as the call left them) */
static void give(CPU* c, u64 p) {
    void* q = own_block(p, "free or delete");
    if (q) {
        SNX_MEM_FREE_D((const void*)(uintptr_t)p, "game malloc");
        g_alloc.dallocx(q, g_alloc.flags);
    } else if (p)
        hook_run_original(c, c->pc);
}

u64 alloc_malloc(CPU* c, u64 size) {
    (void)c;
    return take(size, 16, 0);
}
u64 alloc_calloc(CPU* c, u64 n, u64 size) {
    (void)c;
    if (size && n > ~0ull / size) return 0;
    return take(n * size, 16, 1);
}
u64 alloc_realloc(CPU* c, u64 p, u64 size) {
    if (!p) return take(size, 16, 0);
    void* q = own_block(p, "realloc");
    if (!q) { /* the game's own allocator's: its own realloc */
        hook_run_original(c, c->pc);
        return c->x[0];
    }
    SNX_MEM_FREE_D((const void*)(uintptr_t)p, "game malloc"); /* the old block goes back as rallocx resizes it */
    u64 r = guest(g_alloc.rallocx(q, size ? (size_t)size : 1, g_alloc.flags | ALLOC_LG_ALIGN(lg(align_of(size, 16)))));
    live_add(r ? r : p); /* no room: p stays the game's */
    SNX_MEM_ALLOC_D((const void*)(uintptr_t)(r ? r : p), size, "game malloc");
    return r;
}
void alloc_free(CPU* c, u64 p) { give(c, p); }
/* whether p is a block the arena gave and still holds */
static int held(u64 p) { return p >= g_alloc.lo && p < g_alloc.hi && live_has(p); }

/* nn::mem::StandardAllocator, and nn::nlibsdk::heap, the SDK's heap under it. A game whose own malloc is its
   StandardAllocator's hands the allocator a block malloc gave, which the arena made and the heap never saw: the heap
   stops the game on a pointer outside its pages. The allocator's functions end in CentralHeap's, or on a thread with
   a cache in those the TlsHeapCache's table holds, which StandardAllocator and CachedHeap call through it, so these
   take an arena block as the arena's and give any other pointer to their own code. The results are the heap's: a
   size, or 0 and an errno */
#define ALLOC_ENOMEM 12
#define ALLOC_EINVAL 22
/* the function's own code, with the registers as the call left them: its result */
static u64 heap_own(CPU* c) {
    hook_run_original(c, c->pc);
    return c->x[0];
}
/* StandardAllocator::GetSizeOf, CentralHeap::GetAllocSize and the cache's: an arena block's size is jemalloc's */
u64 alloc_std_size(CPU* c, u64 self, u64 p) {
    (void)self;
    return held(p) ? (u64)g_alloc.sallocx(GP(p), g_alloc.flags) : heap_own(c);
}
i32 alloc_heap_free(CPU* c, u64 heap, u64 p) {
    void* q = own_block(p, "the SDK heap's free");
    (void)heap;
    if (!q) return (i32)heap_own(c);
    SNX_MEM_FREE_D((const void*)(uintptr_t)p, "game malloc");
    g_alloc.dallocx(q, g_alloc.flags);
    return 0;
}
/* the size the block was asked with, which the arena knows already */
i32 alloc_heap_free_size(CPU* c, u64 heap, u64 p, u64 size) {
    (void)size;
    return alloc_heap_free(c, heap, p);
}
/* the block resized, moved or not, at *out: 0, or ENOMEM with *out 0 and the block as it was when there is no room.
   As CentralHeap answers, a size of 0 is EINVAL and one with its top bit set ENOMEM, *out untouched */
i32 alloc_heap_realloc(CPU* c, u64 heap, u64 p, u64 size, u64 out) {
    u64 r;
    (void)heap;
    if (!held(p)) return (i32)heap_own(c);
    if (!size) return ALLOC_EINVAL;
    if (size >> 63) return ALLOC_ENOMEM;
    r = alloc_realloc(c, p, size);
    snx_st_le64(hook_host(c, out), r);
    return r ? 0 : ALLOC_ENOMEM;
}
/* the block shrunk in place to size: the arena keeps it whole, as the heap keeps a small block in its size class, so
   0 when size fits in it and EINVAL when it doesn't (a shrink grows nothing); ENOMEM for a size with its top bit set */
i32 alloc_heap_shrink(CPU* c, u64 heap, u64 p, u64 size) {
    (void)heap;
    if (!held(p)) return (i32)heap_own(c);
    if (size >> 63) return ALLOC_ENOMEM;
    return size <= (u64)g_alloc.sallocx(GP(p), g_alloc.flags) ? 0 : ALLOC_EINVAL;
}
u64 alloc_aligned(CPU* c, u64 align, u64 size) {
    (void)c;
    if (!align || (align & (align - 1))) return 0;
    return take(size, align, 0);
}
i32 alloc_posix_memalign(CPU* c, u64 out, u64 align, u64 size) {
    if (!align || (align & (align - 1)) || align % 8) return 22; /* EINVAL */
    u64 p = take(size, align, 0);
    if (!p) return 12; /* ENOMEM */
    snx_st_le64(hook_host(c, out), p);
    return 0;
}
u64 alloc_strdup(CPU* c, u64 s) {
    size_t n = strlen((const char*)hook_host(c, s)) + 1;
    u64 p = take(n, 16, 0);
    if (p) memcpy(hook_host(c, p), hook_host(c, s), n);
    return p;
}
u64 alloc_strndup(CPU* c, u64 s, u64 max) {
    size_t n = strnlen((const char*)hook_host(c, s), (size_t)max);
    u64 p = take(n + 1, 16, 0);
    if (p) {
        memcpy(hook_host(c, p), hook_host(c, s), n);
        *hook_host(c, p + n) = 0;
    }
    return p;
}
/* operator new: on no room, the function's own code, which throws std::bad_alloc as the game's build does (or
   returns what its own allocator finds); a nothrow one returns 0 */
u64 alloc_new(CPU* c, u64 size) {
    u64 p = take(size, 16, 0);
    if (!p) {
        hook_run_original(c, c->pc);
        return c->x[0];
    }
    return p;
}
u64 alloc_new_aligned(CPU* c, u64 size, u64 align) {
    u64 p = take(size, align, 0);
    if (!p) {
        hook_run_original(c, c->pc);
        return c->x[0];
    }
    return p;
}
u64 alloc_new_nothrow(CPU* c, u64 size, u64 nothrow) {
    (void)c, (void)nothrow;
    return take(size, 16, 0);
}
u64 alloc_new_aligned_nothrow(CPU* c, u64 size, u64 align, u64 nothrow) {
    (void)c, (void)nothrow;
    return take(size, align, 0);
}
/* every operator delete: what the sized and aligned ones say more, the arena knows already */
void alloc_delete(CPU* c, u64 p) { give(c, p); }

/* ============================================================ std::vector's storage */
/* std::__vector_base<T, std::allocator<T>>::~__vector_base() of a T with nothing to destroy, which every such T
   compiles to: begin = this->__begin_; if (begin) { this->__end_ = begin; operator delete(begin); }. A pointer the
   arena didn't make goes to the function's own code, as the registers left it */
void alloc_vector_free(CPU* c, u64 self) {
    u64 begin = snx_ld_le64(hook_host(c, self));
    if (!begin) return;
    void* q = own_block(begin, "std::vector's destructor");
    if (!q) {
        hook_run_original(c, c->pc);
        return;
    }
    snx_st_le64(hook_host(c, self + 8), begin);
    SNX_MEM_FREE_D((const void*)(uintptr_t)begin, "game malloc");
    g_alloc.dallocx(q, g_alloc.flags);
}

/* whether a module's image holds the size bytes at guest address a */
static int in_module(u64 a, u64 size) {
    for (u32 i = 0; g_mod && i < g_mod->nmods; i++)
        if (a >= g_mod->mods[i].base && a - g_mod->mods[i].base + size <= g_mod->mods[i].size) return 1;
    return 0;
}
/* whether a call to t calls operator delete: t is its entry, or a PLT stub (adrp x16; ldr x17, [x16, #off];
   add x16, x16, #off; br x17) whose GOT entry holds it */
static int calls_delete(u64 t) {
    static const char* const names[] = {"_ZdlPv", "_ZdlPvm"};
    u64 got;
    u32 w[4];
    for (int i = 0; i < 2; i++) {
        u64 fn = kernel_find_symbol_exact(names[i]);
        if (fn && fn == t) return 1;
    }
    if (!in_module(t, 16)) return 0;
    for (int i = 0; i < 4; i++) w[i] = snx_ld_le32(GP(t + 4 * (u64)i));
    if ((w[0] & 0x9F00001Fu) != 0x90000010u || (w[1] & 0xFFC003FFu) != 0xF9400211u || w[3] != 0xD61F0220u) return 0;
    got = (t & ~0xFFFull) + ((u64)(i64)((i32)(((w[0] >> 5 & 0x7FFFF) << 2 | (w[0] >> 29 & 3)) << 11) >> 11) << 12) +
          ((w[1] >> 10 & 0xFFF) << 3);
    if (!in_module(got, 8)) return 0;
    u64 to = snx_ld_le64(GP(got));
    for (int i = 0; i < 2; i++)
        if (to && to == kernel_find_symbol_exact(names[i])) return 1;
    return 0;
}
/* the check of alloc_vector_free's pattern: the function at addr is the destructor of a T with nothing to destroy,
   which has no loop (no branch back), and calls nothing but operator delete, once (a call or a tail call). One that
   destroys its elements loops over them, and is left to its own code */
int alloc_vector_shape(u64 addr) {
    u64 a, furthest = addr;
    int deletes = 0;
    if (!in_module(addr, 4)) return 0;
    for (a = addr; a < addr + 64 * 4 && in_module(a, 4); a += 4) {
        u32 w = snx_ld_le32(GP(a));
        u64 t;
        int ends = 0;
        if ((w & 0x7C000000u) == 0x14000000u) { /* b, bl */
            t = a + (u64)(i64)((i32)(w << 6) >> 4);
            if (calls_delete(t))
                deletes++;
            else if ((w >> 31) || t <= a)
                return 0; /* a call of something else, or a loop */
            else if (t > furthest)
                furthest = t;
            ends = !(w >> 31);
        } else if ((w & 0xFF000010u) == 0x54000000u || (w & 0x7E000000u) == 0x34000000u) { /* b.cond, cbz, cbnz */
            t = a + ((u64)(i64)((i32)(w << 8) >> 13) << 2);
            if (t <= a) return 0;
            if (t > furthest) furthest = t;
        } else if ((w & 0x7E000000u) == 0x36000000u) { /* tbz, tbnz */
            t = a + ((u64)(i64)((i32)(w << 13) >> 18) << 2);
            if (t <= a) return 0;
            if (t > furthest) furthest = t;
        } else if ((w & 0xFFFFFC1Fu) == 0xD65F0000u) { /* ret */
            ends = 1;
        } else if ((w & 0xFE000000u) == 0xD6000000u || (w & 0xFF000000u) == 0xD4000000u) {
            return 0; /* br, blr, an exception */
        }
        if (ends && a >= furthest) return deletes == 1;
    }
    return 0;
}

/* ============================================================ operator new beside a copied operator delete */
/* A game's operator delete may be its own allocator's code, and the game's build may copy that code into its callers,
   inlined or as a function no symbol names, where no hook takes it. One game's delete looks the block's allocator up
   in a table by the block's address, table[(block - base) >> 20]: a block of the arena that reaches a copy of it is
   looked up past the table's end, past the heap. So in a module whose delete does that lookup and is copied,
   operator new keeps its own code (alloc_new_shape, every operator new's check): what it returns is the game's, which
   every copy knows; malloc, free and the hooked deletes stay the arena's. A delete that finds its allocator another
   way (others lock the thread's heap and ask it) leaves its module's new hooked, copied or not.

   The lookup is a 64-bit ldr at a register offset (ldr xt, [xn, xm], any extend or shift) on the delete's walk
   (below), whose offset is the block's address (x0 at the entry, or a mov's copy of it) less another register (sub
   xd, xn, xm), then through any 64-bit ubfm (lsr, ubfx, lsl) or and of an immediate. An offset made any other way (an
   add, an asr, a 32-bit sub or shift, the size argument less something) or a 32-bit entry is not one.

   A copy is told by what the delete reads: the guest addresses its code at the entry loads from (an adrp, then an
   add or an ldr from that register), straight on past each conditional branch, past an early return to the branch
   that skipped it, and through one leading b. Code that loads all of them within COPY_WINDOW instructions, outside
   the family's own functions (whose code runs only for what the arena didn't make), is a copy. A delete that loads
   fewer than 2 such addresses gives no verdict, as any code reading its one global would look alike: its module's
   new stays hooked */
#define COPY_LOADS  8  /* the addresses of a delete's that a copy has to load, at most */
#define COPY_WALK   64 /* the instructions read from a delete's entry */
#define COPY_WINDOW 24 /* the instructions a copy's loads lie within */
#define COPY_SKIPS  48 /* the family's functions in a module that the search leaves out */
static const char* const k_deletes[] = {"_ZdlPv",
                                        "_ZdaPv",
                                        "_ZdlPvm",
                                        "_ZdaPvm",
                                        "_ZdlPvRKSt9nothrow_t",
                                        "_ZdaPvRKSt9nothrow_t",
                                        "_ZdlPvSt11align_val_t",
                                        "_ZdaPvSt11align_val_t",
                                        "_ZdlPvmSt11align_val_t",
                                        "_ZdaPvmSt11align_val_t",
                                        "_ZdlPvSt11align_val_tRKSt9nothrow_t",
                                        "_ZdaPvSt11align_val_tRKSt9nothrow_t"};
/* the family's other functions (hooks.hook), whose code runs only through hook_run_original */
static const char* const k_family[] = {"malloc",
                                       "calloc",
                                       "realloc",
                                       "free",
                                       "aligned_alloc",
                                       "memalign",
                                       "posix_memalign",
                                       "strdup",
                                       "strndup",
                                       "_Znwm",
                                       "_Znam",
                                       "_ZnwmSt11align_val_t",
                                       "_ZnamSt11align_val_t",
                                       "_ZnwmRKSt9nothrow_t",
                                       "_ZnamRKSt9nothrow_t",
                                       "_ZnwmSt11align_val_tRKSt9nothrow_t",
                                       "_ZnamSt11align_val_tRKSt9nothrow_t"};
static int listed(const char* name, const char* const* names, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (!strcmp(name, names[i])) return 1;
    return 0;
}

/* the registers instruction w writes, as made_step and lookup_step follow them: none for a store, a branch or a
   system instruction but x30 for bl and blr and its own for mrs, both for an ldp and its kin, else its first */
static u32 written(u32 w) {
    if ((w & 0x3A400000u) == 0x28400000u) return 1u << (w & 31) | 1u << (w >> 10 & 31); /* ldp and its kin */
    if ((w & 0x3BC00000u) == 0x39000000u || (w & 0x3A400000u) == 0x28000000u) return 0; /* str, stp */
    if ((w & 0x1C000000u) == 0x14000000u) { /* branches and system instructions */
        if ((w & 0xFC000000u) == 0x94000000u || (w & 0xFFFFFC1Fu) == 0xD63F0000u) return 1u << 30;
        return (w & 0xFFF00000u) == 0xD5300000u ? 1u << (w & 31) : 0; /* mrs */
    }
    return 1u << (w & 31);
}

/* the registers that hold a guest address the code made: x<r>'s in at[r] while bit r of known is set */
typedef struct {
    u64 at[32];
    u32 known;
} Made;
/* instruction w at pc on m: the guest address it loads from, an ldr (64 or 32 bits, or ldrsw) at an offset from a
   register m knows, else 0. An adrp or an add of an immediate to such a register makes one; any other instruction
   that writes a register forgets it */
static u64 made_step(Made* m, u64 pc, u32 w) {
    u32 d = w & 31, n = w >> 5 & 31;
    if ((w & 0x9F000000u) == 0x90000000u) { /* adrp */
        m->at[d] = (pc & ~0xFFFull) + ((u64)(i64)((i32)(((w >> 5 & 0x7FFFF) << 2 | (w >> 29 & 3)) << 11) >> 11) << 12);
        m->known |= 1u << d;
        return 0;
    }
    if ((w & 0xFFC00000u) == 0x91000000u) { /* add xd, xn, #imm12 */
        if (n != 31 && (m->known >> n & 1)) {
            m->at[d] = m->at[n] + (w >> 10 & 0xFFF);
            m->known |= 1u << d;
        } else {
            m->known &= ~(1u << d);
        }
        return 0;
    }
    if ((w & 0xFFC00000u) == 0xF9400000u || (w & 0xFFC00000u) == 0xB9400000u || (w & 0xFFC00000u) == 0xB9800000u) {
        u64 a = n != 31 && (m->known >> n & 1) ? m->at[n] + (w >> 10 & 0xFFF) * ((w >> 30) == 3 ? 8 : 4) : 0;
        m->known &= ~(1u << d); /* ldr xt, ldr wt, ldrsw xt, [xn, #imm] */
        return a;
    }
    m->known &= ~written(w);
    return 0;
}

/* the registers that hold, along a delete's walk, the block's address (block) and an offset made from it (offset),
   as alloc_new_shape's comment says */
typedef struct {
    u32 block, offset;
} Lookup;
/* instruction w on l: 1 when it is the lookup, a 64-bit ldr at a register offset that l knows */
static int lookup_step(Lookup* l, u32 w) {
    u32 n = w >> 5 & 31, k = w >> 16 & 31, to = written(w), block = 0, offset = 0;
    /* ldr xt, [xn, xm{, extend}], read before it writes xt, which may be xm */
    int hit = (w & 0xFFE00C00u) == 0xF8600800u && (l->offset >> k & 1);
    if ((w & 0xFFE0FFE0u) == 0xAA0003E0u) { /* mov xd, xm */
        block = l->block >> k & 1;
        offset = l->offset >> k & 1;
    } else if ((w & 0xFF20FC00u) == 0xCB000000u) { /* sub xd, xn, xm */
        offset = l->block >> n & 1;
    } else if ((w & 0xFFC00000u) == 0xD3400000u || (w & 0xFF800000u) == 0x92000000u) { /* ubfm, and xd, xn, #imm */
        offset = l->offset >> n & 1;
    }
    l->block = (l->block & ~to) | (block ? to : 0);
    l->offset = (l->offset & ~to) | (offset ? to : 0);
    return hit;
}

/* the addresses the delete at entry loads from (alloc_new_shape's comment says which), at most COPY_LOADS, in sig:
   their count. *end is where what was read from entry ends, before a leading b went elsewhere; *lookup whether the
   walk does the lookup */
static u32 delete_loads(u64 entry, u64 sig[COPY_LOADS], u64* end, int* lookup) {
    Made m = {{0}, 0};
    Lookup l = {1, 0}; /* the block in x0 */
    u64 pc = entry, pending[COPY_WALK];
    u32 n = 0, np = 0, k;
    int followed = 0;
    *end = 0;
    *lookup = 0;
    for (int i = 0; i < COPY_WALK && in_module(pc, 4); i++) {
        u32 w = snx_ld_le32(GP(pc));
        u64 a;
        if ((w & 0xFC000000u) == 0x14000000u) { /* b: followed once, before any load */
            if (!*end) *end = pc + 4;
            if (n || followed) break;
            followed = 1;
            pc += (u64)(i64)((i32)(w << 6) >> 4);
            m.known = 0;
            continue;
        }
        if ((w & 0xFE000000u) == 0xD6000000u) {    /* br, blr, ret and their authenticated forms */
            if ((w >> 21 & 15) == 2 && !n && np) { /* an early return, as for a null pointer: the branch past it */
                pc = pending[--np];
                continue;
            }
            break;
        }
        if ((w & 0xFC000000u) == 0x94000000u) break;                              /* bl */
        if ((w & 0x7E000000u) == 0x34000000u || (w & 0xFF000010u) == 0x54000000u) /* cbz, cbnz, b.cond */
            pending[np++] = pc + ((u64)(i64)((i32)(w << 8) >> 13) << 2);
        else if ((w & 0x7E000000u) == 0x36000000u) /* tbz, tbnz */
            pending[np++] = pc + ((u64)(i64)((i32)(w << 13) >> 18) << 2);
        *lookup |= lookup_step(&l, w);
        a = made_step(&m, pc, w);
        for (k = 0; a && k < n && sig[k] != a; k++) {}
        if (a && k == n && n < COPY_LOADS) sig[n++] = a;
        pc += 4;
    }
    if (!*end) *end = pc;
    return n;
}

/* the places in [lo, hi) whose code loads every address of sig (n) within COPY_WINDOW instructions, outside skip's
   ranges: their count, the first (the load that completes it) at *first */
static u32 delete_copies(u64 lo, u64 hi, const u64* sig, u32 n, const u64 (*skip)[2], u32 nskip, u64* first) {
    Made m = {{0}, 0};
    u64 seen[COPY_LOADS] = {0}; /* the instruction, counted from 1, of each address's last load; 0 for none */
    u32 count = 0, k;
    for (u64 pc = lo; pc + 4 <= hi; pc += 4) {
        u64 a = made_step(&m, pc, snx_ld_le32(GP(pc))), at = (pc - lo) / 4 + 1;
        int all = 1;
        if (!a) continue;
        for (k = 0; k < n && sig[k] != a; k++) {}
        if (k == n) continue;
        seen[k] = at;
        for (k = 0; k < n; k++) all &= seen[k] && at - seen[k] < COPY_WINDOW;
        if (!all) continue;
        memset(seen, 0, sizeof seen);
        for (k = 0; k < nskip && (pc < skip[k][0] || pc >= skip[k][1]); k++) {}
        if (k < nskip) continue;
        if (!count++) *first = pc;
    }
    return count;
}

/* whether a and b (na and nb addresses) hold the same addresses */
static int same_loads(const u64* a, u32 na, const u64* b, u32 nb) {
    u32 i, k;
    if (na != nb) return 0;
    for (i = 0; i < na; i++) {
        for (k = 0; k < nb && b[k] != a[i]; k++) {}
        if (k == nb) return 0;
    }
    return 1;
}

/* operator new's check: 0 when the module that holds addr copies the code of an operator delete that does the
   lookup, so that its new keeps its own code; read once a module, and said once */
int alloc_new_shape(u64 addr) {
    enum { DELETES = sizeof k_deletes / sizeof *k_deletes };
    const SnxModInfo* mi = NULL;
    const char* name;
    u64 a, skip[COPY_SKIPS][2], sigs[DELETES][COPY_LOADS], first = 0, first_of = 0;
    u32 i, m, k, nskip = 0, nsig = 0, nloads[DELETES], count = 0, said = 0;
    for (m = 0; g_mod && m < g_mod->nmods; m++)
        if (addr >= g_mod->mods[m].base && addr - g_mod->mods[m].base < g_mod->mods[m].size) {
            mi = &g_mod->mods[m];
            break;
        }
    if (!mi) return 1;
    if (m < sizeof g_alloc.copied && g_alloc.copied[m]) return g_alloc.copied[m] > 0;
    /* the addresses of each delete that does the lookup, once for each set, and every delete's and the family's
       functions, which the search leaves out */
    for (i = 0; kernel_symbol(i, &a, &name); i++) {
        u64 size = kernel_symbol_size(i), sig[COPY_LOADS], end;
        u32 n;
        int lookup;
        if (!name || a < mi->base || a - mi->base >= mi->size || nskip == COPY_SKIPS) continue;
        if (listed(name, k_family, sizeof k_family / sizeof *k_family)) {
            if (size) skip[nskip][0] = a, skip[nskip][1] = a + size, nskip++;
            continue;
        }
        if (!listed(name, k_deletes, DELETES)) continue;
        n = delete_loads(a, sig, &end, &lookup);
        skip[nskip][0] = a, skip[nskip][1] = end > a + size ? end : a + size, nskip++;
        for (k = 0; k < nsig && !same_loads(sigs[k], nloads[k], sig, n); k++) {}
        if (!lookup || n < 2 || k < nsig || nsig == DELETES) continue;
        memcpy(sigs[nsig], sig, n * sizeof *sig);
        nloads[nsig++] = n;
    }
    for (k = 0; k < nsig; k++) {
        u64 at = 0;
        u32 c = delete_copies(mi->base + mi->text_off, mi->base + mi->text_off + mi->text_size, sigs[k], nloads[k],
                              (const u64(*)[2])skip, nskip, &at);
        if (c && !count) first = at, first_of = k;
        count += c;
    }
    if (m < sizeof g_alloc.copied) g_alloc.copied[m] = count ? -1 : 1;
    for (i = 0; count && kernel_symbol(i, &a, &name); i++) { /* the delete whose code the first copy has */
        u64 sig[COPY_LOADS], end;
        u32 n;
        int lookup;
        if (said || !name || a < mi->base || a - mi->base >= mi->size || !listed(name, k_deletes, DELETES)) continue;
        n = delete_loads(a, sig, &end, &lookup);
        if (!lookup || !same_loads(sigs[first_of], nloads[first_of], sig, n)) continue;
        LOG("hooks: %s copies the code of its %s at %llx, which looks a block's allocator up by its address and loads "
            "from %llx and %u more guest addresses, to %u place%s no hook takes, the first at %llx: its operator new "
            "keeps its own code, as a block of the arena would go there to the game's own allocator\n",
            mi->name, name, (unsigned long long)a, (unsigned long long)sig[0], n - 1, count, count == 1 ? "" : "s",
            (unsigned long long)first);
        said = 1;
    }
    return !count;
}

/* ============================================================ a game's own allocator objects */
/* A game may define malloc itself, as code that takes the block from an allocator object of its own through the
   object's virtual table: one game's malloc calls a static object's Allocate, and its operator new the thread's
   current allocator's. Such objects keep their own books (the bytes and blocks handed out, against a capacity), and
   the game's code frees through the objects themselves as well as through free and operator delete. A block of the
   arena that reaches an object's Free, which no hook takes, is counted off books that never counted it: that game's
   went below zero, and every allocation after was refused. So where the program's malloc (or calloc, realloc,
   aligned_alloc) is such code, the family is off (alloc_kept): the game's allocator runs whole, as on the console.
   An operator new alone that calls an object, beside a malloc that is the SDK allocator's, keeps the family: another
   game's does (alloc_new_shape's), its free asking the SDK heap, whose functions the family answers.

   The call through the table, a C++ virtual call: on the function's walk (its first ALLOC_SCAN instructions, up to
   where it ends past every forward branch), a 64-bit load of an object's first word (ldr xt, [xn] or ldraa/ldrab with
   no offset) into another register, a 64-bit load at an offset from that table (ldr xe, [xt, #imm]), and an indirect
   branch or call to the entry (br, blr and their pointer-authenticated forms) with the object in x0, the load's own
   x0 or a mov's copy of the object; each register keeps what it holds until another instruction writes it (written).
   A PLT stub's single load from its GOT, a function pointer in a global, a table of functions a global points to
   (no object passed) and the SDK's own functions' direct calls are not one */
#define ALLOC_SCAN 128u
/* the register instruction w loads a 64-bit word into from [xn, #imm] (ldr, or ldraa/ldrab: *imm 0 when their offset
   is), -1 for another instruction */
static int load64(u32 w, u32* n, u32* imm) {
    if ((w & 0xFFC00000u) == 0xF9400000u) { /* ldr xt, [xn, #imm12 * 8] */
        *n = w >> 5 & 31, *imm = (w >> 10 & 0xFFFu) * 8;
        return (int)(w & 31);
    }
    if ((w & 0xFF200400u) == 0xF8200400u) { /* ldraa, ldrab: S:imm9, scaled by 8 */
        *n = w >> 5 & 31, *imm = (w >> 12 & 0x1FFu) | (w >> 22 & 1u) << 9;
        return (int)(w & 31);
    }
    return -1;
}
/* the register an indirect branch or call (br, blr, braa, brab, blraa, blrab and their zero-modifier forms) goes to,
   -1 for another instruction */
static int branch_reg(u32 w) {
    if ((w & 0xFFDFFC1Fu) == 0xD61F0000u || (w & 0xFEDFF800u) == 0xD61F0800u) return (int)(w >> 5 & 31);
    return -1;
}
/* whether the function at addr (size bytes, 0 when unknown) calls through an object's table */
int alloc_object_call(u64 addr, u64 size) {
    u32 table = 0, entry = 0, object = 0; /* registers that hold an object's table, an entry of it, the object */
    u64 end = addr + (size && size < ALLOC_SCAN * 4 ? size : ALLOC_SCAN * 4), furthest = addr;
    for (u64 a = addr; a < end && in_module(a, 4); a += 4) {
        u32 w = snx_ld_le32(GP(a)), n = 0, imm = 0, gone = written(w);
        int b = branch_reg(w), t = load64(w, &n, &imm), ends = 0;
        if (b >= 0 && (entry >> b & 1) && (object & 1)) return 1;
        u32 was_table = t >= 0 && (table >> n & 1);
        u32 copy = (w & 0xFFE0FFE0u) == 0xAA0003E0u && (object >> (w >> 16 & 31) & 1);
        table &= ~gone, entry &= ~gone, object &= ~gone;
        if (copy) object |= 1u << (w & 31); /* mov xd, xm of the object */
        if (t >= 0 && t != 31) {
            if (was_table)
                entry |= 1u << t;
            else if (!imm && (u32)t != n)
                table = 1u << t, entry = 0, object = n != 31 ? 1u << n : 0;
        }
        /* where the function ends: an unconditional branch, a return or an indirect branch past every forward
           branch's target */
        if ((w & 0xFC000000u) == 0x14000000u) { /* b: a jump within the function, else its end (a tail call) */
            u64 to = a + (u64)(i64)((i32)(w << 6) >> 4);
            if (to > a && to < end) {
                if (to > furthest) furthest = to;
            } else
                ends = 1;
        } else if ((w & 0xFF000010u) == 0x54000000u || (w & 0x7E000000u) == 0x34000000u) { /* b.cond, cbz, cbnz */
            u64 to = a + ((u64)(i64)((i32)(w << 8) >> 13) << 2);
            if (to > furthest) furthest = to;
        } else if ((w & 0x7E000000u) == 0x36000000u) { /* tbz, tbnz */
            u64 to = a + ((u64)(i64)((i32)(w << 13) >> 18) << 2);
            if (to > furthest) furthest = to;
        } else if ((w & 0xFFFFFC1Fu) == 0xD65F0000u || (w & 0xFFFFFBFFu) == 0xD65F0BFFu || (b >= 0 && !(w >> 21 & 1)))
            ends = 1; /* ret, retaa, retab, br and its authenticated forms */
        if (ends && a >= furthest) break;
    }
    return 0;
}
/* the reason the family is off for the game's own allocator, NULL when it may run (said once) */
const char* alloc_kept(void) {
    static const char* const names[] = {"malloc", "calloc", "realloc", "aligned_alloc"};
    static int done;
    static char why[256];
    const char* name;
    u64 a;
    if (done) return why[0] ? why : NULL;
    done = 1;
    for (u32 i = 0; kernel_symbol(i, &a, &name); i++) {
        u32 m;
        if (!name || !listed(name, names, sizeof names / sizeof *names) || !alloc_object_call(a, kernel_symbol_size(i)))
            continue;
        for (m = 0; g_mod && m < g_mod->nmods; m++)
            if (a >= g_mod->mods[m].base && a - g_mod->mods[m].base < g_mod->mods[m].size) break;
        snprintf(why, sizeof why,
                 "the game's %s (%s+%llx) takes its blocks from an allocator object of its own through the object's "
                 "table, and the game frees through such objects too, which no hook takes",
                 name, g_mod && m < g_mod->nmods && g_mod->mods[m].name ? g_mod->mods[m].name : "?",
                 (unsigned long long)(g_mod && m < g_mod->nmods ? a - g_mod->mods[m].base : a));
        return why;
    }
    return NULL;
}
