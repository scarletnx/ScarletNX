/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The board's RAM moved into another object (snx_ctr_memory_adopt), as the GPU's device does when its driver will not
   import the RAM (gpu/vk/device.c): a memfd stands in for its dma-buf, so every host runs it. What the RAM held is
   there, every view of it is the new object's (the host's, an alias, a run of pages, VRAM), a watched page still
   traps writes and a read-only alias stays read-only, and an object that does not map is refused with every view as
   it was. On Windows the call refuses. Then a view the host cannot protect for write tracking: said, with its address
   and the system's errno. */
#include "3ds/memory/ctr_memory.h"
#include "3ds/memory/host.h"
#include "common/memory/host.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#ifndef _WIN32
#include <errno.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

#define FCRAM 0x20000000u
#define VRAM  0x18000000u
#define RW    (SNX_CTR_READ | SNX_CTR_WRITE)
static struct {
    SnxCtrMemory* m;
    volatile unsigned watched; /* the handler's steps; volatile, as it runs from a fault (3ds_memory_track.c) */
} g;
static void handler(void* context, uint32_t pa, SnxCtrTrackLevel level) {
    (void)context;
    assert(pa == FCRAM && level == SNX_CTR_TRACK_WATCH);
    snx_ctr_track_set(g.m, pa, 4096, SNX_CTR_TRACK_NONE);
    ++g.watched;
}
static bool resolve(void* context, uint32_t va) { return snx_ctr_track_fault(context, va); }
/* the region at physical address pa: where the host holds it */
static unsigned char* region(uint32_t pa) {
    const SnxCtrProfile* p = snx_ctr_memory_profile(g.m);
    for (size_t i = 0; i < p->region_count; ++i)
        if (p->regions[i].base == pa) return snx_ctr_memory_region(g.m, i);
    return NULL;
}
#ifdef __linux__
/* the protection /proc/self/maps gives the page at address ("rw-s"), "" when no mapping holds it */
static const char* protection(const void* address) {
    static char perms[5];
    unsigned long lo, hi, at = (unsigned long)address;
    char line[512];
    bool found = false;
    FILE* f = fopen("/proc/self/maps", "r");
    while (f && !found && fgets(line, sizeof line, f))
        found = sscanf(line, "%lx-%lx %4s", &lo, &hi, perms) == 3 && lo <= at && at < hi;
    if (f) fclose(f);
    return found ? perms : "";
}
#endif
int main(void) {
    g.m = snx_ctr_memory_new(SNX_CTR_OLD);
    SnxCtrAddressSpace* s = snx_ctr_space_new(g.m);
    assert(g.m && s);
    unsigned char* base = snx_ctr_space_base(s);
    uint64_t size;
    const unsigned char* ram = snx_ctr_memory_ram(g.m, &size);
    unsigned char *fcram = region(FCRAM), *vram = region(VRAM);
    assert(ram && fcram && vram && size == 0x08000000u + 0x00600000u + 0x00080000u + 0x00080000u);
    uint64_t fcram_at = (uint64_t)(fcram - ram), vram_at = (uint64_t)(vram - ram);
    /* a page and its read-only alias, a run of three pages, a VRAM page */
    assert(snx_ctr_map(s, 0x100000, FCRAM, 4096, RW) == SNX_CTR_MEM_OK);
    assert(snx_ctr_map(s, 0x200000, FCRAM, 4096, SNX_CTR_READ) == SNX_CTR_MEM_OK);
    assert(snx_ctr_map(s, 0x400000, FCRAM + 0x1000, 0x3000, RW) == SNX_CTR_MEM_OK);
    assert(snx_ctr_map(s, 0x1f000000, VRAM, 4096, RW) == SNX_CTR_MEM_OK);
    static unsigned char bytes[0x3000], got[0x3000];
    for (unsigned i = 0; i < sizeof bytes; ++i) bytes[i] = (unsigned char)(i * 7 + 3);
    assert(snx_ctr_write(s, 0x400000, bytes, sizeof bytes) == SNX_CTR_MEM_OK);
    assert(snx_ctr_phys_write(g.m, VRAM + 9, "vram", 4) == SNX_CTR_MEM_OK);
    assert(snx_ctr_phys_write(g.m, FCRAM + 5, "page", 4) == SNX_CTR_MEM_OK);
    snx_ctr_track_handler(g.m, handler, NULL);
    snx_ctr_track_set(g.m, FCRAM, 4096, SNX_CTR_TRACK_WATCH);
    SnxHostMemory next; /* the object the RAM moves into, and a view of it the memory knows nothing of */
    assert(snx_host_memory_new(&next, size));
#ifdef _WIN32
    assert(!snx_ctr_memory_adopt(g.m, next.handle));
    assert(snx_ctr_read(s, 0x400000, got, sizeof got) == SNX_CTR_MEM_OK && !memcmp(got, bytes, sizeof got));
    snx_host_memory_free(&next);
    printf("memory adopt: refused on Windows, the RAM as it was\n");
#else
    unsigned checks = 0;
    /* an object that does not map: refused, every view still the old object's */
    int pipe_ends[2];
    assert(!pipe(pipe_ends));
    errno = 0;
    assert(!snx_ctr_memory_adopt(g.m, pipe_ends[0]) && errno);
    close(pipe_ends[0]);
    close(pipe_ends[1]);
    fcram[0x40] = 0x77;
    assert(snx_ctr_read(s, 0x400000, got, sizeof got) == SNX_CTR_MEM_OK && !memcmp(got, bytes, sizeof got));
    assert(base[0x200040] == 0x77);
    ++checks;

    unsigned char* outside = next.view;
    assert(snx_ctr_memory_adopt(g.m, next.handle));
    uint64_t moved;
    assert(snx_ctr_memory_ram(g.m, &moved) == ram && moved == size && region(FCRAM) == fcram); /* same addresses */
#ifdef __linux__
    /* each view at the protection it had: the watched page's without writes, the alias read-only */
    assert(!strcmp(protection(base + 0x100000), "r--s") && !strcmp(protection(base + 0x200000), "r--s"));
    assert(!strcmp(protection(base + 0x400000), "rw-s") && !strcmp(protection(base + 0x402000), "rw-s"));
    assert(!strcmp(protection(base + 0x300000), "---p")); /* never mapped: still the reservation */
    ++checks;
#endif
    /* what the RAM held, through the API, the flat space and the new object alike */
    assert(snx_ctr_read(s, 0x400000, got, sizeof got) == SNX_CTR_MEM_OK && !memcmp(got, bytes, sizeof got));
    assert(!memcmp(outside + fcram_at + 0x1000, bytes, sizeof bytes) && !memcmp(base + 0x400000, bytes, sizeof bytes));
    assert(!memcmp(outside + vram_at + 9, "vram", 4) && !memcmp(base + 0x1f000009, "vram", 4));
    assert(!memcmp(outside + fcram_at + 5, "page", 4) && !memcmp(base + 0x200005, "page", 4));
    assert(outside[fcram_at + 0x40] == 0x77);
    checks += 3;
    /* every view the new object's: a write to it shows in each, and one in each shows in it */
    volatile unsigned char* v = outside;
    volatile unsigned char* flat = base;
    v[fcram_at + 0x3fff] = 0xa1; /* the run's last page */
    v[vram_at + 100] = 0xa2;
    v[fcram_at + 0x10] = 0xa3;
    assert(flat[0x402fff] == 0xa1 && flat[0x1f000064] == 0xa2 && flat[0x100010] == 0xa3 && flat[0x200010] == 0xa3);
    assert(fcram[0x10] == 0xa3 && vram[100] == 0xa2);
    flat[0x401000] = 0xb1;
    assert(v[fcram_at + 0x2000] == 0xb1);
    assert(snx_ctr_phys_write(g.m, VRAM + 200, "\xb2", 1) == SNX_CTR_MEM_OK && v[vram_at + 200] == 0xb2);
    checks += 2;
    /* the watched page's write traps in its flat view, and lands once resolved */
    ctr_host_faults(base, NULL, resolve, s);
    flat[0x100020] = 0xc1;
    assert(g.watched == 1 && v[fcram_at + 0x20] == 0xc1 && snx_ctr_track_level(g.m, FCRAM) == SNX_CTR_TRACK_NONE);
    ctr_host_faults(NULL, NULL, NULL, NULL);
    ++checks;
    /* a view the host cannot protect says so, at the 1st, 2nd and 4th failure: the VRAM page's view taken away behind
       the memory's back, which mprotect refuses as the dma-buf's views refuse once the process has too many mappings */
    munmap(base + 0x1f000000, 4096);
    errno = 0;
    assert(mprotect(base + 0x1f000000, 4096, PROT_READ) && errno);
    int refusal = errno;
    FILE* said = tmpfile();
    int standard_error = dup(2);
    assert(said && standard_error >= 0);
    fflush(stderr);
    assert(dup2(fileno(said), 2) == 2);
    for (unsigned i = 0; i < 4; ++i)
        snx_ctr_track_set(g.m, VRAM, 4096, i % 2 ? SNX_CTR_TRACK_NONE : SNX_CTR_TRACK_WATCH);
    assert(dup2(standard_error, 2) == 2);
    close(standard_error);
    char text[512] = {0}, want[512], line[160];
    rewind(said);
    assert(fread(text, 1, sizeof text - 1, said) > 0);
    fclose(said);
    want[0] = 0;
    for (unsigned n = 1; n <= 4; n *= 2) {
        snprintf(line, sizeof line,
                 "3ds memory: write tracking cannot protect the view at guest 0x1f000000 (errno %d): its traps are "
                 "wrong from here; failures so far: %u\n",
                 refusal, n);
        strcat(want, line);
    }
    if (strcmp(text, want)) {
        fprintf(stderr, "said:\n%swanted:\n%s", text, want);
        return 1;
    }
    ++checks;
    munmap(outside, (size_t)size); /* the memory holds the object from here, and closes it */
    printf("memory adopt: %u checks\n", checks);
#endif
    snx_ctr_space_free(s);
    snx_ctr_memory_free(g.m);
    return 0;
}
