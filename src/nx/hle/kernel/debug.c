/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from Eden (src/core/arm/debug.cpp, src/core/arm/symbols.cpp):
   SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
   SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
   SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
   SPDX-FileCopyrightText: Copyright 2022 yuzu Emulator Project */
/* the dumps: a thread's registers with a backtrace named from the game's symbols, the last SVCs and IPC requests, and
   every thread's state (threads.txt, F5) */
#include "nx/hle/kernel/kernel_int.h"
#include "nx/hle/hle.h"
#include "nx/runtime/arm.h"
#include "nx/runtime/loader.h"

/* ============================================================ guest memory */
/* whether [a, a + n) can be read for a dump: inside the address space, with no page of it free or inaccessible. The
   dumps run from the crash handler, so no guest read goes unchecked; mem_query is safe there, even in a thread that
   holds the memory lock */
static int readable(u64 a, u64 n) {
    u64 p = a, end;
    if (!g_mem || a < AS_BASE || a >= AS_END || n > AS_END - a) return 0;
    end = a + n;
    while (p < end) {
        MemBlock b;
        mem_query(p, &b);
        if (b.state == MS_FREE || b.state == MS_INACCESSIBLE || !b.size) return 0;
        p = b.addr + b.size;
    }
    return 1;
}

/* ============================================================ symbols */
/* The game's exported functions, from each module's dynamic symbol table as Eden's Symbols::GetSymbols finds it: the
   MOD0 header named at offset 4 of the image, its dynamic section, and the symbols from DT_SYMTAB up to DT_STRTAB. A
   name stays in guest memory (read-only module data), kept as its address there (Sym, in g_hle.debug.syms) */

static int sym_cmp(const void* a, const void* b) {
    u64 x = ((const Sym*)a)->addr, y = ((const Sym*)b)->addr;
    return x < y ? -1 : x > y;
}
static u64 rd(u64 a, int wide) { return wide ? gr64(a) : gr32(a); }
static void module_symbols(const SnxModInfo* mi, int wide) {
    u64 base = mi->base, end = mi->base + mi->size, mod0, dyn, symtab = 0, strtab = 0, syment = wide ? 24 : 16, s;
    u32 cap = g_hle.debug.nsyms;
    if (!readable(base, 8)) return;
    mod0 = base + gr32(base + 4);
    if (mod0 + 8 > end || !readable(mod0, 8) || gr32(mod0) != 0x30444F4Du) return; /* "MOD0" */
    for (dyn = mod0 + (u64)(i64)(i32)gr32(mod0 + 4); dyn + (wide ? 16 : 8) <= end && readable(dyn, wide ? 16 : 8);
         dyn += wide ? 16 : 8) {
        u64 tag = rd(dyn, wide), val = rd(dyn + (wide ? 8 : 4), wide);
        if (!tag) break;
        if (tag == 5) /* DT_STRTAB */
            strtab = val;
        else if (tag == 6) /* DT_SYMTAB */
            symtab = val;
        else if (tag == 11 && val) /* DT_SYMENT */
            syment = val;
    }
    if (!symtab || strtab <= symtab || base + strtab > end) return;
    for (s = base + symtab; s + syment <= base + strtab && readable(s, syment); s += syment) {
        u32 name = gr32(s);
        u8 info = wide ? GP(s)[4] : GP(s)[12];
        u64 value = wide ? gr64(s + 8) : gr32(s + 4), size = wide ? gr64(s + 16) : gr32(s + 8);
        if ((info & 15) != 2 || !value || value >= mi->size) continue; /* defined functions (STT_FUNC) */
        if (g_hle.debug.nsyms == cap) {
            Sym* n;
            cap = cap ? cap * 2 : 1024;
            n = realloc(g_hle.debug.syms, sizeof *n * (size_t)cap);
            if (!n) return;
            g_hle.debug.syms = n;
        }
        g_hle.debug.syms[g_hle.debug.nsyms].addr = base + value;
        g_hle.debug.syms[g_hle.debug.nsyms].size = size;
        g_hle.debug.syms[g_hle.debug.nsyms].name = base + strtab + name;
        g_hle.debug.nsyms++;
    }
}
void kernel_load_symbols(void) {
    u32 i;
    if (!g_mod) return;
    for (i = 0; i < g_mod->nmods; i++) module_symbols(&g_mod->mods[i], !g_arm_mod);
    if (g_hle.debug.nsyms) qsort(g_hle.debug.syms, g_hle.debug.nsyms, sizeof *g_hle.debug.syms, sym_cmp);
    LOG("symbols: %u functions named\n", g_hle.debug.nsyms);
}
/* the address of the function the game exports as name (nvn_trace.c finds nvnBootstrapLoader so), 0 when none */
u64 kernel_find_symbol_exact(const char* name) {
    size_t len = strlen(name) + 1;
    for (u32 i = 0; i < g_hle.debug.nsyms; i++)
        if (readable(g_hle.debug.syms[i].name, len) && !memcmp(GP(g_hle.debug.syms[i].name), name, len)) return g_hle.debug.syms[i].addr;
    return 0;
}
/* the i-th exported function, in address order: its address, and its name in guest memory, NULL when the name isn't
   readable. A name is a string of the module's DT_STRTAB, so it ends inside the module. 0 past the last */
int kernel_symbol(u32 i, u64* addr, const char** name) {
    if (i >= g_hle.debug.nsyms) return 0;
    *addr = g_hle.debug.syms[i].addr;
    *name = readable(g_hle.debug.syms[i].name, 1) ? (const char*)GP(g_hle.debug.syms[i].name) : NULL;
    return 1;
}
/* the length of the guest string at a, at most max, when it is readable and ends within max; else 0 */
static int gstr(u64 a, int max) {
    int n = 0;
    while (n < max && readable(a + (u64)n, 1) && GP(a)[n]) {
        if (GP(a)[n] < 32 || GP(a)[n] >= 127) return 0;
        n++;
    }
    return n < max && readable(a + (u64)n, 1) ? n : 0;
}
/* "<name+off>" for a code address: the function it is in, or the module and the offset into it; "" outside them */
static void sym_name(u64 a, char* out, size_t n) {
    u32 lo = 0, hi = g_hle.debug.nsyms, i;
    out[0] = 0;
    while (lo < hi) { /* the last symbol at or below a */
        u32 mid = lo + (hi - lo) / 2;
        if (g_hle.debug.syms[mid].addr <= a)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo && (!g_hle.debug.syms[lo - 1].size || a < g_hle.debug.syms[lo - 1].addr + g_hle.debug.syms[lo - 1].size)) {
        const Sym* s = &g_hle.debug.syms[lo - 1];
        int len = gstr(s->name, 160);
        if (len && (s->size || a - s->addr < 0x10000)) {
            snprintf(out, n, "<%.*s+%llx>", len, (const char*)GP(s->name), (unsigned long long)(a - s->addr));
            return;
        }
    }
    for (i = 0; g_mod && i < g_mod->nmods; i++) {
        const SnxModInfo* mi = &g_mod->mods[i];
        if (a >= mi->base && a < mi->base + mi->size) {
            snprintf(out, n, "<%s+%llx>", mi->name ? mi->name : "module", (unsigned long long)(a - mi->base));
            return;
        }
    }
}

/* ============================================================ registers and backtrace */
/* fp (x29, r11 in A32 code, r7 in Thumb) points at the frame record, the caller's fp and then the return address, as
   Eden's GetAArch64Backtrace and GetAArch32Backtrace walk them. The walk stops at an fp that is misaligned or not
   mapped. Up to max return addresses after lr go to out; their count */
static int frames(u64 fp, int wide, u64* out, int max) {
    u64 rec = wide ? 16 : 8;
    int n = 0;
    while (n < max && fp && !(fp & (wide ? 7 : 3)) && readable(fp, rec)) {
        u64 lr = rd(fp + rec / 2, wide);
        if (!lr) break;
        out[n++] = lr;
        fp = rd(fp, wide);
    }
    return n;
}
static void backtrace(int lvl, u64 lr, u64 fp, int wide) {
    char s[200];
    u64 f[31];
    int i, n = frames(fp, wide, f, 31);
    sym_name(lr, s, sizeof s);
    logf_(lvl, "   #0 %llx %s\n", (unsigned long long)lr, s);
    for (i = 0; i < n; i++) {
        sym_name(f[i], s, sizeof s);
        logf_(lvl, "   #%d %llx %s\n", i + 1, (unsigned long long)f[i], s);
    }
}
/* the code from lo to hi, 16 words (0x40 bytes) a line, each line only when it is mapped; the word at mark in
   brackets */
static void dump_code(int lvl, const char* what, u64 lo, u64 hi, u64 mark) {
    u64 a;
    mark &= ~3ull;
    for (a = lo & ~3ull; a < hi; a += 0x40) {
        char line[16 * 10 + 1];
        size_t l = 0;
        int i;
        if (!readable(a, 0x40)) continue;
        for (i = 0; i < 16; i++) {
            u64 w = a + 4 * (u64)i;
            l += (size_t)sprintf(line + l, w == mark ? "[%08x]" : " %08x ", gr32(w));
        }
        logf_(lvl, "  %s %llx:%s\n", what, (unsigned long long)a, line);
    }
}
/* words 32-bit words at what each register that points into mapped memory (aligned, above 0x10000) points at. The
   registers can be a running thread's (a sleep's saved ones are rewritten at its next sleep): each is read once, and
   the value checked is the one used. Reading r[i] again after the check let a thread dump read guest address 0x1c */
static void dump_pointees(int lvl, const char* reg, const u64* r, int nr, int words) {
    int i, k;
    for (i = 0; i < nr; i++) {
        char line[8 * 9 + 1];
        size_t l = 0;
        u64 a = __atomic_load_n(&r[i], __ATOMIC_RELAXED);
        if (a < 0x10000 || (a & 3) || !readable(a, 4 * (u64)words)) continue;
        for (k = 0; k < words && k < 8; k++) l += (size_t)sprintf(line + l, " %08x", gr32(a + 4 * (u64)k));
        logf_(lvl, "  mem %s%d %llx:%s\n", reg, i, (unsigned long long)a, line);
    }
}
/* the last 40 SVCs (the ring svc.c's dispatch fills) and 32 IPC requests, oldest first. Each ring's count is read
   once: other threads go on filling them */
static void dump_rings(int lvl) {
    u32 n = __atomic_load_n(&g_hle.kernel.svc_ring_n, __ATOMIC_RELAXED), i, first;
    first = n > 40 ? n - 40 : 0;
    logf_(lvl, "  last SVCs:\n");
    for (i = first; i < n; i++) {
        const SvcRec* r = &g_hle.kernel.svc_ring[i & 63];
        logf_(lvl, "    svc %02x (%llx %llx %llx %llx) -> %x, %llx  lr=%llx\n", r->imm, (unsigned long long)r->a[0],
              (unsigned long long)r->a[1], (unsigned long long)r->a[2], (unsigned long long)r->a[3], r->res,
              (unsigned long long)r->o1, (unsigned long long)r->lr);
    }
    n = ipc_ring_count();
    first = n > 32 ? n - 32 : 0;
    logf_(lvl, "  last IPC requests:\n");
    for (i = first; i < n; i++) {
        char line[200];
        ipc_ring_text(i, line, sizeof line);
        logf_(lvl, "    %s\n", line);
    }
}
static void dump_regs32(const KThread* t) {
    const SnxArmCPU* a = &t->arm;
    u64 r[16];
    char ps[200], ls[200];
    int i;
    for (i = 0; i < 16; i++) r[i] = a->r[i];
    for (i = 0; i < 16; i += 4)
        WARN("  r%-2d= %08x  r%-2d= %08x  r%-2d= %08x  r%-2d= %08x\n", i, a->r[i], i + 1, a->r[i + 1], i + 2, a->r[i + 2],
             i + 3, a->r[i + 3]);
    WARN("  cpsr=%08x (%s) fpscr=%08x tpidrurw=%08x tpidruro=%08x\n", a->cpsr, a->cpsr & SNX_ARM_T ? "Thumb" : "A32",
         a->fpscr, a->tpidrurw, a->tpidruro);
    sym_name(a->r[15], ps, sizeof ps);
    sym_name(a->r[14], ls, sizeof ls);
    WARN("  pc %s  lr %s\n", ps, ls);
    backtrace(0, a->r[14], a->cpsr & SNX_ARM_T ? a->r[7] : a->r[11], 0);
    dump_code(0, "code", (u64)a->r[15] - 0x20, (u64)a->r[15] + 0x20, a->r[15]);
    dump_code(0, "code at lr", (u64)a->r[14] - 0x40, (u64)a->r[14] + 0x10, a->r[14]);
    dump_pointees(0, "r", r, 13, 4);
}
void kernel_dump_regs(CPU* c) {
    KThread* t = c ? (KThread*)c->thread : NULL;
    char ps[200], ls[200];
    int i;
    if (!c) return;
    if (g_arm_mod && t)
        dump_regs32(t);
    else {
        for (i = 0; i < 28; i += 4)
            WARN("  x%-2d= %016llx  x%-2d= %016llx  x%-2d= %016llx  x%-2d= %016llx\n", i, (unsigned long long)c->x[i],
                 i + 1, (unsigned long long)c->x[i + 1], i + 2, (unsigned long long)c->x[i + 2], i + 3,
                 (unsigned long long)c->x[i + 3]);
        WARN("  x28= %016llx\n", (unsigned long long)c->x[28]);
        WARN("  pc=%llx lr=%llx sp=%llx fp=%llx tls=%llx\n", (unsigned long long)c->pc, (unsigned long long)c->x[30],
             (unsigned long long)c->x[31], (unsigned long long)c->x[29], (unsigned long long)c->tpidrro);
        sym_name(c->pc, ps, sizeof ps);
        sym_name(c->x[30], ls, sizeof ls);
        WARN("  pc %s  lr %s\n", ps, ls);
        backtrace(0, c->x[30], c->x[29], 1);
        dump_code(0, "code", c->pc - 0x20, c->pc + 0x20, c->pc);
        dump_pointees(0, "x", c->x, 31, 4);
    }
    dump_rings(0);
}

/* ============================================================ threads */
/* the name the SDK gave the thread, from the nn::os::ThreadType its TLS points at (Eden's GetNameFromThreadType64 and
   GetNameFromThreadType32): only when that is the thread's argument too, or it has none, so a thread the SDK didn't
   make isn't misread */
static int sdk_name(const KThread* t, char* out, int n) {
    int wide = !g_arm_mod, len;
    u64 type, name;
    out[0] = 0;
    if (!readable(t->tls + 0x1F8, 8)) return 0;
    type = wide ? gr64(t->tls + 0x1F8) : gr32(t->tls + 0x1FC);
    if (!type || (t->arg && type != t->arg) || !readable(type, wide ? 0x1B0 : 0xEC)) return 0;
    if (wide)
        name = gr64(type + (gr16(type + 0x46) == 1 ? 0x1A0 : 0x1A8));
    else
        name = gr32(type + (gr16(type + 0x26) == 1 ? 0xE4 : 0xE8));
    len = name ? gstr(name, n) : 0;
    memcpy(out, GP(name), (size_t)len);
    out[len] = 0;
    return len;
}
/* " (12345678)", the word at a, or " (unmapped)" */
static const char* word_at(u64 a, char* s, size_t n) {
    if (readable(a, 4))
        snprintf(s, n, " (%08x)", gr32(a));
    else
        snprintf(s, n, " (unmapped)");
    return s;
}
static const char* thread_state(const KThread* t, char* s, size_t n) {
    char v1[24], v2[24];
    if (t->state == 0) return "not started";
    switch (t->woken ? 0 : t->wait_kind) {
        case 1:
            if (t->wait_tid)
                snprintf(s, n, "waiting on %s (thread %lu)", t->wait_name[0] ? t->wait_name : "objects",
                         (unsigned long)t->wait_tid);
            else
                snprintf(s, n, "waiting on %s", t->wait_name[0] ? t->wait_name : "objects");
            break;
        case 2:
            snprintf(s, n, "waiting for the mutex at %llx%s, owned by thread %llu", (unsigned long long)t->wait_addr,
                     word_at(t->wait_addr, v1, sizeof v1),
                     (unsigned long long)(t->lock_owner ? kt_at(t->lock_owner)->id : 0));
            break;
        case 3:
            snprintf(s, n, "waiting on the condition variable %llx%s, mutex %llx%s", (unsigned long long)t->wait_key,
                     word_at(t->wait_key, v1, sizeof v1), (unsigned long long)t->wait_addr,
                     word_at(t->wait_addr, v2, sizeof v2));
            break;
        case 4:
            snprintf(s, n, "waiting on the address %llx%s", (unsigned long long)t->wait_addr,
                     word_at(t->wait_addr, v1, sizeof v1));
            break;
        case 5: snprintf(s, n, "pausing a thread"); break;
        default:
            if (t->on_core >= 0)
                snprintf(s, n, "running on core %d", t->on_core);
            else
                snprintf(s, n, "%s", t->in_svc ? "in an SVC off its core (a sleep or an IPC request)" : "off its core");
            return s;
    }
    if (t->wait_timeout >= 0) {
        size_t l = strlen(s);
        snprintf(s + l, n - l, " with timeout (%lld us)", (long long)(t->wait_timeout / 1000));
    }
    return s;
}
/* A thread that called svcSleepThread 100 times or more since the last dump polls something: its registers at its last
   sleep, what they point at, and the code around the sleep's return address and its three callers'. Counted per
   thread slot (g_hle.debug.sleeps), from 0 again when a new thread takes the slot */
static void dump_sleeps(const KThread* t, u64 since) {
    char ls[200];
    u64 f[3];
    int i, n;
    sym_name(t->sleep_lr, ls, sizeof ls);
    LOG("    %llu svcSleepThread calls since the last dump, the last from %llx %s, with:\n", (unsigned long long)since,
        (unsigned long long)t->sleep_lr, ls);
    for (i = 0; i < 31; i += 4)
        LOG("     x%-2d= %llx  x%-2d= %llx  x%-2d= %llx  x%-2d= %llx\n", i, (unsigned long long)t->sleep_x[i], i + 1,
            (unsigned long long)(i + 1 < 31 ? t->sleep_x[i + 1] : 0), i + 2,
            (unsigned long long)(i + 2 < 31 ? t->sleep_x[i + 2] : 0), i + 3,
            (unsigned long long)(i + 3 < 31 ? t->sleep_x[i + 3] : 0));
    dump_pointees(1, "x", t->sleep_x, 31, 8);
    dump_code(1, "code at the sleep", t->sleep_lr - 0x20, t->sleep_lr + 0x20, t->sleep_lr);
    if (g_arm_mod) return; /* a 32-bit thread's frame pointer isn't among the SVC's registers */
    n = frames(t->sleep_x[29], 1, f, 3);
    for (i = 0; i < n; i++) dump_code(1, "code at a caller", f[i] - 0x20, f[i] + 0x20, f[i]);
}
/* Read without g_klock or the pool's lock: the dump is how a hang is diagnosed, and a hang may be a thread that keeps
   the lock. The values can be a moment stale or mid-change (a slot freed or taken meanwhile), which is all a dump
   needs. Mutex owners are named from the thread a waiter recorded, which needs no handle lookup: the waiter holds a
   reference to it */
void kernel_dump_threads(void) {
    int i, n = 0;
    for (i = 0; i < KTHREAD_MAX; i++) n += g_hle.thread.threads[i].o.type == KO_THREAD;
    LOG("threads (%d):\n", n);
    for (i = 0; i < KTHREAD_MAX; i++) {
        const KThread* t = &g_hle.thread.threads[i];
        char name[64], st[240], ps[200], ls[200], mask[48];
        int wide = !g_arm_mod, k, nf;
        u64 pc, lr, sp, fp, f[10];
        if (t->o.type != KO_THREAD || t->state == 2) continue;
        if (!sdk_name(t, name, sizeof name)) snprintf(name, sizeof name, "%s", t->name);
        mask[0] = 0;
        if (t->affinity != 1ull << t->core)
            snprintf(mask, sizeof mask, " (ideal %d, mask %llx)", t->ideal_core, (unsigned long long)t->affinity);
        LOG("  thread %llu '%s' prio %d (base %d) core %d%s: %s%s%s\n", (unsigned long long)t->id, name, t->prio,
            t->base_prio, t->core, mask, thread_state(t, st, sizeof st),
            t->on_core == -2 ? " (waiting for its core)" : "", t->suspended ? " (paused by svcSetThreadActivity)" : "");
        if (g_hle.debug.sleeps[i].tid != t->id) {
            g_hle.debug.sleeps[i].tid = t->id;
            g_hle.debug.sleeps[i].n = 0;
        }
        if (t->state == 0) continue;
        pc = wide ? t->cpu.pc : t->arm.r[15];
        lr = wide ? t->cpu.x[30] : t->arm.r[14];
        sp = wide ? t->cpu.x[31] : t->arm.r[13];
        fp = wide ? t->cpu.x[29] : t->arm.cpsr & SNX_ARM_T ? t->arm.r[7] : t->arm.r[11];
        sym_name(pc, ps, sizeof ps);
        sym_name(lr, ls, sizeof ls);
        LOG("    lr=%llx %s pc=%llx %s sp=%llx\n", (unsigned long long)lr, ls, (unsigned long long)pc, ps,
            (unsigned long long)sp);
        nf = frames(fp, wide, f, 10);
        for (k = 0; k < nf; k++) {
            sym_name(f[k], ls, sizeof ls);
            LOG("    #%d %llx %s\n", k + 1, (unsigned long long)f[k], ls);
        }
        if (t->id == g_hle.thread.main_tid) /* the main thread's callers: where it waits on the rest of the game */
            for (k = 0; k < nf && k < 3; k++) dump_code(1, "code at a caller", f[k] - 0x20, f[k] + 0x20, f[k]);
        if (t->sleep_n - g_hle.debug.sleeps[i].n >= 100) dump_sleeps(t, t->sleep_n - g_hle.debug.sleeps[i].n);
        g_hle.debug.sleeps[i].n = t->sleep_n;
    }
    dump_rings(1);
}
