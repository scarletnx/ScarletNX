/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef _WIN32
#define _GNU_SOURCE
#endif
#include "gnuabi/memory/memory.h"
#include "gnuabi/abi/errno/errno.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "common/platform/vm.h"
#ifndef _WIN32
#include <sys/mman.h> /* madvise */
#endif

/* a page's flags, which another thread may change while this one reads them */
static unsigned page_flags(const GnuMemory* m, uint64_t page) {
    return __atomic_load_n(&m->pages[page], __ATOMIC_RELAXED);
}
static void pages_set(GnuMemory* m, uint64_t first, uint64_t count, unsigned flags) {
    for (uint64_t i = first; i < first + count; i++)
        __atomic_store_n(&m->pages[i], (unsigned char)flags, __ATOMIC_RELAXED);
}
void gnu_memory_lock(GnuMemory* m) { gnu_mutex_lock(&m->lock); }
void gnu_memory_unlock(GnuMemory* m) { gnu_mutex_unlock(&m->lock); }

static uint64_t up(uint64_t value) { return (value + GNU_PAGE_SIZE - 1) & ~(uint64_t)(GNU_PAGE_SIZE - 1); }
static int range(GnuMemory* m, uint64_t address, uint64_t length) {
    return m->base && address >= GNU_PAGE_SIZE && !(address & (GNU_PAGE_SIZE - 1)) && length && length <= m->size &&
           address <= m->size - length && up(length) <= m->size - address;
}
static int host_protect_pages(GnuMemory* m, uint64_t address, uint64_t length, unsigned permissions) {
    /* Guest instructions are data: host execution permission is never granted. */
    unsigned p = permissions & GNU_PROT_WRITE                    ? PLAT_MEM_READ | PLAT_MEM_WRITE
                 : permissions & (GNU_PROT_READ | GNU_PROT_EXEC) ? PLAT_MEM_READ
                                                                 : PLAT_MEM_NONE;
    return plat_mem_protect(m->base + address, length, p) ? -GNU_ENOMEM : 0;
}
/* What a mapped guest page asks of the host's: its permissions, without write while compiled code is watched on it */
static unsigned host_wants(unsigned flags) {
    if (!(flags & GNU_PAGE_MAPPED)) return 0;
    return flags & GNU_PAGE_CODE ? flags & (GNU_PROT_READ | GNU_PROT_EXEC) : flags & 7u;
}
/* [address, address + length) given permissions on the host. A host page larger than a guest page also holds guest
   pages outside the range, so the host pages at its ends get what any of their guest pages wants: a page sharing a
   host page with a more permissive one isn't protected (a guest fault there goes unseen, and so does a guest write
   to compiled code beside writable data) */
static int host_protect(GnuMemory* m, uint64_t address, uint64_t length, unsigned permissions) {
    uint64_t hp = m->host_page, end = address + length, first = address & ~(hp - 1),
             last = (end - 1) & ~(hp - 1), edge[2] = {first, last}; /* the first and last host pages touched */
    if (!((address | end) & (hp - 1))) return host_protect_pages(m, address, length, permissions);
    if (last > first + hp && host_protect_pages(m, first + hp, last - first - hp, permissions)) return -GNU_ENOMEM;
    for (int i = 0; i < (last != first ? 2 : 1); i++) {
        unsigned wants = 0;
        for (uint64_t g = edge[i]; g < edge[i] + hp; g += GNU_PAGE_SIZE)
            wants |= g >= address && g < end ? permissions : host_wants(m->pages[g / GNU_PAGE_SIZE]);
        if (host_protect_pages(m, edge[i], hp, wants)) return -GNU_ENOMEM;
    }
    return 0;
}
/* executable pages in [a, a + n) are about to change */
static void code_changes(GnuMemory* m, uint64_t a, uint64_t n) {
    if (!m->code_changed) return;
    for (uint64_t i = a / GNU_PAGE_SIZE; i < (a + n + GNU_PAGE_SIZE - 1) / GNU_PAGE_SIZE; i++)
        if (m->pages[i] & GNU_PROT_EXEC) {
            m->code_changed(m->code_context, a, n);
            return;
        }
}
static struct {
    uint64_t host_page; /* gnu_memory_host_page's */
} g_memory;
void gnu_memory_host_page(uint64_t page) { g_memory.host_page = page; }
int gnu_memory_init(GnuMemory* m) {
    memset(m, 0, sizeof *m);
    m->size = GNU_ADDRESS_LIMIT;
    m->host_page = GNU_PAGE_SIZE;
#ifndef _WIN32
    /* --host-page's larger power of two works as a host with those pages (tests) */
    if (plat_mem_page() > GNU_PAGE_SIZE) m->host_page = plat_mem_page();
    if (g_memory.host_page > m->host_page) m->host_page = g_memory.host_page;
#endif
    m->base = plat_mem_reserve(m->size, m->host_page); /* aligned to the host page */
    m->pages = calloc((size_t)(m->size / GNU_PAGE_SIZE), 1);
    m->mmap_next = 0x40000000ull;
    gnu_mutex_init(&m->lock, 1);
    if (!m->base || !m->pages) {
        gnu_memory_destroy(m);
        return -GNU_ENOMEM;
    }
    return 0;
}
void gnu_memory_destroy(GnuMemory* m) {
    if (m->base) plat_mem_release(m->base, m->size);
    free(m->pages);
    gnu_mutex_destroy(&m->lock);
    memset(m, 0, sizeof *m);
}
void* gnu_memory_access(GnuMemory* m, uint64_t a, size_t n, unsigned p) {
    if (!m->base || a > m->size || n > m->size - a) return NULL;
    if (!n) return m->base + a;
    for (uint64_t i = a / GNU_PAGE_SIZE; i <= (a + n - 1) / GNU_PAGE_SIZE; i++) {
        unsigned flags = page_flags(m, i);
        if ((flags & (GNU_PAGE_MAPPED | p)) != (GNU_PAGE_MAPPED | p)) return NULL;
        /* the runtime (a read() into the buffer) is about to write over compiled code */
        if ((p & GNU_PROT_WRITE) && (flags & GNU_PAGE_CODE)) gnu_memory_code_written(m, i * GNU_PAGE_SIZE);
    }
    return m->base + a;
}
void gnu_memory_watch_code(GnuMemory* m, uint64_t a, uint64_t n) {
    if (!n || a >= m->size) return;
    if (n > m->size - a) n = m->size - a;
    gnu_mutex_lock(&m->lock);
    for (uint64_t i = a / GNU_PAGE_SIZE; i <= (a + n - 1) / GNU_PAGE_SIZE; i++) {
        unsigned flags = m->pages[i];
        if ((flags & (GNU_PAGE_MAPPED | GNU_PROT_WRITE | GNU_PAGE_CODE)) != (GNU_PAGE_MAPPED | GNU_PROT_WRITE)) continue;
        if (host_protect(m, i * GNU_PAGE_SIZE, GNU_PAGE_SIZE, flags & ~GNU_PROT_WRITE & 7u)) continue;
        pages_set(m, i, 1, flags | GNU_PAGE_CODE);
    }
    gnu_mutex_unlock(&m->lock);
}
/* With a host page larger than a guest page, the write makes the whole host page writable: every watched page in it
   is written as far as its code goes, as none of them is protected any more */
/* Another thread's write to the same page can fault while this one makes it writable: it finds the page writable
   (not watched) under the lock, and its store is retried as well */
int gnu_memory_code_written(GnuMemory* m, uint64_t a) {
    uint64_t page = a & ~(uint64_t)(GNU_PAGE_SIZE - 1), host = a & ~(m->host_page - 1);
    if (a >= m->size) return 0;
    gnu_mutex_lock(&m->lock);
    unsigned flags = m->pages[page / GNU_PAGE_SIZE];
    if (!(flags & GNU_PAGE_CODE)) {
        gnu_mutex_unlock(&m->lock);
        return (flags & (GNU_PAGE_MAPPED | GNU_PROT_WRITE)) == (GNU_PAGE_MAPPED | GNU_PROT_WRITE);
    }
    for (uint64_t g = host; g < host + m->host_page; g += GNU_PAGE_SIZE) {
        unsigned f = m->pages[g / GNU_PAGE_SIZE];
        if (!(f & GNU_PAGE_CODE)) continue;
        pages_set(m, g / GNU_PAGE_SIZE, 1, f & ~GNU_PAGE_CODE);
        if (m->code_changed) m->code_changed(m->code_context, g, GNU_PAGE_SIZE);
    }
    int ok = !host_protect(m, page, GNU_PAGE_SIZE, flags & 7u);
    gnu_mutex_unlock(&m->lock);
    return ok;
}
int gnu_memory_read(GnuMemory* m, uint64_t a, void* d, size_t n) {
    void* source = gnu_memory_access(m, a, n, GNU_PROT_READ);
    if (!source) return -GNU_EFAULT;
    if (n) memcpy(d, source, n);
    return 0;
}
int gnu_memory_write(GnuMemory* m, uint64_t a, const void* s, size_t n) {
    void* destination = gnu_memory_access(m, a, n, GNU_PROT_WRITE);
    if (!destination) return -GNU_EFAULT;
    if (n) memcpy(destination, s, n);
    return 0;
}
int gnu_memory_read_string(GnuMemory* m, uint64_t a, char* d, size_t capacity) {
    for (size_t i = 0; i < capacity; i++) {
        if (gnu_memory_read(m, a + i, d + i, 1)) return -GNU_EFAULT;
        if (!d[i]) return 0;
    }
    return -GNU_ENAMETOOLONG;
}
static int map_locked(GnuMemory* m, uint64_t a, uint64_t n, unsigned p, int replace) {
    if (!range(m, a, n) || (p & ~7u)) return -GNU_EINVAL;
    n = up(n);
    if (!replace)
        for (uint64_t i = a / GNU_PAGE_SIZE; i < (a + n) / GNU_PAGE_SIZE; i++)
            if (m->pages[i] & GNU_PAGE_MAPPED) return -GNU_EEXIST;
    code_changes(m, a, n);
    /* committed by the protect where the host commits (Windows) */
    if (host_protect(m, a, n, GNU_PROT_READ | GNU_PROT_WRITE)) return -GNU_ENOMEM;
    memset(m->base + a, 0, (size_t)n);
    if (host_protect(m, a, n, p)) return -GNU_ENOMEM;
    pages_set(m, a / GNU_PAGE_SIZE, n / GNU_PAGE_SIZE, GNU_PAGE_MAPPED | p);
    return 0;
}
static int unmap_locked(GnuMemory* m, uint64_t a, uint64_t n) {
    if (!range(m, a, n)) return -GNU_EINVAL;
    n = up(n);
    code_changes(m, a, n);
#ifdef _WIN32
    if (plat_mem_decommit(m->base + a, n)) return -GNU_ENOMEM;
#else
    if (host_protect(m, a, n, 0)) return -GNU_ENOMEM;
    { /* the host pages wholly inside: a mapping zeroes its pages anyway, so neighbours' host pages keep theirs */
        uint64_t mask = m->host_page - 1, lo = (a + mask) & ~mask, hi = (a + n) & ~mask;
        if (lo < hi) (void)madvise(m->base + lo, (size_t)(hi - lo), MADV_DONTNEED);
    }
#endif
    pages_set(m, a / GNU_PAGE_SIZE, n / GNU_PAGE_SIZE, 0);
    return 0;
}
static int protect_locked(GnuMemory* m, uint64_t a, uint64_t n, unsigned p) {
    if (!n) return (a & (GNU_PAGE_SIZE - 1)) || (p & ~7u) ? -GNU_EINVAL : 0;
    if (!range(m, a, n) || (p & ~7u)) return -GNU_EINVAL;
    n = up(n);
    for (uint64_t i = a / GNU_PAGE_SIZE; i < (a + n) / GNU_PAGE_SIZE; i++)
        if (!(m->pages[i] & GNU_PAGE_MAPPED)) return -GNU_ENOMEM;
    code_changes(m, a, n);
    if (host_protect(m, a, n, p)) return -GNU_ENOMEM;
    pages_set(m, a / GNU_PAGE_SIZE, n / GNU_PAGE_SIZE, GNU_PAGE_MAPPED | p);
    return 0;
}
static int64_t mmap_locked(GnuMemory* m, uint64_t a, uint64_t n, unsigned p, uint64_t flags) {
    const uint64_t fixed = 0x10, noreplace = 0x100000;
    if (!n || n > m->size - GNU_PAGE_SIZE || p & ~7u || (flags & 3) == 0 || (flags & 3) == 3) return -GNU_EINVAL;
    if (!(flags & 0x20)) return -GNU_ENODEV; /* file mappings are handled by the VFS caller */
    if (flags & ~(3ull | 0x10 | 0x20 | 0x100 | 0x800 | 0x1000 | 0x2000 | 0x8000 | 0x10000 | 0x20000 | 0x100000))
        return -GNU_EINVAL;
    n = up(n);
    if (flags & (fixed | noreplace)) {
        int r = map_locked(m, a, n, p, (flags & fixed) && !(flags & noreplace));
        return r ? r : (int64_t)a;
    }
    a &= ~(uint64_t)(GNU_PAGE_SIZE - 1);
    if (a >= GNU_PAGE_SIZE && a <= m->size - n) {
        int r = map_locked(m, a, n, p, 0);
        if (!r) return (int64_t)a;
    }
    /* Keep mappings below the fixed stack region; search wraps for freed holes. */
    const uint64_t low = 0x40000000ull, high = 0xe0000000ull;
    if (n > high - low) return -GNU_ENOMEM;
    for (unsigned pass = 0; pass < 2; pass++) {
        uint64_t start = pass ? low : m->mmap_next;
        uint64_t end = pass ? m->mmap_next : high;
        if (start > end || n > end - start) continue;
        for (uint64_t candidate = start; candidate <= end - n;) {
            uint64_t i = candidate / GNU_PAGE_SIZE, last = (candidate + n) / GNU_PAGE_SIZE;
            while (i < last && !(m->pages[i] & GNU_PAGE_MAPPED)) i++;
            if (i == last) {
                int r = map_locked(m, candidate, n, p, 0);
                if (r) return r;
                m->mmap_next = candidate + n;
                return (int64_t)candidate;
            }
            candidate = (i + 1) * GNU_PAGE_SIZE;
        }
    }
    return -GNU_ENOMEM;
}
static uint64_t brk_locked(GnuMemory* m, uint64_t requested) {
    if (!requested || requested < m->brk_base || requested > m->brk_limit) return m->brk_current;
    uint64_t old_page = up(m->brk_current), new_page = up(requested);
    if (new_page > old_page && map_locked(m, old_page, new_page - old_page, 3, 0)) return m->brk_current;
    if (new_page < old_page && unmap_locked(m, new_page, old_page - new_page)) return m->brk_current;
    m->brk_current = requested;
    return requested;
}
static size_t maps_locked(GnuMemory* m, char* destination, size_t capacity) {
    size_t used = 0;
    for (uint64_t i = 0; i < m->size / GNU_PAGE_SIZE;) {
        if (!(m->pages[i] & GNU_PAGE_MAPPED)) {
            i++;
            continue;
        }
        uint64_t begin = i;
        unsigned p = m->pages[i] & 15u;
        do { i++; } while (i < m->size / GNU_PAGE_SIZE && (m->pages[i] & 15u) == p);
        char line[128];
        int n = snprintf(line, sizeof line, "%016llx-%016llx %c%c%cp 00000000 00:00 0\n",
                         (unsigned long long)(begin * GNU_PAGE_SIZE), (unsigned long long)(i * GNU_PAGE_SIZE),
                         p & 1 ? 'r' : '-', p & 2 ? 'w' : '-', p & 4 ? 'x' : '-');
        if (n > 0) {
            size_t copy = (size_t)n;
            if (used >= capacity)
                copy = 0;
            else if (copy > capacity - used)
                copy = capacity - used;
            if (copy) memcpy(destination + used, line, copy);
            used += (size_t)n;
        }
    }
    return used;
}
/* the map's calls, each under its lock */
int gnu_memory_map(GnuMemory* m, uint64_t a, uint64_t n, unsigned p, int replace) {
    gnu_mutex_lock(&m->lock);
    int r = map_locked(m, a, n, p, replace);
    gnu_mutex_unlock(&m->lock);
    return r;
}
int gnu_memory_unmap(GnuMemory* m, uint64_t a, uint64_t n) {
    gnu_mutex_lock(&m->lock);
    int r = unmap_locked(m, a, n);
    gnu_mutex_unlock(&m->lock);
    return r;
}
int gnu_memory_protect(GnuMemory* m, uint64_t a, uint64_t n, unsigned p) {
    gnu_mutex_lock(&m->lock);
    int r = protect_locked(m, a, n, p);
    gnu_mutex_unlock(&m->lock);
    return r;
}
int64_t gnu_memory_mmap(GnuMemory* m, uint64_t a, uint64_t n, unsigned p, uint64_t flags) {
    gnu_mutex_lock(&m->lock);
    int64_t r = mmap_locked(m, a, n, p, flags);
    gnu_mutex_unlock(&m->lock);
    return r;
}
uint64_t gnu_memory_brk(GnuMemory* m, uint64_t requested) {
    gnu_mutex_lock(&m->lock);
    uint64_t r = brk_locked(m, requested);
    gnu_mutex_unlock(&m->lock);
    return r;
}
size_t gnu_memory_maps(GnuMemory* m, char* destination, size_t capacity) {
    gnu_mutex_lock(&m->lock);
    size_t r = maps_locked(m, destination, capacity);
    gnu_mutex_unlock(&m->lock);
    return r;
}
