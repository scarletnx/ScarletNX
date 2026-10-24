/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The game paused as a whole: nothing that changes guest memory or the kernel's state runs until it resumes. The guest
   threads stop first, so that nothing new is submitted (kernel_hold), then host1x's jobs and the GPU's queue run out
   and the GPU thread holds with the renderer idle, then the audio clock stops, and guest time last. A resume goes the
   other way. The frontend's thread does both, and skips its own guest work (hid, vsync) while paused (main.c) */
#include "nx/runtime/pause.h"
#include "nx/gpu/gpu.h"
#include "nx/gpu/video/host1x.h"
#include "nx/hle/kernel/kernel.h"
#include "nx/hle/services/services.h"
#include "nx/memory/mem.h"
#include "nx/memory/own.h"
#include "nx/runtime/clock.h"
#include "nx/runtime/options.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"
#include "nx/runtime/platform/unattended.h"
#include <stdlib.h>
#include <string.h>

/* how long each part may take to stop before the pause gives up */
#define PAUSE_MS 2000

/* depth: the pauses in force, read by any thread. player: the player's own is one of them. The unattended pause in
   force: its index in the setting (row, the next one when none is), its end in unattended_ms, and what it saw at its
   start */
static struct {
    int depth, player;
    u64 since;
    int row, in_row;
    u64 row_end, hash, bytes, guest_at;
} g_pause;

int game_pause(void) {
    u64 t0;
    if (g_pause.depth) {
        __atomic_store_n(&g_pause.depth, g_pause.depth + 1, __ATOMIC_RELEASE);
        return 0;
    }
    t0 = plat_ns();
    if (kernel_hold(PAUSE_MS)) return -1;
    if (host1x_drain(PAUSE_MS) || gpu_hold(PAUSE_MS)) {
        kernel_release();
        return -1;
    }
    audio_hold();
    clock_pause();
    g_pause.since = plat_ns();
    __atomic_store_n(&g_pause.depth, 1, __ATOMIC_RELEASE);
    LOG("pause: the game is paused at guest time %.3f s (stopping took %.1f ms)\n", (double)guest_ns() / 1e9,
        (double)(g_pause.since - t0) / 1e6);
    return 0;
}
void game_resume(void) {
    if (!g_pause.depth) return;
    __atomic_store_n(&g_pause.depth, g_pause.depth - 1, __ATOMIC_RELEASE);
    if (g_pause.depth) return;
    clock_resume();
    audio_release();
    gpu_release();
    kernel_release();
    LOG("pause: the game goes on after %.2f s paused\n", (double)(plat_ns() - g_pause.since) / 1e9);
}
int game_paused(void) { return __atomic_load_n(&g_pause.depth, __ATOMIC_ACQUIRE) > 0; }
void game_pause_toggle(void) {
    if (g_pause.player) {
        g_pause.player = 0;
        game_resume();
    } else if (!game_pause())
        g_pause.player = 1;
}

/* a 64-bit word mixed into h: a multiply and a shift per word, so a few GiB hash in about a second */
static u64 mix(u64 h, u64 w) {
    h = (h ^ w) * 0x9E3779B97F4A7C15ull;
    return h ^ (h >> 29);
}
/* in 64 KiB pieces, so that a page the GPU owns leaves only its piece unread */
#define HASH_PIECE ((u64)64 << 10)
u64 pause_memory_hash(u64* bytes) {
    u64 h = 0xCBF29CE484222325ull, a = AS_BASE, n = 0;
    MemBlock b;
    while (a < AS_END) {
        mem_query(a, &b);
        if (!b.size || b.addr + b.size <= a) break;
        if (b.state != MS_FREE && b.state != MS_INACCESSIBLE && b.state != MS_IO && (b.perm & PERM_R)) {
            u64 c, end = b.addr + b.size;
            h = mix(mix(mix(h, b.addr), b.size), b.state);
            for (c = b.addr; c < end; c += HASH_PIECE) {
                u64 len = end - c < HASH_PIECE ? end - c : HASH_PIECE, i, w;
                const u8* p = GP(c);
                if (own_hash(c, len, &h)) continue; /* trapped: its owners' identity is in h instead */
                for (i = 0; i + 8 <= len; i += 8) {
                    memcpy(&w, p + i, 8);
                    h = mix(h, w);
                }
                n += len;
            }
        }
        a = b.addr + b.size;
    }
    if (bytes) *bytes = n;
    return h;
}

/* the setting's pause number i as at:length (ms), 0 when there are fewer */
static int row_get(int i, u64* at, u64* len) {
    const char* p = g_settings.pauses;
    for (;;) {
        char* e;
        unsigned long long a = strtoull(p, &e, 10), l;
        if (e == p || *e != ':') return 0;
        p = e + 1;
        l = strtoull(p, &e, 10);
        if (e == p) return 0;
        if (!i--) {
            *at = a;
            *len = l;
            return 1;
        }
        if (*e != ',') return 0;
        p = e + 1;
    }
}
void pause_unattended(void) {
    u64 ms = unattended_ms(), at, len, bytes, h;
    if (!g_pause.in_row) {
        if (!row_get(g_pause.row, &at, &len) || ms < at) return;
        if (game_pause()) {
            WARN("pause: the unattended pause at %llu ms didn't happen\n", (unsigned long long)at);
            g_pause.row++;
            return;
        }
        g_pause.in_row = 1;
        g_pause.row_end = ms + len;
        g_pause.guest_at = guest_ns();
        g_pause.hash = pause_memory_hash(&g_pause.bytes);
        LOG("pause: unattended pause %d for %llu ms, guest memory %llu MiB, hash %016llx\n", g_pause.row + 1,
            (unsigned long long)len, (unsigned long long)(g_pause.bytes >> 20), (unsigned long long)g_pause.hash);
        return;
    }
    if (ms < g_pause.row_end) return;
    h = pause_memory_hash(&bytes);
    if (h == g_pause.hash && bytes == g_pause.bytes && guest_ns() == g_pause.guest_at)
        LOG("pause: unattended pause %d ends, guest memory and guest time unchanged\n", g_pause.row + 1);
    else
        WARN("pause: unattended pause %d ends, and something ran while it held: guest memory %s (%016llx, %llu MiB, "
             "was %016llx, %llu MiB), guest time %s\n",
             g_pause.row + 1, h == g_pause.hash && bytes == g_pause.bytes ? "unchanged" : "CHANGED",
             (unsigned long long)h, (unsigned long long)(bytes >> 20), (unsigned long long)g_pause.hash,
             (unsigned long long)(g_pause.bytes >> 20), guest_ns() == g_pause.guest_at ? "unchanged" : "MOVED");
    g_pause.in_row = 0;
    g_pause.row++;
    game_resume();
}
