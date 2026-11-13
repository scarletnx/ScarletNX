/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The registry of the guest's live texture objects (gpu/guest_textures.c): an address counts its objects and goes
   with the last of them, and each address stays in reach while the table grows and while others leave it. */
#include "nx/gpu/guest_textures.h"
#include "nx/runtime/platform/log.h"
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

void fatal(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    exit(1);
}

#define N 5000 /* past the table's first sizes: it grows three times */
static u64 at(u32 i) { return 0x400000000ull + (u64)i * 0x20000; }
static int by_gva(const void* a, const void* b) {
    const GpuTexture *x = a, *y = b;
    return x->gva < y->gva ? -1 : x->gva > y->gva;
}
/* the registry's addresses in v, sorted: how many */
static u32 sorted(GpuTexture* v) {
    const u32 n = gpu_textures(v, N);
    assert(n <= N);
    qsort(v, n, sizeof *v, by_gva);
    return n;
}

int main(void) {
    GpuTexture* v = calloc(N, sizeof *v);
    assert(v);

    /* nothing yet. An object with no address is not entered, and an address not there is not removed */
    assert(!gpu_textures_added() && !gpu_textures(v, N));
    gpu_texture_add(0, 100);
    gpu_texture_remove(at(1));
    assert(!gpu_textures_added() && !gpu_textures(v, N));

    /* three objects at one address: one entry that counts them, with the size of the largest */
    gpu_texture_add(at(0), 100);
    gpu_texture_add(at(0), 300);
    gpu_texture_add(at(0), 200);
    assert(gpu_textures(v, N) == 1 && v[0].gva == at(0) && v[0].refs == 3 && v[0].size == 300);
    gpu_texture_remove(at(0));
    gpu_texture_remove(at(0));
    assert(gpu_textures(v, N) == 1 && v[0].refs == 1);
    gpu_texture_remove(at(0));
    assert(!gpu_textures(v, N));
    gpu_texture_remove(at(0)); /* one end too many changes nothing */
    /* the address ended once, when its last object did, and is live no more: the list gives it one time */
    {
        u64 ended[4] = {0};
        assert(!gpu_texture_live(at(0)) && gpu_textures_ended(ended, 4) == 1 && ended[0] == at(0));
        assert(!gpu_textures_ended(ended, 4));
        /* an object made there again is live, and its end is listed again; one of two at an address ends nothing */
        gpu_texture_add(at(0), 1);
        gpu_texture_add(at(0), 1);
        assert(gpu_texture_live(at(0)) && !gpu_texture_live(at(1)));
        gpu_texture_remove(at(0));
        assert(gpu_texture_live(at(0)) && !gpu_textures_ended(ended, 4));
        gpu_texture_remove(at(0));
        assert(gpu_textures_ended(ended, 1) == 1 && ended[0] == at(0) && !gpu_textures_ended(ended, 4));
    }
    assert(!gpu_textures(v, N) && gpu_textures_added() == 5);

    /* many addresses: all there after the table grew */
    for (u32 i = 0; i < N; i++) gpu_texture_add(at(i), i + 1);
    assert(sorted(v) == N);
    for (u32 i = 0; i < N; i++) assert(v[i].gva == at(i) && v[i].size == i + 1 && v[i].refs == 1);
    /* a copy into less room: the count is the registry's, and no more entries than the room go out */
    {
        GpuTexture few[2] = {{0}};
        assert(gpu_textures(few, 1) == N && few[0].refs == 1 && !few[1].refs);
    }

    /* one in three leaves: the others stay, and each is still found where its search goes (a second object at its
       address joins its entry, and makes no new one) */
    for (u32 i = 0; i < N; i += 3) gpu_texture_remove(at(i));
    const u32 left = N - (N + 2) / 3;
    assert(sorted(v) == left);
    for (u32 i = 0, k = 0; i < N; i++)
        if (i % 3) {
            assert(v[k].gva == at(i) && v[k].size == i + 1 && v[k].refs == 1);
            k++;
        }
    for (u32 i = 0; i < N; i++)
        if (i % 3) gpu_texture_add(at(i), 0);
    assert(sorted(v) == left);
    for (u32 k = 0; k < left; k++) assert(v[k].refs == 2);

    /* the ones that left come back, and then all go, in another order */
    for (u32 i = 0; i < N; i += 3) gpu_texture_add(at(i), 7);
    assert(sorted(v) == N);
    for (u32 i = N; i--;) {
        gpu_texture_remove(at(i));
        if (i % 3) gpu_texture_remove(at(i));
    }
    assert(!gpu_textures(v, N));
    assert(gpu_textures_added() == 5 + N + left + (N + 2) / 3);
    /* the ends, a few at a time: N addresses ended, as many as the list holds, each given once */
    {
        u64 ended[100];
        u32 got = 0;
        for (u32 n; (n = gpu_textures_ended(ended, 100));) got += n;
        assert(got == (N < 1024 ? N : 1024) && !gpu_textures_ended(ended, 100));
    }

    free(v);
    puts("guest_textures: ok");
    return 0;
}
