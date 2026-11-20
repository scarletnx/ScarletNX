/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Atmosphere-format cheats (dmnt's cheat VM), the subset that memory-poking patches use: the game's data folder holds
   cheats/<build id>.txt, the main module's build id as 16 hex digits, and every cheat in it runs once per frame shown.
   Opcodes: 0 store a static value, 5 load a register from memory, 6 store a static value at a register's address,
   7 arithmetic on a register. Other opcodes stop that cheat and are logged once. Memory is checked before each access:
   an address outside the guest's mapped memory skips the access, it never faults. */
#include "nx/runtime/cheats.h"
#include "common/utilities/cheat_list.h"
#include "nx/runtime/main.h"
#include "nx/runtime/loader.h"
#include "nx/hle/filesystem/fs.h"
#include "nx/memory/mem.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define CHEAT_MAX_WORDS 4096
#define CHEAT_MAX       64

typedef struct {
    char name[64];
    u32 first, count; /* its words in g_words */
} Cheat;

static PMutex cheats_lock = PMUTEX_INIT;
static int cheats_loaded;
static u32 g_words[CHEAT_MAX_WORDS], g_nwords;
static Cheat g_cheats[CHEAT_MAX];
static int g_ncheats;
static u64 g_main_base;

static int guest_ok(u64 addr, u64 size, u32 perm) {
    u64 end = addr + size;
    MemBlock b;
    if (end < addr || end > AS_SIZE) return 0;
    while (addr < end) {
        mem_query(addr, &b);
        if ((b.perm & perm) != perm || b.addr + b.size <= addr) return 0;
        addr = b.addr + b.size;
    }
    return 1;
}
/* a value of w bytes in guest memory, which is little-endian */
static u64 rd(u64 a, u32 w) {
    if (w != 1 && w != 2 && w != 4 && w != 8) return 0;
    if (!guest_ok(a, w, PERM_R)) return 0;
    return w == 1 ? *GP(a) : w == 2 ? gr16(a) : w == 4 ? gr32(a) : gr64(a);
}
static void wr(u64 a, u32 w, u64 v) {
    if (w != 1 && w != 2 && w != 4 && w != 8) return;
    if (!guest_ok(a, w, PERM_W)) return;
    if (w == 1)
        *GP(a) = (u8)v;
    else if (w == 2)
        gw16(a, (u16)v);
    else if (w == 4)
        gw32(a, (u32)v);
    else
        gw64(a, v);
}

static void cheats_load(void) {
    const SnxModInfo* main_mod = NULL;
    char path[64], id[17];
    for (u32 i = 0; g_mod && i < g_mod->nmods; i++)
        if (!strcmp(g_mod->mods[i].name, "main")) main_mod = &g_mod->mods[i];
    if (!main_mod) {
        WARN("cheats: no main module, none loaded\n");
        return;
    }
    g_main_base = main_mod->base;
    for (int i = 0; i < 8; i++) snprintf(id + 2 * i, 3, "%02X", main_mod->build_id[i]);
    snprintf(path, sizeof path, "cheats/%s.txt", id);
    u64 size;
    u8* text = fs_read_file(path, &size);
    if (!text) {
        LOG("cheats: none (no %s in the data folder)\n", path);
        return;
    }
    Cheat* cur = NULL;
    for (u64 p = 0; p < size;) {
        u64 e = p;
        while (e < size && text[e] != '\n' && text[e] != '\r') e++;
        char line[256];
        size_t n = (size_t)(e - p) < sizeof line - 1 ? (size_t)(e - p) : sizeof line - 1;
        memcpy(line, text + p, n);
        line[n] = 0;
        p = e + 1;
        char* s = line;
        while (isspace((unsigned char)*s)) s++;
        if (*s == '[' || *s == '{') { /* a cheat's name starts it, read as the launcher lists it */
            if (g_ncheats == CHEAT_MAX) break;
            cur = &g_cheats[g_ncheats++];
            snx_cheat_name(s, cur->name, sizeof cur->name);
            cur->first = g_nwords;
            cur->count = 0;
            continue;
        }
        while (*s && cur) {
            char* end;
            unsigned long v = strtoul(s, &end, 16);
            if (end == s) break;
            if (g_nwords == CHEAT_MAX_WORDS) break;
            g_words[g_nwords++] = (u32)v;
            cur->count++;
            s = end;
            while (isspace((unsigned char)*s)) s++;
        }
    }
    free(text);
    for (int i = 0; i < g_ncheats; i++)
        LOG("cheats: \"%s\" from %s (%u words)%s\n", g_cheats[i].name, path, g_cheats[i].count,
            snx_cheat_off(g_settings.game_cheats_off, g_cheats[i].name) ? ", off: cheats_off= names it" : "");
}

static void run_cheat(const Cheat* c) {
    u64 reg[16] = {0};
    const u32* w = g_words + c->first;
    u32 n = c->count, i = 0;
    while (i < n) {
        u32 op = w[i] >> 28;
        u32 width = (w[i] >> 24) & 15;
        switch (op) {
            case 0: { /* 0TMR00AA AAAAAAAA V[V]: [base(M) + reg R + A] = V */
                if (i + (width == 8 ? 4 : 3) > n) return; /* a value of 8 bytes is two words */
                u32 m = (w[i] >> 20) & 15, r = (w[i] >> 16) & 15;
                u64 a = ((u64)(w[i] & 0xFF) << 32) | w[i + 1];
                u64 v = width == 8 ? ((u64)w[i + 2] << 32) | w[i + 3] : w[i + 2];
                if (m == 0) wr(g_main_base + reg[r] + a, width, v);
                i += width == 8 ? 4 : 3;
                break;
            }
            case 5: { /* 5TMR00AA AAAAAAAA: R = [base(M) + A]; 5T0R10AA AAAAAAAA: R = [R + A] */
                if (i + 2 > n) return;
                u32 m = (w[i] >> 20) & 15, r = (w[i] >> 16) & 15, from_reg = (w[i] >> 12) & 15;
                u64 a = ((u64)(w[i] & 0xFF) << 32) | w[i + 1];
                if (from_reg)
                    reg[r] = rd(reg[r] + a, width);
                else if (m == 0)
                    reg[r] = rd(g_main_base + a, width);
                i += 2;
                break;
            }
            case 6: { /* 6T0RIor0 VVVVVVVV VVVVVVVV: [R (+ reg o)] = V, R += T when I */
                if (i + 3 > n) return;
                u32 r = (w[i] >> 16) & 15, inc = (w[i] >> 12) & 15, off_on = (w[i] >> 8) & 15, off_r = (w[i] >> 4) & 15;
                u64 v = ((u64)w[i + 1] << 32) | w[i + 2];
                wr(reg[r] + (off_on ? reg[off_r] : 0), width, v);
                if (inc) reg[r] += width;
                i += 3;
                break;
            }
            case 7: { /* 7T0RC000 VVVVVVVV: R = R op V */
                if (i + 2 > n) return;
                u32 r = (w[i] >> 16) & 15, c7 = (w[i] >> 12) & 15;
                u64 v = w[i + 1];
                switch (c7) {
                    case 0: reg[r] += v; break;
                    case 1: reg[r] -= v; break;
                    case 2: reg[r] *= v; break;
                    case 3: reg[r] <<= v; break;
                    case 4: reg[r] >>= v; break;
                    default: return;
                }
                i += 2;
                break;
            }
            default:
                WARN_LIMIT(4, "cheats: \"%s\" uses opcode %x, not supported: it stops there\n", c->name, op);
                return;
        }
    }
}

void cheats_frame(void) {
    pm_lock(&cheats_lock);
    if (!cheats_loaded) {
        cheats_load();
        cheats_loaded = 1;
    }
    for (int i = 0; i < g_ncheats; i++)
        if (!snx_cheat_off(g_settings.game_cheats_off, g_cheats[i].name)) run_cheat(&g_cheats[i]);
    pm_unlock(&cheats_lock);
}
