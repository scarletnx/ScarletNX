/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "3ds/memory/ctr_memory.h"
#include "3ds/memory/host.h"
#include "common/memory/host.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* the guest's permissions go to the host layer as they are */
_Static_assert(SNX_CTR_READ == SNX_HOST_READ && SNX_CTR_WRITE == SNX_HOST_WRITE && SNX_CTR_EXEC == SNX_HOST_EXEC,
               "permission bits");

/* A guest address of a physical RAM page in a flat space: the reverse map write tracking protects through */
typedef struct {
    uint32_t va, next; /* next: the page's next alias + 1, 0 for none */
    uint8_t space;     /* its index in spaces[] */
} Alias;
#define TRACK_SPACES 4
/* Two-level ARM-sized page metadata, not an emulated hardware MMU table. region: the profile's region the frame is in,
   plus 1, set at map time so that an access finds its host memory without a scan of the regions; 0 when unmapped */
typedef struct {
    uint32_t pa;
    unsigned char permissions, region;
} Page;
struct SnxCtrMemory {
    const SnxCtrProfile* profile;
    /* each RAM region's memory: its part of host's view, the regions one after another (NULL for MMIO) */
    unsigned char* backing[8];
    SnxHostMemory host;
    void* mmio_context;
    SnxCtrMmioRead read;
    SnxCtrMmioWrite write;
    uint64_t epoch;
    /* write tracking (snx_ctr_track_*), by RAM page of the host object (its offset / 4096) */
    uint32_t pages;
    uint8_t* level;
    uint32_t* alias_head; /* the page's first alias + 1, 0 for none */
    Alias* aliases;
    uint32_t alias_count, alias_capacity, alias_free; /* alias_free: a freed entry + 1, 0 for none */
    SnxCtrAddressSpace* spaces[TRACK_SPACES];         /* the flat spaces, whose views a level protects */
    uint32_t tracked;                                 /* pages above SNX_CTR_TRACK_NONE */
    uint32_t protect_failures;                        /* views of tracked pages the host could not protect */
    SnxCtrTrackHandler handler;
    void* handler_context;
    bool resolving;
};
struct SnxCtrAddressSpace {
    SnxCtrMemory* memory;
    Page* tables[1024];
    /* the flat space: every guest address an offset into it, the mappings views of the memory's host object; NULL
       without one */
    unsigned char* base;
    uint8_t index; /* in the memory's spaces[], for a flat space */
};

SnxCtrMemory* snx_ctr_memory_new(SnxCtrModel model) {
    const SnxCtrProfile* p = snx_ctr_profile(model);
    if (!p || p->region_count > 8) return NULL;
    SnxCtrMemory* m = calloc(1, sizeof *m);
    if (!m) return NULL;
    m->profile = p;
    m->epoch = 1;
    uint64_t ram = 0;
    for (size_t i = 0; i < p->region_count; ++i)
        if (p->regions[i].kind != SNX_CTR_MMIO) ram += p->regions[i].size;
    /* one host memory object, whatever the host: its view is page-aligned (the GPU imports each region's part, and a
       flat space maps its pages) */
    if (!snx_host_memory_new(&m->host, ram)) {
        free(m);
        return NULL;
    }
    uint64_t offset = 0;
    for (size_t i = 0; i < p->region_count; ++i) {
        if (p->regions[i].kind == SNX_CTR_MMIO) continue;
        m->backing[i] = m->host.view + offset;
        offset += p->regions[i].size;
    }
    m->pages = (uint32_t)(ram / 4096);
    m->level = calloc(m->pages, 1);
    m->alias_head = calloc(m->pages, sizeof *m->alias_head);
    if (!m->level || !m->alias_head) {
        snx_ctr_memory_free(m);
        return NULL;
    }
    return m;
}
unsigned char* snx_ctr_memory_region(const SnxCtrMemory* m, size_t index) {
    return m && index < m->profile->region_count ? m->backing[index] : NULL;
}
const unsigned char* snx_ctr_memory_ram(const SnxCtrMemory* m, uint64_t* size) {
    *size = m->host.size;
    return m->host.view;
}
const SnxCtrProfile* snx_ctr_memory_profile(const SnxCtrMemory* m) { return m ? m->profile : NULL; }
void snx_ctr_memory_free(SnxCtrMemory* m) {
    if (!m) return;
    snx_host_memory_free(&m->host);
    free(m->level);
    free(m->alias_head);
    free(m->aliases);
    free(m);
}
void snx_ctr_memory_mmio(SnxCtrMemory* m, void* context, SnxCtrMmioRead read, SnxCtrMmioWrite write) {
    if (!m) return;
    m->mmio_context = context;
    m->read = read;
    m->write = write;
}
SnxCtrAddressSpace* snx_ctr_space_new(SnxCtrMemory* m) {
    if (!m) return NULL;
    SnxCtrAddressSpace* s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->memory = m;
    if (ctr_host_flat()) {
        while (s->index < TRACK_SPACES && m->spaces[s->index]) ++s->index;
        if (s->index == TRACK_SPACES || !(s->base = snx_host_reserve(CTR_HOST_SPACE + CTR_HOST_GUARD))) {
            free(s);
            return NULL;
        }
        m->spaces[s->index] = s;
    }
    return s;
}
unsigned char* snx_ctr_space_base(const SnxCtrAddressSpace* s) { return s ? s->base : NULL; }
/* the host object's offset of a physical RAM address, the region's: its backing's place in the view */
static uint64_t host_offset(const SnxCtrMemory* m, const SnxCtrRegion* region, uint32_t pa) {
    size_t i = (size_t)(region - m->profile->regions);
    return (uint64_t)(m->backing[i] - m->host.view) + (pa - region->base);
}
static void alias_remove(SnxCtrMemory* m, uint32_t index, const SnxCtrAddressSpace* s, uint32_t va);
static uint32_t ram_index(const SnxCtrMemory* m, uint32_t pa);
void snx_ctr_space_free(SnxCtrAddressSpace* s) {
    if (!s) return;
    if (s->base) { /* its aliases go with it */
        for (uint32_t t = 0; t < 1024; ++t)
            for (uint32_t i = 0; s->tables[t] && i < 1024; ++i)
                if (s->tables[t][i].region) {
                    uint32_t index = ram_index(s->memory, s->tables[t][i].pa);
                    if (index != UINT32_MAX) alias_remove(s->memory, index, s, t << 22 | i << 12);
                }
        s->memory->spaces[s->index] = NULL;
        snx_host_release(s->base, CTR_HOST_SPACE + CTR_HOST_GUARD);
    }
    for (size_t i = 0; i < 1024; ++i) free(s->tables[i]);
    free(s);
}
static Page* page(const SnxCtrAddressSpace* s, uint32_t address) {
    Page* table = s->tables[address >> 22];
    return table ? &table[(address >> 12) & 1023] : NULL;
}
/* ---- write tracking ---- */

/* the RAM page index of a physical address (its host object offset / 4096), UINT32_MAX outside RAM */
static uint32_t ram_index(const SnxCtrMemory* m, uint32_t pa) {
    const SnxCtrRegion* r = snx_ctr_physical_region(m->profile, pa, 1);
    if (!r || r->kind == SNX_CTR_MMIO) return UINT32_MAX;
    return (uint32_t)(host_offset(m, r, pa) / 4096);
}
/* the host protection of a guest page at a level: owned pages trap every access, watched ones writes */
static unsigned tracked_permissions(unsigned level, unsigned permissions) {
    return level == SNX_CTR_TRACK_OWN ? 0u : level == SNX_CTR_TRACK_WATCH ? permissions & ~SNX_CTR_WRITE : permissions;
}
/* digits of value in base 10 or 16 at at, for a line a fault handler writes (no stdio): the end of them */
static char* digits(char* at, uint32_t value, unsigned base, unsigned width) {
    char buffer[10];
    unsigned n = 0;
    do {
        buffer[n++] = "0123456789abcdef"[value % base];
        value /= base;
    } while (value || n < width);
    while (n) *at++ = buffer[--n];
    return at;
}
/* A RAM page's flat view at va given its tracking level's protection. A host that cannot says so, at the 1st, 2nd,
   4th... failure, as the page's traps are wrong from then on (the GPU's copy missed, or a fault where none should be):
   mprotect gives ENOMEM once the process has as many mappings as the system allows (vm.max_map_count), which the views
   of a GPU's dma-buf can come to, as a mapping of one never merges with its neighbours again */
static void protect(SnxCtrMemory* m, unsigned char* base, uint32_t va, unsigned permissions) {
    if (snx_host_protect(base, va, 4096, permissions)) return;
    int error = errno;
    uint32_t n = ++m->protect_failures;
    if (n & (n - 1)) return;
    char line[160], *at = line;
    static const char head[] = "3ds memory: write tracking cannot protect the view at guest 0x", middle[] = " (errno ",
                      tail[] = "): its traps are wrong from here; failures so far: ";
    memcpy(at, head, sizeof head - 1);
    at = digits(at + sizeof head - 1, va, 16, 8);
    memcpy(at, middle, sizeof middle - 1);
    at = digits(at + sizeof middle - 1, (uint32_t)error, 10, 1);
    memcpy(at, tail, sizeof tail - 1);
    at = digits(at + sizeof tail - 1, n, 10, 1);
    *at++ = '\n';
    ctr_host_say(line, (size_t)(at - line));
}
/* every flat view of the page given its level's protection */
static void apply(SnxCtrMemory* m, uint32_t index) {
    for (uint32_t a = m->alias_head[index]; a; a = m->aliases[a - 1].next) {
        const Alias* alias = &m->aliases[a - 1];
        SnxCtrAddressSpace* s = m->spaces[alias->space];
        protect(m, s->base, alias->va, tracked_permissions(m->level[index], page(s, alias->va)->permissions));
    }
}
static bool alias_add(SnxCtrMemory* m, uint32_t index, const SnxCtrAddressSpace* s, uint32_t va) {
    uint32_t a = m->alias_free;
    if (a)
        m->alias_free = m->aliases[a - 1].next;
    else {
        if (m->alias_count == m->alias_capacity) {
            uint32_t n = m->alias_capacity ? m->alias_capacity * 2 : 4096;
            Alias* grown = realloc(m->aliases, n * sizeof *grown);
            if (!grown) return false;
            m->aliases = grown;
            m->alias_capacity = n;
        }
        a = ++m->alias_count;
    }
    m->aliases[a - 1] = (Alias){va, m->alias_head[index], s->index};
    m->alias_head[index] = a;
    return true;
}
static void alias_remove(SnxCtrMemory* m, uint32_t index, const SnxCtrAddressSpace* s, uint32_t va) {
    for (uint32_t* link = &m->alias_head[index]; *link; link = &m->aliases[*link - 1].next) {
        Alias* alias = &m->aliases[*link - 1];
        if (alias->va != va || alias->space != s->index) continue;
        uint32_t a = *link;
        *link = alias->next;
        alias->next = m->alias_free;
        m->alias_free = a;
        return;
    }
}
void snx_ctr_track_handler(SnxCtrMemory* m, SnxCtrTrackHandler handler, void* context) {
    m->handler = handler;
    m->handler_context = context;
}
void snx_ctr_track_set(SnxCtrMemory* m, uint32_t pa, uint32_t size, SnxCtrTrackLevel level) {
    if (!m || !size) return;
    for (uint64_t at = pa & ~4095u; at < (uint64_t)pa + size; at += 4096) {
        uint32_t index = ram_index(m, (uint32_t)at);
        if (index == UINT32_MAX || m->level[index] == level) continue;
        m->tracked += (level != SNX_CTR_TRACK_NONE) - (m->level[index] != SNX_CTR_TRACK_NONE);
        m->level[index] = (uint8_t)level;
        apply(m, index);
    }
}
SnxCtrTrackLevel snx_ctr_track_level(const SnxCtrMemory* m, uint32_t pa) {
    uint32_t index = m ? ram_index(m, pa) : UINT32_MAX;
    return index == UINT32_MAX ? SNX_CTR_TRACK_NONE : (SnxCtrTrackLevel)m->level[index];
}
/* One step down for the page of pa: what the GPU owns lands (owned to watched), or its copies go (watched to none),
   by the handler. A handler that leaves the page where it was is a bug in it: the step is taken anyway, so the
   access goes on, and said */
static void step(SnxCtrMemory* m, uint32_t index, uint32_t pa) {
    unsigned before = m->level[index];
    m->resolving = true;
    if (m->handler) m->handler(m->handler_context, pa & ~4095u, (SnxCtrTrackLevel)before);
    m->resolving = false;
    if (m->level[index] >= before) {
        static const char line[] = "3ds memory: a GPU page's handler did not resolve it\n";
        ctr_host_say(line, sizeof line - 1);
        snx_ctr_track_set(m, pa & ~4095u, 4096, before == SNX_CTR_TRACK_OWN ? SNX_CTR_TRACK_WATCH : SNX_CTR_TRACK_NONE);
    }
}
/* the CPU (this API, a bus master's access by physical address, or the checked bus) is about to read or write
   [pa, pa + size): its tracked pages resolved first */
static void touch(SnxCtrMemory* m, uint32_t pa, size_t size, bool write) {
    if (!m->tracked || m->resolving) return;
    for (uint64_t at = pa & ~4095u; at < (uint64_t)pa + size; at += 4096) {
        uint32_t index = ram_index(m, (uint32_t)at);
        if (index == UINT32_MAX) continue;
        while (m->level[index] == SNX_CTR_TRACK_OWN || (write && m->level[index] == SNX_CTR_TRACK_WATCH))
            step(m, index, (uint32_t)at);
    }
}
/* the same for a virtual range of a space, page by page */
static void touch_virtual(SnxCtrAddressSpace* s, uint32_t va, size_t size, bool write) {
    if (!s->memory->tracked || s->memory->resolving) return;
    for (uint64_t at = va & ~4095u; at < (uint64_t)va + size; at += 4096) {
        Page* p = page(s, (uint32_t)at);
        if (p && p->region) touch(s->memory, p->pa, 4096, write);
    }
}
bool snx_ctr_track_fault(SnxCtrAddressSpace* s, uint32_t va) {
    SnxCtrMemory* m = s->memory;
    Page* p = page(s, va);
    if (!p || !p->region || m->resolving) return false;
    uint32_t index = ram_index(m, p->pa);
    if (index == UINT32_MAX || m->level[index] == SNX_CTR_TRACK_NONE) return false; /* the guest's own data abort */
    step(m, index, p->pa);
    return true;
}

/* ---- the RAM moved into another object ---- */

/* The flat space's views of RAM pages mapped from host's object at their addresses, each at its guest permissions less
   its tracking level's: a run of pages one after another in the space and in the object at one protection is one map.
   MMIO pages were never mapped there, and stay inaccessible */
static bool map_space(const SnxCtrAddressSpace* s, const SnxHostMemory* host) {
    uint64_t start = 0, from = 0, run = 0;
    unsigned protection = 0;
    for (uint64_t va = 0; va <= CTR_HOST_SPACE; va += 4096) {
        const Page* p = va < CTR_HOST_SPACE ? page(s, (uint32_t)va) : NULL;
        if (!p && !run && va < CTR_HOST_SPACE) { /* no table: its 4 MiB at once */
            va |= 0x3FF000;
            continue;
        }
        uint32_t index = p && p->region ? ram_index(s->memory, p->pa) : UINT32_MAX;
        unsigned permissions = index == UINT32_MAX ? 0 : tracked_permissions(s->memory->level[index], p->permissions);
        if (run && index != UINT32_MAX && (uint64_t)index * 4096 == from + run && permissions == protection) {
            run += 4096;
            continue;
        }
        if (run && !snx_host_map(host, s->base, start, from, run, protection)) return false;
        run = index == UINT32_MAX ? 0 : 4096;
        start = va;
        from = (uint64_t)index * 4096;
        protection = permissions;
    }
    return true;
}
/* every view of the RAM mapped from host's object: the object's own, then each flat space's */
static bool map_views(const SnxCtrMemory* m, const SnxHostMemory* host) {
    if (!snx_host_map(host, host->view, 0, 0, host->size, SNX_HOST_READ | SNX_HOST_WRITE)) return false;
    for (unsigned i = 0; i < TRACK_SPACES; ++i)
        if (m->spaces[i] && !map_space(m->spaces[i], host)) return false;
    return true;
}
bool snx_ctr_memory_adopt(SnxCtrMemory* m, intptr_t handle) {
#ifdef _WIN32
    (void)m;
    (void)handle;
    return false; /* only a placeholder takes a view (memory/host.c), and the object's own view is none */
#else
    SnxHostMemory next = {handle, m->host.view, m->host.size};
    if (!snx_host_memory_fill(&next, m->host.view)) return false;
    if (!map_views(m, &next)) {
        int error = errno;
        /* every view back on the old object, which still holds the RAM: nothing wrote it meanwhile. Views of two
           objects would be RAM whose aliases differ: no run goes on from there */
        if (!map_views(m, &m->host)) {
            static const char line[] = "3ds memory: the RAM's views could not be mapped back from its object\n";
            ctr_host_say(line, sizeof line - 1);
            abort();
        }
        errno = error;
        return false;
    }
    snx_host_memory_close(&m->host);
    m->host = next;
    return true;
#endif
}

/* A board-wide epoch conservatively invalidates every reservation on any
   successful store or mapping change, including aliases and other spaces.
   Saturation disables exclusive success rather than permitting an ABA wrap. */
static void changed(SnxCtrMemory* m) {
    if (m->epoch != UINT64_MAX) ++m->epoch;
}
uint64_t snx_ctr_memory_epoch(const SnxCtrAddressSpace* s) { return s ? s->memory->epoch : 0; }
static bool valid_range(uint32_t va, uint32_t size) {
    return size && !((va | size) & 4095) && (uint64_t)va + size <= 0x100000000ull;
}
static bool permissions_valid(unsigned permissions) { return !(permissions & ~7u); }
SnxCtrMemResult snx_ctr_map(SnxCtrAddressSpace* s, uint32_t va, uint32_t pa, uint32_t size, unsigned permissions) {
    if (!s || !valid_range(va, size) || (pa & 4095) || !permissions_valid(permissions)) return SNX_CTR_MEM_INVALID;
    const SnxCtrRegion* region = snx_ctr_physical_region(s->memory->profile, pa, size);
    if (!region) return SNX_CTR_MEM_INVALID;
    if (region->kind == SNX_CTR_MMIO && (permissions & SNX_CTR_EXEC)) return SNX_CTR_MEM_PERMISSION;
    for (uint64_t at = va; at < (uint64_t)va + size; at += 4096) {
        Page* p = page(s, (uint32_t)at);
        if (p && p->region) return SNX_CTR_MEM_OVERLAP;
    }
    /* Allocating empty tables is harmless on failure; no PTE is published yet. */
    for (uint64_t at = va; at < (uint64_t)va + size; at += 4096) {
        unsigned index = (uint32_t)at >> 22;
        if (!s->tables[index]) s->tables[index] = calloc(1024, sizeof(Page));
        if (!s->tables[index]) return SNX_CTR_MEM_NOMEM;
    }
    /* the flat space's view of the pages, before they are published: MMIO stays inaccessible there, and an access
       from compiled code faults (ctr_host_faults) */
    if (s->base && region->kind != SNX_CTR_MMIO &&
        !snx_host_map(&s->memory->host, s->base, va, host_offset(s->memory, region, pa), size, permissions))
        return SNX_CTR_MEM_NOMEM;
    for (uint64_t offset = 0; offset < size; offset += 4096)
        *page(s, va + (uint32_t)offset) = (Page){pa + (uint32_t)offset, (unsigned char)permissions,
                                                 (unsigned char)(region - s->memory->profile->regions + 1)};
    /* a flat view of a RAM page is one of its aliases, at its level's protection */
    if (s->base && region->kind != SNX_CTR_MMIO)
        for (uint64_t offset = 0; offset < size; offset += 4096) {
            uint32_t index = ram_index(s->memory, pa + (uint32_t)offset);
            if (!alias_add(s->memory, index, s, va + (uint32_t)offset)) {
                for (uint64_t undo = 0; undo < offset; undo += 4096)
                    alias_remove(s->memory, ram_index(s->memory, pa + (uint32_t)undo), s, va + (uint32_t)undo);
                (void)snx_host_unmap(s->base, va, size);
                for (uint64_t at = 0; at < size; at += 4096) *page(s, va + (uint32_t)at) = (Page){0};
                return SNX_CTR_MEM_NOMEM;
            }
            if (s->memory->level[index])
                protect(s->memory, s->base, va + (uint32_t)offset,
                        tracked_permissions(s->memory->level[index], permissions));
        }
    changed(s->memory);
    return SNX_CTR_MEM_OK;
}
/* the flat space's side of a protect or an unmap, over the runs of RAM pages in the range: an MMIO page was never
   mapped there and stays inaccessible */
static bool host_change(SnxCtrAddressSpace* s, uint32_t va, uint32_t size, unsigned permissions, bool unmap) {
    uint32_t run = 0, start = va;
    for (uint64_t at = va; at <= (uint64_t)va + size; at += 4096) {
        bool ram = false;
        if (at < (uint64_t)va + size) {
            const SnxCtrRegion* r = snx_ctr_physical_region(s->memory->profile, page(s, (uint32_t)at)->pa, 4096);
            ram = r && r->kind != SNX_CTR_MMIO;
        }
        if (ram) {
            if (!run) start = (uint32_t)at;
            run += 4096;
            continue;
        }
        if (run && !(unmap ? snx_host_unmap(s->base, start, run) : snx_host_protect(s->base, start, run, permissions)))
            return false;
        run = 0;
    }
    return true;
}
static SnxCtrMemResult change(SnxCtrAddressSpace* s, uint32_t va, uint32_t size, unsigned permissions, bool unmap) {
    if (!s || !valid_range(va, size) || !permissions_valid(permissions)) return SNX_CTR_MEM_INVALID;
    for (uint64_t at = va; at < (uint64_t)va + size; at += 4096) {
        Page* p = page(s, (uint32_t)at);
        if (!p || !p->region) return SNX_CTR_MEM_UNMAPPED;
        const SnxCtrRegion* r = snx_ctr_physical_region(s->memory->profile, p->pa, 4096);
        if (!unmap && (permissions & SNX_CTR_EXEC) && r->kind == SNX_CTR_MMIO) return SNX_CTR_MEM_PERMISSION;
    }
    if (s->base && !host_change(s, va, size, permissions, unmap)) return SNX_CTR_MEM_NOMEM;
    for (uint64_t at = va; at < (uint64_t)va + size; at += 4096) {
        Page* p = page(s, (uint32_t)at);
        uint32_t index = s->base ? ram_index(s->memory, p->pa) : UINT32_MAX;
        if (unmap) {
            if (index != UINT32_MAX) alias_remove(s->memory, index, s, (uint32_t)at);
            *p = (Page){0};
        } else {
            p->permissions = (unsigned char)permissions;
            if (index != UINT32_MAX && s->memory->level[index]) /* a tracked page keeps its level's protection */
                protect(s->memory, s->base, (uint32_t)at, tracked_permissions(s->memory->level[index], permissions));
        }
    }
    changed(s->memory);
    return SNX_CTR_MEM_OK;
}
SnxCtrMemResult snx_ctr_protect(SnxCtrAddressSpace* s, uint32_t va, uint32_t size, unsigned permissions) {
    return change(s, va, size, permissions, false);
}
SnxCtrMemResult snx_ctr_unmap(SnxCtrAddressSpace* s, uint32_t va, uint32_t size) {
    return change(s, va, size, 0, true);
}
SnxCtrMemResult snx_ctr_query(const SnxCtrAddressSpace* s, uint32_t va, uint32_t* pa, unsigned* permissions) {
    if (!s) return SNX_CTR_MEM_INVALID;
    Page* p = page(s, va);
    if (!p || !p->region) return SNX_CTR_MEM_UNMAPPED;
    if (pa) *pa = p->pa + (va & 4095);
    if (permissions) *permissions = p->permissions;
    return SNX_CTR_MEM_OK;
}
/* the host memory at offset into p's frame; NULL with *mmio set for a device's page */
static unsigned char* page_host(const SnxCtrMemory* m, const Page* p, uint32_t offset, bool* mmio) {
    size_t i = (size_t)p->region - 1;
    const SnxCtrRegion* r = &m->profile->regions[i];
    *mmio = r->kind == SNX_CTR_MMIO;
    return *mmio ? NULL : m->backing[i] + (p->pa - r->base) + offset;
}
SnxCtrMemResult snx_ctr_check_ram(SnxCtrAddressSpace* s, uint32_t va, size_t size, unsigned access) {
    if (!s || !access || !permissions_valid(access) || size > 0x100000000ull - va) return SNX_CTR_MEM_INVALID;
    for (size_t off = 0; off < size;) {
        uint32_t at = va + (uint32_t)off;
        Page* p = page(s, at);
        if (!p || !p->region) return SNX_CTR_MEM_UNMAPPED;
        if ((p->permissions & access) != access) return SNX_CTR_MEM_PERMISSION;
        bool mmio;
        (void)page_host(s->memory, p, 0, &mmio);
        if (mmio) return SNX_CTR_MEM_DEVICE;
        size_t n = 4096 - (at & 4095);
        if (n > size - off) n = size - off;
        off += n;
    }
    return SNX_CTR_MEM_OK;
}
static SnxCtrMemResult transfer(SnxCtrAddressSpace* s, uint32_t va, void* buffer, size_t size, unsigned access,
                                bool ram_only) {
    if (!s || !buffer || !size || size > 0x100000000ull - va) return SNX_CTR_MEM_INVALID;
    touch_virtual(s, va, size, access == SNX_CTR_WRITE);
    /* within one page, as nearly every access is (each instruction's fetch among them): one lookup */
    if ((va & 4095) + size <= 4096) {
        Page* p = page(s, va);
        if (!p || !p->region) return SNX_CTR_MEM_UNMAPPED;
        if (!(p->permissions & access)) return SNX_CTR_MEM_PERMISSION;
        bool mmio;
        unsigned char* host = page_host(s->memory, p, va & 4095, &mmio);
        if (!mmio) {
            if (access == SNX_CTR_WRITE) {
                memcpy(host, buffer, size);
                changed(s->memory);
            } else
                memcpy(buffer, host, size);
            return SNX_CTR_MEM_OK;
        }
    }
    size_t offset = 0;
    while (offset < size) {
        uint32_t at = va + (uint32_t)offset;
        Page* p = page(s, at);
        if (!p || !p->region) return SNX_CTR_MEM_UNMAPPED;
        if (!(p->permissions & access)) return SNX_CTR_MEM_PERMISSION;
        bool mmio;
        (void)page_host(s->memory, p, 0, &mmio);
        size_t n = 4096 - (at & 4095);
        if (mmio) {
            if (ram_only || offset || size > n || (size != 1 && size != 2 && size != 4 && size != 8) ||
                (va & (size - 1)) || access == SNX_CTR_EXEC)
                return SNX_CTR_MEM_DEVICE;
            SnxCtrMemory* m = s->memory;
            uint32_t pa = p->pa + (at & 4095);
            bool ok = access == SNX_CTR_WRITE ? m->write && m->write(m->mmio_context, pa, buffer, size)
                                              : m->read && m->read(m->mmio_context, pa, buffer, size);
            if (ok && access == SNX_CTR_WRITE) changed(m);
            return ok ? SNX_CTR_MEM_OK : SNX_CTR_MEM_DEVICE;
        }
        if (n > size - offset) n = size - offset;
        offset += n;
    }
    for (offset = 0; offset < size;) {
        uint32_t at = va + (uint32_t)offset;
        Page* p = page(s, at);
        bool mmio;
        unsigned char* host = page_host(s->memory, p, at & 4095, &mmio);
        size_t n = 4096 - (at & 4095);
        if (n > size - offset) n = size - offset;
        if (access == SNX_CTR_WRITE)
            memcpy(host, (const unsigned char*)buffer + offset, n);
        else
            memcpy((unsigned char*)buffer + offset, host, n);
        offset += n;
    }
    if (access == SNX_CTR_WRITE) changed(s->memory);
    return SNX_CTR_MEM_OK;
}
SnxCtrMemResult snx_ctr_read(SnxCtrAddressSpace* s, uint32_t va, void* output, size_t size) {
    return transfer(s, va, output, size, SNX_CTR_READ, false);
}
SnxCtrMemResult snx_ctr_fetch(SnxCtrAddressSpace* s, uint32_t va, void* output, size_t size) {
    return transfer(s, va, output, size, SNX_CTR_EXEC, false);
}
SnxCtrMemResult snx_ctr_write(SnxCtrAddressSpace* s, uint32_t va, const void* input, size_t size) {
    return transfer(s, va, (void*)input, size, SNX_CTR_WRITE, false);
}
SnxCtrMemResult snx_ctr_read_ram(SnxCtrAddressSpace* s, uint32_t va, void* output, size_t size) {
    return transfer(s, va, output, size, SNX_CTR_READ, true);
}
SnxCtrMemResult snx_ctr_write_ram(SnxCtrAddressSpace* s, uint32_t va, const void* input, size_t size) {
    return transfer(s, va, (void*)input, size, SNX_CTR_WRITE, true);
}
static SnxCtrMemResult device(SnxCtrMemory* m, uint32_t pa, void* buffer, size_t size, bool write) {
    if (!m || !buffer || !size || size > UINT32_MAX) return SNX_CTR_MEM_INVALID;
    const SnxCtrRegion* r = snx_ctr_physical_region(m->profile, pa, (uint32_t)size);
    if (!r) return SNX_CTR_MEM_INVALID;
    if (r->kind == SNX_CTR_MMIO) return SNX_CTR_MEM_DEVICE;
    touch(m, pa, size, write);
    unsigned char* host = m->backing[r - m->profile->regions] + (pa - r->base);
    if (write) {
        memcpy(host, buffer, size);
        changed(m);
    } else
        memcpy(buffer, host, size);
    return SNX_CTR_MEM_OK;
}
SnxCtrMemResult snx_ctr_phys_read(SnxCtrMemory* m, uint32_t pa, void* output, size_t size) {
    return device(m, pa, output, size, false);
}
SnxCtrMemResult snx_ctr_phys_write(SnxCtrMemory* m, uint32_t pa, const void* input, size_t size) {
    return device(m, pa, (void*)input, size, true);
}
