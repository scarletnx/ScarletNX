/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A native back end's code under Unicorn for the back ends' tests (host_unicorn.h) */
#include "support/host_unicorn.h"
#include "support/la64_interp.h"
#include "common/cpu/recompiler/llvm/elf_object.h"
#include "common/utilities/bits.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unicorn/unicorn.h>

#define PAGE 4096u
#define NTRAPS (PAGE / 16 - 1) /* the last entry is the exit's */
#define STACK_SIZE (256u << 10)

/* an architecture: Unicorn's, its CPU, the ELF machine, a return's word (the traps' and the exit's), and the ABI's
   registers: the arguments, the results, the stack and the link */
typedef struct {
    const char* name;
    uc_arch arch;
    uc_mode mode;
    int cpu;
    unsigned em;
    uint32_t ret;
    int args[6], result[2], sp, lr, pc;
    int big;
} Arch;
static const Arch g_arch[] = {
    [HOSTUC_ARM64] = {"arm64", UC_ARCH_ARM64, UC_MODE_ARM, UC_CPU_ARM64_MAX, EM_AARCH64, 0xD65F03C0u,
                      {UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2, UC_ARM64_REG_X3, UC_ARM64_REG_X4,
                       UC_ARM64_REG_X5},
                      {UC_ARM64_REG_X0, UC_ARM64_REG_X1}, UC_ARM64_REG_SP, UC_ARM64_REG_X30, UC_ARM64_REG_PC, 0},
    [HOSTUC_RV64] = {"riscv64", UC_ARCH_RISCV, UC_MODE_RISCV64, UC_CPU_RISCV64_ANY, EM_RISCV, 0x00008067u,
                     {UC_RISCV_REG_A0, UC_RISCV_REG_A1, UC_RISCV_REG_A2, UC_RISCV_REG_A3, UC_RISCV_REG_A4,
                      UC_RISCV_REG_A5},
                     {UC_RISCV_REG_A0, UC_RISCV_REG_A1}, UC_RISCV_REG_SP, UC_RISCV_REG_RA, UC_RISCV_REG_PC, 0},
    [HOSTUC_PPC64LE] = {"ppc64le", UC_ARCH_PPC, UC_MODE_PPC64 | UC_MODE_BIG_ENDIAN, UC_CPU_PPC64_POWER9_V2_0, EM_PPC64,
                        0x4E800020u, /* blr */
                        {UC_PPC_REG_3, UC_PPC_REG_4, UC_PPC_REG_5, UC_PPC_REG_6, UC_PPC_REG_7, UC_PPC_REG_8},
                        {UC_PPC_REG_3, UC_PPC_REG_4}, UC_PPC_REG_1, UC_PPC_REG_LR, UC_PPC_REG_PC, 0},
    [HOSTUC_MIPS64EL] = {"mips64el", UC_ARCH_MIPS, UC_MODE_MIPS64 | UC_MODE_LITTLE_ENDIAN, UC_CPU_MIPS64_MIPS64R2_GENERIC,
                         EM_MIPS, 0x03E00008u, /* jr ra (its delay slot the next word's: the page's words alternate) */
                         {UC_MIPS_REG_4, UC_MIPS_REG_5, UC_MIPS_REG_6, UC_MIPS_REG_7, UC_MIPS_REG_8, UC_MIPS_REG_9},
                         {UC_MIPS_REG_2, UC_MIPS_REG_3}, UC_MIPS_REG_29, UC_MIPS_REG_31, UC_MIPS_REG_PC, 0},
    /* la64_interp.c's: its registers by number */
    [HOSTUC_LA64] = {"loongarch64", (uc_arch)0, (uc_mode)0, 0, EM_LOONGARCH, 0x4C000020u, /* jirl zero, ra, 0 */
                     {4, 5, 6, 7, 8, 9}, {4, 5}, 3, 1, 0, 0},
    [HOSTUC_PPC64] = {"ppc64", UC_ARCH_PPC, UC_MODE_PPC64 | UC_MODE_BIG_ENDIAN, UC_CPU_PPC64_POWER9_V2_0, EM_PPC64,
                      0x4E800020u, {UC_PPC_REG_3, UC_PPC_REG_4, UC_PPC_REG_5, UC_PPC_REG_6, UC_PPC_REG_7, UC_PPC_REG_8},
                      {UC_PPC_REG_3, UC_PPC_REG_4}, UC_PPC_REG_1, UC_PPC_REG_LR, UC_PPC_REG_PC, 1},
    [HOSTUC_MIPS64] = {"mips64", UC_ARCH_MIPS, UC_MODE_MIPS64 | UC_MODE_BIG_ENDIAN, UC_CPU_MIPS64_MIPS64R2_GENERIC,
                       EM_MIPS, 0x03E00008u,
                       {UC_MIPS_REG_4, UC_MIPS_REG_5, UC_MIPS_REG_6, UC_MIPS_REG_7, UC_MIPS_REG_8, UC_MIPS_REG_9},
                       {UC_MIPS_REG_2, UC_MIPS_REG_3}, UC_MIPS_REG_29, UC_MIPS_REG_31, UC_MIPS_REG_PC, 1},
    [HOSTUC_S390X] = {"s390x", UC_ARCH_S390X, UC_MODE_BIG_ENDIAN, UC_CPU_S390X_Z196, EM_S390,
                      0x07FE07FEu, /* br r14, twice: the 2-byte return in each half of a word */
                      {UC_S390X_REG_R2, UC_S390X_REG_R3, UC_S390X_REG_R4, UC_S390X_REG_R5, UC_S390X_REG_R6, -1},
                      {UC_S390X_REG_R2, UC_S390X_REG_R3}, UC_S390X_REG_R15, UC_S390X_REG_R14, UC_S390X_REG_PC, 1},
};
/* MIPS's virtual TLB: every address its own */
static bool identity(uc_engine* uc, uint64_t vaddr, uc_mem_type type, uc_tlb_entry* r, void* user) {
    (void)uc, (void)type, (void)user;
    r->paddr = vaddr;
    r->perms = UC_PROT_ALL;
    return true;
}

typedef struct {
    char* name;
    uint64_t at;
} UcSym;

struct HostUc {
    const Arch* a;
    uc_engine* uc;
    unsigned char* code; /* the region: code, then its trap page */
    size_t cap, used;
    unsigned char* trap;
    void* fn[NTRAPS];
    char* name[NTRAPS];
    unsigned char wide[NTRAPS];
    unsigned ntraps;
    unsigned char* stack;
    UcSym* syms;
    size_t nsyms, cap_syms;
    La64* la; /* the interpreter's, where Unicorn has none of the architecture (no uc) */
    La64Region regions[64];
    size_t nregions;
    void (*swap)(void* user); /* a big-endian architecture's (hostuc_swapper) */
    void* swap_user;
    uint64_t sp; /* the entry's */
};
static void swap(HostUc* u) {
    if (u->a->big && u->swap) u->swap(u->swap_user);
}
int hostuc_big(HostUcArch arch) { return g_arch[arch].big; }
void hostuc_swapper(HostUc* u, void (*fn)(void* user), void* user) { u->swap = fn, u->swap_user = user; }
/* a word in the architecture's order */
static void put_word(const Arch* a, unsigned char* p, uint32_t w) {
    if (a->big) w = snx_bswap32(w);
    memcpy(p, &w, 4);
}

const char* hostuc_name(HostUcArch arch) { return g_arch[arch].name; }

void* hostuc_pages(size_t size) {
    size_t bytes = (size + PAGE - 1) & ~(size_t)(PAGE - 1);
    void* p;
#ifdef MAP_FIXED_NOREPLACE
    uint64_t at = 0x10000000, step = bytes > (1u << 24) ? bytes : 1u << 24;
    for (; at + bytes <= 0x100000000ull; at += step) {
        p = mmap((void*)(uintptr_t)at, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED_NOREPLACE, -1,
                 0);
        if (p == (void*)(uintptr_t)at) return p;
        if (p != MAP_FAILED) munmap(p, bytes); /* a kernel without the flag takes it as a hint */
    }
#endif
    p = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}
int hostuc_map(HostUc* u, void* p, size_t size) {
    uc_err e;
    if (u->la) {
        if (u->nregions == sizeof u->regions / sizeof *u->regions) return -1;
        u->regions[u->nregions].lo = (uint64_t)(uintptr_t)p;
        u->regions[u->nregions++].hi = (uint64_t)(uintptr_t)p + ((size + PAGE - 1) & ~(size_t)(PAGE - 1));
        u->la->regions = u->regions, u->la->nregions = u->nregions;
        return 0;
    }
    e = uc_mem_map_ptr(u->uc, (uint64_t)(uintptr_t)p, (size + PAGE - 1) & ~(size_t)(PAGE - 1), UC_PROT_ALL, p);
    if (e != UC_ERR_OK) {
        fprintf(stderr, "%s unicorn: no map of %p: %s\n", u->a->name, p, uc_strerror(e));
        return -1;
    }
    return 0;
}

/* ---- what a callee of AAPCS64 keeps ---- */

/* X19 to X29, and X18, the platform's; the low halves of V8 to V15 */
#define KEPT_X 12
#define KEPT_D 8
static int x_reg(unsigned n) { /* X29 and X30 are apart in Unicorn's numbers */
    return n == 29 ? UC_ARM64_REG_X29 : n == 30 ? UC_ARM64_REG_X30 : UC_ARM64_REG_X0 + (int)n;
}
static uint64_t mark_x(unsigned n) { return 0x5151000000000000ull + n * 0x0101010101ull; }
static uint64_t mark_d(unsigned n) { return 0x6262000000000000ull + n * 0x0303030303ull; }
static void mark_kept(HostUc* u) {
    unsigned k;
    for (k = 0; k < KEPT_X; ++k) {
        uint64_t v = mark_x(18 + k);
        uc_reg_write(u->uc, x_reg(18 + k), &v);
    }
    for (k = 0; k < KEPT_D; ++k) {
        uint64_t v[2] = {mark_d(8 + k), 0xBAD0BAD1BAD2BAD3ull};
        uc_reg_write(u->uc, UC_ARM64_REG_V8 + (int)k, v);
    }
}
/* what came back changed of what the entry marked, and its stack pointer: 0 for nothing, said */
static int kept_changed(HostUc* u) {
    int bad = 0;
    unsigned k;
    uint64_t v, sp;
    for (k = 0; k < KEPT_X; ++k) {
        uc_reg_read(u->uc, x_reg(18 + k), &v);
        if (v != mark_x(18 + k))
            bad = 1, fprintf(stderr, "%s: X%u came back %llx, was %llx\n", u->a->name, 18 + k, (unsigned long long)v,
                             (unsigned long long)mark_x(18 + k));
    }
    for (k = 0; k < KEPT_D; ++k) {
        uint64_t q[2];
        uc_reg_read(u->uc, UC_ARM64_REG_V8 + (int)k, q);
        if (q[0] != mark_d(8 + k))
            bad = 1, fprintf(stderr, "%s: D%u came back %llx, was %llx\n", u->a->name, 8 + k, (unsigned long long)q[0],
                             (unsigned long long)mark_d(8 + k));
    }
    uc_reg_read(u->uc, u->a->sp, &sp);
    if (sp != u->sp) bad = 1, fprintf(stderr, "%s: SP came back %lld off\n", u->a->name, (long long)(sp - u->sp));
    return bad;
}

/* ---- the traps ---- */

/* the native function of a trap the code reached, with the first six arguments (System V passes them the same way, a
   function of fewer ignoring the rest), its result into the result register (both halves of a wide one); the trap's
   return goes back */
typedef uint64_t (*Native)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
typedef HostUcPair (*NativeWide)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
/* the call's type is not the function's: the function sanitizer's check of it is off, as arm_x64.c's run_block's */
[[clang::no_sanitize("function")]] static uint64_t call_native(void* fn, const uint64_t* a) {
    return ((Native)fn)(a[0], a[1], a[2], a[3], a[4], a[5]);
}
[[clang::no_sanitize("function")]] static HostUcPair call_wide(void* fn, const uint64_t* a) {
    return ((NativeWide)fn)(a[0], a[1], a[2], a[3], a[4], a[5]);
}
static void on_trap(uc_engine* uc, uint64_t address, uint32_t size, void* user) {
    HostUc* u = (HostUc*)user;
    unsigned k = (unsigned)((address - (uint64_t)(uintptr_t)u->trap) / 16), i;
    uint64_t a[6], r;
    (void)size;
    if (k >= u->ntraps || (address & 15)) return;
    for (i = 0; i < 6; ++i)
        if (u->a->args[i] >= 0) uc_reg_read(uc, u->a->args[i], &a[i]);
        else { /* s390x's sixth: its stack slot, 160 past the stack pointer */
            uint64_t sp;
            uc_reg_read(uc, u->a->sp, &sp);
            memcpy(&a[i], (const void*)(uintptr_t)(sp + 160), 8);
            if (u->a->big) a[i] = snx_bswap64(a[i]);
        }
    swap(u); /* the C's numbers while it runs */
    if (u->wide[k]) {
        HostUcPair w = call_wide(u->fn[k], a);
        r = w.lo;
        uc_reg_write(uc, u->a->result[1], &w.hi);
    } else
        r = call_native(u->fn[k], a);
    swap(u);
    uc_reg_write(uc, u->a->result[0], &r);
}

/* the interpreter's trap: the native function's call as on_trap's, then its return (to ra); the exit's stops */
static int la_trap(La64* c, void* user) {
    HostUc* u = (HostUc*)user;
    uint64_t exit = (uint64_t)(uintptr_t)(u->trap + PAGE - 16);
    unsigned k = (unsigned)((c->pc - (uint64_t)(uintptr_t)u->trap) / 16);
    if (c->pc == exit) return 0;
    if (k >= u->ntraps || (c->pc & 15)) {
        snprintf(c->why, sizeof c->why, "a trap page's word no native function is: %llx", (unsigned long long)c->pc);
        return -1;
    }
    if (u->wide[k]) {
        HostUcPair w = call_wide(u->fn[k], &c->r[4]);
        c->r[4] = w.lo, c->r[5] = w.hi;
    } else
        c->r[4] = call_native(u->fn[k], &c->r[4]);
    c->pc = c->r[1];
    return 1;
}

HostUc* hostuc_new(HostUcArch arch, size_t code_cap) {
    HostUc* u = (HostUc*)calloc(1, sizeof *u);
    uc_hook hook;
    unsigned k;
    if (!u) return NULL;
    u->a = &g_arch[arch];
    code_cap = (code_cap + PAGE - 1) & ~(size_t)(PAGE - 1);
    if (arch == HOSTUC_LA64) {
        if (!(u->la = (La64*)calloc(1, sizeof *u->la)) || la64_selftest(u->la)) {
            if (u->la) fprintf(stderr, "loongarch64 interpreter: %s\n", u->la->why);
            hostuc_free(u);
            return NULL;
        }
        u->code = (unsigned char*)hostuc_pages(code_cap + PAGE);
        u->stack = (unsigned char*)hostuc_pages(STACK_SIZE);
        if (!u->code || !u->stack || hostuc_map(u, u->code, code_cap + PAGE) || hostuc_map(u, u->stack, STACK_SIZE)) {
            hostuc_free(u);
            return NULL;
        }
        u->cap = code_cap;
        u->trap = u->code + code_cap;
        for (k = 0; k < PAGE / 4; ++k) memcpy(u->trap + 4 * k, &u->a->ret, 4);
        u->la->trap_lo = (uint64_t)(uintptr_t)u->trap, u->la->trap_hi = u->la->trap_lo + PAGE;
        u->la->trap = la_trap, u->la->user = u;
        return u;
    }
    if (uc_open(u->a->arch, u->a->mode, &u->uc) != UC_ERR_OK) {
        free(u);
        return NULL;
    }
    uc_ctl_set_cpu_model(u->uc, u->a->cpu);
    if (arch == HOSTUC_PPC64LE || arch == HOSTUC_PPC64) {
        /* 64-bit (SF), the hypervisor's real mode (HV: every address its own), little-endian (LE) or not */
        uint64_t msr = 1ull << 63 | 1ull << 60 | (arch == HOSTUC_PPC64LE), got = 0;
        uc_reg_write(u->uc, UC_PPC_REG_MSR, &msr);
        uc_reg_read(u->uc, UC_PPC_REG_MSR, &got);
        if (got != msr) {
            fprintf(stderr, "%s unicorn: MSR %llx doesn't hold (an unpatched Unicorn 2.1.4)\n", u->a->name,
                    (unsigned long long)msr);
            uc_close(u->uc);
            free(u);
            return NULL;
        }
    }
    u->code = (unsigned char*)hostuc_pages(code_cap + PAGE);
    u->stack = (unsigned char*)hostuc_pages(STACK_SIZE);
    if (!u->code || !u->stack || hostuc_map(u, u->code, code_cap + PAGE) || hostuc_map(u, u->stack, STACK_SIZE)) {
        hostuc_free(u);
        return NULL;
    }
    u->cap = code_cap;
    u->trap = u->code + code_cap;
    for (k = 0; k < PAGE / 4; ++k) put_word(u->a, u->trap + 4 * k, u->a->ret);
    if (arch == HOSTUC_S390X) {
        /* 64-bit addressing (the PSW mask's EA and BA bits, 31 and 32 of its big-endian numbering) */
        uint64_t pswm = 0;
        uc_reg_read(u->uc, UC_S390X_REG_PSWM, &pswm);
        pswm |= 1ull << 32 | 1ull << 31;
        uc_reg_write(u->uc, UC_S390X_REG_PSWM, &pswm);
    }
    if (arch == HOSTUC_MIPS64EL || arch == HOSTUC_MIPS64) {
        uc_hook tlb;
        for (k = 1; k < PAGE / 4; k += 2) memset(u->trap + 4 * k, 0, 4); /* each jr's delay slot a nop */
        uc_ctl_tlb_mode(u->uc, UC_TLB_VIRTUAL);
        uc_hook_add(u->uc, &tlb, UC_HOOK_TLB_FILL, (void*)identity, NULL, 1, 0);
    }
    if (uc_hook_add(u->uc, &hook, UC_HOOK_CODE, (void*)on_trap, u, (uint64_t)(uintptr_t)u->trap,
                    (uint64_t)(uintptr_t)u->trap + PAGE - 1) != UC_ERR_OK) {
        hostuc_free(u);
        return NULL;
    }
    return u;
}

void hostuc_free(HostUc* u) {
    size_t k;
    if (!u) return;
    if (u->uc) uc_close(u->uc);
    free(u->la);
    for (k = 0; k < u->ntraps; ++k) free(u->name[k]);
    for (k = 0; k < u->nsyms; ++k) free(u->syms[k].name);
    free(u->syms);
    free(u);
}

void* hostuc_alloc(HostUc* u, size_t size) {
    void* p = hostuc_pages(size);
    if (!p || hostuc_map(u, p, size)) return NULL;
    return p;
}

uint64_t hostuc_trap_of(HostUc* u, const char* name) {
    unsigned k;
    for (k = 0; k < u->ntraps; ++k)
        if (!strcmp(u->name[k], name)) return (uint64_t)(uintptr_t)(u->trap + 16 * k);
    return 0;
}
static uint64_t native(HostUc* u, const char* name, void* fn, int wide) {
    uint64_t at = hostuc_trap_of(u, name);
    if (at) return at;
    if (u->ntraps == NTRAPS) return 0;
    u->fn[u->ntraps] = fn;
    u->name[u->ntraps] = strdup(name);
    u->wide[u->ntraps] = (unsigned char)wide;
    return (uint64_t)(uintptr_t)(u->trap + 16 * u->ntraps++);
}
uint64_t hostuc_native(HostUc* u, const char* name, void* fn) { return native(u, name, fn, 0); }
uint64_t hostuc_native_wide(HostUc* u, const char* name, void* fn) { return native(u, name, fn, 1); }

void hostuc_changed(HostUc* u, const void* p, size_t size) {
    if (!u->uc) return; /* the interpreter reads the words as they are */
    uc_ctl_remove_cache(u->uc, (uint64_t)(uintptr_t)p, (uint64_t)(uintptr_t)p + size);
}
uint64_t hostuc_code(HostUc* u, const void* bytes, size_t size) {
    size_t at = (u->used + 15) & ~(size_t)15;
    if (at + size > u->cap) return 0;
    memcpy(u->code + at, bytes, size);
    u->used = at + size;
    hostuc_changed(u, u->code + at, size);
    return (uint64_t)(uintptr_t)(u->code + at);
}

int hostuc_define(HostUc* u, const char* name, uint64_t address) {
    if (u->nsyms == u->cap_syms) {
        UcSym* grown = (UcSym*)realloc(u->syms, (u->cap_syms ? 2 * u->cap_syms : 256) * sizeof *u->syms);
        if (!grown) return -1;
        u->syms = grown;
        u->cap_syms = u->cap_syms ? 2 * u->cap_syms : 256;
    }
    if (!(u->syms[u->nsyms].name = strdup(name))) return -1;
    u->syms[u->nsyms++].at = address;
    return 0;
}
uint64_t hostuc_symbol(HostUc* u, const char* name) {
    size_t k;
    for (k = 0; k < u->nsyms; ++k)
        if (!strcmp(u->syms[k].name, name)) return u->syms[k].at;
    return 0;
}

/* the bytes a relocation of the architecture's type writes, where they're a number of its order (a big-endian one's
   turned over around snx_elf_relocate, which writes this host's): 0 for a type not known here */
static unsigned reloc_bytes(unsigned em, unsigned type) {
    if (em == EM_PPC64) return type == SNX_PPC_ADDR64 || type == SNX_PPC_REL64 ? 8 : 0;
    if (em == EM_MIPS) return type == SNX_MIPS_64 ? 8 : type == SNX_MIPS_PC32 ? 4 : 0;
    if (em == EM_S390) return type == SNX_390_64 || type == SNX_390_PC64 ? 8 : type == SNX_390_PC32DBL ? 4 : 0;
    return 0;
}
static int load(HostUc* u, const unsigned char* object, size_t size, char* error, size_t error_size);
/* the object's sections laid out one after another in the region (the data it holds bytes of and .bss zeroed, then the
   code), its symbols at their places, each RELA section's relocations written; an object of the other byte order's
   tables read turned over */
int hostuc_load(HostUc* u, const unsigned char* object, size_t size, char* error, size_t error_size) {
    unsigned char* copy;
    int rc;
    if (size < EI_NIDENT || object[EI_DATA] == snx_elf_host_data()) return load(u, object, size, error, error_size);
    if (!(copy = (unsigned char*)malloc(size))) return -1;
    memcpy(copy, object, size);
    if (snx_elf_byteswap(copy, size, 0)) {
        snprintf(error, error_size, "an object of the other byte order whose tables aren't inside it");
        free(copy);
        return -1;
    }
    rc = load(u, copy, size, error, error_size);
    free(copy);
    return rc;
}
static int load(HostUc* u, const unsigned char* object, size_t size, char* error, size_t error_size) {
    const Elf64_Ehdr* eh = (const Elf64_Ehdr*)(const void*)object;
    const Elf64_Shdr* sh;
    uint64_t* at;
    size_t k, r;
    const Elf64_Sym* syms = NULL;
    size_t nsyms = 0;
    const char* strtab = NULL;
    if (size < sizeof *eh || memcmp(eh->e_ident, ELFMAG, SELFMAG) || eh->e_machine != u->a->em) {
        snprintf(error, error_size, "not an ELF %s object", u->a->name);
        return -1;
    }
    sh = (const Elf64_Shdr*)(const void*)(object + eh->e_shoff);
    at = (uint64_t*)calloc(eh->e_shnum, sizeof *at);
    if (!at) return -1;
    for (k = 0; k < eh->e_shnum; ++k)
        if (sh[k].sh_type == SHT_SYMTAB) {
            syms = (const Elf64_Sym*)(const void*)(object + sh[k].sh_offset);
            nsyms = sh[k].sh_size / sizeof *syms;
            strtab = (const char*)(object + sh[sh[k].sh_link].sh_offset);
        }
    /* the data first, as a module's read-only data is before its code: the code's distances to it are negative */
    for (int code = 0; code < 2; ++code)
        for (k = 0; k < eh->e_shnum; ++k)
            if ((sh[k].sh_flags & SHF_ALLOC) && (sh[k].sh_type == SHT_PROGBITS || sh[k].sh_type == SHT_NOBITS) &&
                sh[k].sh_size && !(sh[k].sh_flags & SHF_EXECINSTR) == !code) {
                /* .bss and its kin: zeroes of their size */
                void* zero = sh[k].sh_type == SHT_NOBITS ? calloc(1, sh[k].sh_size) : NULL;
                at[k] = hostuc_code(u, zero ? zero : object + sh[k].sh_offset, sh[k].sh_size);
                free(zero);
                if (!at[k]) {
                    snprintf(error, error_size, "no room for section %zu", k);
                    free(at);
                    return -1;
                }
            }
    for (k = 0; k < nsyms; ++k)
        if (syms[k].st_shndx != SHN_UNDEF && syms[k].st_shndx < eh->e_shnum && syms[k].st_name &&
            ELF64_ST_TYPE(syms[k].st_info) != STT_SECTION) {
            if (u->nsyms == u->cap_syms) {
                u->cap_syms = u->cap_syms ? 2 * u->cap_syms : 256;
                u->syms = (UcSym*)realloc(u->syms, u->cap_syms * sizeof *u->syms);
            }
            u->syms[u->nsyms].name = strdup(strtab + syms[k].st_name);
            u->syms[u->nsyms++].at = at[syms[k].st_shndx] + syms[k].st_value;
        }
    for (k = 0; k < eh->e_shnum; ++k) {
        const Elf64_Rela* rela;
        size_t target = sh[k].sh_info;
        if (sh[k].sh_type != SHT_RELA || !at[target]) continue;
        rela = (const Elf64_Rela*)(const void*)(object + sh[k].sh_offset);
        for (r = 0; r < sh[k].sh_size / sizeof *rela; ++r) {
            const Elf64_Sym* s = &syms[snx_elf_symbol(u->a->em, rela[r].r_info)];
            unsigned type = snx_elf_type(u->a->em, rela[r].r_info);
            uint64_t S, P = at[target] + rela[r].r_offset;
            int e;
            if (ELF64_ST_TYPE(s->st_info) == STT_SECTION) S = at[s->st_shndx];
            else if (s->st_shndx != SHN_UNDEF)
                S = at[s->st_shndx] + s->st_value;
            else if (!(S = hostuc_trap_of(u, strtab + s->st_name)) && !(S = hostuc_symbol(u, strtab + s->st_name))) {
                snprintf(error, error_size, "an undefined symbol no native function or definition is: %s",
                         strtab + s->st_name);
                free(at);
                return -1;
            }
            if (u->a->big) {
                unsigned n = reloc_bytes(u->a->em, type);
                if (!n) {
                    snprintf(error, error_size, "relocation %u of a big-endian object: not known here", type);
                    free(at);
                    return -1;
                }
                e = snx_elf_relocate(u->a->em, (unsigned char*)(uintptr_t)P,
                                     (size_t)(at[target] + sh[target].sh_size - P), type, S, rela[r].r_addend, P, 0, 0);
                if (n == 8) {
                    uint64_t v;
                    memcpy(&v, (void*)(uintptr_t)P, 8), v = snx_bswap64(v), memcpy((void*)(uintptr_t)P, &v, 8);
                } else {
                    uint32_t v;
                    memcpy(&v, (void*)(uintptr_t)P, 4), v = snx_bswap32(v), memcpy((void*)(uintptr_t)P, &v, 4);
                }
            } else
                e = snx_elf_relocate(u->a->em, (unsigned char*)(uintptr_t)P,
                                     (size_t)(at[target] + sh[target].sh_size - P), type, S, rela[r].r_addend, P, 0, 0);
            if (e) {
                snprintf(error, error_size, "relocation %u of %s: %d", type, strtab + s->st_name, e);
                free(at);
                return -1;
            }
        }
    }
    hostuc_changed(u, u->code, u->cap);
    free(at);
    return 0;
}

int hostuc_call(HostUc* u, uint64_t fn, uint64_t a0, uint64_t a1, uint64_t* result) {
    /* the stack's top less a caller's frame header (ELFv2's callee saves LR at 16(r1), in its caller's frame) */
    uint64_t sp = (uint64_t)(uintptr_t)(u->stack + STACK_SIZE - 256), lr = (uint64_t)(uintptr_t)(u->trap + PAGE - 16);
    uc_err e;
    if (u->la) {
        La64* c = u->la;
        memset(c->r, 0, sizeof c->r);
        c->r[4] = a0, c->r[5] = a1, c->r[3] = sp, c->r[1] = lr, c->pc = fn, c->ll_on = 0;
        if (la64_run(c, 1u << 22)) {
            fprintf(stderr, "loongarch64 interpreter: stopped at %llx (%+lld from the code): %s\n",
                    (unsigned long long)c->pc, (long long)(c->pc - (uint64_t)(uintptr_t)u->code), c->why);
            return -1;
        }
        if (result) *result = c->r[4];
        return 0;
    }
    uc_reg_write(u->uc, u->a->args[0], &a0);
    uc_reg_write(u->uc, u->a->args[1], &a1);
    uc_reg_write(u->uc, u->a->sp, &sp);
    uc_reg_write(u->uc, u->a->lr, &lr);
    if (u->a->arch == UC_ARCH_PPC) uc_reg_write(u->uc, UC_PPC_REG_12, &fn); /* an ELFv2 global entry's */
    if (u->a->arch == UC_ARCH_MIPS) uc_reg_write(u->uc, UC_MIPS_REG_25, &fn); /* t9: an n64 PIC callee's */
    u->sp = sp;
    if (u->a->arch == UC_ARCH_ARM64) mark_kept(u);
    swap(u);
    e = uc_emu_start(u->uc, fn, lr, 0, 1u << 22);
    swap(u);
    if (e != UC_ERR_OK) {
        uint64_t pc;
        uc_reg_read(u->uc, u->a->pc, &pc);
        fprintf(stderr, "%s unicorn: stopped at %llx (%+lld from the code): %s\n", u->a->name, (unsigned long long)pc,
                (long long)(pc - (uint64_t)(uintptr_t)u->code), uc_strerror(e));
        return -1;
    }
    if (result) uc_reg_read(u->uc, u->a->result[0], result);
    return u->a->arch == UC_ARCH_ARM64 && kept_changed(u) ? -1 : 0;
}
