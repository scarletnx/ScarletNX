/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The BC7 encoder's choices, kept between runs. The encoder's search over modes, partitions and rotations is most of
   its time (bc7_encode.glsl's Encode); the winner of each block is twelve bits (EncodeChoice), and an encode that is
   given it runs that job alone and makes the same block, bit for bit (tests/glsl/bc7_glsl.cpp's choices_same: 13 to
   17 times less of the CPU's time there). So each level's choices are kept by the level's key (the BC7 cache's,
   vk_bc7_key: its ASTC bytes, shape and build), two bytes a block, 0 where none is known yet: the encode of a strip
   reads the strip's (astc.c bc7_choices_in) and its own come back once its submission is done (vk_bc7_choices_put).
   They are written to the game's file at the exit (vk_bc7_choices_save) and read as the next run starts
   (vk_bc7_choices_load), so a level an earlier run encoded skips the search from its first use. Everything here is
   the GPU thread's */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "common/platform/file.h"
#include "common/platform/time.h"
#include "common/utilities/common.h"
#include "nx/runtime/platform/log.h"
#include "nx/gpu/vk/vk.h"
#include <zstd.h>

typedef struct {
    u64 key[2];
    u16* words; /* a word a block, 0 where no encode has chosen yet */
    u64 n;      /* the level's blocks; 0: a free slot */
} Bc7ChoiceLevel;
static struct {
    Bc7ChoiceLevel* map;
    u32 cap, used;    /* slots, a power of two; 0: none yet */
    u64 bytes;        /* the words held */
    u64 given, known; /* blocks given a choice to take, and of them how many had one, since the start */
    char path[1400];  /* the game's file, which vk_bc7_choices_load read and vk_bc7_choices_save writes */
} g_bc7_choices;

static Bc7ChoiceLevel* choice_slot(Bc7ChoiceLevel* map, u32 cap, const u64 key[2]) {
    for (u32 i = (u32)key[0] & (cap - 1);; i = (i + 1) & (cap - 1)) {
        Bc7ChoiceLevel* e = &map[i];
        if (!e->n || (e->key[0] == key[0] && e->key[1] == key[1])) return e;
    }
}
u16* vk_bc7_choices_find(const u64 key[2], u64 n) {
    if (!g_bc7_choices.cap) return NULL;
    Bc7ChoiceLevel* e = choice_slot(g_bc7_choices.map, g_bc7_choices.cap, key);
    return e->n == n ? e->words : NULL;
}
u16* vk_bc7_choices_take(const u64 key[2], u64 n) {
    if (!n) return NULL;
    if ((u64)(g_bc7_choices.used + 1) * 2 > g_bc7_choices.cap) {
        const u32 cap = g_bc7_choices.cap ? g_bc7_choices.cap * 2 : 1024;
        Bc7ChoiceLevel* grown = calloc(cap, sizeof *grown);
        if (!grown) return NULL;
        for (u32 i = 0; i < g_bc7_choices.cap; i++)
            if (g_bc7_choices.map[i].n) *choice_slot(grown, cap, g_bc7_choices.map[i].key) = g_bc7_choices.map[i];
        free(g_bc7_choices.map);
        g_bc7_choices.map = grown;
        g_bc7_choices.cap = cap;
    }
    Bc7ChoiceLevel* e = choice_slot(g_bc7_choices.map, g_bc7_choices.cap, key);
    if (e->n == n) return e->words;
    if (e->n) { /* the same key with another count: another level's shape, never the same bytes; made again */
        g_bc7_choices.bytes -= e->n * 2;
        free(e->words);
        e->n = 0;
        g_bc7_choices.used--;
    }
    u16* words = calloc((size_t)n, 2);
    if (!words) return NULL;
    *e = (Bc7ChoiceLevel){{key[0], key[1]}, words, n};
    g_bc7_choices.used++;
    g_bc7_choices.bytes += n * 2;
    return words;
}
void vk_bc7_choices_put(const u64 key[2], u64 n, u64 first, const void* got, u64 count) {
    u16* words = vk_bc7_choices_take(key, n);
    if (!words || first > n || count > n - first) return;
    for (u64 i = 0; i < count; i++) {
        u32 w;
        memcpy(&w, (const u8*)got + i * 4, 4);
        words[first + i] = (u16)(w & 0xFFFu);
    }
}
void vk_bc7_choices_given(u64 blocks, u64 known) {
    g_bc7_choices.given += blocks;
    g_bc7_choices.known += known;
}
void vk_bc7_choices_stats(u64* levels, u64* bytes, u64* given, u64* known) {
    *levels = g_bc7_choices.used;
    *bytes = g_bc7_choices.bytes;
    *given = g_bc7_choices.given;
    *known = g_bc7_choices.known;
}

/* the file: BC7_CHOICES_MAGIC, the encoder's version, how many levels, the bytes of the levels' records and of their
   zstd frame, then that frame; each level's record its key, its blocks and a word a block. Packed at the exit and
   unpacked whole as the game starts, as the shader cache is: the disk holds a twelfth of it, the memory all of it */
#define BC7_CHOICES_MAGIC "snx-bc7choice 2"
#define BC7_CHOICES_ZSTD  9 /* 11.5 MB in 1.1 s on one CPU for a racing game's 160 MB; 15 takes 10 s for 10.8 */
typedef struct {
    char magic[16];
    u64 encoder; /* VK_BC7_ENCODER_VERSION */
    u64 levels;
    u64 raw, packed; /* the records' bytes, and their zstd frame's */
} Bc7ChoicesHead;
typedef struct {
    u64 key[2];
    u64 n;
} Bc7ChoicesRecord;
static_assert(sizeof(Bc7ChoicesHead) == 48 && sizeof(Bc7ChoicesRecord) == 24, "the file's records are packed");

/* the levels' records of raw bytes at p into the kept ones: how many levels were read */
static u64 choices_read(const char* path, const u8* p, u64 raw, u64 levels) {
    u64 at = 0, read = 0;
    for (u64 i = 0; i < levels; i++) {
        Bc7ChoicesRecord r;
        if (raw - at < sizeof r) {
            LOG("vulkan: %s holds %llu levels' records, not the %llu its head says: the levels before are read\n", path,
                (unsigned long long)i, (unsigned long long)levels);
            break;
        }
        memcpy(&r, p + at, sizeof r);
        at += sizeof r;
        if (!r.n || (raw - at) / 2 < r.n) {
            LOG("vulkan: %s ends inside its level %llu of %llu (%llu blocks, %llu bytes left): the levels before it "
                "are read\n",
                path, (unsigned long long)i, (unsigned long long)levels, (unsigned long long)r.n,
                (unsigned long long)(raw - at));
            break;
        }
        u16* words = vk_bc7_choices_take(r.key, r.n);
        if (!words) {
            LOG("vulkan: no memory for the BC7 choices of %llu more levels of %s: the levels before them are read\n",
                (unsigned long long)(levels - i), path);
            break;
        }
        memcpy(words, p + at, (size_t)r.n * 2);
        at += r.n * 2;
        read++;
    }
    return read;
}

u64 vk_bc7_choices_load(const char* path) {
    snprintf(g_bc7_choices.path, sizeof g_bc7_choices.path, "%s", path);
    PFileMap m = {0};
    if (plat_file_map(&m, path) || !m.data) return 0; /* none: no run of this game here encoded a level yet */
    const u64 t0 = plat_ns();
    Bc7ChoicesHead h;
    if (m.size < sizeof h) {
        LOG("vulkan: %s is %llu bytes, shorter than the BC7 choices' head of %zu: not read, and written again at the "
            "exit\n",
            path, (unsigned long long)m.size, sizeof h);
        plat_file_unmap(&m);
        return 0;
    }
    memcpy(&h, m.data, sizeof h);
    if (memcmp(h.magic, BC7_CHOICES_MAGIC, sizeof BC7_CHOICES_MAGIC) || h.encoder != VK_BC7_ENCODER_VERSION) {
        LOG("vulkan: %s begins \"%.15s\" of BC7 encoder %llu, not this build's \"%s\" of encoder %u: its choices are "
            "not read, and the file is written again at the exit\n",
            path, h.magic, (unsigned long long)h.encoder, BC7_CHOICES_MAGIC, (unsigned)VK_BC7_ENCODER_VERSION);
        plat_file_unmap(&m);
        return 0;
    }
    if (h.packed != m.size - sizeof h) {
        LOG("vulkan: %s says its choices take %llu bytes packed, but %llu follow its head: not read, and written again "
            "at the exit\n",
            path, (unsigned long long)h.packed, (unsigned long long)(m.size - sizeof h));
        plat_file_unmap(&m);
        return 0;
    }
    u8* raw = malloc(h.raw ? (size_t)h.raw : 1);
    const size_t got = raw ? ZSTD_decompress(raw, (size_t)h.raw, m.data + sizeof h, (size_t)h.packed) : 0;
    if (!raw || ZSTD_isError(got) || got != h.raw) {
        LOG("vulkan: %s's %llu bytes of BC7 choices could not be unpacked to the %llu its head says (%s): not read, "
            "and written again at the exit\n",
            path, (unsigned long long)h.packed, (unsigned long long)h.raw,
            !raw                ? "no memory"
            : ZSTD_isError(got) ? ZSTD_getErrorName(got)
                                : "another size");
        free(raw);
        plat_file_unmap(&m);
        return 0;
    }
    plat_file_unmap(&m);
    const u64 levels = choices_read(path, raw, h.raw, h.levels);
    free(raw);
    LOG("vulkan: the BC7 encoder's choices of %llu levels (%llu KB, %llu KB packed) came from %s in %.0f ms: those "
        "levels skip the search\n",
        (unsigned long long)levels, (unsigned long long)(h.raw >> 10), (unsigned long long)(h.packed >> 10), path,
        (double)(plat_ns() - t0) / 1e6);
    return levels;
}

void vk_bc7_choices_save(void) {
    const char* path = g_bc7_choices.path;
    if (!*path || !g_bc7_choices.used) return; /* a run that encoded nothing keeps the last run's file */
    const u64 t0 = plat_ns();
    const u64 raw = (u64)g_bc7_choices.used * sizeof(Bc7ChoicesRecord) + g_bc7_choices.bytes;
    const size_t bound = ZSTD_compressBound((size_t)raw);
    u8* rec = malloc((size_t)raw);
    u8* out = malloc(sizeof(Bc7ChoicesHead) + bound);
    size_t packed = 0;
    int ok = rec && out;
    u64 at = 0;
    for (u32 i = 0; ok && i < g_bc7_choices.cap; i++) {
        const Bc7ChoiceLevel* e = &g_bc7_choices.map[i];
        if (!e->n) continue;
        const Bc7ChoicesRecord r = {{e->key[0], e->key[1]}, e->n};
        memcpy(rec + at, &r, sizeof r);
        memcpy(rec + at + sizeof r, e->words, (size_t)e->n * 2);
        at += sizeof r + e->n * 2;
    }
    if (ok) {
        packed = ZSTD_compress(out + sizeof(Bc7ChoicesHead), bound, rec, (size_t)raw, BC7_CHOICES_ZSTD);
        ok = !ZSTD_isError(packed);
    }
    char tmp[1400];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE* f = ok ? fopen(tmp, "wb") : NULL;
    if (ok) {
        Bc7ChoicesHead h = {
            .encoder = VK_BC7_ENCODER_VERSION, .levels = g_bc7_choices.used, .raw = raw, .packed = packed};
        memcpy(h.magic, BC7_CHOICES_MAGIC, sizeof BC7_CHOICES_MAGIC);
        memcpy(out, &h, sizeof h);
        ok = f && fwrite(out, 1, sizeof h + packed, f) == sizeof h + packed;
    }
    if (f && fclose(f)) ok = 0;
    free(rec);
    free(out);
    if (!ok || plat_replace_file(tmp, path)) {
        remove(tmp);
        WARN("vulkan: the BC7 encoder's choices of %u levels (%llu KB) could not be written to %s: it keeps the last "
             "run's\n",
             g_bc7_choices.used, (unsigned long long)(raw >> 10), path);
        return;
    }
    LOG("vulkan: the BC7 encoder's choices of %u levels (%llu KB, %llu KB packed by zstd level %d) went to %s in %.0f "
        "ms, for the next run to skip their search\n",
        g_bc7_choices.used, (unsigned long long)(raw >> 10), (unsigned long long)(packed >> 10), BC7_CHOICES_ZSTD, path,
        (double)(plat_ns() - t0) / 1e6);
}
