/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#if !defined(_WIN32) && !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE /* setenv */
#endif
/* Unicorn (the CPU the backend's own differential tests compare with, tests/arm_test.py) runs each instruction first,
   on the guest's memory itself: its writes are logged with the bytes they replaced and put back, then the backend's
   block for the same instruction runs from the same registers, and the two are compared. Blocks are compiled one
   instruction each here (an IT instruction with those it covers), cached by address and length */
#include "vita/runtime/lockstep.h"
#include "snx_settings.h"
#ifndef SNX_VITA_LOCKSTEP
#include "common/utilities/common.h"
static struct {
    int said; /* vita_lockstep's refusal */
} g_lockstep;
VitaLockstep vita_lockstep_step(VitaProcess* p, VitaThread* t) {
    (void)p, (void)t;
    if (g_settings.vita_lockstep >= 0 && !g_lockstep.said) {
        g_lockstep.said = 1;
        snx_error("lockstep: vita_lockstep=%d needs a build configured with -DSNX_VITA_LOCKSTEP=ON: this run checks "
                  "nothing against Unicorn (docs/VITA.md)",
                  g_settings.vita_lockstep);
    }
    return VITA_LOCKSTEP_OFF;
}
void vita_lockstep_free(void) {}
#else
#include "common/cpu/arm/exec.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unicorn/unicorn.h>

#define WRITES 64u
#define CACHE  (1u << 16)

typedef struct {
    uint32_t address, size;
    unsigned char old[16], new[16];
} Write;
static struct {
    int state; /* 0 not started, 1 waiting for the import call, 2 running, -1 off */
    uint64_t start, steps, limit;
    uc_engine* uc;
    SnxArmExec* exec;
    struct {
        uint32_t address, size;
    } mapped[1100];
    unsigned mapped_count;
    Write writes[WRITES];
    unsigned write_count;
    VitaProcess* process;
    struct {
        uint64_t key;
        SnxArmBlock block;
    } cache[CACHE];
} g;

/* Unicorn stops where the group's code ends: counting instructions differs from it in an IT block */
static uint32_t g_group_start, g_group_end;
static void on_code(uc_engine* uc, uint64_t address, uint32_t size, void* user) {
    (void)size, (void)user;
    if (address < g_group_start || address >= g_group_end) uc_emu_stop(uc);
}
static void on_write(uc_engine* uc, uc_mem_type type, uint64_t address, int size, int64_t value, void* user) {
    (void)uc, (void)type, (void)value, (void)user;
    if (g.write_count == WRITES || size > 16) return;
    Write* w = &g.writes[g.write_count++];
    w->address = (uint32_t)address, w->size = (uint32_t)size;
    memcpy(w->old, g.process->mem + w->address, (size_t)size);
}
/* every range the guest can reach, mapped into Unicorn in place (and the ones gone unmapped) */
static bool map_all(VitaProcess* p) {
    struct {
        uint32_t address, size;
    } want[1100];
    unsigned n = 0;
    for (unsigned i = 0; i < p->image.segment_count; ++i)
        want[n].address = p->image.segment[i].address, want[n++].size = (p->image.segment[i].size + 0xfff) & ~0xfffu;
    want[n].address = VITA_RUNTIME_PAGE, want[n++].size = VITA_TLS_BASE + VITA_THREADS * VITA_TLS_SIZE - VITA_RUNTIME_PAGE;
    for (unsigned i = 0; i < VITA_ALLOCATIONS; ++i)
        if (p->allocations[i].size) want[n].address = p->allocations[i].address, want[n++].size = p->allocations[i].size;
    for (unsigned i = 0; i < g.mapped_count;) { /* gone or changed */
        bool keep = false;
        for (unsigned j = 0; j < n && !keep; ++j)
            keep = want[j].address == g.mapped[i].address && want[j].size == g.mapped[i].size;
        if (keep) {
            ++i;
            continue;
        }
        uc_mem_unmap(g.uc, g.mapped[i].address, g.mapped[i].size);
        g.mapped[i] = g.mapped[--g.mapped_count];
    }
    for (unsigned j = 0; j < n; ++j) {
        bool have = false;
        for (unsigned i = 0; i < g.mapped_count && !have; ++i)
            have = want[j].address == g.mapped[i].address && want[j].size == g.mapped[i].size;
        if (have) continue;
        uc_err e = uc_mem_map_ptr(g.uc, want[j].address, want[j].size, UC_PROT_ALL, p->mem + want[j].address);
        if (e != UC_ERR_OK) {
            printf("lockstep: cannot map 0x%08" PRIx32 " (%s)\n", want[j].address, uc_strerror(e));
            return false;
        }
        g.mapped[g.mapped_count].address = want[j].address, g.mapped[g.mapped_count++].size = want[j].size;
    }
    return true;
}
static bool start(void) {
    if (uc_open(UC_ARCH_ARM, UC_MODE_ARM, &g.uc) != UC_ERR_OK) return false;
    uc_ctl_set_cpu_model(g.uc, UC_CPU_ARM_MAX);
    uc_hook hook;
    char error[256];
    setenv("UC_IGNORE_REG_BREAK", "1", 1); /* the TLS registers' old names, which Unicorn 2.1 still takes */
    if (uc_hook_add(g.uc, &hook, UC_HOOK_MEM_WRITE, (void*)on_write, NULL, 1, 0) != UC_ERR_OK ||
        uc_hook_add(g.uc, &hook, UC_HOOK_CODE, (void*)on_code, NULL, 1, 0) != UC_ERR_OK)
        return false;
    if (!(g.exec = snx_arm_exec_new(error, sizeof error))) {
        printf("lockstep: no compiler (%s)\n", error);
        return false;
    }
    return true;
}
/* the halfwords (Thumb) or words of the next instruction group at pc, and how many instructions */
static unsigned group(VitaProcess* p, uint32_t pc, bool thumb, unsigned* instructions, bool* svc) {
    *svc = false;
    if (!thumb) {
        uint32_t w = vita_read32(p, pc);
        *svc = (w & 0x0f000000u) == 0x0f000000u;
        *instructions = 1;
        return 1;
    }
    unsigned units = 0, count = 0, covered = 0;
    do {
        uint16_t h1 = vita_read16(p, pc + units * 2);
        uint16_t h2 = vita_read16(p, pc + units * 2 + 2);
        SnxThumb t;
        unsigned len = snx_thumb_decode_profile(SNX_ARM_PROFILE_A9, h1, h2, &t);
        if (!len) len = (h1 >> 11) >= 0x1d ? 2 : 1;
        if (!count && (h1 & 0xff00) == 0xdf00) *svc = true;
        if (!count && t.shape == TH_IT) {
            unsigned mask = t.imm & 15;
            for (covered = 4; !(mask & 1); mask >>= 1) --covered;
        } else if (count)
            --covered;
        units += len;
        ++count;
    } while (covered && count < 6);
    *instructions = count;
    return units;
}
static SnxArmBlock compile(VitaProcess* p, uint32_t pc, bool thumb, unsigned units) {
    uint64_t key = ((uint64_t)pc << 1 | thumb) << 4 | units;
    unsigned slot = (unsigned)((key * 0x9E3779B97F4A7C15ull) >> 48) & (CACHE - 1);
    if (g.cache[slot].key == key) return g.cache[slot].block;
    uint16_t halves[16];
    uint32_t words[8];
    for (unsigned i = 0; i < units; ++i) {
        if (thumb)
            halves[i] = vita_read16(p, pc + i * 2);
        else
            words[i] = vita_read32(p, pc + i * 4);
    }
    size_t consumed = 0;
    SnxArmBlock block = snx_arm_exec_compile(g.exec, thumb ? (const void*)halves : (const void*)words, units, pc, thumb,
                                             SNX_ARM_PROFILE_A9, 0, &consumed);
    g.cache[slot].key = key, g.cache[slot].block = block;
    return block;
}
static void put(int reg, uint64_t value) { uc_reg_write(g.uc, reg, &value); }
static uint32_t get32(int reg) {
    uint64_t value = 0;
    uc_reg_read(g.uc, reg, &value);
    return (uint32_t)value;
}
VitaLockstep vita_lockstep_step(VitaProcess* p, VitaThread* t) {
    if (!g.state) {
        g.state = g_settings.vita_lockstep >= 0 ? 1 : -1;
        g.start = g_settings.vita_lockstep >= 0 ? (uint64_t)g_settings.vita_lockstep : 0;
        g.limit = g_settings.vita_lockstep_steps > 0 ? (uint64_t)g_settings.vita_lockstep_steps : UINT64_MAX;
    }
    if (g.state < 0) return VITA_LOCKSTEP_OFF;
    if (g.state == 1) {
        if (p->hle_calls < g.start) return VITA_LOCKSTEP_OFF;
        if (!start()) {
            printf("lockstep: cannot start Unicorn\n");
            g.state = -1;
            return VITA_LOCKSTEP_OFF;
        }
        printf("lockstep: from import call %" PRIu64 "\n", p->hle_calls);
        g.state = 2;
    }
    if (g.steps >= g.limit) return VITA_LOCKSTEP_OFF;
    g.process = p;
    SnxArmCPU* c = &t->cpu;
    uint32_t pc = c->r[15];
    bool thumb = !!(c->cpsr & SNX_ARM_T), svc;
    unsigned instructions, units = group(p, pc, thumb, &instructions, &svc);
    if (svc) return VITA_LOCKSTEP_NATIVE;
    if (!map_all(p)) {
        g.state = -1;
        return VITA_LOCKSTEP_OFF;
    }
    /* Unicorn from the thread's state */
    static const int regs[16] = {UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2,  UC_ARM_REG_R3,  UC_ARM_REG_R4,  UC_ARM_REG_R5,
                                 UC_ARM_REG_R6, UC_ARM_REG_R7, UC_ARM_REG_R8,  UC_ARM_REG_R9,  UC_ARM_REG_R10, UC_ARM_REG_R11,
                                 UC_ARM_REG_R12, UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC};
    put(UC_ARM_REG_FPEXC, 0x40000000u);
    put(UC_ARM_REG_CPSR, c->cpsr);
    for (unsigned i = 0; i < 15; ++i) put(regs[i], c->r[i]);
    put(UC_ARM_REG_FPSCR, c->fpscr);
    for (unsigned i = 0; i < 32; ++i) put(UC_ARM_REG_D0 + (int)i, c->d[i]);
    put(UC_ARM_REG_C13_C0_3, c->tpidruro);
    put(UC_ARM_REG_C13_C0_2, c->tpidrurw);
    g.write_count = 0;
    uc_ctl_remove_cache(g.uc, pc, pc + units * (thumb ? 2 : 4));
    g_group_start = pc, g_group_end = pc + units * (thumb ? 2 : 4);
    uc_err e = uc_emu_start(g.uc, pc | thumb, g_group_end, 0, instructions + 8);
    uint32_t u[16], ucpsr = get32(UC_ARM_REG_CPSR), ufpscr = get32(UC_ARM_REG_FPSCR);
    uint64_t ud[32];
    for (unsigned i = 0; i < 16; ++i) u[i] = get32(regs[i]);
    for (unsigned i = 0; i < 32; ++i) uc_reg_read(g.uc, UC_ARM_REG_D0 + (int)i, &ud[i]);
    /* Unicorn's writes taken back, newest first */
    for (unsigned i = 0; i < g.write_count; ++i) memcpy(g.writes[i].new, p->mem + g.writes[i].address, g.writes[i].size);
    for (unsigned i = g.write_count; i-- > 0;) memcpy(p->mem + g.writes[i].address, g.writes[i].old, g.writes[i].size);
    SnxArmCPU before = *c;
    SnxArmBlock block = compile(p, pc, thumb, units);
    if (!block) {
        printf("lockstep: the backend cannot compile 0x%08" PRIx32 "\n", pc);
        return VITA_LOCKSTEP_MISMATCH;
    }
    block(c);
    ++g.steps;
    bool same = e == UC_ERR_OK && c->exit == SNX_ARM_OK;
    for (unsigned i = 0; i < 16; ++i) same = same && u[i] == c->r[i];
    same = same && (ucpsr & 0xf80f0020u) == (c->cpsr & 0xf80f0020u) && ufpscr == c->fpscr;
    for (unsigned i = 0; i < 32; ++i) same = same && ud[i] == c->d[i];
    for (unsigned i = 0; i < g.write_count; ++i)
        same = same && !memcmp(g.writes[i].new, p->mem + g.writes[i].address, g.writes[i].size);
    if (same) return VITA_LOCKSTEP_RAN;
    printf("lockstep: step %" PRIu64 " at 0x%08" PRIx32 "%s differs (%u units:", g.steps, pc, thumb ? " (Thumb)" : "",
           units);
    for (unsigned i = 0; i < units; ++i)
        printf(" %0*x", thumb ? 4 : 8,
               thumb ? (unsigned)vita_read16(p, pc + i * 2) : (unsigned)vita_read32(p, pc + i * 4));
    printf(")%s%s\n", e != UC_ERR_OK ? ", Unicorn: " : "", e != UC_ERR_OK ? uc_strerror(e) : "");
    if (c->exit != SNX_ARM_OK) printf("  backend exit %u syndrome 0x%08" PRIx32 "\n", (unsigned)c->exit, c->syndrome);
    for (unsigned i = 0; i < 16; ++i)
        if (u[i] != c->r[i] || before.r[i] != c->r[i])
            printf("  r%u: before 0x%08" PRIx32 " unicorn 0x%08" PRIx32 " backend 0x%08" PRIx32 "%s\n", i, before.r[i], u[i],
                   c->r[i], u[i] != c->r[i] ? "  <--" : "");
    printf("  cpsr: before 0x%08" PRIx32 " unicorn 0x%08" PRIx32 " backend 0x%08" PRIx32 "\n", before.cpsr, ucpsr,
           c->cpsr);
    printf("  fpscr: before 0x%08" PRIx32 " unicorn 0x%08" PRIx32 " backend 0x%08" PRIx32 "\n", before.fpscr, ufpscr,
           c->fpscr);
    for (unsigned i = 0; i < 32; ++i)
        if (ud[i] != c->d[i])
            printf("  d%u: before 0x%016" PRIx64 " unicorn 0x%016" PRIx64 " backend 0x%016" PRIx64 "\n", i, before.d[i],
                   ud[i], c->d[i]);
    for (unsigned i = 0; i < g.write_count; ++i)
        if (memcmp(g.writes[i].new, p->mem + g.writes[i].address, g.writes[i].size))
            printf("  memory 0x%08" PRIx32 " (%u bytes) differs\n", g.writes[i].address, g.writes[i].size);
    return VITA_LOCKSTEP_MISMATCH;
}
void vita_lockstep_free(void) {
    if (g.uc) uc_close(g.uc);
    if (g.exec) snx_arm_exec_free(g.exec);
    memset(&g, 0, sizeof g);
}
#endif
