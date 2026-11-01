/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* nvn_direct's command buffers (gpu/nvn/nvn_cmd.c): GetCommandMemoryUsed counts the bytes the game's driver writes for
   each command (one game records a display list, allocates what that says from a heap sized by the console's figures,
   and records it again into exactly that), records past those bytes go on in the runtime's memory, which comes back
   when the game adds its memory again, the game's memory callback runs when the driver would run out, and
   nvnCommandBufferCopyCommands copies a recording's records into another command buffer so the game can take the
   source's memory back at once: the copy follows the source's JUMPs, keeps its CALLs, and moves the texture snapshots a
   record carries to its own copy. The records are walked here as the replay walks them (gpu/nvn/nvn_replay.c) */
#include "nx/gpu/nvn/nvn_object.h"
#include "nx/gpu/nvn/nvn_state.h"
#include "gpu/nvn/nvn.hook.h"
#include "common/filesystem/host.h" /* PMutex */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static u8 memory[1 << 20];
u8* g_mem = memory; /* guest address a is memory + a */

/* the one pool: command memory from 0x10000 on */
u64 nvn_pool_memory(u64 pool) { return pool ? 0x10000 : 0; }
u64 nvn_pool_gpu(u64 pool) { return pool ? 0x10000 : 0; }
/* the runtime's own memory a command buffer is lent: from 0x80000 on */
static u64 g_lent = 0x80000;
u64 kmem_alloc(u64 size, u32 state, u32 perm) {
    u64 at = g_lent;
    (void)state;
    (void)perm;
    g_lent += size;
    return at;
}
/* verify mode is off: nothing shadows the lent memory */
void nvn_verify_memory_back(u64 mem, u64 size) {
    (void)mem;
    (void)size;
}
void pm_init(PMutex* m) { (void)m; }
void pm_lock(PMutex* m) { (void)m; }
void pm_unlock(PMutex* m) { (void)m; }
void logf_(int lvl, const char* fmt, ...) {
    (void)lvl;
    (void)fmt;
}
void fatal(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "FATAL: ");
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    exit(1);
}

/* the game's memory callback (a guest function, run through the dispatch table): it adds 0x100 bytes at 0x7000. Its
   calls and the size each asked for */
#define CALLBACK_PC 0x2000000ull
static void callback(CPU* c, u64 entry);
static struct {
    int calls;
    u64 size;
    GuestFn disp[1];
} g_cb = {0, 0, {callback}};
static void callback(CPU* c, u64 entry) {
    (void)entry;
    g_cb.calls++;
    g_cb.size = c->x[2];
    nvn_CommandBufferAddCommandMemory(c, c->x[0], 1, 0x7000, 0x100);
    c->pc = c->x[30];
}

static int failures;
static void check(int ok, const char* what) {
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", what);
        failures++;
    }
}

/* the records of the recording at p, in order, across JUMPs (CALLs listed, not followed), to its last */
typedef struct {
    u32 op[64], size[64];
    u64 at[64];
    int n;
} Walk;
static void walk(u64 p, Walk* w) {
    w->n = 0;
    for (;;) {
        NvnRecHdr h;
        u32 op;
        memcpy(&h, GP(p), sizeof h);
        op = NVN_REC_OP(h.op);
        if (op == OP_END || w->n == 64) return;
        if (op == OP_JUMP) {
            p = gr64(p + 8);
            continue;
        }
        w->op[w->n] = op;
        w->size[w->n] = h.size;
        w->at[w->n++] = p;
        if (h.op & NVN_REC_LAST) return;
        p += h.size;
    }
}
/* bytes [from, to) all hold v */
static int untouched(u64 from, u64 to, u8 v) {
    for (; from < to; from++)
        if (*GP(from) != v) return 0;
    return 1;
}
/* a display list's commands as one game records them for a model: a texture and a uniform buffer for the vertex
   stage, a color state, an indexed draw from a base vertex, and a compute stage's texture */
static void display_list(CPU* c, u64 cb, u64 st) {
    nvn_CommandBufferBindTexture(c, cb, 0, 0, 0x10200102);
    nvn_CommandBufferBindUniformBuffer(c, cb, 0, 1, 0x605640000ull, 0x100);
    nvn_CommandBufferBindColorState(c, cb, st);
    nvn_CommandBufferDrawElementsBaseVertex(c, cb, 4, 2, 36, 0x605660140ull, 12);
    nvn_CommandBufferBindTexture(c, cb, 5, 0, 0x10200102);
}

int main(void) {
    CPU cpu;
    const u64 cb = 0x100, cb2 = 0x200, tex = 0x300, colors = 0x400, handles = 0x480, called = 0x490, rgba = 0x4a0;
    NvnTexture t;
    Walk a, b;
    u64 h, h2, snap;
    nvn_init();
    memset(&cpu, 0, sizeof cpu);
    cpu.code_lo = CALLBACK_PC;
    cpu.code_hi = CALLBACK_PC + 4;
    cpu.disp = g_cb.disp;
    cpu.x[30] = 0x1234; /* the NVN call's return, where the callback returns */
    memset(&t, 0, sizeof t);
    t.magic = NVN_MAGIC('T', 'E', 'X', 0);
    t.d.width = 640;
    t.d.height = 360;
    t.gpu = 0x123450000ull;
    memcpy(GP(tex), &t, sizeof t);
    gw64(colors, tex);

    /* a recording to call, then the source: a clear (the driver's 48 bytes leave 16 to spare past its record), a render
       target (its texture's snapshot in the record, far bigger than the driver's 96 bytes: the rest of the recording
       goes on in the runtime's memory past a JUMP), more memory added, a draw and a call */
    nvn_CommandBufferInitialize(&cpu, cb, 0);
    nvn_CommandBufferAddCommandMemory(&cpu, cb, 1, 0, 0x1000);
    nvn_CommandBufferBeginRecording(&cpu, cb);
    nvn_CommandBufferDrawArrays(&cpu, cb, 4, 0, 3);
    gw64(called, nvn_CommandBufferEndRecording(&cpu, cb));
    nvn_CommandBufferAddCommandMemory(&cpu, cb, 1, 0x1000, 0x400);
    nvn_CommandBufferBeginRecording(&cpu, cb);
    nvn_CommandBufferClearColor(&cpu, cb, 0, rgba, 0xf);
    nvn_CommandBufferSetRenderTargets(&cpu, cb, 1, colors, 0, 0, 0);
    nvn_CommandBufferAddCommandMemory(&cpu, cb, 1, 0x2000, 0x1000); /* the recording goes on where it is */
    nvn_CommandBufferDrawArrays(&cpu, cb, 4, 6, 9);
    nvn_CommandBufferCallCommands(&cpu, cb, 1, called);
    h = nvn_CommandBufferEndRecording(&cpu, cb);
    walk(h, &a);
    check(a.n == 4 && a.op[0] == OP_CLEAR_COLOR && a.op[1] == OP_SET_RT && a.op[2] == OP_DRAW && a.op[3] == OP_CALL,
          "the source's records");
    check(a.at[0] == 0x11000 && a.at[1] >= 0x80000 && a.at[3] >= 0x80000,
          "the source's clear in the game's memory, the records past the driver's bytes in the runtime's");
    check(gr64(a.at[0] + 32 + 8) == a.at[1], "the JUMP after the clear, in the 16 bytes the driver's spare");

    /* copied into another command buffer, the source's records then overwritten */
    nvn_CommandBufferInitialize(&cpu, cb2, 0);
    nvn_CommandBufferAddCommandMemory(&cpu, cb2, 1, 0x8000, 0x1000);
    nvn_CommandBufferBeginRecording(&cpu, cb2);
    nvn_CommandBufferDrawArrays(&cpu, cb2, 4, 1, 2);
    gw64(handles, h);
    nvn_CommandBufferCopyCommands(&cpu, cb2, 1, handles);
    h2 = nvn_CommandBufferEndRecording(&cpu, cb2);
    for (int i = 0; i < a.n; i++) memset(GP(a.at[i]), 0xee, a.size[i]);
    walk(h2, &b);
    check(b.n == 5 && b.op[0] == OP_DRAW && b.op[1] == OP_CLEAR_COLOR && b.op[2] == OP_SET_RT && b.op[3] == OP_DRAW &&
              b.op[4] == OP_CALL,
          "the copy's records, after the copying buffer's own");
    check(b.at[0] == 0x18000 && b.at[1] == b.at[0] + b.size[0], "the copy starts in the copying buffer's memory");
    check(nvn_CommandBufferGetCommandMemoryUsed(&cpu, cb2) == b.size[0] + 48 + 96 + b.size[3],
          "the copy counts the driver's bytes of the records it copied (the call none)");
    snap = gr64(b.at[2] + 8 + offsetof(NvnPSetRT, color));
    check(snap > b.at[2] && snap < b.at[2] + b.size[2], "the render target's snapshot moved into the copy");
    check(snap && !memcmp(GP(snap), &t, NVN_TEX_SNAP), "the snapshot holds the texture");
    check(snx_ld_le32(GP(b.at[3] + 8 + offsetof(NvnPDraw, first))) == 6, "the draw copied");
    check(gr64(b.at[4] + 8) == gr64(called), "the call still calls the recording where it is");

    /* a command buffer whose memory runs out with no callback to add more (one game's is a bare ret): the recording
       goes on in the runtime's memory, which counts nothing of the game's used bytes */
    {
        const u64 cb3 = 0x500;
        u64 h3, top;
        u32 pieces;
        nvn_CommandBufferInitialize(&cpu, cb3, 0);
        nvn_CommandBufferAddCommandMemory(&cpu, cb3, 1, 0x3000, 0x60);
        nvn_CommandBufferBeginRecording(&cpu, cb3);
        for (int i = 0; i < 6; i++) nvn_CommandBufferDrawArrays(&cpu, cb3, 4, i, 3);
        h3 = nvn_CommandBufferEndRecording(&cpu, cb3);
        walk(h3, &a);
        check(a.n == 6 && a.op[5] == OP_DRAW && snx_ld_le32(GP(a.at[5] + 8 + offsetof(NvnPDraw, first))) == 5,
              "the recording goes on past the game's memory");
        check(a.at[0] >= 0x13000 && a.at[0] < 0x13060 && a.at[5] >= 0x80000,
              "its last records are in the runtime's memory");
        check(nvn_CommandBufferGetCommandMemoryUsed(&cpu, cb3) <= 0x60, "those count nothing of used");
        /* the game adds the same memory again and records over it: the piece of the recording written over comes back,
           and the runtime's memory takes no more */
        u64 lent_at = a.at[5] & ~0xffffull;
        top = g_lent;
        nvn_CommandBufferAddCommandMemory(&cpu, cb3, 1, 0x3000, 0x60);
        pieces = g_nvn.cmd.np;
        nvn_CommandBufferBeginRecording(&cpu, cb3);
        for (int i = 0; i < 6; i++) nvn_CommandBufferDrawArrays(&cpu, cb3, 4, i, 3);
        h3 = nvn_CommandBufferEndRecording(&cpu, cb3);
        check(g_nvn.cmd.np == pieces, "the piece of the recording written over comes back as the new one's is cut");
        walk(h3, &a);
        check(a.n == 6 && (a.at[5] & ~0xffffull) == lent_at && g_lent == top, "the runtime's memory takes no more");
    }

    /* one game's display lists: recorded once into scratch memory, GetCommandMemoryUsed gives the size the game
       allocates for each (from a heap sized by the console's figures), and the list is recorded again into exactly
       that. The bytes are the driver's (measured under nvn_verify: a texture or uniform buffer bind 20, a color state
       12, a draw from a base vertex 48, a compute stage's bind none), the second recording takes no byte past them
       and needs no callback, and the list replays whole */
    {
        const u64 cb4 = 0x600, st = 0x4c0;
        const u64 used = 20 + 20 + 12 + 48;
        u64 h4;
        gw32(st, 0x1);
        nvn_CommandBufferInitialize(&cpu, cb4, 0);
        nvn_CommandBufferSetMemoryCallback(&cpu, cb4, CALLBACK_PC);
        nvn_CommandBufferAddCommandMemory(&cpu, cb4, 1, 0x4000, 0x1000);
        nvn_CommandBufferBeginRecording(&cpu, cb4);
        display_list(&cpu, cb4, st);
        nvn_CommandBufferEndRecording(&cpu, cb4);
        check(nvn_CommandBufferGetCommandMemoryUsed(&cpu, cb4) == used,
              "a display list counts the driver's 100 bytes, not its records' 168");
        memset(GP(0x15000), 0xa5, 0x200);
        nvn_CommandBufferAddCommandMemory(&cpu, cb4, 1, 0x5000, used);
        check(nvn_CommandBufferGetCommandMemoryFree(&cpu, cb4) == used, "the memory free is what the game added");
        nvn_CommandBufferBeginRecording(&cpu, cb4);
        display_list(&cpu, cb4, st);
        h4 = nvn_CommandBufferEndRecording(&cpu, cb4);
        check(nvn_CommandBufferGetCommandMemoryUsed(&cpu, cb4) == used &&
                  nvn_CommandBufferGetCommandMemoryFree(&cpu, cb4) == 0,
              "the second recording counts the same bytes and fills the memory");
        check(g_cb.calls == 0, "no callback: the driver wouldn't run out");
        check(untouched(0x15000 + used, 0x15200, 0xa5), "no byte written past the bytes counted");
        walk(h4, &a);
        check(a.n == 5 && a.op[0] == OP_BIND_TEX1 && a.op[1] == OP_BIND_UBO && a.op[2] == OP_BIND_STATE &&
                  a.op[3] == OP_DRAW_IDX && a.op[4] == OP_BIND_TEX1,
              "the display list replays whole, in order");
        check(snx_ld_le32(GP(a.at[3] + 8 + offsetof(NvnPDrawIdx, base_vertex))) == 12, "its draw's base vertex");

        /* the driver runs out: 40 bytes, two binds take them, the draw calls the game's callback, which adds more */
        nvn_CommandBufferAddCommandMemory(&cpu, cb4, 1, 0x6000, 40);
        nvn_CommandBufferBeginRecording(&cpu, cb4);
        nvn_CommandBufferBindTexture(&cpu, cb4, 0, 0, 0x10200102);
        nvn_CommandBufferBindUniformBuffer(&cpu, cb4, 0, 1, 0x605640000ull, 0x100);
        check(g_cb.calls == 0 && nvn_CommandBufferGetCommandMemoryUsed(&cpu, cb4) == 40, "two binds fill 40 bytes");
        nvn_CommandBufferDrawElements(&cpu, cb4, 4, 2, 36, 0x605660140ull);
        h4 = nvn_CommandBufferEndRecording(&cpu, cb4);
        check(g_cb.calls == 1 && g_cb.size == 24, "the draw's 24 bytes call the callback once");
        check(nvn_CommandBufferGetCommandMemoryUsed(&cpu, cb4) == 24 &&
                  nvn_CommandBufferGetCommandMemorySize(&cpu, cb4) == 0x100,
              "the draw counts in the memory the callback added");
        walk(h4, &a);
        check(a.n == 3 && a.op[2] == OP_DRAW_IDX, "the recording replays whole across the callback's memory");
    }

    /* a record that doesn't fit with no room for a JUMP: a draw takes all the bytes counted (its size, nobody measured
       the driver's), a color state counts 12 of its 40: the draw moves to the runtime's memory and the JUMP takes its
       place, the recording's handle */
    {
        const u64 cb5 = 0x700, st = 0x4c8;
        u64 h5;
        gw32(st, 0x1);
        nvn_CommandBufferInitialize(&cpu, cb5, 0);
        nvn_CommandBufferAddCommandMemory(&cpu, cb5, 1, 0x9000, 0x1000);
        nvn_CommandBufferBeginRecording(&cpu, cb5);
        nvn_CommandBufferDrawArrays(&cpu, cb5, 4, 7, 3);
        nvn_CommandBufferBindColorState(&cpu, cb5, st);
        h5 = nvn_CommandBufferEndRecording(&cpu, cb5);
        walk(h5, &a);
        check(h5 == 0x19000 && NVN_REC_OP(gr32(h5)) == OP_JUMP, "the handle holds the JUMP");
        check(a.n == 2 && a.op[0] == OP_DRAW && a.op[1] == OP_BIND_STATE && a.at[0] >= 0x80000 &&
                  snx_ld_le32(GP(a.at[0] + 8 + offsetof(NvnPDraw, first))) == 7,
              "the draw moved, then the color state");
        check(nvn_CommandBufferGetCommandMemoryUsed(&cpu, cb5) == a.size[0] + 12,
              "the draw's bytes and the state's 12");
    }

    /* one game's blocks of command memory: it adds a block, records, adds the rest of the block from the driver's used
       bytes for the next recording, and later adds the whole block again for a new first recording, while still
       calling the second. Adding memory isn't writing over it: the second recording's piece stays in use, and replays
       as recorded after other recordings took the runtime's memory */
    {
        const u64 cb7 = 0x900, cb8 = 0x980, st = 0x4d0;
        u64 r1, r2, r1b;
        u32 pieces;
        Walk w2;
        gw32(st, 0x1);
        nvn_CommandBufferInitialize(&cpu, cb7, 0);
        nvn_CommandBufferAddCommandMemory(&cpu, cb7, 1, 0xb000, 0x1000);
        nvn_CommandBufferBeginRecording(&cpu, cb7);
        nvn_CommandBufferBindColorState(&cpu, cb7, st);
        r1 = nvn_CommandBufferEndRecording(&cpu, cb7);
        check(nvn_CommandBufferGetCommandMemoryUsed(&cpu, cb7) == 12, "the first recording: a color state's 12 bytes");
        nvn_CommandBufferAddCommandMemory(&cpu, cb7, 1, 0xb000 + 12, 0x1000 - 12);
        nvn_CommandBufferBeginRecording(&cpu, cb7);
        nvn_CommandBufferBindChannelMaskState(&cpu, cb7, st);
        nvn_CommandBufferDrawElements(&cpu, cb7, 4, 2, 6, 0x605660140ull);
        r2 = nvn_CommandBufferEndRecording(&cpu, cb7);
        walk(r2, &w2);
        pieces = g_nvn.cmd.np;
        nvn_CommandBufferAddCommandMemory(&cpu, cb7, 1, 0xb000, 0x1000);
        check(g_nvn.cmd.np == pieces, "adding the block again gives back no piece");
        nvn_CommandBufferBeginRecording(&cpu, cb7);
        nvn_CommandBufferBindColorState(&cpu, cb7, st);
        r1b = nvn_CommandBufferEndRecording(&cpu, cb7);
        walk(r1b, &a);
        check(a.n == 1 && r1b != r1, "the new first recording");
        check(g_nvn.cmd.np == pieces,
              "writing over the first recording gives back its piece alone, the second's stays");
        nvn_CommandBufferInitialize(&cpu, cb8, 0); /* another command buffer takes the runtime's memory */
        for (int k = 0; k < 64; k++) {
            nvn_CommandBufferAddCommandMemory(&cpu, cb8, 1, 0xc000, 0x1000);
            nvn_CommandBufferBeginRecording(&cpu, cb8);
            for (int i = 0; i < 16; i++) nvn_CommandBufferBindColorState(&cpu, cb8, st);
            nvn_CommandBufferEndRecording(&cpu, cb8);
        }
        walk(r2, &a);
        check(a.n == 2 && a.op[0] == OP_BIND_STATE && a.op[1] == OP_DRAW_IDX && a.at[0] == w2.at[0] &&
                  snx_ld_le32(GP(a.at[0] + 8)) == 2 && snx_ld_le32(GP(a.at[1] + 8 + offsetof(NvnPDrawIdx, count))) == 6,
              "the second recording replays as recorded");
    }

    /* a recording of calls alone: the driver writes them to control memory and counts no command bytes, nvn_direct
       counts 4, so the next recording starts past it */
    {
        const u64 cb9 = 0xa00, ch = 0x4e0;
        u64 c1, c2;
        nvn_CommandBufferInitialize(&cpu, cb9, 0);
        nvn_CommandBufferAddCommandMemory(&cpu, cb9, 1, 0xd000, 0x100);
        gw64(ch, gr64(called));
        nvn_CommandBufferBeginRecording(&cpu, cb9);
        nvn_CommandBufferCallCommands(&cpu, cb9, 1, ch);
        nvn_CommandBufferCallCommands(&cpu, cb9, 1, ch);
        c1 = nvn_CommandBufferEndRecording(&cpu, cb9);
        check(nvn_CommandBufferGetCommandMemoryUsed(&cpu, cb9) == 4, "a recording of calls counts 4 bytes");
        nvn_CommandBufferBeginRecording(&cpu, cb9);
        nvn_CommandBufferDrawArrays(&cpu, cb9, 4, 0, 3);
        c2 = nvn_CommandBufferEndRecording(&cpu, cb9);
        walk(c1, &a);
        check(a.n == 2 && a.op[0] == OP_CALL && a.op[1] == OP_CALL && gr64(a.at[1] + 8) == gr64(called) && c2 != c1,
              "it replays its calls after the next recording");
    }

    /* a recording after one whose records all went to the runtime's memory: its own records lie in its own counted
       bytes or in a piece, never in the bytes the game was told the first one took. The game reuses the first one's 20
       bytes for a new recording while it still calls the second */
    {
        const u64 cb10 = 0xa80, hs = 0x4f0, sync = 0x4f8;
        u64 r1, r2;
        Walk w;
        nvn_CommandBufferInitialize(&cpu, cb10, 0);
        nvn_CommandBufferAddCommandMemory(&cpu, cb10, 1, 0xe000, 0x1000);
        nvn_CommandBufferBeginRecording(&cpu, cb10);
        nvn_CommandBufferBindTexture(&cpu, cb10, 0, 0, 0x10200102); /* 20 bytes counted, its 24-byte record a piece's */
        r1 = nvn_CommandBufferEndRecording(&cpu, cb10);
        gw64(hs, r1);
        nvn_CommandBufferBeginRecording(&cpu, cb10);
        nvn_CommandBufferCallCommands(&cpu, cb10, 1, hs);
        r2 = nvn_CommandBufferEndRecording(&cpu, cb10);
        walk(r2, &w);
        check(w.n == 1 && (w.at[0] < 0x1e000 || w.at[0] >= 0x1e000 + 20),
              "the second recording's record lies past the first one's 20 bytes");
        nvn_CommandBufferAddCommandMemory(&cpu, cb10, 1, 0xe000, 20);
        nvn_CommandBufferBeginRecording(&cpu, cb10);
        nvn_CommandBufferFenceSync(&cpu, cb10, sync, 0, 0);
        nvn_CommandBufferEndRecording(&cpu, cb10);
        walk(r2, &w);
        check(w.n == 1 && w.op[0] == OP_CALL && gr64(w.at[0] + 8) == r1,
              "the second recording still calls the first after a new one takes the first one's bytes");
    }

    /* recordings without records: the runtime's one END, which replays nothing and counts nothing */
    {
        const u64 cb6 = 0x800;
        u64 e1, e2;
        nvn_CommandBufferInitialize(&cpu, cb6, 0);
        nvn_CommandBufferAddCommandMemory(&cpu, cb6, 1, 0xa000, 0x100);
        nvn_CommandBufferBeginRecording(&cpu, cb6);
        e1 = nvn_CommandBufferEndRecording(&cpu, cb6);
        nvn_CommandBufferBeginRecording(&cpu, cb6);
        e2 = nvn_CommandBufferEndRecording(&cpu, cb6);
        walk(e1, &a);
        check(e1 && e1 == e2 && a.n == 0 && NVN_REC_OP(gr32(e1)) == OP_END, "an empty recording replays nothing");
        check(nvn_CommandBufferGetCommandMemoryUsed(&cpu, cb6) == 0 && untouched(0x1a000, 0x1a100, 0),
              "and writes and counts nothing");
    }
    /* Barriers retain the driver's command-memory cost even when resource consumers supply their ordering.
       Copies must preserve that cost and the shader invalidation bits. */
    for (int bits = 0; bits < 256; bits++) {
        nvn_CommandBufferAddCommandMemory(&cpu, cb, 1, 0x4000, 0x1000);
        nvn_CommandBufferBeginRecording(&cpu, cb);
        nvn_CommandBufferDrawArrays(&cpu, cb, 4, 0, 3);
        u64 used = nvn_CommandBufferGetCommandMemoryUsed(&cpu, cb);
        nvn_CommandBufferBarrier(&cpu, cb, bits);
        check(nvn_CommandBufferGetCommandMemoryUsed(&cpu, cb) == used + 12,
              "barriers retain the driver's counted command bytes");
        nvn_CommandBufferDrawArrays(&cpu, cb, 4, 3, 3);
        h = nvn_CommandBufferEndRecording(&cpu, cb);
        walk(h, &a);
        check(a.n == 3 && a.op[0] == OP_DRAW && a.op[2] == OP_DRAW,
              "barriers preserve the surrounding draws");
        check(a.op[1] == OP_BARRIER && snx_ld_le32(GP(a.at[1] + 8)) == (u32)bits,
              "shader invalidation stays between its producer and consumer");
        nvn_CommandBufferAddCommandMemory(&cpu, cb2, 1, 0x8000, 0x1000);
        nvn_CommandBufferBeginRecording(&cpu, cb2);
        gw64(handles, h);
        nvn_CommandBufferCopyCommands(&cpu, cb2, 1, handles);
        h2 = nvn_CommandBufferEndRecording(&cpu, cb2);
        check(nvn_CommandBufferGetCommandMemoryUsed(&cpu, cb2) ==
                  nvn_CommandBufferGetCommandMemoryUsed(&cpu, cb),
              "copying barriers preserves command-memory accounting");
        walk(h2, &b);
        check(b.n == a.n && !memcmp(a.op, b.op, (size_t)a.n * sizeof a.op[0]),
              "copying commands preserves the barrier stream");
    }
    if (failures) return 1;
    puts("nvn_commands: ok");
    return 0;
}
