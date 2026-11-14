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
   function's own code (hook_run_original), which made it.

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
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
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
    u64 mapped; /* statistics: the window's bytes mapped */
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
            __atomic_add_fetch(&g_alloc.mapped, size, __ATOMIC_RELAXED);
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
    __atomic_sub_fetch(&g_alloc.mapped, size, __ATOMIC_RELAXED);
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
extern void* mallocx(size_t size, int flags) __attribute__((weak));
extern void* rallocx(void* p, size_t size, int flags) __attribute__((weak));
extern void dallocx(void* p, int flags) __attribute__((weak));
extern size_t sallocx(const void* p, int flags) __attribute__((weak));
extern int mallctl(const char* name, void* old, size_t* oldlen, void* new_value, size_t newlen) __attribute__((weak));
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
    int lk; /* a spin lock: held for one probe sequence */
    u64 at[LIVE_SLOTS];
    u32 n, used, said; /* the blocks held, and the slots not empty: theirs and the tombstones */
} g_live;
static void live_lock(void) {
    while (__atomic_exchange_n(&g_live.lk, 1, __ATOMIC_ACQUIRE)) {}
}
static void live_unlock(void) { __atomic_store_n(&g_live.lk, 0, __ATOMIC_RELEASE); }
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

/* ============================================================ the functions */
/* the arena's pointer of guest address a, NULL for one it didn't make: in the window and a block it gave (counted
   out of the table, as the caller gives it back) */
static void* own_block(u64 a, const char* by) {
    if (a < g_alloc.lo || a >= g_alloc.hi) return NULL;
    if (live_take(a)) return GP(a);
    if (__atomic_fetch_add(&g_live.said, 1, __ATOMIC_RELAXED) < 16)
        LOG("hooks: %s gave %llx, in the arena's window but no block it gave: the game's own code takes it\n", by,
            (unsigned long long)a);
    return NULL;
}
static u64 guest(const void* p) { return p ? (u64)((const u8*)p - g_mem) : 0; }
static int lg(u64 align) {
    int l = 0;
    while (((u64)1 << l) < align) l++;
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
    return p;
}
/* p given back: the arena's own, or the hooked function's own code's (the registers as the call left them) */
static void give(CPU* c, u64 p) {
    void* q = own_block(p, "free or delete");
    if (q)
        g_alloc.dallocx(q, g_alloc.flags);
    else if (p)
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
    u64 r = guest(g_alloc.rallocx(q, size ? (size_t)size : 1, g_alloc.flags | ALLOC_LG_ALIGN(lg(align_of(size, 16)))));
    live_add(r ? r : p); /* no room: p stays the game's */
    return r;
}
void alloc_free(CPU* c, u64 p) { give(c, p); }
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
