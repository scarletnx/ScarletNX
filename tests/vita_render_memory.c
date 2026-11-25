/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Guest memory as the Vita's renderer (gpu/render.c) has the GPU read and write it, on a device that imports it
   (VK_EXT_external_memory_host) and on the same device without the extension (vk_ext_external_memory_host=off), where
   the GPU works on copies: each check holds on both, the import being what a copy stands for. No game and no shader
   program is needed: the GPU's work is sceGxm's transfers, scenes that load a color surface and store it, and decoded
   pictures. A name runs that check alone, and "import" or "copy" before it that device alone. Skips (77) without a
   Vulkan device */
#include "vita/gpu/gxm.h"
#include "vita/gpu/render.h"
#include "vita/gpu/vk.h"
#include "vita/runtime/process.h"
#include "vita/runtime/watch.h"
#include "common/platform/vm.h"
#include "nx/runtime/ini.h"
#include "snx_settings.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <signal.h>
#include <unistd.h>
#endif

#define BLOCK 0x40000u /* a memory block of the checks, as the console gives CDRAM's */
#define PAGE  0x1000u

static struct {
    const char* check;
    const char* device;
    unsigned failures;
} g_test;
#define CHECK(c, ...)                                                                                                  \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            ++g_test.failures;                                                                                         \
            fprintf(stderr, "FAIL %s (%s) line %d: ", g_test.check, g_test.device, __LINE__);                          \
            fprintf(stderr, __VA_ARGS__);                                                                              \
            fputc('\n', stderr);                                                                                       \
        }                                                                                                              \
    } while (0)

#ifndef _WIN32
static void on_fault(int signal, siginfo_t* info, void* context) {
    (void)context;
    if (vita_watch_fault((uintptr_t)info->si_addr)) return;
    static const char said[] = "a fault the watch does not know\n";
    if (write(2, said, sizeof said - 1) < 0) _exit(3);
    _exit(signal);
}
static void on_alarm(int signal) {
    (void)signal;
    static const char said[] = "a check hangs\n";
    if (write(2, said, sizeof said - 1) < 0) _exit(3);
    _exit(2);
}
#endif

/* a process with its memory, its sceGxm state and the renderer, and nothing else of a game's */
static VitaProcess* process_make(void) {
    VitaProcess* p = calloc(1, sizeof *p);
    if (!p) return NULL;
    p->user = (VitaRegion){VITA_USER_BASE, VITA_USER_END, VITA_USER_BASE};
    p->cdram = (VitaRegion){VITA_CDRAM_BASE, VITA_CDRAM_END, VITA_CDRAM_BASE};
    p->mem = plat_mem_reserve(VITA_SPACE + 0x10000, 0);
    if (plat_mem_page() > PAGE) p->page_refs = calloc(VITA_SPACE / plat_mem_page(), sizeof *p->page_refs);
    p->gxm = calloc(1, sizeof *p->gxm);
    if (!p->mem || !p->gxm || (plat_mem_page() > PAGE && !p->page_refs) || !vita_watch_init(p) || !vita_render_init(p))
        return NULL;
    return p;
}
static void process_free(VitaProcess* p) {
    vita_render_free(p);
    vita_watch_free();
    plat_mem_release(p->mem, VITA_SPACE + 0x10000);
    free(p->page_refs);
    free(p->gxm);
    free(p);
}
static uint32_t block_make(VitaProcess* p, uint32_t size, bool mapped) {
    uint32_t at = vita_alloc(p, &p->cdram, size, BLOCK);
    if (at && mapped) vita_render_map(p, at, size);
    return at;
}
/* the byte a pattern has at i: none repeats within a page, and two patterns differ everywhere */
static unsigned char pattern(unsigned which, uint32_t i) { return (unsigned char)(which * 0x35u + i * 7u + (i >> 8)); }
static void fill(VitaProcess* p, uint32_t at, uint32_t size, unsigned which) {
    unsigned char* to = vita_ptr(p, at);
    for (uint32_t i = 0; i < size; ++i) to[i] = pattern(which, i);
}
/* the bytes of [at, at + size) that are not the pattern's from its byte first on */
static uint32_t wrong_from(VitaProcess* p, uint32_t at, uint32_t size, unsigned which, uint32_t first) {
    const unsigned char* got = vita_ptr(p, at);
    uint32_t n = 0;
    for (uint32_t i = 0; i < size; ++i) n += got[i] != pattern(which, first + i);
    return n;
}
static uint32_t wrong(VitaProcess* p, uint32_t at, uint32_t size, unsigned which) {
    return wrong_from(p, at, size, which, 0);
}
/* size bytes copied by the GPU, as a transfer of one row */
static bool copy(VitaProcess* p, uint32_t from, uint32_t to, uint32_t size) {
    return vita_render_transfer_copy(p, size / 4, 1, 4, from, 0, 0, (int32_t)size, to, 0, 0, (int32_t)size);
}
/* sceGxmFinish: what is recorded submitted and done */
static void finish(VitaProcess* p) { vita_render_wait(p, vita_render_finish(p)); }

/* what the CPU wrote is what the GPU reads, what the GPU wrote what the CPU reads, again after each changes */
static void check_round_trip(VitaProcess* p) {
    uint32_t a = block_make(p, BLOCK, true), b = block_make(p, BLOCK, true);
    for (unsigned n = 1; n < 4; ++n) {
        fill(p, a + n * 0x100, 0x3000, n);
        CHECK(copy(p, a + n * 0x100, b + 0x2000, 0x3000), "the transfer is refused");
        finish(p);
        CHECK(!wrong(p, b + 0x2000, 0x3000, n), "pass %u: %u bytes of the GPU's copy are wrong", n,
              (unsigned)wrong(p, b + 0x2000, 0x3000, n));
        /* the GPU's copy read by the GPU, the CPU's write over a part of it */
        fill(p, b + 0x2800, 0x400, n + 8);
        CHECK(copy(p, b + 0x2000, a + 0x10000, 0x3000), "the transfer back is refused");
        finish(p);
        uint32_t bad = wrong(p, a + 0x10000, 0x800, n) + wrong(p, a + 0x10800, 0x400, n + 8) +
                       wrong_from(p, a + 0x10c00, 0x2400, n, 0xc00);
        CHECK(!bad, "pass %u: %u bytes of the copy back are wrong", n, (unsigned)bad);
    }
}
/* a chain of copies in one recording, each reading what the one before it wrote */
static void check_chain(VitaProcess* p) {
    uint32_t a = block_make(p, BLOCK, true), b = block_make(p, BLOCK, true);
    fill(p, a, 0x2000, 1);
    CHECK(copy(p, a, b, 0x2000) && copy(p, b, a + 0x8000, 0x2000) && copy(p, a + 0x8000, b + 0x9100, 0x2000),
          "a transfer is refused");
    finish(p);
    CHECK(!wrong(p, b, 0x2000, 1) && !wrong(p, a + 0x8000, 0x2000, 1) && !wrong(p, b + 0x9100, 0x2000, 1),
          "the chain's bytes are wrong: %u, %u, %u", (unsigned)wrong(p, b, 0x2000, 1),
          (unsigned)wrong(p, a + 0x8000, 0x2000, 1), (unsigned)wrong(p, b + 0x9100, 0x2000, 1));
}
/* two submissions: the first's writes are in guest memory once it is done, the second's once the second is */
static void check_serials(VitaProcess* p) {
    uint32_t size = 4 * BLOCK, a = block_make(p, size, true), b = block_make(p, size, true);
    fill(p, a, size, 1);
    CHECK(copy(p, a, b, 0x1000), "the transfer is refused");
    uint64_t first = vita_render_finish(p);
    bool ok = true;
    for (unsigned i = 0; i < 64 && ok; ++i) ok = copy(p, a + 0x1000, b + 0x1000, size - 0x1000);
    vita_render_wait(p, first); /* the second's work recorded and not submitted */
    CHECK(!wrong(p, b, 0x1000, 1), "%u bytes of the first submission are wrong", (unsigned)wrong(p, b, 0x1000, 1));
    uint64_t second = vita_render_finish(p);
    CHECK(ok && second > first, "the second submission is not made");
    vita_render_wait(p, second);
    CHECK(!wrong(p, b, size, 1), "%u bytes of both submissions are wrong", (unsigned)wrong(p, b, size, 1));
}
/* a block unmapped with a transfer into it recorded: its bytes are in guest memory, and the CPU's after it are the
   GPU's once the block is mapped again */
static void check_unmap(VitaProcess* p) {
    uint32_t a = block_make(p, BLOCK, true), b = block_make(p, BLOCK, true);
    fill(p, a, 0x2000, 1);
    CHECK(copy(p, a, b + 0x800, 0x2000), "the transfer is refused");
    vita_render_unmap(p, b);
    CHECK(!wrong(p, b + 0x800, 0x2000, 1), "%u bytes are not in guest memory once the block is unmapped",
          (unsigned)wrong(p, b + 0x800, 0x2000, 1));
    fill(p, b + 0x800, 0x2000, 2);
    vita_render_map(p, b, BLOCK);
    CHECK(copy(p, b + 0x800, a + 0x8000, 0x2000), "the transfer back is refused");
    finish(p);
    CHECK(!wrong(p, a + 0x8000, 0x2000, 2), "%u bytes written while the block was unmapped are old",
          (unsigned)wrong(p, a + 0x8000, 0x2000, 2));
}
/* the CPU writes a page whose other bytes a transfer still in flight writes: the GPU reads them, then and later */
static void check_beside(VitaProcess* p) {
    uint32_t a = block_make(p, BLOCK, true), b = block_make(p, BLOCK, true);
    fill(p, a, 0x1000, 1);
    fill(p, b + 0x4000, 0x1000, 9);
    CHECK(copy(p, a, b + 0x4000, 0x800), "the transfer is refused");
    fill(p, b + 0x4800, 0x400, 2); /* beside the GPU's bytes, on their page */
    CHECK(copy(p, b + 0x4800, b + 0x10000, 0x400), "the transfer of the page's other bytes is refused");
    finish(p);
    CHECK(!wrong(p, b + 0x10000, 0x400, 2), "%u bytes written beside a GPU write in flight are old on the GPU",
          (unsigned)wrong(p, b + 0x10000, 0x400, 2));
    CHECK(!wrong(p, b + 0x4000, 0x800, 1) && !wrong(p, b + 0x4800, 0x400, 2),
          "the page's bytes are wrong once the GPU is done");
}
/* the same once the GPU is done and nothing looked yet, so its writes are not in guest memory: a rectangle whose rows
   cross pages, the CPU's bytes between them, the GPU reading both */
static void check_beside_done(VitaProcess* p) {
    uint32_t a = block_make(p, BLOCK, true), b = block_make(p, BLOCK, true);
    fill(p, a, 0x2000, 1);
    fill(p, b, 0x8000, 4);
    CHECK(vita_render_transfer_copy(p, 25, 80, 4, a, 0, 0, 100, b, 4, 1, 252), "the rectangle is refused");
    vita_render_finish(p);
    vkQueueWaitIdle(vita_vk()->queue);
    for (uint32_t y = 0; y < 80; ++y) fill(p, b + 252 + y * 252 + 116, 152, 5);
    CHECK(copy(p, b, b + 0x20000, 0x8000), "the transfer of both is refused");
    finish(p);
    uint32_t missing = 0, lost = 0;
    for (uint32_t y = 0; y < 80; ++y) {
        missing += wrong_from(p, b + 0x20000 + 252 + y * 252 + 16, 100, 1, y * 100);
        lost += wrong(p, b + 0x20000 + 252 + y * 252 + 116, 152, 5);
    }
    CHECK(!missing, "%u bytes of the rectangle are old on the GPU", (unsigned)missing);
    CHECK(!lost, "%u bytes between its rows are old on the GPU", (unsigned)lost);
}
/* the same, where the bytes beside are looked at once the GPU is done, and the page was protected meanwhile */
static void check_beside_later(VitaProcess* p) {
    uint32_t a = block_make(p, BLOCK, true), b = block_make(p, BLOCK, true);
    fill(p, a, 0x1000, 1);
    CHECK(copy(p, a, b + 0x4000, 0x800), "the transfer is refused");
    fill(p, b + 0x4800, 0x400, 2);
    CHECK(copy(p, a, b + 0x8000, 0x100), "another transfer is refused"); /* the copy is brought up to date here */
    vita_watch_protect(b + 0x4000, 0x1000); /* as a texture's or a surface's copy of the page does */
    finish(p);
    CHECK(copy(p, b + 0x4800, b + 0x10000, 0x400), "the transfer of the page's other bytes is refused");
    finish(p);
    CHECK(!wrong(p, b + 0x10000, 0x400, 2), "%u bytes written beside a GPU write are old on the GPU for good",
          (unsigned)wrong(p, b + 0x10000, 0x400, 2));
}
/* writes to a page left writable (a GPU write of its other bytes was in flight) while no other page is written */
static void check_beside_quiet(VitaProcess* p) {
    uint32_t a = block_make(p, BLOCK, true), b = block_make(p, BLOCK, true);
    fill(p, a, 0x1000, 1);
    CHECK(copy(p, a, b + 0x4000, 0x800), "the transfer is refused");
    fill(p, b + 0x4800, 0x100, 2);
    vita_render_show(p, b + 0x20000, 32, 32, 8); /* the block brought up to date, with no write of the GPU's after */
    finish(p);
    fill(p, b + 0x4a00, 0x100, 3); /* the page is writable already: no fault says it was written */
    CHECK(copy(p, b + 0x4a00, a + 0x10000, 0x100), "the transfer is refused");
    finish(p);
    CHECK(!wrong(p, a + 0x10000, 0x100, 3), "%u bytes of a page left writable are old on the GPU",
          (unsigned)wrong(p, a + 0x10000, 0x100, 3));
}
/* a rectangle the GPU writes into a wider picture: the bytes between its rows are the CPU's, which writes them while
   the transfer is in flight */
static void check_gaps(VitaProcess* p) {
    uint32_t a = block_make(p, BLOCK, true), b = block_make(p, BLOCK, true);
    fill(p, a, 0x1000, 1);
    fill(p, b, 0x4000, 4);
    CHECK(copy(p, a + 0x800, b + 0x100, 16), "the transfer is refused"); /* up to the rectangle's first row */
    CHECK(vita_render_transfer_copy(p, 16, 16, 4, a, 0, 0, 64, b, 4, 1, 256), "the rectangle is refused");
    for (uint32_t y = 0; y < 16; ++y) fill(p, b + 0x100 + y * 256 + 16 + 64, 128, 5); /* right of each row */
    finish(p);
    CHECK(!wrong_from(p, b + 0x100, 16, 1, 0x800), "the bytes before the rectangle are not in guest memory");
    uint32_t lost = 0, missing = 0;
    for (uint32_t y = 0; y < 16; ++y) {
        lost += wrong(p, b + 0x100 + y * 256 + 16 + 64, 128, 5);
        missing += wrong_from(p, b + 0x100 + y * 256 + 16, 64, 1, y * 64);
    }
    CHECK(!missing, "%u bytes of the rectangle are not in guest memory", (unsigned)missing);
    CHECK(!lost, "%u bytes the CPU wrote between the rectangle's rows are lost", (unsigned)lost);
}
/* more writes in one submission than a small table of them holds, the last one into another block */
static void check_many(VitaProcess* p) {
    uint32_t a = block_make(p, BLOCK, true), b = block_make(p, BLOCK, true), c = block_make(p, BLOCK, true);
    fill(p, a, 0x1000, 1);
    fill(p, b, BLOCK, 4);
    bool ok = true;
    for (uint32_t i = 0; i < 600 && ok; ++i) ok = copy(p, a + (i & 63) * 16, b + i * 64, 16);
    CHECK(ok && copy(p, a, c + 0x100, 0x200), "a transfer is refused");
    for (uint32_t i = 0; i < 600; ++i)
        fill(p, b + i * 64 + 32, 16, 5); /* between the writes, while they are in flight */
    finish(p);
    uint32_t missing = 0, lost = 0;
    for (uint32_t i = 0; i < 600; ++i) {
        missing += wrong_from(p, b + i * 64, 16, 1, (i & 63) * 16);
        lost += wrong(p, b + i * 64 + 32, 16, 5);
    }
    CHECK(!missing, "%u bytes the GPU wrote are not in guest memory", (unsigned)missing);
    CHECK(!lost, "%u bytes the CPU wrote between the GPU's writes are lost", (unsigned)lost);
    CHECK(!wrong(p, c + 0x100, 0x200, 1), "the write into another block is not in guest memory");
}
/* a few pages written, more than the watch remembers one by one, and a range the system writes (a file read): the GPU
   reads each */
static void check_pages(VitaProcess* p) {
    static const unsigned counts[] = {3, 2000, 1, 700, 40};
    uint32_t size = 32 * BLOCK, a = block_make(p, size, true), b = block_make(p, size, true);
    fill(p, a, size, 1);
    for (unsigned n = 0; n < sizeof counts / sizeof *counts; ++n) {
        for (uint32_t i = 0; i < counts[n]; ++i) fill(p, a + i * 7 % 2048 * 0x1000 + n * 16, 16, 2 + n);
        if (n == 3) {
            vita_watch_release(a + 0x100000, 0x200000);
            fill(p, a + 0x100000, 0x200000, 9);
        }
        CHECK(copy(p, a, b, size), "pass %u: the transfer is refused", n);
        finish(p);
        CHECK(!memcmp(vita_ptr(p, a), vita_ptr(p, b), size), "pass %u: the GPU's copy differs", n);
    }
}
/* two ranges of one block mapped apart, and a transfer across both: what the GPU wrote through one is read */
static void check_straddle(VitaProcess* p) {
    uint32_t a = block_make(p, BLOCK, true), b = block_make(p, BLOCK, false);
    vita_render_map(p, b, 0x8000);
    vita_render_map(p, b + 0x8000, 0x8000);
    fill(p, a, 0x2000, 1);
    fill(p, b, 0x10000, 4);
    CHECK(copy(p, b + 0x7000, b + 0x20000, 0x2000), "the transfer across the mappings is refused");
    finish(p);
    CHECK(copy(p, a, b + 0x7000, 0x1000), "the transfer into one mapping is refused");
    CHECK(copy(p, a + 0x1000, b + 0x8000, 0x1000), "the transfer into the other mapping is refused");
    CHECK(copy(p, b + 0x7000, b + 0x10000, 0x2000), "the transfer across the mappings is refused");
    finish(p);
    CHECK(!wrong(p, b + 0x10000, 0x2000, 1), "%u bytes written through two mappings are old through the block",
          (unsigned)wrong(p, b + 0x10000, 0x2000, 1));
    CHECK(copy(p, b + 0x7000, b + 0x14000, 0x2000), "the transfer across the mappings is refused");
    finish(p);
    CHECK(!wrong(p, b + 0x14000, 0x2000, 1), "%u bytes written through two mappings are old through the block for good",
          (unsigned)wrong(p, b + 0x14000, 0x2000, 1));
}
/* memory that was there before its writes were watched (the module's segments), mapped a second time, and once more
   while a part of it is mapped. Another block is mapped between, so the memory a copy gets is not its old one */
static void check_again(VitaProcess* p) {
    vita_watch_free();
    uint32_t a = vita_alloc(p, &p->user, BLOCK, BLOCK);
    fill(p, a, 0x8000, 1);
    CHECK(vita_watch_init(p), "the watch is refused");
    uint32_t b = block_make(p, BLOCK, true), between = block_make(p, BLOCK, false);
    fill(p, between, BLOCK, 7);
    for (unsigned n = 0; n < 3; ++n) {
        vita_render_map(p, between, BLOCK);
        CHECK(copy(p, between, between + 0x10000, 0x1000), "the transfer between is refused");
        finish(p);
        vita_render_unmap(p, between);
        vita_render_map(p, a, BLOCK);
        CHECK(copy(p, a, b + n * 0x8000, 0x8000), "transfer %u is refused", n);
        finish(p);
        CHECK(!wrong(p, b + n * 0x8000, 0x8000, 1), "mapping %u: %u bytes the GPU reads are wrong", n,
              (unsigned)wrong(p, b + n * 0x8000, 0x8000, 1));
        vita_render_unmap(p, a);
    }
    vita_render_map(p, between, BLOCK);
    vita_render_unmap(p, between);
    vita_render_map(p, a, 0x4000);
    vita_render_map(p, a, BLOCK);
    CHECK(copy(p, a + 0x2000, b + 0x18000, 0x4000), "the transfer across the mappings is refused");
    finish(p);
    CHECK(!wrong_from(p, b + 0x18000, 0x4000, 1, 0x2000), "a second mapping: %u bytes the GPU reads are wrong",
          (unsigned)wrong_from(p, b + 0x18000, 0x4000, 1, 0x2000));
}
/* a block given back while a transfer into it is in flight, and another block made in its place: nothing hangs, and
   the new block holds what the CPU writes there */
static void check_freed(VitaProcess* p) {
    uint32_t a = block_make(p, BLOCK, true), b = block_make(p, BLOCK, false);
    fill(p, a, 0x1000, 1);
    CHECK(copy(p, a, b + 0x1000, 0x1000), "the transfer is refused");
    vita_free(p, b);
    finish(p);
    CHECK(block_make(p, BLOCK, false) == b, "the block is made elsewhere");
    CHECK(copy(p, a, b + 0x2000, 0x1000), "the transfer into the new block is refused");
    vita_free(p, b);
    CHECK(block_make(p, BLOCK, false) == b, "the block is made elsewhere");
    fill(p, b, 0x8000, 2);
    finish(p);
    CHECK(!wrong(p, b, 0x8000, 2), "%u bytes of the new block are the old one's", (unsigned)wrong(p, b, 0x8000, 2));
    CHECK(copy(p, b, a + 0x8000, 0x4000), "the transfer from the new block is refused");
    finish(p);
    CHECK(!wrong(p, a + 0x8000, 0x4000, 2), "%u bytes of the new block are old on the GPU",
          (unsigned)wrong(p, a + 0x8000, 0x4000, 2));
}
/* what the CPU writes after the work is recorded and before it is submitted: the GPU reads it, as it reads memory
   when it runs */
static void check_submit(VitaProcess* p) {
    uint32_t a = block_make(p, BLOCK, true), b = block_make(p, BLOCK, true);
    fill(p, a, 0x1000, 1);
    CHECK(copy(p, a, b, 0x1000), "the transfer is refused");
    fill(p, a, 0x1000, 2);
    finish(p);
    CHECK(!wrong(p, b, 0x1000, 2), "%u bytes written before the submission are old", (unsigned)wrong(p, b, 0x1000, 2));
}

/* a scene on a color surface of width x height, its rows stride pixels, nothing drawn: loaded and stored */
static uint64_t scene(VitaProcess* p, uint32_t at, uint32_t width, uint32_t height, uint32_t stride, uint32_t type) {
    GxmContext* c = &p->gxm->context;
    p->gxm->targets[0] = (GxmTarget){.width = width, .height = height};
    c->has_color = true, c->target = 0;
    c->color = (GxmColorSurface){.width = width, .height = height, .stride = stride, .data = at, .type = type};
    vita_render_begin_scene(p, c);
    return vita_render_end_scene(p, c);
}
/* a color surface whose rows are wider than what is drawn and whose last page it shares: the padding and the page's
   other bytes are the CPU's, while the scene is in flight and after */
static void check_surface(VitaProcess* p) {
    uint32_t a = block_make(p, BLOCK, true), b = block_make(p, BLOCK, true), size = 64 * 4 * 24;
    fill(p, b, size, 1);
    scene(p, b, 48, 24, 64, GXM_COLOR_SURFACE_LINEAR);
    for (uint32_t y = 0; y < 24; ++y) fill(p, b + y * 256 + 192, 64, 2);
    fill(p, b + size, 0x400, 3);
    scene(p, b, 48, 24, 64, GXM_COLOR_SURFACE_LINEAR);
    CHECK(copy(p, b + size, a, 0x400), "the transfer is refused");
    finish(p);
    uint32_t drawn = 0, padding = 0;
    for (uint32_t y = 0; y < 24; ++y) {
        drawn += wrong_from(p, b + y * 256, 192, 1, y * 256);
        padding += wrong(p, b + y * 256 + 192, 64, 2);
    }
    CHECK(!drawn, "%u bytes of the surface are wrong", (unsigned)drawn);
    CHECK(!padding, "%u bytes of the surface's padding are not what the CPU wrote", (unsigned)padding);
    CHECK(!wrong(p, a, 0x400, 3), "%u bytes beside the surface are old on the GPU", (unsigned)wrong(p, a, 0x400, 3));
    /* the CPU draws into the surface once the GPU is done, and the next scene starts from it */
    fill(p, b + 256 * 4, 192, 5);
    scene(p, b, 48, 24, 64, GXM_COLOR_SURFACE_LINEAR);
    CHECK(copy(p, b + 256 * 4, a + 0x1000, 192), "the transfer is refused");
    finish(p);
    CHECK(!wrong(p, b + 256 * 4, 192, 5) && !wrong(p, a + 0x1000, 192, 5), "the CPU's row of the surface is lost");
}
/* a tiled color surface whose last tiles its height does not fill: the scene's store reaches guest memory whole, the
   tile past the bytes its rows would take with it */
static void check_tiled(VitaProcess* p) {
    uint32_t b = block_make(p, BLOCK, true), rows = 64 * 4 * 48, size = 2 * 2 * 4096;
    fill(p, b, size, 1);
    uint64_t serial = scene(p, b, 64, 48, 64, GXM_COLOR_SURFACE_TILED);
    fill(p, b + rows, size - rows, 2); /* once the scene loaded it: its store comes after */
    vita_render_wait(p, serial);
    CHECK(!wrong_from(p, b + rows, 16 * 128, 1, rows), "%u bytes of the last tile are not what the scene stored",
          (unsigned)wrong_from(p, b + rows, 16 * 128, 1, rows));
}
/* a depth surface stored at the scene's end: every sample the background's, in its rows alone */
static void check_depth(VitaProcess* p) {
    if (!vita_vk()->depth16_blit) return;
    uint32_t a = block_make(p, BLOCK, true), b = block_make(p, BLOCK, true);
    GxmContext* c = &p->gxm->context;
    c->has_depth = true;
    c->depth = (GxmDepthSurface){.control = GXM_DEPTH_D16 | GXM_DEPTH_FORCE_STORE | 64u << GXM_DEPTH_STRIDE_SHIFT,
                                 .depth = b + 0x800,
                                 .background_depth = 1.0f};
    fill(p, b, 0x4000, 1);
    scene(p, a, 48, 24, 48, GXM_COLOR_SURFACE_LINEAR);
    for (uint32_t y = 0; y < 24; ++y) fill(p, b + 0x800 + y * 128 + 96, 32, 2);
    finish(p);
    uint32_t stored = 0, padding = 0;
    for (uint32_t y = 0; y < 24; ++y) {
        for (uint32_t x = 0; x < 48; ++x) stored += vita_read16(p, b + 0x800 + y * 128 + x * 2) != 0xffff;
        padding += wrong(p, b + 0x800 + y * 128 + 96, 32, 2);
    }
    CHECK(!stored, "%u samples of the depth surface are not the background's", (unsigned)stored);
    CHECK(!padding, "%u bytes of the depth's padding are not what the CPU wrote", (unsigned)padding);
    CHECK(!wrong(p, b, 0x800, 1), "the bytes before the depth surface are wrong");
}
/* a decoded picture, whole and in its place */
static void check_picture(VitaProcess* p) {
    uint32_t b = block_make(p, BLOCK, false), size = 32 * 16 * 3 / 2;
    unsigned char luma[32 * 16], u[16 * 8], v[16 * 8];
    for (unsigned i = 0; i < sizeof luma; ++i) luma[i] = pattern(1, i);
    for (unsigned i = 0; i < sizeof u; ++i) u[i] = pattern(2, i), v[i] = pattern(3, i);
    fill(p, b, 0x2000, 4);
    CHECK(vita_render_picture(p, b + 0x400, 32, 0x10, 32, 16, (const uint8_t* const[3]){luma, u, v},
                              (const int[3]){32, 16, 16}),
          "the picture is refused");
    finish(p);
    CHECK(!wrong(p, b + 0x400, sizeof luma, 1) && !wrong(p, b + 0x400 + sizeof luma, sizeof u, 2) &&
              !wrong(p, b + 0x400 + sizeof luma + sizeof u, sizeof v, 3),
          "the picture's planes are wrong");
    CHECK(!wrong(p, b, 0x400, 4) && !wrong_from(p, b + 0x400 + size, 0x1000, 4, 0x400 + size),
          "the bytes around the picture are wrong");
}

static const struct {
    const char* name;
    void (*run)(VitaProcess* p);
    bool copy_only; /* what the import leaves to its driver */
} checks[] = {
    {"round_trip", check_round_trip, false},
    {"chain", check_chain, false},
    {"serials", check_serials, false},
    {"unmap", check_unmap, false},
    {"beside", check_beside, false},
    {"beside_done", check_beside_done, false},
    {"beside_later", check_beside_later, false},
    {"beside_quiet", check_beside_quiet, false},
    {"gaps", check_gaps, false},
    {"many", check_many, false},
    {"pages", check_pages, false},
    {"straddle", check_straddle, false},
    {"again", check_again, false},
    {"submit", check_submit, false},
    {"surface", check_surface, false},
    {"tiled", check_tiled, true},
    {"depth", check_depth, false},
    {"picture", check_picture, false},
    {"freed", check_freed, true},
};

int main(int argc, char** argv) {
#ifndef _WIN32
    struct sigaction action = {.sa_sigaction = on_fault, .sa_flags = SA_SIGINFO | SA_NODEFER};
    sigaction(SIGSEGV, &action, NULL);
    sigaction(SIGBUS, &action, NULL); /* macOS's, for a page whose protection refuses the write */
    signal(SIGALRM, on_alarm);
#endif
    const char* devices[2] = {"import", "copy"};
    const char* only_device =
        argc > 1 && (!strcmp(argv[1], devices[0]) || !strcmp(argv[1], devices[1])) ? argv[1] : NULL;
    const char* only = argc > (only_device ? 2 : 1) ? argv[only_device ? 2 : 1] : NULL;
    unsigned ran = 0;
    for (unsigned d = 0; d < 2; ++d) {
        if (only_device && strcmp(only_device, devices[d])) continue;
        char why[512];
        ini_read(ini_find(g_vk_extensions_ini, "vk_ext_external_memory_host"), d ? "off" : "auto");
        if (!vita_vk_init(NULL, why, sizeof why)) {
            printf("SKIP no Vulkan device (%s)\n", why);
            return 77;
        }
        g_test.device = devices[d];
        bool imports = vita_vk()->host_import;
        if (d && imports) {
            fprintf(stderr, "FAIL the device imports guest memory with vk_ext_external_memory_host=off\n");
            return 1;
        }
        for (unsigned i = 0; i < sizeof checks / sizeof *checks && (d || imports); ++i) {
            if ((only && strcmp(only, checks[i].name)) || (!d && checks[i].copy_only)) continue;
            g_test.check = checks[i].name;
            unsigned before = g_test.failures;
#ifndef _WIN32
            alarm(60);
#endif
            VitaProcess* p = process_make();
            if (!p) {
                fprintf(stderr, "FAIL %s (%s): the renderer is not made\n", g_test.check, g_test.device);
                return 1;
            }
            checks[i].run(p);
            process_free(p);
            ++ran;
            printf("%s %s (%s)\n", g_test.failures == before ? "ok" : "FAIL", g_test.check, g_test.device);
            fflush(stdout);
        }
        if (!d && !imports) printf("the device does not import guest memory: the copies alone are checked\n");
        vita_vk_free();
    }
    if (!ran) {
        fprintf(stderr, "FAIL no check is named %s\n", only ? only : "");
        return 1;
    }
    printf("%s: %u checks, %u failures\n", g_test.failures ? "FAIL" : "PASS", ran, g_test.failures);
    return g_test.failures ? 1 : 0;
}
