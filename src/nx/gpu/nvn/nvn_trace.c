/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* nvn_trace.h: gpu_hook_trace's lines, for the draws and dispatches the queues replay */
#include "nx/gpu/nvn/nvn_trace.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "nx/gpu/nvn/nvn_object.h"
#include "nx/gpu/nvn/nvn_tic.h"
#include "nx/runtime/loader.h"
#include "nx/runtime/options.h"
#include "nx/runtime/platform/log.h"

enum { MEMO = 256, TRACE_OPS = 8, TRACE_WORDS = 128 };

int g_nvn_trace_live;

static struct {
    int started;
    u64 main_base;            /* for a dispatch's caller, an offset from it */
    int trace;                /* the draws and dispatches it still logs, once it started */
    u32 presents, trace_from; /* the presents replayed, and how many the trace waits for (gpu_hook_trace_from) */
    /* gpu_hook_trace_words: a draw's constant buffer words the trace logs (n 0: none), and the last ones logged */
    struct {
        u32 stage, slot, off, n;
        u64 last_addr;
        u32 last[TRACE_WORDS];
        int logged;
    } words;
    /* the records since the last draw or dispatch the trace logged that are neither binds nor barriers */
    u32 ops[TRACE_OPS], nops;
    /* program hashes by code address, with the code's first 8 bytes to tell a new program at the same address */
    struct {
        u64 code, head, hash;
        u32 bytes;
    } memo[MEMO];
} g_trace;

/* a draw or dispatch as a trace line names it */
typedef struct {
    int dispatch;
    u64 program, caller;
    u32 format;
} Seen;

static void live(void) { g_nvn_trace_live = g_trace.trace > 0 && g_trace.presents >= g_trace.trace_from; }

u64 nvn_trace_program_hash(u64 code, u32 bytes) {
    u8 chunk[256];
    u64 head = 0;
    if (!code) return 0;
    if (!bytes) bytes = NVN_TRACE_CODE;
    if (gmem_read(code, &head, sizeof head)) return 0; /* its first page unmapped (gpu.h: -1) */
    u32 slot = (u32)((code >> 4) ^ (code >> 12)) % MEMO;
    if (g_trace.memo[slot].code == code && g_trace.memo[slot].bytes == bytes && g_trace.memo[slot].head == head)
        return g_trace.memo[slot].hash;
    u64 h = 0xcbf29ce484222325ull;
    for (u32 at = 0; at < bytes; at += sizeof chunk) {
        u32 n = bytes - at < sizeof chunk ? bytes - at : (u32)sizeof chunk;
        gmem_read_code(code + at, chunk, n);
        for (u32 i = 0; i < n; i++) h = (h ^ chunk[i]) * 0x100000001b3ull;
    }
    g_trace.memo[slot].code = code, g_trace.memo[slot].bytes = bytes, g_trace.memo[slot].head = head;
    g_trace.memo[slot].hash = h;
    return h;
}

int nvn_trace_image(const DispatchState* x, u32 slot, u32 off, Surf2D* out) {
    u8 raw[32];
    u32 handle, tic[8];
    *out = (Surf2D){0};
    if (slot >= DX_CBUFS) return 0;
    DsCbBlock b = dx_cb_inline(x, slot);
    if (b.bytes) {
        if (b.size < 4 || off > b.size - 4) return 0;
        handle = ds_inline_word(b, off);
    } else {
        u64 size = x->cb_size[slot] ? x->cb_size[slot] : 0x10000;
        if (!(x->cb_on >> slot & 1) || !x->cb_addr[slot] || (u64)off + 4 > size ||
            gmem_read(x->cb_addr[slot] + off, raw, 4)) /* -1: unmapped */
            return 0;
        handle = snx_ld_le32(raw);
    }
    u32 id = handle & 0xfffffu;
    if (!x->tex_pool || id > x->tex_max || gmem_read(x->tex_pool + 32ull * id, raw, sizeof raw)) return 0;
    for (u32 i = 0; i < 8; i++) tic[i] = snx_ld_le32(raw + 4 * i);
    return nvn_tic_surface(tic, out);
}

void nvn_trace_start(void) {
    if (!g_trace.started) nvn_trace_arm();
}

void nvn_trace_arm(void) {
    memset(&g_trace, 0, sizeof g_trace);
    g_trace.started = 1;
    if (g_mod)
        for (u32 i = 0; i < g_mod->nmods; i++)
            if (g_mod->mods[i].name && !strcmp(g_mod->mods[i].name, "main")) g_trace.main_base = g_mod->mods[i].base;
    g_trace.trace = g_settings.gpu_hook_trace;
    g_trace.trace_from = (u32)g_settings.gpu_hook_trace_from;
    {
        unsigned stage, slot, n;
        int off;
        char w[sizeof g_settings.gpu_hook_trace_words];
        memcpy(w, g_settings.gpu_hook_trace_words, sizeof w);
        w[sizeof w - 1] = 0;
        for (char* p = w; *p; p++)
            if (*p == ',') *p = ' '; /* commas pass through any shell */
        if (g_trace.trace && sscanf(w, "%u %u %i %u", &stage, &slot, &off, &n) == 4) {
            if (stage < DS_GFX_STAGES && slot < DS_CBUFS && off >= 0 && n)
                g_trace.words.stage = stage, g_trace.words.slot = slot, g_trace.words.off = (u32)off,
                g_trace.words.n = n < TRACE_WORDS ? n : TRACE_WORDS;
            else
                WARN("hooks: gpu_hook_trace_words names no stage (0-%d), slot (0-%d) and word count: none logged\n",
                     DS_GFX_STAGES - 1, DS_CBUFS - 1);
        }
    }
    live();
}

void nvn_trace_present(void) {
    g_trace.presents++;
    live();
    if (g_nvn_trace_live) LOG("hooks: trace present %u (the presents replayed)\n", g_trace.presents);
}

void nvn_trace_flush(const NvnDraw* d) {
    LOG("hooks: trace queue %x at a wait or the end of a submission\n", (unsigned)d->ds.owner);
}

/* the records that only bind state: a trace line lists the other records since the draw or dispatch before it */
static int binds(u32 op) {
    switch (op) {
        case OP_BIND_PROGRAM:
        case OP_BIND_TEX:
        case OP_BIND_SAMP:
        case OP_BIND_IMG:
        case OP_BIND_TEX1:
        case OP_BIND_UBO:
        case OP_BIND_SSBO:
        case OP_BIND_BUFS:
        case OP_PUSH_CONSTANTS:
        case OP_SET_TEX_POOL:
        case OP_SET_SAMP_POOL: return 1;
    }
    return 0;
}

/* a trace line, written piece by piece. The log cuts a line at 2048 bytes (platform/log.c), so a long one goes on in
   a line of its own that says so */
enum { TRACE_LINE = 1800, TRACE_PIECE = 512 };
typedef struct {
    char s[TRACE_LINE + TRACE_PIECE];
    size_t n;
} Line;
static void put(Line* l, const char* fmt, ...) {
    char piece[TRACE_PIECE];
    va_list a;
    va_start(a, fmt);
    int k = vsnprintf(piece, sizeof piece, fmt, a);
    va_end(a);
    if (k < 0) return;
    if ((size_t)k >= sizeof piece) k = (int)sizeof piece - 1;
    if (l->n && l->n + (size_t)k > TRACE_LINE) {
        LOG("%s\n", l->s);
        l->n = (size_t)snprintf(l->s, sizeof l->s, "hooks: trace, the line before goes on");
    }
    memcpy(l->s + l->n, piece, (size_t)k + 1);
    l->n += (size_t)k;
}

/* an Ounce dispatch's packed c[0] (nvn_draw_c0_layout): its buffers, and the image each texture and image handle
   names (nvn_trace_image), each at its offset in c[0] as a module would name it */
static void trace_c0(Line* l, const NvnDraw* d, const DispatchState* x) {
    static const char* const name[NVN_C0_LISTS] = {"ubo", "ssbo", "tex", "septex", NULL, "img"};
    u32 at[NVN_C0_LISTS], count[NVN_C0_LISTS];
    if (!nvn_draw_c0_layout(d, at, count)) {
        put(l, ", constant buffers %#x", x->cb_on);
        return;
    }
    for (u32 k = 0; k < NVN_C0_LISTS; k++) {
        if (!name[k]) continue;
        for (u32 i = 0; i < count[k]; i++) {
            u32 size = k < NVN_C0_TEXTURES ? 16 : 8, off = at[k] + size * i;
            if (off + size > DS_INLINE_BANK0) break;
            const u8* e = x->bank0 + off;
            if (k < NVN_C0_TEXTURES) {
                u64 addr = snx_ld_le32(e) | (u64)snx_ld_le32(e + 4) << 32;
                put(l, ", %s c0+%x %llx+%x", name[k], off, (unsigned long long)addr, snx_ld_le32(e + 8));
                continue;
            }
            Surf2D s;
            if (nvn_trace_image(x, 0, off, &s))
                put(l, ", %s c0+%x %x: %llx format %#x %ux%u%s", name[k], off, snx_ld_le32(e),
                    (unsigned long long)s.addr, s.format, s.width, s.height, s.linear ? " linear" : "");
            else
                put(l, ", %s c0+%x %x: no color target", name[k], off, snx_ld_le32(e));
        }
    }
}

/* gpu_hook_trace_words: the words of a draw's constant buffer, as floats, or that they are as last logged */
static void trace_words(Line* l, const NvnDraw* d) {
    u32 st = g_trace.words.stage, slot = g_trace.words.slot, n = g_trace.words.n, w[TRACE_WORDS] = {0};
    u64 addr = 0;
    u8 raw[4 * TRACE_WORDS];
    DsCbBlock b = ds_cb_block(d->bind.cb_inline[st][slot], d->inl.bank0[st], d->inl.bank2[st]);
    if (b.bytes)
        for (u32 i = 0; i < n; i++) w[i] = ds_inline_word(b, g_trace.words.off + 4 * i);
    else {
        addr = d->bind.cb_addr[st][slot];
        if (!addr || gmem_read(addr + g_trace.words.off, raw, 4 * n)) {
            put(l, ", constant buffer %u.%u %s", st, slot, addr ? "unmapped" : "unbound");
            return;
        }
        for (u32 i = 0; i < n; i++) w[i] = snx_ld_le32(raw + 4 * i);
    }
    if (g_trace.words.logged && addr == g_trace.words.last_addr && !memcmp(w, g_trace.words.last, 4 * n)) {
        put(l, ", constant buffer %u.%u at %llx as before", st, slot, (unsigned long long)addr);
        return;
    }
    put(l, ", constant buffer %u.%u at %llx +%#x:", st, slot, (unsigned long long)addr, g_trace.words.off);
    for (u32 i = 0; i < n; i++) {
        f32 f;
        memcpy(&f, &w[i], 4);
        put(l, " %.7g", (double)f);
    }
    g_trace.words.logged = 1;
    g_trace.words.last_addr = addr;
    memcpy(g_trace.words.last, w, 4 * n);
}

/* gpu_hook_trace: a draw or dispatch as a trace line names it, with the records between it and the one before that
   aren't state binds or barriers */
static void trace(const NvnDraw* d, const Seen* v, const DispatchState* x) {
    Line l;
    l.n = 0;
    g_trace.trace--;
    live();
    if (v->dispatch) {
        put(&l,
            "hooks: trace queue %x dispatch program %016llx (%u bytes at %llx) caller main+%llx grid %ux%ux%u block "
            "%ux%ux%u%s",
            (unsigned)d->ds.owner, (unsigned long long)v->program, x->code_bytes ? x->code_bytes : NVN_TRACE_CODE,
            (unsigned long long)x->code, (unsigned long long)v->caller, x->grid[0], x->grid[1], x->grid[2], x->block[0],
            x->block[1], x->block[2], x->indirect ? " (indirect)" : "");
        trace_c0(&l, d, x);
    } else {
        const DsTargets* t = &d->ds.tgt;
        put(&l,
            "hooks: trace queue %x draw fragment program %016llx, target 0 format %#x %ux%u at %llx, depth format "
            "%#x %ux%u at %llx, viewport %g %g %g %g",
            (unsigned)d->ds.owner, (unsigned long long)v->program, v->format, t->count ? t->color[0].width : 0,
            t->count ? t->color[0].height : 0, t->count ? (unsigned long long)t->color[0].gva : 0ull, t->depth.format,
            t->depth.width, t->depth.height, (unsigned long long)t->depth.gva, (double)d->vp[0][0], (double)d->vp[0][1],
            (double)d->vp[0][2], (double)d->vp[0][3]);
        if (g_trace.words.n) trace_words(&l, d);
    }
    if (g_trace.nops) {
        put(&l, ", after records");
        for (u32 i = 0; i < g_trace.nops && i < TRACE_OPS; i++) put(&l, " %u", g_trace.ops[i]);
        if (g_trace.nops > TRACE_OPS) put(&l, " and %u more", g_trace.nops - TRACE_OPS);
        g_trace.nops = 0;
    }
    LOG("%s\n", l.s);
}

void nvn_trace_record(NvnDraw* d, u32 op, u64 payload) {
    Seen v = {0};
    DispatchState x;
    switch (op) {
        case OP_DISPATCH: {
            const NvnPDispatch* p = NVN_OBJ(const NvnPDispatch, payload);
            if (!nvn_draw_dispatch_state(d, payload, &x)) return;
            v.dispatch = 1;
            v.program = nvn_trace_program_hash(x.code, x.code_bytes);
            v.caller = p->caller && p->caller >= g_trace.main_base ? p->caller - g_trace.main_base : 0;
            trace(d, &v, &x);
            return;
        }
        case OP_DRAW:
        case OP_DRAW_IDX:
        case OP_DRAW_MESH:
            v.program = nvn_trace_program_hash(d->code[DS_PROGRAM_SLOTS - 1], 0);
            v.format = d->ds.tgt.count ? d->ds.tgt.color[0].format : 0;
            trace(d, &v, NULL);
            return;
    }
    if (!binds(op) && op != OP_BARRIER) {
        if (g_trace.nops < TRACE_OPS) g_trace.ops[g_trace.nops] = op;
        g_trace.nops++;
    }
}
