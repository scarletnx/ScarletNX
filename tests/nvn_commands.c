/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* nvn_direct's command buffers (gpu/nvn/nvn_cmd.c): recordings across command memory segments, the runtime's memory
   lent when the game's runs out, and nvnCommandBufferCopyCommands, which copies a recording's records into another
   command buffer so the game can take the source's memory back at once: the copy follows the source's JUMPs, keeps its
   CALLs, and moves the texture snapshots a record carries to its own copy. The records are walked here as the replay
   walks them (gpu/nvn/nvn_replay.c) */
#include "nx/gpu/nvn/nvn_object.h"
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

static int failures;
static void check(int ok, const char* what) {
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", what);
        failures++;
    }
}

/* the records of the recording at p, in order, across JUMPs (CALLs listed, not followed) */
typedef struct {
    u32 op[64], size[64];
    u64 at[64];
    int n;
} Walk;
static void walk(u64 p, Walk* w) {
    w->n = 0;
    for (;;) {
        NvnRecHdr h;
        memcpy(&h, GP(p), sizeof h);
        if (h.op == OP_END || w->n == 64) return;
        if (h.op == OP_JUMP) {
            p = gr64(p + 8);
            continue;
        }
        w->op[w->n] = h.op;
        w->size[w->n] = h.size;
        w->at[w->n++] = p;
        p += h.size;
    }
}

int main(void) {
    CPU cpu;
    const u64 cb = 0x100, cb2 = 0x200, tex = 0x300, colors = 0x400, handles = 0x480, called = 0x490;
    NvnTexture t;
    Walk a, b;
    u64 h, h2, snap;
    memset(&cpu, 0, sizeof cpu);
    memset(&t, 0, sizeof t);
    t.magic = NVN_MAGIC('T', 'E', 'X', 0);
    t.d.width = 640;
    t.d.height = 360;
    t.gpu = 0x123450000ull;
    memcpy(GP(tex), &t, sizeof t);
    gw64(colors, tex);

    /* a recording to call, then the source: a render target (its texture's snapshot in the record), a draw, a call,
       crossing from one segment of command memory into another */
    nvn_CommandBufferInitialize(&cpu, cb, 0);
    nvn_CommandBufferAddCommandMemory(&cpu, cb, 1, 0, 0x1000);
    nvn_CommandBufferBeginRecording(&cpu, cb);
    nvn_CommandBufferDrawArrays(&cpu, cb, 4, 0, 3);
    gw64(called, nvn_CommandBufferEndRecording(&cpu, cb));
    nvn_CommandBufferAddCommandMemory(&cpu, cb, 1, 0x1000, 0x400);
    nvn_CommandBufferBeginRecording(&cpu, cb);
    nvn_CommandBufferSetRenderTargets(&cpu, cb, 1, colors, 0, 0, 0);
    nvn_CommandBufferAddCommandMemory(&cpu, cb, 1, 0x2000, 0x1000); /* the next records in another segment */
    nvn_CommandBufferDrawArrays(&cpu, cb, 4, 6, 9);
    nvn_CommandBufferCallCommands(&cpu, cb, 1, called);
    h = nvn_CommandBufferEndRecording(&cpu, cb);
    walk(h, &a);
    check(a.n == 3 && a.op[0] == OP_SET_RT && a.op[1] == OP_DRAW && a.op[2] == OP_CALL, "the source's records");
    check(a.at[0] < 0x11400 && a.at[1] >= 0x12000, "the source crosses into its second segment");

    /* copied into another command buffer, the source's memory then overwritten */
    nvn_CommandBufferInitialize(&cpu, cb2, 0);
    nvn_CommandBufferAddCommandMemory(&cpu, cb2, 1, 0x8000, 0x1000);
    nvn_CommandBufferBeginRecording(&cpu, cb2);
    nvn_CommandBufferDrawArrays(&cpu, cb2, 4, 1, 2);
    gw64(handles, h);
    nvn_CommandBufferCopyCommands(&cpu, cb2, 1, handles);
    h2 = nvn_CommandBufferEndRecording(&cpu, cb2);
    memset(GP(0x11000), 0xee, 0x400);
    memset(GP(0x12000), 0xee, 0x100);
    walk(h2, &b);
    check(b.n == 4 && b.op[0] == OP_DRAW && b.op[1] == OP_SET_RT && b.op[2] == OP_DRAW && b.op[3] == OP_CALL,
          "the copy's records, after the copying buffer's own");
    check(b.at[1] >= 0x18000 && b.at[3] < 0x19000, "the copy lies in the copying buffer's memory");
    snap = gr64(b.at[1] + 8 + offsetof(NvnPSetRT, color));
    check(snap > b.at[1] && snap < b.at[1] + b.size[1], "the render target's snapshot moved into the copy");
    check(snap && !memcmp(GP(snap), &t, NVN_TEX_SNAP), "the snapshot holds the texture");
    check(snx_ld_le32(GP(b.at[2] + 8 + offsetof(NvnPDraw, first))) == 6, "the draw copied");
    check(gr64(b.at[3] + 8) == gr64(called), "the call still calls the recording where it is");
    /* a command buffer whose memory runs out with no callback to add more (one game's is a bare ret): the recording
       goes on in memory lent to it, which counts nothing of the game's used bytes */
    {
        const u64 cb3 = 0x500;
        u64 h3;
        nvn_CommandBufferInitialize(&cpu, cb3, 0);
        nvn_CommandBufferAddCommandMemory(&cpu, cb3, 1, 0x3000, 0x60);
        nvn_CommandBufferBeginRecording(&cpu, cb3);
        for (int i = 0; i < 6; i++) nvn_CommandBufferDrawArrays(&cpu, cb3, 4, i, 3);
        h3 = nvn_CommandBufferEndRecording(&cpu, cb3);
        walk(h3, &a);
        check(a.n == 6 && a.op[5] == OP_DRAW && snx_ld_le32(GP(a.at[5] + 8 + offsetof(NvnPDraw, first))) == 5,
              "the recording goes on past the game's memory");
        check(a.at[0] >= 0x13000 && a.at[0] < 0x13060 && a.at[5] >= 0x80000, "its last records are in lent memory");
        check(nvn_CommandBufferGetCommandMemoryUsed(&cpu, cb3) <= 0x60, "lent bytes count nothing of used");
        /* the game adds the same memory again for its next recording: what was lent after it comes back, and the next
           overflow takes it again rather than more */
        u64 lent_at = a.at[5] & ~0xffffull, top = g_lent;
        nvn_CommandBufferAddCommandMemory(&cpu, cb3, 1, 0x3000, 0x60);
        nvn_CommandBufferBeginRecording(&cpu, cb3);
        for (int i = 0; i < 6; i++) nvn_CommandBufferDrawArrays(&cpu, cb3, 4, i, 3);
        h3 = nvn_CommandBufferEndRecording(&cpu, cb3);
        walk(h3, &a);
        check(a.n == 6 && (a.at[5] & ~0xffffull) == lent_at && g_lent == top, "the lent memory is taken again");
    }
    if (failures) return 1;
    puts("nvn_commands: ok");
    return 0;
}
