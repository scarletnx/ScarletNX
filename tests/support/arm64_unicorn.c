/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* arm64 code under Unicorn for the arm64 back end's tests (arm64_unicorn.h) */
#include "support/arm64_unicorn.h"
#include "common/cpu/recompiler/llvm/elf_object.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unicorn/unicorn.h>

#define PAGE 4096u
#define NTRAPS (PAGE / 16 - 1) /* the last entry is the exit's */
#define STACK_SIZE (256u << 10)

typedef struct {
    char* name;
    uint64_t at;
} UcSym;

struct ArmUc {
    uc_engine* uc;
    unsigned char* code; /* the region: code, then its trap page */
    size_t cap, used;
    unsigned char* trap;
    void* fn[NTRAPS];
    char* name[NTRAPS];
    unsigned ntraps;
    unsigned char* stack;
    UcSym* syms;
    size_t nsyms, cap_syms;
};

static void* page_alloc(size_t size) {
    void* p = mmap(NULL, (size + PAGE - 1) & ~(size_t)(PAGE - 1), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}
static int map(ArmUc* u, void* p, size_t size) {
    uc_err e = uc_mem_map_ptr(u->uc, (uint64_t)(uintptr_t)p, (size + PAGE - 1) & ~(size_t)(PAGE - 1), UC_PROT_ALL, p);
    if (e != UC_ERR_OK) {
        fprintf(stderr, "arm64 unicorn: no map of %p: %s\n", p, uc_strerror(e));
        return -1;
    }
    return 0;
}

/* the native function of a trap the code reached, with X0 to X5 (System V passes them the same way, a function of
   fewer ignoring the rest), its result into X0; the trap's RET goes back */
typedef uint64_t (*Native)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
static void on_trap(uc_engine* uc, uint64_t address, uint32_t size, void* user) {
    ArmUc* u = (ArmUc*)user;
    unsigned k = (unsigned)((address - (uint64_t)(uintptr_t)u->trap) / 16), i;
    static const int regs[6] = {UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2,
                                UC_ARM64_REG_X3, UC_ARM64_REG_X4, UC_ARM64_REG_X5};
    uint64_t a[6], r;
    (void)size;
    if (k >= u->ntraps) return;
    for (i = 0; i < 6; ++i) uc_reg_read(uc, regs[i], &a[i]);
    r = ((Native)u->fn[k])(a[0], a[1], a[2], a[3], a[4], a[5]);
    uc_reg_write(uc, UC_ARM64_REG_X0, &r);
}

ArmUc* armuc_new(size_t code_cap) {
    ArmUc* u = (ArmUc*)calloc(1, sizeof *u);
    uc_hook hook;
    unsigned k;
    if (!u) return NULL;
    code_cap = (code_cap + PAGE - 1) & ~(size_t)(PAGE - 1);
    if (uc_open(UC_ARCH_ARM64, UC_MODE_ARM, &u->uc) != UC_ERR_OK) {
        free(u);
        return NULL;
    }
    uc_ctl_set_cpu_model(u->uc, UC_CPU_ARM64_MAX);
    u->code = (unsigned char*)page_alloc(code_cap + PAGE);
    u->stack = (unsigned char*)page_alloc(STACK_SIZE);
    if (!u->code || !u->stack || map(u, u->code, code_cap + PAGE) || map(u, u->stack, STACK_SIZE)) {
        armuc_free(u);
        return NULL;
    }
    u->cap = code_cap;
    u->trap = u->code + code_cap;
    for (k = 0; k < PAGE / 4; ++k) { /* RET */
        u->trap[4 * k] = 0xC0, u->trap[4 * k + 1] = 0x03, u->trap[4 * k + 2] = 0x5F, u->trap[4 * k + 3] = 0xD6;
    }
    if (uc_hook_add(u->uc, &hook, UC_HOOK_CODE, (void*)on_trap, u, (uint64_t)(uintptr_t)u->trap,
                    (uint64_t)(uintptr_t)u->trap + PAGE - 1) != UC_ERR_OK) {
        armuc_free(u);
        return NULL;
    }
    return u;
}

void armuc_free(ArmUc* u) {
    size_t k;
    if (!u) return;
    if (u->uc) uc_close(u->uc);
    for (k = 0; k < u->ntraps; ++k) free(u->name[k]);
    for (k = 0; k < u->nsyms; ++k) free(u->syms[k].name);
    free(u->syms);
    free(u);
}

void* armuc_alloc(ArmUc* u, size_t size) {
    void* p = page_alloc(size);
    if (!p || map(u, p, size)) return NULL;
    return p;
}

uint64_t armuc_native(ArmUc* u, const char* name, void* fn) {
    unsigned k;
    for (k = 0; k < u->ntraps; ++k)
        if (!strcmp(u->name[k], name)) return (uint64_t)(uintptr_t)(u->trap + 16 * k);
    if (u->ntraps == NTRAPS) return 0;
    u->fn[u->ntraps] = fn;
    u->name[u->ntraps] = strdup(name);
    return (uint64_t)(uintptr_t)(u->trap + 16 * u->ntraps++);
}
static uint64_t native_of(ArmUc* u, const char* name) {
    unsigned k;
    for (k = 0; k < u->ntraps; ++k)
        if (!strcmp(u->name[k], name)) return (uint64_t)(uintptr_t)(u->trap + 16 * k);
    return 0;
}

uint64_t armuc_code(ArmUc* u, const void* bytes, size_t size) {
    size_t at = (u->used + 15) & ~(size_t)15;
    if (at + size > u->cap) return 0;
    memcpy(u->code + at, bytes, size);
    u->used = at + size;
    uc_ctl_remove_cache(u->uc, (uint64_t)(uintptr_t)(u->code + at), (uint64_t)(uintptr_t)(u->code + at + size));
    return (uint64_t)(uintptr_t)(u->code + at);
}

uint64_t armuc_symbol(ArmUc* u, const char* name) {
    size_t k;
    for (k = 0; k < u->nsyms; ++k)
        if (!strcmp(u->syms[k].name, name)) return u->syms[k].at;
    return 0;
}

/* the object's sections laid out one after another in the region (.text and the others it holds bytes of), its
   symbols at their places, each RELA section's relocations written */
int armuc_load(ArmUc* u, const unsigned char* object, size_t size, char* error, size_t error_size) {
    const Elf64_Ehdr* eh = (const Elf64_Ehdr*)(const void*)object;
    const Elf64_Shdr* sh;
    uint64_t* at;
    size_t k, r;
    const Elf64_Sym* syms = NULL;
    size_t nsyms = 0;
    const char* strtab = NULL;
    if (size < sizeof *eh || memcmp(eh->e_ident, ELFMAG, SELFMAG) || eh->e_machine != EM_AARCH64) {
        snprintf(error, error_size, "not an ELF AArch64 object");
        return -1;
    }
    sh = (const Elf64_Shdr*)(const void*)(object + eh->e_shoff);
    at = (uint64_t*)calloc(eh->e_shnum, sizeof *at);
    if (!at) return -1;
    for (k = 0; k < eh->e_shnum; ++k) {
        if (sh[k].sh_type == SHT_SYMTAB) {
            syms = (const Elf64_Sym*)(const void*)(object + sh[k].sh_offset);
            nsyms = sh[k].sh_size / sizeof *syms;
            strtab = (const char*)(object + sh[sh[k].sh_link].sh_offset);
        }
        if ((sh[k].sh_flags & SHF_ALLOC) && sh[k].sh_type == SHT_PROGBITS && sh[k].sh_size) {
            at[k] = armuc_code(u, object + sh[k].sh_offset, sh[k].sh_size);
            if (!at[k]) {
                snprintf(error, error_size, "no room for section %zu", k);
                free(at);
                return -1;
            }
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
            const Elf64_Sym* s = &syms[ELF64_R_SYM(rela[r].r_info)];
            unsigned type = (unsigned)ELF64_R_TYPE(rela[r].r_info);
            uint64_t S, P = at[target] + rela[r].r_offset;
            int e;
            if (ELF64_ST_TYPE(s->st_info) == STT_SECTION) S = at[s->st_shndx];
            else if (s->st_shndx != SHN_UNDEF)
                S = at[s->st_shndx] + s->st_value;
            else if (!(S = native_of(u, strtab + s->st_name))) {
                snprintf(error, error_size, "an undefined symbol no native function is: %s", strtab + s->st_name);
                free(at);
                return -1;
            }
            e = snx_elf_relocate(EM_AARCH64, (unsigned char*)(uintptr_t)P,
                                 (size_t)(at[target] + sh[target].sh_size - P), type, S, rela[r].r_addend, P, 0, 0);
            if (e) {
                snprintf(error, error_size, "relocation %u of %s: %d", type, strtab + s->st_name, e);
                free(at);
                return -1;
            }
        }
    }
    uc_ctl_remove_cache(u->uc, (uint64_t)(uintptr_t)u->code, (uint64_t)(uintptr_t)(u->code + u->cap));
    free(at);
    return 0;
}

int armuc_call(ArmUc* u, uint64_t fn, uint64_t x0, uint64_t x1, uint64_t* result) {
    uint64_t sp = (uint64_t)(uintptr_t)(u->stack + STACK_SIZE), lr = (uint64_t)(uintptr_t)(u->trap + PAGE - 16);
    uc_err e;
    uc_reg_write(u->uc, UC_ARM64_REG_X0, &x0);
    uc_reg_write(u->uc, UC_ARM64_REG_X1, &x1);
    uc_reg_write(u->uc, UC_ARM64_REG_SP, &sp);
    uc_reg_write(u->uc, UC_ARM64_REG_X30, &lr);
    e = uc_emu_start(u->uc, fn, lr, 0, 1u << 22);
    if (e != UC_ERR_OK) {
        uint64_t pc;
        uc_reg_read(u->uc, UC_ARM64_REG_PC, &pc);
        fprintf(stderr, "arm64 unicorn: stopped at %llx (%+lld from the code): %s\n", (unsigned long long)pc,
                (long long)(pc - (uint64_t)(uintptr_t)u->code), uc_strerror(e));
        return -1;
    }
    if (result) uc_reg_read(u->uc, UC_ARM64_REG_X0, result);
    return 0;
}
